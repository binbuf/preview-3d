#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace import_worker {

// Product policy, deliberately separate from USDZ.  This is an OPC archive
// validator, not a general ZIP extractor: it never writes a package part.
enum class ThreeMfOpcError : uint32_t {
    None, NotZip, Truncated, MultiDisk, InvalidDirectory, UnsupportedCompression,
    Encrypted, UnsafePath, DuplicatePart, EntryLimit, EntryTooLarge,
    AggregateTooLarge, ExpansionRatio, Overlap, MissingContentTypes,
    MissingRootRelationships, MissingStartPart, RelationshipLimit,
    ModelPartLimit, ExternalRelationship, InvalidXml, UnsupportedRequiredFeature,
    Cancelled,
};

struct ThreeMfOpcLimits {
    uint32_t maxEntries = 4096;
    uint32_t maxRelationships = 8192;
    uint32_t maxModelParts = 1024;
    uint32_t maxPathDepth = 32;
    uint64_t maxEntryBytes = 2ull * 1024 * 1024 * 1024;
    uint64_t maxExpandedBytes = 4ull * 1024 * 1024 * 1024;
    uint32_t maxExpansionRatio = 200;
};

struct ThreeMfOpcPart {
    std::string name;
    uint64_t dataOffset;
    uint64_t compressedBytes;
    uint64_t expandedBytes;
    uint32_t crc32;
    uint16_t method;
};
struct ThreeMfOpcPackage { std::vector<ThreeMfOpcPart> parts; std::string startPart; };

ThreeMfOpcError InspectThreeMfOpc(std::span<const std::byte> bytes, ThreeMfOpcPackage* package = nullptr,
                                  const ThreeMfOpcLimits& limits = {},
                                  const std::function<bool()>& isCancelled = {});

// Extracts one already-preflighted stored/Deflate part into bounded memory and
// verifies its central-directory CRC. No path lookup or filesystem access is
// performed here; callers select an exact part from ThreeMfOpcPackage.
ThreeMfOpcError ExtractThreeMfOpcPart(std::span<const std::byte> bytes,
                                      const ThreeMfOpcPart& part,
                                      std::vector<std::byte>& output,
                                      uint64_t maxExpandedBytes,
                                      const std::function<bool()>& isCancelled = {});
}
