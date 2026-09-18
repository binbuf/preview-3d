#pragma once

#include <cstdint>

namespace import_worker {

constexpr uint32_t kUsdSpikeSceneVariant = 0x75736401u;
constexpr uint64_t kUsdSpikePayloadMagic = 0x3150534455334442ull;

enum class UsdSpikeMode : uint32_t {
    ParseAndConvert = 0,
    ResolverLayer = 1,
    HoldAfterParse = 2,
    OneMiBAdvisoryLimit = 3,
};

enum class UsdSpikeFormat : uint32_t {
    Unknown = 0,
    Usda = 1,
    Usdc = 2,
    Usdz = 3,
};

#pragma pack(push, 1)
struct UsdSpikePayloadHeader {
    uint64_t magic;
    uint64_t sourceByteLength;
    uint64_t cancellationEventHandleValue;
    uint32_t mode;
    volatile long state;
    uint32_t succeeded;
    uint32_t detectedFormat;
    uint32_t resolverCalls;
    uint32_t meshCount;
    uint32_t nodeCount;
    uint32_t materialCount;
    uint32_t imageCount;
    uint64_t normalizedHash;
    uint64_t normalizedBytes;
    uint64_t parseMicroseconds;
    uint64_t convertMicroseconds;
    uint64_t privateBytesBefore;
    uint64_t privateBytesAfterParse;
    uint64_t privateBytesAfterConvert;
    uint64_t peakWorkingSetBytes;
};
#pragma pack(pop)
static_assert(sizeof(UsdSpikePayloadHeader) == 124);

int RunUsdSpikePoolMode();

} // namespace import_worker
