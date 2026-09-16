#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace open_with
{
// Durable identities only. No model path or launch history is written to the
// Open With cache. `catalogId` is empty for an ordinary Windows-recommended
// handler and non-empty for a handler recognized by the curated app catalog.
struct CachedHandler
{
    std::wstring extension;
    std::wstring handlerName;
    std::wstring displayName;
    std::wstring catalogId;

    bool operator==(const CachedHandler&) const = default;
};

struct CacheState
{
    std::uint32_t catalogRevision = 0;
    std::uint64_t lastDiscoveryFileTime = 0;
    std::vector<CachedHandler> handlers;
};

// The decoder is intentionally independent of the filesystem so malformed,
// truncated, and oversized cache data can be unit-tested. Both operations are
// bounded; Decode rejects trailing bytes and clears `output` on every failure.
bool EncodeCache(const CacheState& state, std::vector<std::byte>& output);
bool DecodeCache(std::span<const std::byte> bytes, CacheState& output);
}
