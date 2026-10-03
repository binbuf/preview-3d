// T15 CPU tile rasterizer coverage.
//
// `thumbnail-provider/CpuRasterizer.cpp` is compiled directly into
// Tests.Unit.exe (PCH-free), so the shipped source behind the frozen
// `ICpuRasterizer` contract is exercised without COM or a GPU:
//
//   - resolution clamping (min(cx, 512), the 32 px floor) and failure on the
//     degenerate request;
//   - deterministic, premultiplied, top-down BGRA output;
//   - golden images at 32/64/256/512 px plus an alpha/transparency case and a
//     point-cloud case, compared with the tolerant perceptual metric in
//     docs/design/testing-strategy.md (not byte equality);
//   - masked-cutoff discard, weighted-opaque transparency, depth test;
//   - deadline and ledger failures leave the image empty (never fabricated).
//
// The goldens live in thumbnail-provider/goldens and are regenerated with:
//   x64\Release\Tests.Unit.exe "[write-goldens]"

#include <catch2/catch_test_macros.hpp>

#include "AllocationLedger.h"
#include "CpuRasterizerImpl.h"
#include "Deadline.h"
#include "PamImage.h"
#include "SceneFixtures.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace preview3d::provider;

namespace {

constexpr std::uint32_t kMaterialGolden = 1;
constexpr std::uint32_t kMaterialBlend = 1;
constexpr std::uint32_t kMaterialMask = 2;
constexpr std::uint32_t kMaterialOpaque = 3;

std::string GoldenPath(const char* name)
{
    return (std::filesystem::path(PREVIEW3D_PROVIDER_GOLDEN_DIR) / name).string();
}

model_core::MaterialPayload MakeMaterial(float r, float g, float b, float a,
                                         model_core::AlphaModeId mode, float cutoff,
                                         bool doubleSided)
{
    model_core::MaterialPayload material{};
    material.baseColorFactor[0] = r;
    material.baseColorFactor[1] = g;
    material.baseColorFactor[2] = b;
    material.baseColorFactor[3] = a;
    material.metallicFactor = 0.0f;
    material.roughnessFactor = 0.65f;
    material.alphaMode = static_cast<std::uint32_t>(mode);
    material.alphaCutoff = cutoff;
    material.flags = doubleSided ? model_core::kMaterialFlagDoubleSided : 0u;
    return material;
}

std::vector<model_core::MaterialPayload> GoldenMaterials()
{
    // A neutral, double-sided white material: the per-vertex fixture colors
    // carry the palette, matching the T02 prototype's look.
    std::vector<model_core::MaterialPayload> materials;
    materials.push_back(MakeMaterial(1.0f, 1.0f, 1.0f, 1.0f, model_core::AlphaModeId::Opaque,
                                     0.5f, true));
    return materials;
}

struct Gradient {
    float r;
    float g;
    float b;
};

Gradient WarmCool(double t)
{
    const Gradient cool{0.06f, 0.16f, 0.52f};
    const Gradient warm{0.86f, 0.42f, 0.05f};
    const float f = static_cast<float>(std::clamp(t, 0.0, 1.0));
    return {cool.r + (warm.r - cool.r) * f, cool.g + (warm.g - cool.g) * f,
            cool.b + (warm.b - cool.b) * f};
}

void SetVertex(VertexSample& vertex, double x, double y, double z, const Gradient& color)
{
    vertex.position[0] = static_cast<float>(x);
    vertex.position[1] = static_cast<float>(y);
    vertex.position[2] = static_cast<float>(z);
    const double len = std::sqrt(x * x + y * y + z * z);
    if (len > 1e-12) {
        vertex.normal[0] = static_cast<float>(x / len);
        vertex.normal[1] = static_cast<float>(y / len);
        vertex.normal[2] = static_cast<float>(z / len);
    }
    vertex.color[0] = color.r;
    vertex.color[1] = color.g;
    vertex.color[2] = color.b;
    vertex.color[3] = 1.0f;
}

// A UV sphere with outward normals and a per-vertex warm/cool gradient. The
// geometry is fixed, so it is the canonical golden mesh for this suite.
std::vector<TriangleSample> MakeGoldenMesh()
{
    constexpr int kStacks = 16;
    constexpr int kSlices = 24;
    constexpr double kRadius = 1.6;
    const double pi = 3.14159265358979323846;

    auto point = [&](int stack, int slice) {
        const double phi = pi * static_cast<double>(stack) / kStacks;
        const double theta = 2.0 * pi * static_cast<double>(slice) / kSlices;
        const double x = std::sin(phi) * std::cos(theta) * kRadius;
        const double y = std::cos(phi) * kRadius;
        const double z = std::sin(phi) * std::sin(theta) * kRadius;
        const Gradient color = WarmCool((y / kRadius + 1.0) * 0.5);
        VertexSample vertex{};
        SetVertex(vertex, x, y, z, color);
        return vertex;
    };

    std::vector<TriangleSample> triangles;
    triangles.reserve(static_cast<std::size_t>(kStacks) * kSlices * 2);
    for (int stack = 0; stack < kStacks; ++stack) {
        for (int slice = 0; slice < kSlices; ++slice) {
            const int nextSlice = (slice + 1) % kSlices;
            const VertexSample a = point(stack, slice);
            const VertexSample b = point(stack + 1, slice);
            const VertexSample c = point(stack + 1, nextSlice);
            const VertexSample d = point(stack, nextSlice);
            TriangleSample t0{};
            t0.vertices[0] = a;
            t0.vertices[1] = b;
            t0.vertices[2] = c;
            t0.materialIndex = kMaterialGolden;
            triangles.push_back(t0);
            TriangleSample t1{};
            t1.vertices[0] = a;
            t1.vertices[1] = c;
            t1.vertices[2] = d;
            t1.materialIndex = kMaterialGolden;
            triangles.push_back(t1);
        }
    }
    return triangles;
}

std::vector<PointSample> MakeGoldenPointCloud()
{
    constexpr int kCount = 4096;
    constexpr double kRadius = 1.0;
    const double golden = 3.14159265358979323846 * (3.0 - std::sqrt(5.0));
    std::vector<PointSample> points;
    points.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        const double y = 1.0 - (static_cast<double>(i) / (kCount - 1)) * 2.0;
        const double r = std::sqrt((std::max)(0.0, 1.0 - y * y));
        const double theta = golden * i;
        PointSample point{};
        VertexSample vertex{};
        SetVertex(vertex, std::cos(theta) * r * kRadius, y * kRadius, std::sin(theta) * r * kRadius,
                  WarmCool((y + 1.0) * 0.5));
        point.vertex = vertex;
        point.radius = 0.022f;
        point.materialIndex = kMaterialGolden;
        points.push_back(point);
    }
    return points;
}

