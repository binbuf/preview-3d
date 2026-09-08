#include "SyntheticStreamingSource.h"

using model_core::VertexPositionNormalUv0F32;

namespace {

constexpr float kClusterSpacing = 4.0f;
constexpr float kClusterHalfExtent = 0.5f;
constexpr float kBoundsHalfDepth = 0.01f; // clusters are flat grids on the local XY plane

uint32_t SubdivisionsForLod(SyntheticLod lod)
{
    switch (lod) {
    case SyntheticLod::Proxy:
        return 1;
    case SyntheticLod::Mid:
        return 2;
    case SyntheticLod::Full:
        return 4;
    }
    return 1;
}

// A subdivisions x subdivisions grid of quads (2 triangles each,
// non-indexed) on the XY plane at originZ, so Full genuinely carries more
// vertices than Mid, which carries more than Proxy -- a real, checkable
// "more detail" progression rather than a stand-in constant.
std::vector<VertexPositionNormalUv0F32> GenerateGrid(float originX, float originY, float originZ,
                                                       uint32_t subdivisions)
{
    std::vector<VertexPositionNormalUv0F32> vertices;
    vertices.reserve(static_cast<size_t>(subdivisions) * subdivisions * 6);

    float step = (2.0f * kClusterHalfExtent) / static_cast<float>(subdivisions);
    for (uint32_t row = 0; row < subdivisions; ++row) {
        for (uint32_t col = 0; col < subdivisions; ++col) {
            float x0 = originX - kClusterHalfExtent + step * static_cast<float>(col);
            float x1 = x0 + step;
            float y0 = originY - kClusterHalfExtent + step * static_cast<float>(row);
            float y1 = y0 + step;

            auto makeVertex = [&](float x, float y) {
                VertexPositionNormalUv0F32 v{};
                v.px = x;
                v.py = y;
                v.pz = originZ;
                v.nx = 0.0f;
                v.ny = 0.0f;
                v.nz = 1.0f;
                v.u = (x - originX + kClusterHalfExtent) / (2.0f * kClusterHalfExtent);
                v.v = (y - originY + kClusterHalfExtent) / (2.0f * kClusterHalfExtent);
                return v;
            };

            vertices.push_back(makeVertex(x0, y0));
            vertices.push_back(makeVertex(x1, y0));
            vertices.push_back(makeVertex(x1, y1));
            vertices.push_back(makeVertex(x0, y0));
            vertices.push_back(makeVertex(x1, y1));
            vertices.push_back(makeVertex(x0, y1));
        }
    }
    return vertices;
}

} // namespace

std::vector<SyntheticCluster> GenerateSyntheticClusters(uint32_t clusterCount, uint32_t seed)
{
    std::vector<SyntheticCluster> clusters;
    clusters.reserve(clusterCount);

    for (uint32_t i = 0; i < clusterCount; ++i) {
        float originX = static_cast<float>(i) * kClusterSpacing + static_cast<float>(seed % 7) * 0.01f;
        constexpr float originY = 0.0f;
        constexpr float originZ = 0.0f;

        SyntheticCluster cluster;
        cluster.clusterId = i;
        cluster.bounds.min[0] = originX - kClusterHalfExtent;
        cluster.bounds.min[1] = originY - kClusterHalfExtent;
        cluster.bounds.min[2] = originZ - kBoundsHalfDepth;
        cluster.bounds.max[0] = originX + kClusterHalfExtent;
        cluster.bounds.max[1] = originY + kClusterHalfExtent;
        cluster.bounds.max[2] = originZ + kBoundsHalfDepth;

        for (SyntheticLod lod : { SyntheticLod::Proxy, SyntheticLod::Mid, SyntheticLod::Full }) {
            SyntheticLodLevel level;
            level.lod = lod;
            level.vertices = GenerateGrid(originX, originY, originZ, SubdivisionsForLod(lod));
            cluster.lods.push_back(std::move(level));
        }
        clusters.push_back(std::move(cluster));
    }
    return clusters;
}
