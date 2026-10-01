#pragma once

// Deterministic scene generation for the SPIKE-8a driver and tests: a small
// canonical mesh and point cloud for goldens, plus cap-sized generators used to
// measure the provider budgets (2M triangles inspected / 6M points inspected /
// 250k rasterized samples). Sampling is reservoir-based and driven by a stable
// caller seed, so identical inputs produce identical output.

#include "ThumbnailRasterizer.h"

#include <cstdint>
#include <vector>

namespace thumbnail_spike
{

struct MeshScene
{
    std::vector<thumbnail_rasterizer::Triangle> triangles;
    std::uint64_t inspected = 0;
};

struct PointScene
{
    std::vector<thumbnail_rasterizer::Point> points;
    std::uint64_t inspected = 0;
};

std::uint64_t SeedFromString(const char* text);

// Small canonical scenes used for golden images.
MeshScene MakeGoldenMesh();
PointScene MakeGoldenPointCloud();

// Cap-sized scenes: emit `total` primitives, retain at most `maxSamples`
// deterministic representatives, and report how many were inspected.
MeshScene GenerateLargeMesh(std::uint64_t totalTriangles, std::size_t maxSamples, std::uint64_t seed);
PointScene GenerateLargePointCloud(std::uint64_t totalPoints, std::size_t maxSamples, std::uint64_t seed);

} // namespace thumbnail_spike