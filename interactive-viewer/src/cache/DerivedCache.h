#pragma once

// Crash-safe bounded derived-cache prototype -- Gate 2's own deliverable
// wording, not the full production cache design in
// .docs/design/04-rendering-and-streaming.md's "Persistent derived-data
// cache" section. Two deliberate scoping deltas from that section, both
// worth knowing before mistaking this for the finished design:
//
//   (a) identity uses only model_core::MappedFile::Identity() (volume
//       serial + FILE_ID_128 + size) -- no USN/change-journal capture, so
//       a volume-serial/file-ID collision after a journal reset isn't
//       detected. The design doc's "fast identity path" requires both;
//       this prototype proves the cache *mechanics* (hit/miss/corruption/
//       eviction/atomic-write), not the full production staleness
//       guarantee.
//   (b) an entry stores one opaque payload section (arbitrary bytes a
//       caller provides) rather than real serialized SceneSnapshot/chunk-
//       catalog data -- no format adapter in this repo yet produces that
//       shape outside the sandboxed worker's own wire format. Real
//       geometry serialization into this cache is a later Gate 3/4-
//       adjacent concern.
//
// Lives under interactive-viewer/ (not shared/) because ADR-012 frames
// this as viewer-owned, never shared with the worker or thumbnail
// provider. Not wired into Preview3D.vcxproj/Preview3D.cpp yet -- proven
// via Tests.Unit first, same discipline as every D3D12 graphics primitive.

#include <model_core/MappedFile.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct CacheEntryKey
{
    model_core::FileIdentity identity;
    uint32_t parserVersion = 0; // "parser/normalizer versions" per the design doc's opaque-key wording
};

class DerivedCache
{
public:
    struct Options
    {
        // Deliberately small prototype-scale defaults -- the design doc's
        // real caps are 2 GiB/entry and 10 GiB total.
        uint64_t maxEntryBytes = 64ull * 1024 * 1024;
        uint64_t maxTotalBytes = 256ull * 1024 * 1024;
    };

    // Ensures `directory` exists and removes any stray *.tmp files found
    // there -- "abandoned temporaries are removed on the next launch"; this
    // call IS "next launch" in this context.
    static std::optional<DerivedCache> Open(std::wstring directory, const Options& options,
                                             std::wstring& error);

    // Fail-closed: a missing entry, wrong magic/schema version, mismatched
    // identity/parserVersion, length mismatch, or a per-section SHA-256
    // mismatch are all a miss -- never a partial-trust read.
    std::optional<std::vector<std::byte>> TryGet(const CacheEntryKey& key) const;

    // Fails cleanly if payload.size() exceeds Options::maxEntryBytes.
    // Otherwise evicts oldest-by-last-write-time entries first if needed
    // to stay within Options::maxTotalBytes, then writes via a temp-file/
    // flush/rename atomic sequence mirroring
    // interactive-viewer/src/app/Settings.cpp's existing pattern.
    bool Put(const CacheEntryKey& key, std::span<const std::byte> payload, std::wstring& error);

    void Clear();

    // Enumerates the cache directory fresh on every call -- a prototype,
    // not a maintained index.
    uint64_t CurrentTotalBytes() const;

private:
    DerivedCache(std::wstring directory, Options options)
        : directory_(std::move(directory))
        , options_(options)
    {
    }

    std::wstring EntryPath(const CacheEntryKey& key) const;
    void EvictUntilFits(uint64_t incomingBytes);

    std::wstring directory_;
    Options options_;
};
