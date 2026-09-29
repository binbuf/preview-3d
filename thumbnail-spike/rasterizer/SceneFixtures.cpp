#include "SceneFixtures.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace thumbnail_spike
{
namespace
{

using thumbnail_rasterizer::LinearRgb;
using thumbnail_rasterizer::Point;
using thumbnail_rasterizer::Triangle;
using thumbnail_rasterizer::Vec3;

std::uint64_t SplitMix64(std::uint64_t& state)
{
    state += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

struct Vec
{
    double x;
    double y;
    double z;
};

Vec Normalize(const Vec& v)
{
    const double len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len < 1e-15) return { 0.0, 1.0, 0.0 };
    return { v.x / len, v.y / len, v.z / len };
}

Vec Mid(const Vec& a, const Vec& b) { return Normalize({ (a.x + b.x) * 0.5, (a.y + b.y) * 0.5, (a.z + b.z) * 0.5 }); }

LinearRgb Gradient(double t)
{
    // linear-space warm/cool palette shared by the fixtures
    const LinearRgb cool{ 0.06f, 0.16f, 0.52f };
    const LinearRgb warm{ 0.86f, 0.42f, 0.05f };
    const float f = static_cast<float>(std::clamp(t, 0.0, 1.0));
    return { cool.r + (warm.r - cool.r) * f, cool.g + (warm.g - cool.g) * f,
             cool.b + (warm.b - cool.b) * f };
}

void Subdivide(std::vector<Vec>& verts, const std::array<std::array<int, 3>, 20>& facesIn,
               int levels, std::vector<std::array<int, 3>>& facesOut)
{
    std::vector<std::array<int, 3>> faces(facesIn.begin(), facesIn.end());
    for (int level = 0; level < levels; ++level) {
        std::vector<std::array<int, 3>> next;
        next.reserve(faces.size() * 4);
        for (const auto& face : faces) {
            const int a = face[0];
            const int b = face[1];
            const int c = face[2];
            const int ab = static_cast<int>(verts.size());
            verts.push_back(Mid(verts[a], verts[b]));
            const int bc = static_cast<int>(verts.size());
            verts.push_back(Mid(verts[b], verts[c]));
            const int ca = static_cast<int>(verts.size());
            verts.push_back(Mid(verts[c], verts[a]));
            next.push_back({ a, ab, ca });
            next.push_back({ b, bc, ab });
            next.push_back({ c, ca, bc });
            next.push_back({ ab, bc, ca });
        }
        faces.swap(next);
    }
    facesOut = std::move(faces);
}

Triangle MakeSphereTriangle(const Vec& a, const Vec& b, const Vec& c)
{
    Triangle tri{};
    const Vec na = Normalize(a);
    const Vec nb = Normalize(b);
    const Vec nc = Normalize(c);
    tri.p[0] = { a.x, a.y, a.z };
    tri.p[1] = { b.x, b.y, b.z };
    tri.p[2] = { c.x, c.y, c.z };
    tri.n[0] = { na.x, na.y, na.z };
    tri.n[1] = { nb.x, nb.y, nb.z };
    tri.n[2] = { nc.x, nc.y, nc.z };
    tri.color[0] = Gradient((na.y + 1.0) * 0.5);
    tri.color[1] = Gradient((nb.y + 1.0) * 0.5);
    tri.color[2] = Gradient((nc.y + 1.0) * 0.5);
    tri.alpha = 1.0f;
    tri.flags = thumbnail_rasterizer::kTriangleHasNormals | thumbnail_rasterizer::kTriangleDoubleSided;
    return tri;
}

} // namespace

std::uint64_t SeedFromString(const char* text)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (const char* p = text; p && *p; ++p) {
        hash ^= static_cast<std::uint8_t>(*p);
        hash *= 1099511628211ull;
    }
    return hash ? hash : 0x123456789abcdefull;
}

MeshScene MakeGoldenMesh()
{
    std::vector<Vec> verts = {
        { -1, 1.618033988749895, 0 }, { 1, 1.618033988749895, 0 }, { -1, -1.618033988749895, 0 },
        { 1, -1.618033988749895, 0 }, { 0, -1, 1.618033988749895 }, { 0, 1, 1.618033988749895 },
        { 0, -1, -1.618033988749895 }, { 0, 1, -1.618033988749895 }, { 1.618033988749895, 0, -1 },
        { 1.618033988749895, 0, 1 }, { -1.618033988749895, 0, -1 }, { -1.618033988749895, 0, 1 }
    };
    const std::array<std::array<int, 3>, 20> base = { { { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 },
                                                        { 0, 10, 11 }, { 1, 5, 9 }, { 5, 11, 4 }, { 11, 10, 2 },
                                                        { 10, 7, 6 }, { 7, 1, 8 }, { 3, 9, 4 }, { 3, 4, 2 },
                                                        { 3, 2, 6 }, { 3, 6, 8 }, { 3, 8, 9 }, { 4, 9, 5 },
                                                        { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 } } };

    std::vector<std::array<int, 3>> faces;
    Subdivide(verts, base, 1, faces);

    MeshScene scene;
    scene.triangles.reserve(faces.size());
    for (const auto& face : faces) {
        scene.triangles.push_back(
            MakeSphereTriangle(verts[face[0]], verts[face[1]], verts[face[2]]));
    }
    scene.inspected = static_cast<std::uint64_t>(scene.triangles.size());
    return scene;
}

