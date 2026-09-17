#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

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
};

struct UsdzPreflightLimits {
    uint32_t maxEntries = 4096;
    uint32_t maxPathDepth = 32;
    uint64_t maxEntryBytes = 2ull * 1024ull * 1024ull * 1024ull;
    uint64_t maxExpandedBytes = 4ull * 1024ull * 1024ull * 1024ull;
    uint32_t maxExpansionRatio = 200;
};

UsdzPreflightError PreflightUsdz(std::span<const std::byte> bytes,
                                 const UsdzPreflightLimits& limits = {});

} // namespace import_worker
