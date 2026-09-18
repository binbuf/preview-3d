#pragma once

#include <cstdint>

namespace import_worker {

constexpr uint32_t kThreeMfSpikeSceneVariant = 0x336d6601u;
constexpr uint64_t kThreeMfSpikePayloadMagic = 0x31454b495053464dull; // "MFSPIKE1"

enum class ThreeMfSpikeMode : uint32_t {
    ReadAndInspect = 0,
    CancelFromProgress = 1,
    HoldAfterLoad = 2,
    ForceShortRead = 3,
    CancelDuringExtraction = 4,
    CancelDuringLattice = 5,
    SimulateSourceChange = 6,
};

enum class ThreeMfSpikeStatus : uint32_t {
    Pending = 0,
    Success = 1,
    Cancelled = 2,
    Malformed = 3,
    IoFailure = 4,
    ResourceLimit = 5,
    InternalError = 6,
};

#pragma pack(push, 1)
struct ThreeMfSpikePayloadHeader {
    uint64_t magic;
    uint64_t sourceFileHandleValue;
    uint64_t sourceByteLength;
    uint64_t cancellationEventHandleValue;
    uint32_t mode;
    volatile long state;
    uint32_t status;
    uint32_t libraryError;
    uint32_t versionMajor;
    uint32_t versionMinor;
    uint32_t versionMicro;
    uint32_t warningCount;
    uint32_t progressCallbackCount;
    uint32_t progressIdentifierMask;
    uint32_t readCallbackCount;
    uint32_t seekCallbackCount;
    uint32_t sparseSeekCount;
    uint32_t shortReadCount;
    uint32_t sourceChanged;
    uint32_t objectCount;
    uint32_t meshCount;
    uint32_t componentObjectCount;
    uint32_t buildItemCount;
    uint32_t materialResourceCount;
    uint32_t textureCount;
    uint32_t vertexCount;
    uint32_t triangleCount;
    uint32_t beamCount;
    uint32_t ballCount;
    uint32_t representationCount;
    uint32_t tessellatedTriangleCount;
    uint32_t instancingEligibleBeamCount;
    uint32_t reserved;
    uint64_t normalizedHash;
    uint64_t previewHash;
    uint64_t normalizedBytes;
    uint64_t sourceBytesRead;
    uint64_t loadMicroseconds;
    uint64_t extractMicroseconds;
    uint64_t privateBytesBefore;
    uint64_t privateBytesAfterLoad;
    uint64_t privateBytesAfterExtract;
    uint64_t peakWorkingSetBytes;
};
#pragma pack(pop)
static_assert(sizeof(ThreeMfSpikePayloadHeader) == 228);

int RunThreeMfSpikePoolMode();

} // namespace import_worker