PointScene MakeGoldenPointCloud()
{
    constexpr int kCount = 4096;
    const double golden = 3.14159265358979323846 * (3.0 - std::sqrt(5.0));
    PointScene scene;
    scene.points.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        const double y = 1.0 - (static_cast<double>(i) / (kCount - 1)) * 2.0;
        const double r = std::sqrt((std::max)(0.0, 1.0 - y * y));
        const double theta = golden * i;
        Point p;
        p.p = { std::cos(theta) * r, y, std::sin(theta) * r };
        p.color = Gradient((y + 1.0) * 0.5);
        p.radius = 0.022f;
        p.alpha = 1.0f;
        scene.points.push_back(p);
    }
    scene.inspected = static_cast<std::uint64_t>(scene.points.size());
    return scene;
}

MeshScene GenerateLargeMesh(std::uint64_t totalTriangles, std::size_t maxSamples, std::uint64_t seed)
{
    if (maxSamples == 0) maxSamples = 1;
    const std::uint64_t grid = static_cast<std::uint64_t>(std::ceil(std::sqrt(totalTriangles / 2.0)));
    MeshScene scene;
    scene.triangles.reserve((std::min)(maxSamples, static_cast<std::size_t>(totalTriangles)));
    std::uint64_t state = seed ? seed : 0xdeadbeefull;
    std::uint64_t emitted = 0;

    auto emit = [&](const Triangle& tri) {
        if (emitted < maxSamples) {
            scene.triangles.push_back(tri);
        } else {
            const std::uint64_t j = SplitMix64(state) % (emitted + 1);
            if (j < maxSamples) scene.triangles[static_cast<std::size_t>(j)] = tri;
        }
        ++emitted;
    };

    for (std::uint64_t gz = 0; gz < grid && emitted < totalTriangles; ++gz) {
        for (std::uint64_t gx = 0; gx < grid && emitted < totalTriangles; ++gx) {
            const double x0 = (static_cast<double>(gx) / grid) * 2.0 - 1.0;
            const double x1 = (static_cast<double>(gx + 1) / grid) * 2.0 - 1.0;
            const double z0 = (static_cast<double>(gz) / grid) * 2.0 - 1.0;
            const double z1 = (static_cast<double>(gz + 1) / grid) * 2.0 - 1.0;
            const double zc = (z0 + z1) * 0.5;
            const double xc = (x0 + x1) * 0.5;
            auto height = [](double x, double z) {
                return 0.28 * std::sin(3.0 * x) * std::cos(3.0 * z) + 0.14 * std::sin(5.0 * x + 2.0 * z);
            };
            const Vec3 p00{ x0, height(x0, z0), z0 };
            const Vec3 p10{ x1, height(x1, z0), z0 };
            const Vec3 p01{ x0, height(x0, z1), z1 };
            const Vec3 p11{ x1, height(x1, z1), z1 };
            const LinearRgb c00 = Gradient((p00.y + 0.45) / 0.9);
            const LinearRgb c10 = Gradient((p10.y + 0.45) / 0.9);
            const LinearRgb c01 = Gradient((p01.y + 0.45) / 0.9);
            const LinearRgb c11 = Gradient((p11.y + 0.45) / 0.9);
            (void)xc;
            (void)zc;

            Triangle t0{};
            t0.p[0] = p00;
            t0.p[1] = p10;
            t0.p[2] = p01;
            t0.color[0] = c00;
            t0.color[1] = c10;
            t0.color[2] = c01;
            t0.alpha = 1.0f;
            t0.flags = thumbnail_rasterizer::kTriangleDoubleSided;
            emit(t0);
            if (emitted >= totalTriangles) break;

            Triangle t1{};
            t1.p[0] = p10;
            t1.p[1] = p11;
            t1.p[2] = p01;
            t1.color[0] = c10;
            t1.color[1] = c11;
            t1.color[2] = c01;
            t1.alpha = 1.0f;
            t1.flags = thumbnail_rasterizer::kTriangleDoubleSided;
            emit(t1);
        }
    }
    scene.inspected = emitted;
    return scene;
}

PointScene GenerateLargePointCloud(std::uint64_t totalPoints, std::size_t maxSamples, std::uint64_t seed)
{
    if (maxSamples == 0) maxSamples = 1;
    PointScene scene;
    scene.points.reserve((std::min)(maxSamples, static_cast<std::size_t>(totalPoints)));
    std::uint64_t state = seed ? seed : 0xc0ffeeull;
    std::uint64_t emitted = 0;
    const double golden = 3.14159265358979323846 * (3.0 - std::sqrt(5.0));

    auto emit = [&](const Point& p) {
        if (emitted < maxSamples) {
            scene.points.push_back(p);
        } else {
            const std::uint64_t j = SplitMix64(state) % (emitted + 1);
            if (j < maxSamples) scene.points[static_cast<std::size_t>(j)] = p;
        }
        ++emitted;
    };

    for (std::uint64_t i = 0; i < totalPoints; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(totalPoints);
        const double y = 1.0 - t * 2.0;
        const double r = std::sqrt((std::max)(0.0, 1.0 - y * y));
        const double theta = golden * static_cast<double>(i);
        const double radius = 1.0 + 0.12 * std::sin(static_cast<double>(i) * 0.5);
        Point p;
        p.p = { std::cos(theta) * r * radius, y * radius, std::sin(theta) * r * radius };
        p.color = Gradient((y + 1.0) * 0.5);
        p.radius = 0.0f;
        p.alpha = 1.0f;
        emit(p);
    }
    scene.inspected = emitted;
    return scene;
}

} // namespace thumbnail_spike