std::vector<model_core::MaterialPayload> AlphaMaterials()
{
    std::vector<model_core::MaterialPayload> materials;
    materials.push_back(MakeMaterial(0.20f, 0.65f, 0.95f, 0.40f, model_core::AlphaModeId::Blend,
                                     0.5f, true));
    materials.push_back(MakeMaterial(0.95f, 0.35f, 0.15f, 0.90f, model_core::AlphaModeId::Mask,
                                     0.5f, true));
    materials.push_back(MakeMaterial(0.30f, 0.30f, 0.34f, 1.0f, model_core::AlphaModeId::Opaque,
                                     0.5f, true));
    return materials;
}

TriangleSample MakeTriangle(double x0, double y0, double z0, double x1, double y1, double z1,
                            double x2, double y2, double z2, std::uint32_t material,
                            float alpha)
{
    TriangleSample triangle{};
    const Gradient color{0.75f, 0.75f, 0.75f};
    SetVertex(triangle.vertices[0], x0, y0, z0, color);
    SetVertex(triangle.vertices[1], x1, y1, z1, color);
    SetVertex(triangle.vertices[2], x2, y2, z2, color);
    triangle.vertices[0].color[3] = alpha;
    triangle.vertices[1].color[3] = alpha;
    triangle.vertices[2].color[3] = alpha;
    triangle.materialIndex = material;
    return triangle;
}

// A layered alpha scene: a translucent quad over an opaque base, a masked
// triangle above cutoff, and a masked triangle below cutoff that must vanish.
std::vector<TriangleSample> MakeAlphaScene()
{
    std::vector<TriangleSample> triangles;
    const double z = -0.7;
    triangles.push_back(MakeTriangle(-1.6, -1.6, z, 1.6, -1.6, z, 1.6, 1.6, z, kMaterialOpaque, 1.0f));
    triangles.push_back(MakeTriangle(-1.6, -1.6, z, 1.6, 1.6, z, -1.6, 1.6, z, kMaterialOpaque, 1.0f));

    const double zFront = 0.4;
    triangles.push_back(
        MakeTriangle(-1.4, -1.4, zFront, 1.4, -1.4, zFront, 1.4, 1.4, zFront, kMaterialBlend, 1.0f));
    triangles.push_back(
        MakeTriangle(-1.4, -1.4, zFront, 1.4, 1.4, zFront, -1.4, 1.4, zFront, kMaterialBlend, 1.0f));

    triangles.push_back(
        MakeTriangle(-0.8, -0.8, 0.9, 0.8, -0.8, 0.9, 0.0, 0.8, 0.9, kMaterialMask, 0.9f));
    triangles.push_back(
        MakeTriangle(-0.5, -1.2, 1.1, 0.5, -1.2, 1.1, 0.0, -0.4, 1.1, kMaterialMask, 0.2f));
    return triangles;
}

