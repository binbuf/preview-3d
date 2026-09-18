#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace import_worker {

enum class UsdzPreflightError : uint32_t {
    None = 0,
    NotZip,
    Truncated,
    MultiDisk,
    Zip64,
    TooManyEntries,
    UnsafePath,
    DuplicatePath,
    Encrypted,
    UnsupportedCompression,
    EntryTooLarge,
    AggregateTooLarge,
    ExpansionRatio,
    Misaligned,
    InvalidDirectory,
    Cancelled,
};

struct UsdzPreflightLimits {
    uint32_t maxEntries = 4096;
    uint32_t maxPathDepth = 32;
    uint64_t maxEntryBytes = 2ull * 1024ull * 1024ull * 1024ull;
    uint64_t maxExpandedBytes = 4ull * 1024ull * 1024ull * 1024ull;
    uint32_t maxExpansionRatio = 200;
};

// Product-owned view of a stored USDZ entry. The span remains backed by the
// brokered primary mapping; archive contents are never extracted to disk.
struct UsdzEntryView {
    std::string name;
    std::string foldedName;
    uint64_t dataOffset = 0;
    uint64_t byteSize = 0;
};

struct UsdzArchiveView {
    std::vector<UsdzEntryView> entries;
    std::string rootLayerName;
};

// Performs the complete preflight and, on success, optionally returns the
// independently validated stored-entry map used by the USD asset resolver.
// Cancellation is checked while locating and walking the directory.
UsdzPreflightError InspectUsdz(std::span<const std::byte> bytes,
                               UsdzArchiveView* archive,
                               const UsdzPreflightLimits& limits = {},
                               const std::function<bool()>& isCancelled = {});

UsdzPreflightError PreflightUsdz(std::span<const std::byte> bytes,
                                 const UsdzPreflightLimits& limits = {});

} // namespace import_worker
