#pragma once

// USD-002 test-only contract. This deliberately does not extend protocol v10:
// USD-003 owns the production opcode/error/scene contract. The parent packs
// broker-approved assets into one inherited, read-only-in-principle section;
// the host receives opaque broker:/ identifiers and never receives a model
// path or directory handle.

#include <cstddef>
#include <cstdint>

namespace compatibility_host {

inline constexpr std::uint32_t kOpenUsdSpikeMagic = 0x32534455u; // "USD2"
inline constexpr std::uint32_t kOpenUsdSpikeVersion = 1;
inline constexpr std::size_t kOpenUsdIdentifierCapacity = 192;
inline constexpr std::uint32_t kOpenUsdMaxEntries = 64;
inline constexpr std::uint64_t kOpenUsdMaxSectionBytes = 64ull * 1024ull * 1024ull;

enum class OpenUsdSpikeMode : std::uint32_t {
    Compose = 0,
    Hang = 1,
    Crash = 2,
    ConsumeMemory = 3,
};

enum class OpenUsdSpikeStatus : std::uint32_t {
    Pending = 0,
    Success = 1,
    InvalidRequest = 2,
    PayloadIntegrityFailure = 3,
    StageOpenFailure = 4,
    ResolverDenied = 5,
    ResourceLimit = 6,
    InternalFailure = 7,
};

#pragma pack(push, 1)
struct OpenUsdSpikeAsset {
    char identifier[kOpenUsdIdentifierCapacity]{};
    std::uint64_t byteOffset = 0;
    std::uint64_t byteLength = 0;
};

struct OpenUsdSpikeSection {
    std::uint32_t magic = kOpenUsdSpikeMagic;
    std::uint32_t version = kOpenUsdSpikeVersion;
    std::uint64_t sectionByteLength = 0;
    std::uint32_t entryCount = 0;
    OpenUsdSpikeMode mode = OpenUsdSpikeMode::Compose;
    std::uint32_t maxPayloads = 8;
    std::uint32_t maxGraphDepth = 32;
    std::uint32_t maxMilliseconds = 5'000;
    std::uint32_t maxResolverOpens = 64;
    std::uint64_t maxApprovedAssetBytes = kOpenUsdMaxSectionBytes;
    char rootIdentifier[kOpenUsdIdentifierCapacity]{};

    volatile long state = 0; // 0=packed, 1=started, 2=stage open, 3=complete
    OpenUsdSpikeStatus status = OpenUsdSpikeStatus::Pending;
    std::uint32_t resolverOpenCount = 0;
    std::uint32_t resolverDeniedCount = 0;
    std::uint32_t payloadCount = 0;
    std::uint32_t primCount = 0;
    std::uint32_t meshCount = 0;
    std::uint32_t pointCount = 0;
    std::uint32_t faceCount = 0;
    std::uint32_t materialCount = 0;
    std::uint32_t textureAssetCount = 0;
    std::uint32_t registeredPluginCount = 0;
    std::uint32_t externalPluginCount = 0;
    std::uint64_t semanticDigest = 0;
    double metersPerUnit = 0.0;
    char upAxis = '?';
    char reserved0[7]{};
    double worldBounds[6]{};
    std::uint64_t startupMicroseconds = 0;
    std::uint64_t loadNoneMicroseconds = 0;
    std::uint64_t firstGeometryMicroseconds = 0;
    std::uint64_t totalMicroseconds = 0;
    std::uint64_t privateBytes = 0;
    std::uint64_t peakWorkingSetBytes = 0;
    char diagnostic[768]{};
    OpenUsdSpikeAsset entries[kOpenUsdMaxEntries]{};
};

struct OpenUsdSpikeControl {
    std::uint32_t magic = kOpenUsdSpikeMagic;
    std::uint32_t version = kOpenUsdSpikeVersion;
};
#pragma pack(pop)

static_assert(sizeof(OpenUsdSpikeControl) == 8);

} // namespace compatibility_host