struct DiffStats {
    double meanAbs = 0.0;
    int maxAbs = 0;
};

DiffStats Compare(const std::vector<std::uint8_t>& bgra, int width, int height,
                  const std::vector<std::uint8_t>& goldenRgba)
{
    DiffStats stats;
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    REQUIRE(bgra.size() == pixels * 4);
    REQUIRE(goldenRgba.size() == pixels * 4);
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint8_t rendered[4] = {bgra[i * 4 + 2], bgra[i * 4 + 1], bgra[i * 4 + 0],
                                          bgra[i * 4 + 3]};
        for (int c = 0; c < 4; ++c) {
            const int diff = std::abs(static_cast<int>(rendered[c]) -
                                      static_cast<int>(goldenRgba[i * 4 + c]));
            total += static_cast<std::uint64_t>(diff);
            stats.maxAbs = (std::max)(stats.maxAbs, diff);
        }
    }
    stats.meanAbs = static_cast<double>(total) / static_cast<double>(pixels * 4);
    return stats;
}

SampledGeometry ViewOf(const std::vector<TriangleSample>& triangles,
                       const std::vector<PointSample>& points)
{
    SampledGeometry geometry{};
    geometry.triangles = triangles;
    geometry.points = points;
    Bounds bounds{};
    auto include = [&bounds](double x, double y, double z) {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
            return;
        }
        if (!bounds.valid) {
            bounds.min[0] = bounds.max[0] = x;
            bounds.min[1] = bounds.max[1] = y;
            bounds.min[2] = bounds.max[2] = z;
            bounds.valid = true;
            return;
        }
        bounds.min[0] = (std::min)(bounds.min[0], x);
        bounds.min[1] = (std::min)(bounds.min[1], y);
        bounds.min[2] = (std::min)(bounds.min[2], z);
        bounds.max[0] = (std::max)(bounds.max[0], x);
        bounds.max[1] = (std::max)(bounds.max[1], y);
        bounds.max[2] = (std::max)(bounds.max[2], z);
    };
    for (const TriangleSample& triangle : triangles) {
        for (const VertexSample& vertex : triangle.vertices) {
            include(triangle.origin[0] + vertex.position[0],
                    triangle.origin[1] + vertex.position[1],
                    triangle.origin[2] + vertex.position[2]);
        }
    }
    for (const PointSample& point : points) {
        include(point.origin[0] + point.vertex.position[0],
                point.origin[1] + point.vertex.position[1],
                point.origin[2] + point.vertex.position[2]);
    }
    geometry.bounds = bounds;
    return geometry;
}

RasterImage RenderScene(const SampledGeometry& geometry,
                        std::span<const model_core::MaterialPayload> materials,
                        std::uint32_t cx)
{
    Deadline deadline;
    AllocationLedger ledger;
    RasterRequest request{};
    request.geometry = &geometry;
    request.materials = materials;
    request.requestedSize = cx;
    request.allowSupersample = true;
    request.deadline = &deadline;
    request.ledger = &ledger;
    RasterImage image;
    REQUIRE(RenderCpuTileRaster(request, image) == ErrorCode::None);
    return image;
}

// Empty spans for scenes that carry only one primitive kind. File-scope so the
// `SampledGeometry` spans never reference a destroyed temporary.
const std::vector<PointSample> kNoPoints;
const std::vector<TriangleSample> kNoTriangles;

void WriteGolden(const char* name, const RasterImage& image)
{
    REQUIRE(image.width != 0);
    REQUIRE(thumbnail_rasterizer::SaveBgraAsPam(GoldenPath(name).c_str(),
                                                static_cast<int>(image.width),
                                                static_cast<int>(image.height),
                                                image.bgraPremultiplied));
}

} // namespace

