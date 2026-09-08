#pragma once

// A synthetic, purely in-process mapped-geometry generator for Gate 2
// workstream B slice 3 -- deliberately distinct from
// import-worker/src/SyntheticSceneGenerator.cpp (Gate 2 workstream A),
// which serializes into the cross-process wire format
// (shared/model-core/include/model_core/WireFormat.h) and has no
// bounds/LOD concept at all. This one exists purely to exercise
// D3D12UploadRing and fence-complete publication end-to-end with bounded
// clusters that carry real spatial bounds and three explicit LOD levels,
// per .docs/design/10-delivery-plan.md's Gate 2 deliverable list
// ("synthetic mapped geometry generator emitting bounded clusters/proxy/
// three LODs"). No wire format, no shared section, no process boundary
// involved -- output feeds D3D12UploadRing::Upload() directly as plain
// in-memory vertex buffers.

#include <model_core/VertexLayouts.h>

#include <cstdint>
#include <vector>

struct AxisAlignedBounds
{
    float min[3];
    float max[3];
};

enum class SyntheticLod : uint32_t {
    Proxy = 0,
    Mid = 1,
    Full = 2,
};

struct SyntheticLodLevel
{
    SyntheticLod lod;
    std::vector<model_core::VertexPositionNormalUv0F32> vertices;
};

struct SyntheticCluster
{
    uint32_t clusterId;
    AxisAlignedBounds bounds;
    // Always exactly 3 entries, coarsest to finest: Proxy, Mid, Full.
    std::vector<SyntheticLodLevel> lods;
};

// Deterministic: the same (clusterCount, seed) always produces the same
// geometry, byte for byte -- the test suite built on top of this must
// never need to tolerate run-to-run variation.
std::vector<SyntheticCluster> GenerateSyntheticClusters(uint32_t clusterCount, uint32_t seed);
