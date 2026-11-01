#pragma once

// Product-owned bounded admission for the FreeCAD Standard (`.fcstd`) package.
//
// FCStd is a ZIP container. This is a structural scanner, not an extractor to
// disk: it validates the EOCD/central/local headers, rejects encryption,
// multi-disk archives, streaming data descriptors and overlapping entries, and
// applies entry/aggregate/expansion-ratio ceilings before any FreeCAD data is
// interpreted. Entries stay in the brokered read-only mapping; only bounded
// extraction into private memory is ever performed.
//
// The design follows import-worker/src/ThreeMfOpcPreflight.cpp and
// UsdZipPreflight.cpp (source reuse, not process state), but FCStd is a
// generic ZIP package rather than OPC: it requires `Document.xml` and at least
// one evaluated shape payload (`*.brp`).

#include "model_core/ImportError.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace step_host {

enum class FcstdPreflightStatus : std::uint32_t {
    Ok = 0,
    NotZip = 1,
    Truncated = 2,
    MultiDisk = 3,
    Encrypted = 4,
    DataDescriptor = 5,     // streaming sizes in the local header
    UnsupportedCompression = 6,
    UnsafePath = 7,
    DuplicateEntry = 8,
    EntryLimit = 9,
    EntryTooLarge = 10,
    AggregateTooLarge = 11,
    ExpansionRatio = 12,
    InvalidDirectory = 13,
    Overlap = 14,
    MissingDocument = 15,
    MissingShape = 16,
    SourceLimit = 17,
    ReadFailure = 18,
    Cancelled = 19,
};

struct FcstdLimits {
    // Package structure.
    std::uint32_t maxEntries = 4096;
    std::uint32_t maxPathDepth = 16;
    std::uint64_t maxSourceBytes = 2ull * 1024 * 1024 * 1024;
    std::uint64_t maxEntryBytes = 256ull * 1024 * 1024;
    std::uint64_t maxExpandedBytes = 512ull * 1024 * 1024;
    std::uint32_t maxExpansionRatio = 200;
    // Document.xml / GuiDocument.xml interpretation.
    std::uint64_t maxXmlBytes = 8ull * 1024 * 1024;
    std::uint32_t maxXmlNodes = 200'000;
    std::uint32_t maxXmlDepth = 64;
    std::uint32_t maxObjects = 8'192;
};

struct FcstdEntry {
    std::string name;        // exactly as authored
    std::string foldedName;  // lowercase, path-separator normalized
    std::uint64_t dataOffset = 0;
    std::uint64_t compressedBytes = 0;
    std::uint64_t expandedBytes = 0;
    std::uint32_t crc32 = 0;
    std::uint16_t method = 0; // 0 stored, 8 deflate
    bool directory = false;
};

struct FcstdPackage {
    std::vector<FcstdEntry> entries;
    std::string documentEntry;  // `Document.xml` as authored
    bool hasGuiDocument = false;
};

// Validates the package structure and required parts. On success `package`
// (when supplied) carries the entry catalog used by the bounded reader.
FcstdPreflightStatus InspectFcstd(std::span<const std::byte> bytes,
                                  FcstdPackage* package = nullptr,
                                  const FcstdLimits& limits = {},
                                  const std::function<bool()>& isCancelled = {});

// Extracts one already-admitted entry into private memory, enforcing the
// declared expanded size and verifying the central-directory CRC. Nothing is
// written to disk and no path is opened.
FcstdPreflightStatus ExtractFcstdEntry(std::span<const std::byte> bytes,
                                       const FcstdEntry& entry,
                                       std::vector<std::byte>& output,
                                       std::uint64_t maxExpandedBytes,
                                       const std::function<bool()>& isCancelled = {});

// Case-insensitive lookup in the admitted catalog.
const FcstdEntry* FindFcstdEntry(const FcstdPackage& package, std::string_view foldedName);

// Maps a preflight status into the closed product error taxonomy (shared with
// StepHostImport so the test can assert the mapping without the host).
model_core::ImportErrorCode MapFcstdPreflightStatus(std::uint32_t status);

} // namespace step_host