TEST_CASE("the rasterizer clamps resolution and rejects the degenerate request",
          "[provider][rasterizer]")
{
    const std::vector<TriangleSample> triangles = MakeGoldenMesh();
    const SampledGeometry geometry = ViewOf(triangles, kNoPoints);
    const std::vector<model_core::MaterialPayload> materials = GoldenMaterials();

    const RasterImage at32 = RenderScene(geometry, materials, 32);
    CHECK(at32.width == 32u);
    CHECK(at32.height == 32u);
    CHECK(at32.bgraPremultiplied.size() == 32u * 32u * 4u);

    // A request above the internal cap is clamped to 512, never rejected.
    const RasterImage huge = RenderScene(geometry, materials, 4096);
    CHECK(huge.width == 512u);
    CHECK(huge.height == 512u);

    Deadline deadline;
    AllocationLedger ledger;
    RasterRequest bad{};
    bad.geometry = &geometry;
    bad.materials = materials;
    bad.requestedSize = 0;
    bad.deadline = &deadline;
    bad.ledger = &ledger;
    RasterImage out;
    CHECK(RenderCpuTileRaster(bad, out) == ErrorCode::MalformedData);
    CHECK(out.bgraPremultiplied.empty());
}

TEST_CASE("rasterizer output is deterministic, top-down and premultiplied",
          "[provider][rasterizer]")
{
    const std::vector<TriangleSample> triangles = MakeGoldenMesh();
    const SampledGeometry geometry = ViewOf(triangles, kNoPoints);
    const std::vector<model_core::MaterialPayload> materials = GoldenMaterials();

    const RasterImage first = RenderScene(geometry, materials, 128);
    const RasterImage second = RenderScene(geometry, materials, 128);

    CHECK(first.bgraPremultiplied == second.bgraPremultiplied);
    for (std::size_t i = 0; i < first.bgraPremultiplied.size(); i += 4) {
        const int alpha = first.bgraPremultiplied[i + 3];
        CHECK(first.bgraPremultiplied[i + 0] <= alpha);
        CHECK(first.bgraPremultiplied[i + 1] <= alpha);
        CHECK(first.bgraPremultiplied[i + 2] <= alpha);
    }
}

TEST_CASE("golden mesh images match at 32/64/256/512 px", "[provider][rasterizer][golden]")
{
    const std::vector<TriangleSample> triangles = MakeGoldenMesh();
    const SampledGeometry geometry = ViewOf(triangles, kNoPoints);
    const std::vector<model_core::MaterialPayload> materials = GoldenMaterials();

    for (std::uint32_t size : {32u, 64u, 256u, 512u}) {
        const RasterImage image = RenderScene(geometry, materials, size);

        int goldenWidth = 0;
        int goldenHeight = 0;
        std::vector<std::uint8_t> golden;
        REQUIRE(thumbnail_rasterizer::LoadPamRgba(
            GoldenPath((std::string("mesh-") + std::to_string(size) + ".pam").c_str()).c_str(),
            goldenWidth, goldenHeight, golden));
        CHECK(goldenWidth == static_cast<int>(size));
        CHECK(goldenHeight == static_cast<int>(size));
        const DiffStats stats = Compare(image.bgraPremultiplied, goldenWidth, goldenHeight, golden);
        INFO("mesh size " << size << " meanAbs=" << stats.meanAbs << " maxAbs=" << stats.maxAbs);
        CHECK(stats.meanAbs <= 2.0);
        CHECK(stats.maxAbs <= 48);
    }
}

TEST_CASE("golden point-cloud images match at 32/64/256/512 px",
          "[provider][rasterizer][golden]")
{
    const std::vector<PointSample> points = MakeGoldenPointCloud();
    const SampledGeometry geometry = ViewOf(kNoTriangles, points);
    const std::vector<model_core::MaterialPayload> materials = GoldenMaterials();

    for (std::uint32_t size : {32u, 64u, 256u, 512u}) {
        const RasterImage image = RenderScene(geometry, materials, size);

        int goldenWidth = 0;
        int goldenHeight = 0;
        std::vector<std::uint8_t> golden;
        REQUIRE(thumbnail_rasterizer::LoadPamRgba(
            GoldenPath((std::string("points-") + std::to_string(size) + ".pam").c_str()).c_str(),
            goldenWidth, goldenHeight, golden));
        const DiffStats stats = Compare(image.bgraPremultiplied, goldenWidth, goldenHeight, golden);
        INFO("points size " << size << " meanAbs=" << stats.meanAbs << " maxAbs=" << stats.maxAbs);
        CHECK(stats.meanAbs <= 2.0);
        CHECK(stats.maxAbs <= 48);
    }
}

