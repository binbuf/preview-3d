#pragma once

#include <cstdint>

namespace import_worker {

// Test-only StartGeneration scene variant understood exclusively by
// --fbx-spike-pool. It is deliberately absent from ControlProtocol.h: no
// production dispatcher or format route accepts it.
constexpr uint32_t kFbxSpikeEvaluationSceneVariant = 0xfb0001u;
constexpr uint64_t kFbxSpikePayloadMagic = 0x31584e4258503342ull; // "B3PXBNX1"

#pragma pack(push, 1)
struct FbxSpikePayloadHeader {
    uint64_t magic;
    uint64_t sourceByteLength;
    uint64_t cancellationEventHandleValue;
    volatile long evaluationState; // 0=not started, 1=entering, 2=inside allocator callback
    uint32_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(FbxSpikePayloadHeader) == 32);

int RunFbxSpikePoolMode();

} // namespace import_worker