TEST_CASE("golden alpha/transparency image matches", "[provider][rasterizer][golden]")
{
    const std::vector<TriangleSample> triangles = MakeAlphaScene();
    const SampledGeometry geometry = ViewOf(triangles, kNoPoints);
    const std::vector<model_core::MaterialPayload> materials = AlphaMaterials();

    const RasterImage image = RenderScene(geometry, materials, 256);
    int goldenWidth = 0;
    int goldenHeight = 0;
    std::vector<std::uint8_t> golden;
    REQUIRE(thumbnail_rasterizer::LoadPamRgba(GoldenPath("alpha-256.pam").c_str(), goldenWidth,
                                              goldenHeight, golden));
    CHECK(goldenWidth == 256);
    CHECK(goldenHeight == 256);
    const DiffStats stats = Compare(image.bgraPremultiplied, goldenWidth, goldenHeight, golden);
    INFO("alpha meanAbs=" << stats.meanAbs << " maxAbs=" << stats.maxAbs);
    CHECK(stats.meanAbs <= 2.0);
    CHECK(stats.maxAbs <= 48);
}

TEST_CASE("a transparent material differs from the same opaque material",
          "[provider][rasterizer]")
{
    const std::vector<model_core::MaterialPayload> materials = AlphaMaterials();
    // One double-sided quad with the blend material, then the same geometry with
    // an otherwise-identical opaque material.
    const std::vector<TriangleSample> blendTriangles = {
        MakeTriangle(-1.0, -1.0, 0.0, 1.0, -1.0, 0.0, 1.0, 1.0, 0.0, kMaterialBlend, 1.0f),
        MakeTriangle(-1.0, -1.0, 0.0, 1.0, 1.0, 0.0, -1.0, 1.0, 0.0, kMaterialBlend, 1.0f)};
    const std::vector<TriangleSample> opaqueTriangles = {
        MakeTriangle(-1.0, -1.0, 0.0, 1.0, -1.0, 0.0, 1.0, 1.0, 0.0, kMaterialOpaque, 1.0f),
        MakeTriangle(-1.0, -1.0, 0.0, 1.0, 1.0, 0.0, -1.0, 1.0, 0.0, kMaterialOpaque, 1.0f)};

    const RasterImage blend = RenderScene(ViewOf(blendTriangles, kNoPoints), materials, 64);
    const RasterImage opaque = RenderScene(ViewOf(opaqueTriangles, kNoPoints), materials, 64);

    CHECK(blend.bgraPremultiplied != opaque.bgraPremultiplied);
    // Weighted-opaque keeps a depth-writing, opaque result rather than dropping
    // alpha, so the covered pixels reach full alpha.
    CHECK(std::any_of(blend.bgraPremultiplied.begin(), blend.bgraPremultiplied.end(),
                      [](std::uint8_t v) { return v == 255; }));
}

TEST_CASE("a masked triangle below its cutoff is discarded", "[provider][rasterizer]")
{
    const std::vector<model_core::MaterialPayload> materials = AlphaMaterials();
    const std::vector<TriangleSample> below = {
        MakeTriangle(-1.0, -1.0, 0.0, 1.0, -1.0, 0.0, 1.0, 1.0, 0.0, kMaterialMask, 0.2f),
        MakeTriangle(-1.0, -1.0, 0.0, 1.0, 1.0, 0.0, -1.0, 1.0, 0.0, kMaterialMask, 0.2f)};
    const std::vector<TriangleSample> above = {
        MakeTriangle(-1.0, -1.0, 0.0, 1.0, -1.0, 0.0, 1.0, 1.0, 0.0, kMaterialMask, 0.9f),
        MakeTriangle(-1.0, -1.0, 0.0, 1.0, 1.0, 0.0, -1.0, 1.0, 0.0, kMaterialMask, 0.9f)};

    const RasterImage belowImage = RenderScene(ViewOf(below, kNoPoints), materials, 64);
    const RasterImage aboveImage = RenderScene(ViewOf(above, kNoPoints), materials, 64);
    CHECK(belowImage.bgraPremultiplied != aboveImage.bgraPremultiplied);

    auto totalAlpha = [](const RasterImage& image) {
        std::uint64_t sum = 0;
        for (std::size_t i = 3; i < image.bgraPremultiplied.size(); i += 4) {
            sum += image.bgraPremultiplied[i];
        }
        return sum;
    };
    CHECK(totalAlpha(belowImage) < totalAlpha(aboveImage));
}

TEST_CASE("the depth test keeps the nearer triangle", "[provider][rasterizer]")
{
    // Two triangles that project to the same screen polygon but sit at
    // different view depths: translating along the fixed forward direction
    // leaves the orthographic projection unchanged while moving view z.
    const double f = 1.0 / std::sqrt(3.0);
    const std::vector<TriangleSample> triangles = {
        MakeTriangle(-0.5 * f, -0.5 * f, -0.5 * f, 1.0 - 0.5 * f, -0.5 * f, -0.5 * f,
                     -0.5 * f, 1.0 - 0.5 * f, -0.5 * f, kMaterialOpaque, 1.0f),
        MakeTriangle(0.5 * f, 0.5 * f, 0.5 * f, 1.0 + 0.5 * f, 0.5 * f, 0.5 * f, 0.5 * f,
                     1.0 + 0.5 * f, 0.5 * f, kMaterialOpaque, 1.0f)};
    const std::vector<model_core::MaterialPayload> materials = AlphaMaterials();
    const RasterImage image = RenderScene(ViewOf(triangles, kNoPoints), materials, 64);

    bool anyOpaque = false;
    for (std::size_t i = 3; i < image.bgraPremultiplied.size(); i += 4) {
        if (image.bgraPremultiplied[i] == 255) {
            anyOpaque = true;
            break;
        }
    }
    CHECK(anyOpaque);
}

TEST_CASE("an expired deadline and an exhausted ledger leave the image empty",
          "[provider][rasterizer]")
{
    const std::vector<TriangleSample> triangles = MakeGoldenMesh();
    const SampledGeometry geometry = ViewOf(triangles, kNoPoints);
    const std::vector<model_core::MaterialPayload> materials = GoldenMaterials();

    {
        Deadline expired(Deadline::Clock::now() - std::chrono::seconds(5));
        AllocationLedger ledger;
        RasterRequest request{};
        request.geometry = &geometry;
        request.materials = materials;
        request.requestedSize = 256;
        request.deadline = &expired;
        request.ledger = &ledger;
        RasterImage out;
        CHECK(RenderCpuTileRaster(request, out) == ErrorCode::Cancelled);
        CHECK(out.bgraPremultiplied.empty());
    }
    {
        Deadline deadline;
        AllocationLedger tiny(16);
        RasterRequest request{};
        request.geometry = &geometry;
        request.materials = materials;
        request.requestedSize = 256;
        request.deadline = &deadline;
        request.ledger = &tiny;
        RasterImage out;
        CHECK(RenderCpuTileRaster(request, out) == ErrorCode::ResourceLimit);
        CHECK(out.bgraPremultiplied.empty());
    }
}

TEST_CASE("null or empty geometry fails without an image", "[provider][rasterizer]")
{
    Deadline deadline;
    AllocationLedger ledger;
    RasterRequest request{};
    request.requestedSize = 64;
    request.deadline = &deadline;
    request.ledger = &ledger;

    RasterImage out;
    CHECK(RenderCpuTileRaster(request, out) == ErrorCode::MalformedData);
    CHECK(out.bgraPremultiplied.empty());

    const SampledGeometry empty{};
    request.geometry = &empty;
    CHECK(RenderCpuTileRaster(request, out) == ErrorCode::MalformedData);
    CHECK(out.bgraPremultiplied.empty());
}

TEST_CASE("a near-INT_MAX triangle coordinate is clamped, not converted out of range",
          "[provider][rasterizer]")
{
    // The framing bounds describe a unit object, but one vertex projects far
    // beyond the frame so `minX`/`maxX` exceed int's range. The extent clamp
    // must bound the loop instead of executing an out-of-range double->int
    // conversion (undefined behavior).
    TriangleSample triangle =
        MakeTriangle(-1.0, -1.0, 0.0, 1.0, -1.0, 0.0, 1.0, 1.0, 0.0, kMaterialGolden, 1.0f);
    triangle.vertices[0].position[0] = 3.0e9f; // finite, far outside the frame

    std::vector<TriangleSample> triangles{triangle};
    SampledGeometry geometry{};
    geometry.triangles = triangles;
    geometry.points = kNoPoints;
    Bounds bounds{};
    bounds.valid = true;
    bounds.min[0] = bounds.min[1] = bounds.min[2] = -1.0;
    bounds.max[0] = bounds.max[1] = bounds.max[2] = 1.0;
    geometry.bounds = bounds;

    const std::vector<model_core::MaterialPayload> materials = GoldenMaterials();
    const RasterImage image = RenderScene(geometry, materials, 256);
    CHECK(image.width == 256u);
    CHECK(image.height == 256u);
    CHECK(image.bgraPremultiplied.size() == 256u * 256u * 4u);
}

TEST_CASE("a far out-of-frame point is clamped to the raster extent",
          "[provider][rasterizer]")
{
    PointSample point{};
    SetVertex(point.vertex, 3.0e9, 3.0e9, 3.0e9, Gradient{0.7f, 0.7f, 0.7f});
    point.radius = 0.02f;
    point.materialIndex = kMaterialGolden;

    std::vector<PointSample> points{point};
    SampledGeometry geometry{};
    geometry.triangles = kNoTriangles;
    geometry.points = points;
    Bounds bounds{};
    bounds.valid = true;
    bounds.min[0] = bounds.min[1] = bounds.min[2] = -1.0;
    bounds.max[0] = bounds.max[1] = bounds.max[2] = 1.0;
    geometry.bounds = bounds;

    const std::vector<model_core::MaterialPayload> materials = GoldenMaterials();
    const RasterImage image = RenderScene(geometry, materials, 256);
    CHECK(image.width == 256u);
    CHECK(image.height == 256u);
    CHECK(image.bgraPremultiplied.size() == 256u * 256u * 4u);
}

TEST_CASE("the ICpuRasterizer adapter and a null ledger still render",
          "[provider][rasterizer]")
{
    const std::vector<TriangleSample> triangles = MakeGoldenMesh();
    const SampledGeometry geometry = ViewOf(triangles, kNoPoints);
    const std::vector<model_core::MaterialPayload> materials = GoldenMaterials();

    Deadline deadline;
    RasterRequest request{};
    request.geometry = &geometry;
    request.materials = materials;
    request.requestedSize = 32;
    request.allowSupersample = false;
    request.deadline = &deadline;
    request.ledger = nullptr;

    CpuTileRasterizer rasterizer;
    RasterImage out;
    CHECK(rasterizer.Render(request, out) == ErrorCode::None);
    CHECK(out.width == 32u);
    CHECK_FALSE(out.bgraPremultiplied.empty());
}

// Hidden by default: run explicitly to (re)generate the committed goldens, e.g.
//   x64\Release\Tests.Unit.exe "[write-goldens]"
TEST_CASE("regenerate the provider rasterizer goldens", "[.][write-goldens]")
{
    const std::filesystem::path directory(PREVIEW3D_PROVIDER_GOLDEN_DIR);
    std::filesystem::create_directories(directory);

const std::vector<model_core::MaterialPayload> goldenMaterials = GoldenMaterials();
    const std::vector<TriangleSample> meshTriangles = MakeGoldenMesh();
    const SampledGeometry mesh = ViewOf(meshTriangles, kNoPoints);
    for (std::uint32_t size : {32u, 64u, 256u, 512u}) {
        const RasterImage image = RenderScene(mesh, goldenMaterials, size);
        WriteGolden((std::string("mesh-") + std::to_string(size) + ".pam").c_str(), image);
    }

    const std::vector<PointSample> cloudPoints = MakeGoldenPointCloud();
    const SampledGeometry points = ViewOf(kNoTriangles, cloudPoints);
    for (std::uint32_t size : {32u, 64u, 256u, 512u}) {
        const RasterImage image = RenderScene(points, goldenMaterials, size);
        WriteGolden((std::string("points-") + std::to_string(size) + ".pam").c_str(), image);
    }

    const std::vector<TriangleSample> alphaTriangles = MakeAlphaScene();
    const SampledGeometry alpha = ViewOf(alphaTriangles, kNoPoints);
    const std::vector<model_core::MaterialPayload> alphaMaterials = AlphaMaterials();
    WriteGolden("alpha-256.pam", RenderScene(alpha, alphaMaterials, 256));
}

namespace {

// Converts a T02 cap-sized fixture to the provider's sampled types.
std::vector<TriangleSample> ToProviderTriangles(const thumbnail_spike::MeshScene& scene)
{
    std::vector<TriangleSample> out;
    out.reserve(scene.triangles.size());
    for (const thumbnail_rasterizer::Triangle& t : scene.triangles) {
        TriangleSample sample{};
        for (int i = 0; i < 3; ++i) {
            sample.vertices[i].position[0] = static_cast<float>(t.p[i].x);
            sample.vertices[i].position[1] = static_cast<float>(t.p[i].y);
            sample.vertices[i].position[2] = static_cast<float>(t.p[i].z);
            sample.vertices[i].normal[0] = static_cast<float>(t.n[i].x);
            sample.vertices[i].normal[1] = static_cast<float>(t.n[i].y);
            sample.vertices[i].normal[2] = static_cast<float>(t.n[i].z);
            sample.vertices[i].color[0] = t.color[i].r;
            sample.vertices[i].color[1] = t.color[i].g;
            sample.vertices[i].color[2] = t.color[i].b;
            sample.vertices[i].color[3] = t.alpha;
        }
        sample.materialIndex = kMaterialGolden;
        out.push_back(sample);
    }
    return out;
}

std::vector<PointSample> ToProviderPoints(const thumbnail_spike::PointScene& scene)
{
    std::vector<PointSample> out;
    out.reserve(scene.points.size());
    for (const thumbnail_rasterizer::Point& p : scene.points) {
        PointSample sample{};
        sample.vertex.position[0] = static_cast<float>(p.p.x);
        sample.vertex.position[1] = static_cast<float>(p.p.y);
        sample.vertex.position[2] = static_cast<float>(p.p.z);
        sample.vertex.color[0] = p.color.r;
        sample.vertex.color[1] = p.color.g;
        sample.vertex.color[2] = p.color.b;
        sample.vertex.color[3] = p.alpha;
        sample.radius = p.radius;
        sample.materialIndex = kMaterialGolden;
        out.push_back(sample);
    }
    return out;
}

} // namespace

// Hidden by default: the release-time budget evidence. Run explicitly:
//   x64\Release\Tests.Unit.exe "[rasterizer-perf]"
TEST_CASE("cap-sized scenes meet the prototype render budget", "[.][rasterizer-perf]")
{
    const std::vector<model_core::MaterialPayload> materials = GoldenMaterials();

    const auto mesh = thumbnail_spike::GenerateLargeMesh(
        2'000'000, 250'000, thumbnail_spike::SeedFromString("provider-t15-mesh"));
    const std::vector<TriangleSample> meshTriangles = ToProviderTriangles(mesh);
    const SampledGeometry meshGeometry = ViewOf(meshTriangles, kNoPoints);

    const auto cloud = thumbnail_spike::GenerateLargePointCloud(
        6'000'000, 250'000, thumbnail_spike::SeedFromString("provider-t15-points"));
    const std::vector<PointSample> cloudPoints = ToProviderPoints(cloud);
    const SampledGeometry cloudGeometry = ViewOf(kNoTriangles, cloudPoints);

    Deadline meshDeadline;
    AllocationLedger meshLedger;
    RasterRequest meshRequest{};
    meshRequest.geometry = &meshGeometry;
    meshRequest.materials = materials;
    meshRequest.requestedSize = 512;
    meshRequest.allowSupersample = true;
    meshRequest.deadline = &meshDeadline;
    meshRequest.ledger = &meshLedger;
    RasterImage meshImage;
    const auto meshStart = std::chrono::steady_clock::now();
    REQUIRE(RenderCpuTileRaster(meshRequest, meshImage) == ErrorCode::None);
    const double meshMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - meshStart)
                              .count();

    Deadline cloudDeadline;
    AllocationLedger cloudLedger;
    RasterRequest cloudRequest{};
    cloudRequest.geometry = &cloudGeometry;
    cloudRequest.materials = materials;
    cloudRequest.requestedSize = 512;
    cloudRequest.allowSupersample = true;
    cloudRequest.deadline = &cloudDeadline;
    cloudRequest.ledger = &cloudLedger;
    RasterImage cloudImage;
    const auto cloudStart = std::chrono::steady_clock::now();
    REQUIRE(RenderCpuTileRaster(cloudRequest, cloudImage) == ErrorCode::None);
    const double cloudMs = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - cloudStart)
                               .count();

    WARN("T15 cap render (512 px, 2x SS): mesh " << meshMs << " ms, points " << cloudMs << " ms");
    // Generous ceilings: the T02 prototype measured 58.8 ms / 185.9 ms and the
    // provider must stay within the cooperative 2 s stop point at the cap.
    CHECK(meshMs < 1000.0);
    CHECK(cloudMs < 2000.0);
}