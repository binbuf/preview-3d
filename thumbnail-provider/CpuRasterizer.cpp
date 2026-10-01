// T15 product-owned CPU tile rasterizer (see CpuRasterizerImpl.h and
// CpuRasterizer.h). Free of the provider precompiled header, COM and GDI so the
// same source compiles into Preview3DThumbnailProvider.dll and Tests.Unit.exe,
// like StreamSource.cpp / GeometrySampler.cpp / ThumbnailPipeline.cpp.
//
// It replaces the T02 prototype (thumbnail-spike/rasterizer) behind the same
// rendering contract but consumes the frozen provider types: the T14
// `SampledGeometry`, the normalized `model_core::MaterialPayload` table and the
// T06 `Deadline`/`AllocationLedger` services. The isometric framing, lighting
// model, weighted-opaque transparency approximation, point splats, linear-space
// supersample resolve and premultiplied BGRA output mirror the measured
// prototype (docs/tasks/02-spike-rasterizer-prototype.md).

#include "CpuRasterizerImpl.h"

#include "AllocationLedger.h"
#include "Deadline.h"
#include "ProviderLimits.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace preview3d::provider {
namespace {

constexpr double kEps = 1e-12;
constexpr int kMaxRasterSize = 512;
constexpr int kMaxSupersample = 2;
constexpr std::uint64_t kCheckpointInterval = 2048;

// 2x internal supersampling is used only when the cooperative deadline budget
// permits (design/05, "CPU renderer"). The T02 prototype rendered the 512 px
// point cap in ~186 ms at 2x, so 250 ms of remaining stop-point headroom is the
// floor before the 4x-work path is taken; below it the raster falls back to 1x,
// which still meets the bitmap/quality contract.
constexpr std::chrono::milliseconds kSupersampleMinRemaining{250};

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct LinearRgb {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
};

Vec3 Add(const Vec3& a, const Vec3& b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Sub(const Vec3& a, const Vec3& b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Neg(const Vec3& a) noexcept { return {-a.x, -a.y, -a.z}; }
Vec3 Scale(const Vec3& a, double s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
double Dot(const Vec3& a, const Vec3& b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(const Vec3& a, const Vec3& b) noexcept
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double Length(const Vec3& a) noexcept { return std::sqrt(Dot(a, a)); }
Vec3 Normalize(const Vec3& a) noexcept
{
    const double len = Length(a);
    return len > kEps ? Scale(a, 1.0 / len) : Vec3{0.0, 1.0, 0.0};
}

bool IsFinite(const Vec3& a) noexcept
{
    return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}

double Clamp01(double v) noexcept { return std::clamp(v, 0.0, 1.0); }

double ToSrgb(double linear) noexcept
{
    const double c = std::clamp(linear, 0.0, 1.0);
    return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

std::uint8_t ToByte(double value) noexcept
{
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, 255.0)));
}

// Linear-space neutral constants. The viewer palette and material factors are
// already linear (ProviderTypes.h / MaterialPayload.h), so no sRGB decode is
// applied to them; only the fixed frost/shadow tints are authored here directly
// in linear space to match the prototype's appearance.
constexpr LinearRgb kFrost{0.638, 0.672, 0.708};
constexpr LinearRgb kShadow{0.0048, 0.0048, 0.0056};

struct Camera {
    Vec3 right;
    Vec3 up;
    Vec3 forward;
    Vec3 center;
    double pxPerUnit = 1.0;
    double cx = 0.0;
    double cy = 0.0;
    double nearZ = 0.0;
    double farZ = 1.0;
    int ssize = 0;
};

struct ClipVertex {
    Vec3 view;
    Vec3 normal;
    LinearRgb color;
};

ClipVertex LerpVertex(const ClipVertex& a, const ClipVertex& b, double t) noexcept
{
    ClipVertex out;
    out.view = Add(a.view, Scale(Sub(b.view, a.view), t));
    out.normal = Add(a.normal, Scale(Sub(b.normal, a.normal), t));
    out.color = {a.color.r + (b.color.r - a.color.r) * t,
                 a.color.g + (b.color.g - a.color.g) * t,
                 a.color.b + (b.color.b - a.color.b) * t};
    return out;
}

// Sutherland-Hodgman clip against the view-space near plane (z >= nearZ).
void ClipNearPlane(const ClipVertex in[3], double nearZ, std::vector<ClipVertex>& out)
{
    out.clear();
    for (int i = 0; i < 3; ++i) {
        const ClipVertex& cur = in[i];
        const ClipVertex& nxt = in[(i + 1) % 3];
        const double dc = cur.view.z - nearZ;
        const double dn = nxt.view.z - nearZ;
        const bool curIn = dc >= 0.0;
        const bool nxtIn = dn >= 0.0;
        if (curIn) {
            out.push_back(cur);
        }
        if (curIn != nxtIn) {
            const double t = dc / (dc - dn);
            out.push_back(LerpVertex(cur, nxt, t));
        }
    }
}

// Scene-space ambient plus two fixed directional lights; returns straight linear
// RGB. The same neutral rig as the viewer (design/05, "CPU renderer").
LinearRgb Shade(const Vec3& normal, const LinearRgb& albedo) noexcept
{
    static const Vec3 light1 = Normalize(Vec3{0.45, 0.80, 0.62});
    static const Vec3 light2 = Normalize(Vec3{-0.62, 0.30, -0.55});
    static const LinearRgb ambientColor{0.92, 0.94, 1.0};
    static const LinearRgb light1Color{1.0, 0.96, 0.90};
    static const LinearRgb light2Color{0.72, 0.80, 1.0};
    const Vec3 n = Normalize(normal);
    const double d1 = (std::max)(0.0, Dot(n, light1));
    const double d2 = (std::max)(0.0, Dot(n, light2));
    constexpr double ambient = 0.30;
    constexpr double fill = 0.10;

    return {albedo.r * (ambient * ambientColor.r + 0.62 * d1 * light1Color.r +
                        0.34 * d2 * light2Color.r + fill),
            albedo.g * (ambient * ambientColor.g + 0.62 * d1 * light1Color.g +
                        0.34 * d2 * light2Color.g + fill),
            albedo.b * (ambient * ambientColor.b + 0.62 * d1 * light1Color.b +
                        0.34 * d2 * light2Color.b + fill)};
}

// Per-vertex linear albedo: the sampled vertex color modulates the material base
// color (glTF/FBX convention: baseColor = factor * vertexColor).
LinearRgb Modulate(const float vertexRgba[4], const float baseColor[4]) noexcept
{
    return {static_cast<double>(vertexRgba[0]) * baseColor[0],
            static_cast<double>(vertexRgba[1]) * baseColor[1],
            static_cast<double>(vertexRgba[2]) * baseColor[2]};
}

const model_core::MaterialPayload& MaterialAt(
    std::span<const model_core::MaterialPayload> materials,
    std::uint32_t materialIndex) noexcept
{
    if (materialIndex == 0 || materialIndex > materials.size()) {
        // A zero or out-of-range index selects the neutral fallback, never a
        // failure (CpuRasterizer.h).
        static const model_core::MaterialPayload neutral = NeutralMaterial();
        return neutral;
    }
    return materials[materialIndex - 1];
}

double MaterialAlpha(const model_core::MaterialPayload& material, double vertexAlpha) noexcept
{
    const double alpha =
        vertexAlpha * static_cast<double>(material.baseColorFactor[3]);
    switch (static_cast<model_core::AlphaModeId>(material.alphaMode)) {
        case model_core::AlphaModeId::Mask:
            return alpha >= static_cast<double>(material.alphaCutoff) ? 1.0 : 0.0;
        case model_core::AlphaModeId::Blend:
            return Clamp01(alpha);
        case model_core::AlphaModeId::Opaque:
        default:
            return 1.0;
    }
}

bool IsBlend(const model_core::MaterialPayload& material) noexcept
{
    return static_cast<model_core::AlphaModeId>(material.alphaMode) ==
           model_core::AlphaModeId::Blend;
}

// Weighted-opaque transparency approximation: blend toward a neutral frost by
// the material opacity and write an opaque, depth-testing result, so no sorting
// or order-independent transparency is required (design/05).
LinearRgb WeightedOpaque(const LinearRgb& shaded, double opacity) noexcept
{
    const double w = 1.0 - opacity;
    return {shaded.r * opacity + kFrost.r * w, shaded.g * opacity + kFrost.g * w,
            shaded.b * opacity + kFrost.b * w};
}

LinearRgb AddEmissive(const LinearRgb& color, const LinearRgb& emissive) noexcept
{
    return {color.r + emissive.r, color.g + emissive.g, color.b + emissive.b};
}

LinearRgb EmissiveOf(const model_core::MaterialPayload& material) noexcept
{
    return {material.emissiveFactor[0], material.emissiveFactor[1], material.emissiveFactor[2]};
}

void CompositePixel(std::vector<float>& color, std::vector<float>& depth, int ssize, int x, int y,
                    double z, const LinearRgb& shaded, double alpha) noexcept
{
    const int idx = y * ssize + x;
    const float a = static_cast<float>(Clamp01(alpha));
    if (a <= 0.0f) {
        return;
    }
    const float inv = 1.0f - a;
    color[static_cast<std::size_t>(idx) * 4 + 0] =
        static_cast<float>(shaded.r) * a + color[static_cast<std::size_t>(idx) * 4 + 0] * inv;
    color[static_cast<std::size_t>(idx) * 4 + 1] =
        static_cast<float>(shaded.g) * a + color[static_cast<std::size_t>(idx) * 4 + 1] * inv;
    color[static_cast<std::size_t>(idx) * 4 + 2] =
        static_cast<float>(shaded.b) * a + color[static_cast<std::size_t>(idx) * 4 + 2] * inv;
    color[static_cast<std::size_t>(idx) * 4 + 3] = a + color[static_cast<std::size_t>(idx) * 4 + 3] * inv;
    depth[static_cast<std::size_t>(idx)] = static_cast<float>(z);
}

double Edge(double ax, double ay, double bx, double by, double px, double py) noexcept
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

// Cooperative deadline polling at bounded raster intervals.
struct WorkGuard {
    const Deadline* deadline = nullptr;
    std::uint64_t work = 0;

    bool Expired() noexcept
    {
        if (deadline == nullptr) {
            return false;
        }
        if ((++work % kCheckpointInterval) != 0) {
            return false;
        }
        return deadline->expired();
    }
};

void RasterizeTriangle(const Camera& cam, std::vector<float>& color, std::vector<float>& depth,
                       const ClipVertex& v0, const ClipVertex& v1, const ClipVertex& v2,
                       double opacity, const LinearRgb& emissive, bool unlit) noexcept
{
    const double x0 = cam.cx + v0.view.x * cam.pxPerUnit;
    const double y0 = cam.cy - v0.view.y * cam.pxPerUnit;
    const double x1 = cam.cx + v1.view.x * cam.pxPerUnit;
    const double y1 = cam.cy - v1.view.y * cam.pxPerUnit;
    const double x2 = cam.cx + v2.view.x * cam.pxPerUnit;
    const double y2 = cam.cy - v2.view.y * cam.pxPerUnit;
    if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1) ||
        !std::isfinite(x2) || !std::isfinite(y2)) {
        return;
    }

    const double z0 = (v0.view.z - cam.nearZ) / (cam.farZ - cam.nearZ);
    const double z1 = (v1.view.z - cam.nearZ) / (cam.farZ - cam.nearZ);
    const double z2 = (v2.view.z - cam.nearZ) / (cam.farZ - cam.nearZ);

    const double area = Edge(x0, y0, x1, y1, x2, y2);
    if (std::fabs(area) < kEps) {
        return;
    }
    const double invArea = 1.0 / area;
    const bool positive = area > 0.0;

    const int ssize = cam.ssize;
    const double minX = (std::min)(x0, (std::min)(x1, x2));
    const double maxX = (std::max)(x0, (std::max)(x1, x2));
    const double minY = (std::min)(y0, (std::min)(y1, y2));
    const double maxY = (std::max)(y0, (std::max)(y1, y2));
    const int px0 = (std::max)(0, static_cast<int>(std::floor(minX - 0.5)));
    const int px1 = (std::min)(ssize - 1, static_cast<int>(std::ceil(maxX + 0.5)));
    const int py0 = (std::max)(0, static_cast<int>(std::floor(minY - 0.5)));
    const int py1 = (std::min)(ssize - 1, static_cast<int>(std::ceil(maxY + 0.5)));
    if (px1 < px0 || py1 < py0) {
        return;
    }

    for (int py = py0; py <= py1; ++py) {
        const double sy = py + 0.5;
        for (int px = px0; px <= px1; ++px) {
            const double sx = px + 0.5;
            const double w0 = Edge(x1, y1, x2, y2, sx, sy);
            const double w1 = Edge(x2, y2, x0, y0, sx, sy);
            const double w2 = Edge(x0, y0, x1, y1, sx, sy);
            if (positive) {
                if (w0 < -kEps || w1 < -kEps || w2 < -kEps) {
                    continue;
                }
            } else {
                if (w0 > kEps || w1 > kEps || w2 > kEps) {
                    continue;
                }
            }
            const double b0 = w0 * invArea;
            const double b1 = w1 * invArea;
            const double b2 = w2 * invArea;
            const double z = b0 * z0 + b1 * z1 + b2 * z2;
            const std::size_t idx = static_cast<std::size_t>(py) * ssize + px;
            if (z >= depth[idx]) {
                continue;
            }
            const Vec3 n = Add(Scale(v0.normal, b0), Add(Scale(v1.normal, b1), Scale(v2.normal, b2)));
            const LinearRgb c{b0 * v0.color.r + b1 * v1.color.r + b2 * v2.color.r,
                              b0 * v0.color.g + b1 * v1.color.g + b2 * v2.color.g,
                              b0 * v0.color.b + b1 * v1.color.b + b2 * v2.color.b};
            LinearRgb shaded = unlit ? c : Shade(n, c);
            shaded = AddEmissive(shaded, emissive);
            LinearRgb finalColor = shaded;
            if (opacity < 1.0) {
                finalColor = WeightedOpaque(shaded, opacity);
            }
            CompositePixel(color, depth, ssize, px, py, z, finalColor, 1.0);
        }
    }
}

bool RenderPoints(const Camera& cam, const SampledGeometry& geometry, double defaultRadius,
                  std::span<const model_core::MaterialPayload> materials,
                  std::vector<float>& color, std::vector<float>& depth, WorkGuard& guard)
{
    const Vec3 ambientNormal = Normalize(Neg(cam.forward));
    for (const PointSample& point : geometry.points) {
        const Vec3 p{point.origin[0] + point.vertex.position[0],
                     point.origin[1] + point.vertex.position[1],
                     point.origin[2] + point.vertex.position[2]};
        if (!IsFinite(p)) {
            continue;
        }
        if (guard.Expired()) {
            return false;
        }

        const model_core::MaterialPayload& material = MaterialAt(materials, point.materialIndex);
        const double alpha = MaterialAlpha(material, point.vertex.color[3]);
        if (alpha <= 0.0) {
            continue;
        }
        const LinearRgb albedo = Modulate(point.vertex.color, material.baseColorFactor);
        const LinearRgb emissive = EmissiveOf(material);
        const bool unlit = (material.flags & model_core::kMaterialFlagUnlit) != 0;
        LinearRgb shaded = unlit ? albedo : Shade(ambientNormal, albedo);
        shaded = AddEmissive(shaded, emissive);

        const Vec3 rel = Sub(p, cam.center);
        const double vx = Dot(rel, cam.right);
        const double vy = Dot(rel, cam.up);
        const double vz = Dot(rel, cam.forward);
        if (vz < cam.nearZ) {
            continue;
        }
        const double sx = cam.cx + vx * cam.pxPerUnit;
        const double sy = cam.cy - vy * cam.pxPerUnit;
        if (!std::isfinite(sx) || !std::isfinite(sy)) {
            continue;
        }
        const double z = (vz - cam.nearZ) / (cam.farZ - cam.nearZ);
        if (z < 0.0 || z > 1.0) {
            continue;
        }

        const double worldRadius =
            point.radius > 0.0f ? static_cast<double>(point.radius) : defaultRadius;
        double radiusPx = worldRadius * cam.pxPerUnit;
        radiusPx = std::clamp(radiusPx, 1.0, 128.0);

        const int px0 = (std::max)(0, static_cast<int>(std::floor(sx - radiusPx - 1.0)));
        const int px1 = (std::min)(cam.ssize - 1, static_cast<int>(std::ceil(sx + radiusPx + 1.0)));
        const int py0 = (std::max)(0, static_cast<int>(std::floor(sy - radiusPx - 1.0)));
        const int py1 = (std::min)(cam.ssize - 1, static_cast<int>(std::ceil(sy + radiusPx + 1.0)));
        if (px1 < px0 || py1 < py0) {
            continue;
        }

        for (int py = py0; py <= py1; ++py) {
            const double fy = py + 0.5 - sy;
            for (int px = px0; px <= px1; ++px) {
                const double fx = px + 0.5 - sx;
                const double dist = std::sqrt(fx * fx + fy * fy);
                const double coverage = std::clamp(radiusPx + 0.5 - dist, 0.0, 1.0);
                if (coverage <= 0.0) {
                    continue;
                }
                const std::size_t idx = static_cast<std::size_t>(py) * cam.ssize + px;
                if (z >= depth[idx]) {
                    continue;
                }
                CompositePixel(color, depth, cam.ssize, px, py, z, shaded, coverage * alpha);
            }
        }
    }
    return true;
}

void RenderFloorShadow(const Camera& cam, const Bounds& bounds, double usable,
                       std::vector<float>& color) noexcept
{
    const Vec3 base{(bounds.min[0] + bounds.max[0]) * 0.5, bounds.min[1],
                    (bounds.min[2] + bounds.max[2]) * 0.5};
    const Vec3 rel = Sub(base, cam.center);
    const double bx = cam.cx + Dot(rel, cam.right) * cam.pxPerUnit;
    const double by = cam.cy - Dot(rel, cam.up) * cam.pxPerUnit;
    const double sigma = (std::max)(1.0, usable * 0.5 * 0.55);
    constexpr double verticalSquash = 0.42;
    constexpr double strength = 0.34;

    const int radius = static_cast<int>(std::ceil(sigma * 3.0));
    const int px0 = (std::max)(0, static_cast<int>(std::floor(bx)) - radius);
    const int px1 = (std::min)(cam.ssize - 1, static_cast<int>(std::ceil(bx)) + radius);
    const int py0 = (std::max)(0, static_cast<int>(std::floor(by)) - radius);
    const int py1 = (std::min)(cam.ssize - 1, static_cast<int>(std::ceil(by)) + radius);

    const double denom = 2.0 * sigma * sigma;
    for (int py = py0; py <= py1; ++py) {
        const double dy = (py + 0.5 - by) / verticalSquash;
        for (int px = px0; px <= px1; ++px) {
            const double dx = px + 0.5 - bx;
            const double d2 = dx * dx + dy * dy;
            const double a = strength * std::exp(-d2 / denom);
            if (a <= 0.0) {
                continue;
            }
            const std::size_t idx = static_cast<std::size_t>(py) * cam.ssize + px;
            const float inv = static_cast<float>(1.0 - a);
            color[idx * 4 + 0] = static_cast<float>(kShadow.b * a) + color[idx * 4 + 0] * inv;
            color[idx * 4 + 1] = static_cast<float>(kShadow.g * a) + color[idx * 4 + 1] * inv;
            color[idx * 4 + 2] = static_cast<float>(kShadow.r * a) + color[idx * 4 + 2] * inv;
            color[idx * 4 + 3] = static_cast<float>(a) + color[idx * 4 + 3] * inv;
        }
    }
}

bool RenderTriangles(const Camera& cam, const SampledGeometry& geometry,
                     std::span<const model_core::MaterialPayload> materials,
                     std::vector<float>& color, std::vector<float>& depth,
                     std::vector<ClipVertex>& clipBuffer, WorkGuard& guard)
{
    for (const TriangleSample& triangle : geometry.triangles) {
        if (guard.Expired()) {
            return false;
        }

        const Vec3 p[3] = {
            {triangle.origin[0] + triangle.vertices[0].position[0],
             triangle.origin[1] + triangle.vertices[0].position[1],
             triangle.origin[2] + triangle.vertices[0].position[2]},
            {triangle.origin[0] + triangle.vertices[1].position[0],
             triangle.origin[1] + triangle.vertices[1].position[1],
             triangle.origin[2] + triangle.vertices[1].position[2]},
            {triangle.origin[0] + triangle.vertices[2].position[0],
             triangle.origin[1] + triangle.vertices[2].position[1],
             triangle.origin[2] + triangle.vertices[2].position[2]},
        };
        if (!IsFinite(p[0]) || !IsFinite(p[1]) || !IsFinite(p[2])) {
            continue;
        }
        const Vec3 edge0 = Sub(p[1], p[0]);
        const Vec3 edge1 = Sub(p[2], p[0]);
        const Vec3 areaVector = Cross(edge0, edge1);
        if (Length(areaVector) < kEps) {
            continue;
        }
        const Vec3 geoNormal = Normalize(areaVector);

        const model_core::MaterialPayload& material = MaterialAt(materials, triangle.materialIndex);
        const bool doubleSided = (material.flags & model_core::kMaterialFlagDoubleSided) != 0;
        const bool backface = Dot(geoNormal, cam.forward) > 0.0;
        if (backface && !doubleSided) {
            continue;
        }

        const double vertexAlpha =
            (static_cast<double>(triangle.vertices[0].color[3]) +
             static_cast<double>(triangle.vertices[1].color[3]) +
             static_cast<double>(triangle.vertices[2].color[3])) /
            3.0;
        const double alpha = MaterialAlpha(material, vertexAlpha);
        if (alpha <= 0.0) {
            // A masked triangle below its cutoff is discarded.
            continue;
        }
        const bool blend = IsBlend(material);
        const double opacity = blend ? alpha : 1.0;
        const LinearRgb emissive = EmissiveOf(material);
        const bool unlit = (material.flags & model_core::kMaterialFlagUnlit) != 0;

        const bool hasNormals = Length(Vec3{triangle.vertices[0].normal[0],
                                            triangle.vertices[0].normal[1],
                                            triangle.vertices[0].normal[2]}) > kEps &&
                                Length(Vec3{triangle.vertices[1].normal[0],
                                            triangle.vertices[1].normal[1],
                                            triangle.vertices[1].normal[2]}) > kEps &&
                                Length(Vec3{triangle.vertices[2].normal[0],
                                            triangle.vertices[2].normal[1],
                                            triangle.vertices[2].normal[2]}) > kEps;

        ClipVertex verts[3];
        for (int i = 0; i < 3; ++i) {
            const VertexSample& vertex = triangle.vertices[i];
            const Vec3 rel = Sub(p[i], cam.center);
            verts[i].view = {Dot(rel, cam.right), Dot(rel, cam.up), Dot(rel, cam.forward)};
            Vec3 n;
            if (hasNormals) {
                n = Vec3{vertex.normal[0], vertex.normal[1], vertex.normal[2]};
            } else {
                n = geoNormal;
            }
            if (backface) {
                n = Neg(n);
            }
            verts[i].normal = Normalize(n);
            verts[i].color = Modulate(vertex.color, material.baseColorFactor);
        }

        ClipNearPlane(verts, cam.nearZ, clipBuffer);
        if (clipBuffer.size() < 3) {
            continue;
        }
        for (std::size_t i = 1; i + 1 < clipBuffer.size(); ++i) {
            RasterizeTriangle(cam, color, depth, clipBuffer[0], clipBuffer[i], clipBuffer[i + 1],
                              opacity, emissive, unlit);
        }
    }
    return true;
}

} // namespace

ErrorCode RenderCpuTileRaster(const RasterRequest& request, RasterImage& out) noexcept
{
    out = RasterImage{};

    if (request.geometry == nullptr || request.requestedSize == 0) {
        // The degenerate cx == 0 is rejected at the COM boundary (ADR-0015); a
        // rasterizer called with it reports malformed input, never an image.
        return ErrorCode::MalformedData;
    }
    const SampledGeometry& geometry = *request.geometry;
    if (!geometry.bounds.valid ||
        (geometry.triangles.empty() && geometry.points.empty())) {
        return ErrorCode::MalformedData;
    }
    if (request.deadline != nullptr && request.deadline->expired()) {
        return ErrorCode::Cancelled;
    }

    const std::uint32_t size =
        (std::min)(request.requestedSize, static_cast<std::uint32_t>(kMaxRasterSize));

    int supersample = 1;
    if (request.allowSupersample) {
        const bool budget = request.deadline == nullptr ||
                            request.deadline->remaining() >= kSupersampleMinRemaining;
        if (budget) {
            supersample = kMaxSupersample;
        }
    }

    const std::uint64_t ssize = static_cast<std::uint64_t>(size) * static_cast<std::uint64_t>(supersample);

    // Charge the raster targets, scratch and output against the T06 ledger
    // before allocating them; a charge that would cross the ceiling returns the
    // null-bitmap limit HRESULT instead (design/05, "Stream ingestion").
    const auto targetPixels = CheckedMultiply(ssize, ssize);
    const auto colorBytes = targetPixels.has_value() ? CheckedMultiply(*targetPixels, 16u)
                                                     : std::nullopt;
    const auto depthBytes = targetPixels.has_value() ? CheckedMultiply(*targetPixels, 4u)
                                                     : std::nullopt;
    const auto outputBytes = CheckedMultiply(static_cast<std::uint64_t>(size) * size, 4u);
    if (!targetPixels.has_value() || !colorBytes.has_value() || !depthBytes.has_value() ||
        !outputBytes.has_value()) {
        return ErrorCode::ResourceLimit;
    }
    const auto subtotal = CheckedAdd(*colorBytes, *depthBytes);
    const auto totalBytes = subtotal.has_value() ? CheckedAdd(*subtotal, *outputBytes)
                                                 : std::nullopt;
    if (!totalBytes.has_value()) {
        return ErrorCode::ResourceLimit;
    }
    std::optional<AllocationReservation> reservation;
    if (request.ledger != nullptr) {
        reservation = request.ledger->ReserveScoped(*totalBytes);
        if (!reservation.has_value()) {
            return ErrorCode::ResourceLimit;
        }
    }

    Camera cam;
    cam.center = {geometry.bounds.min[0] * 0.5 + geometry.bounds.max[0] * 0.5,
                  geometry.bounds.min[1] * 0.5 + geometry.bounds.max[1] * 0.5,
                  geometry.bounds.min[2] * 0.5 + geometry.bounds.max[2] * 0.5};
    cam.forward = Normalize(Vec3{-1.0, -1.0, -1.0});
    cam.right = Normalize(Cross(Vec3{0.0, 1.0, 0.0}, cam.forward));
    cam.up = Cross(cam.forward, cam.right);
    cam.ssize = static_cast<int>(ssize);

    double maxPointRadius = 0.0;
    for (const PointSample& point : geometry.points) {
        if (std::isfinite(point.radius) && point.radius > maxPointRadius) {
            maxPointRadius = point.radius;
        }
    }
    const Vec3 extent{geometry.bounds.max[0] - geometry.bounds.min[0],
                      geometry.bounds.max[1] - geometry.bounds.min[1],
                      geometry.bounds.max[2] - geometry.bounds.min[2]};
    const double diag = (std::max)(Length(extent), 1e-9);
    const double defaultRadius = diag * 0.006;
    if (maxPointRadius <= 0.0) {
        maxPointRadius = defaultRadius;
    }

    double minX = std::numeric_limits<double>::infinity();
    double maxX = -std::numeric_limits<double>::infinity();
    double minY = std::numeric_limits<double>::infinity();
    double maxY = -std::numeric_limits<double>::infinity();
    double minZ = std::numeric_limits<double>::infinity();
    double maxZ = -std::numeric_limits<double>::infinity();
    for (int ci = 0; ci < 8; ++ci) {
        const Vec3 corner{(ci & 1) ? geometry.bounds.max[0] : geometry.bounds.min[0],
                          (ci & 2) ? geometry.bounds.max[1] : geometry.bounds.min[1],
                          (ci & 4) ? geometry.bounds.max[2] : geometry.bounds.min[2]};
        const Vec3 rel = Sub(corner, cam.center);
        minX = (std::min)(minX, Dot(rel, cam.right));
        maxX = (std::max)(maxX, Dot(rel, cam.right));
        minY = (std::min)(minY, Dot(rel, cam.up));
        maxY = (std::max)(maxY, Dot(rel, cam.up));
        minZ = (std::min)(minZ, Dot(rel, cam.forward));
        maxZ = (std::max)(maxZ, Dot(rel, cam.forward));
    }
    minX -= maxPointRadius;
    maxX += maxPointRadius;
    minY -= maxPointRadius;
    maxY += maxPointRadius;
    minZ -= maxPointRadius;
    maxZ += maxPointRadius;

    const double span = (std::max)(maxZ - minZ, 1e-9);
    double contentExtent = (std::max)((maxX - minX) * 0.5, (maxY - minY) * 0.5);
    if (contentExtent < 1e-9) {
        contentExtent = 1e-9;
    }
    constexpr double margin = 0.07;
    const double usable = static_cast<double>(ssize) * (1.0 - 2.0 * margin);
    cam.pxPerUnit = (usable * 0.5) / contentExtent;
    cam.cx = static_cast<double>(ssize) * 0.5 - (minX + maxX) * 0.5 * cam.pxPerUnit;
    cam.cy = static_cast<double>(ssize) * 0.5 + (minY + maxY) * 0.5 * cam.pxPerUnit;
    cam.nearZ = minZ - span * 0.05;
    cam.farZ = maxZ + span * 0.05;
    if (cam.farZ - cam.nearZ < kEps) {
        cam.farZ = cam.nearZ + kEps;
    }

    std::vector<float> color;
    std::vector<float> depth;
    std::vector<ClipVertex> clipBuffer;
    try {
        color.assign(static_cast<std::size_t>(*targetPixels) * 4, 0.0f);
        depth.assign(static_cast<std::size_t>(*targetPixels), std::numeric_limits<float>::infinity());
        clipBuffer.reserve(4);
    } catch (...) {
        out = RasterImage{};
        return ErrorCode::OutOfMemory;
    }

    WorkGuard guard;
    guard.deadline = request.deadline;

    RenderFloorShadow(cam, geometry.bounds, usable, color);

    if (!RenderTriangles(cam, geometry, request.materials, color, depth, clipBuffer, guard)) {
        out = RasterImage{};
        return ErrorCode::Cancelled;
    }
    if (!RenderPoints(cam, geometry, defaultRadius, request.materials, color, depth, guard)) {
        out = RasterImage{};
        return ErrorCode::Cancelled;
    }

    try {
        out.width = size;
        out.height = size;
        out.bgraPremultiplied.assign(static_cast<std::size_t>(size) * size * 4, 0u);
    } catch (...) {
        out = RasterImage{};
        return ErrorCode::OutOfMemory;
    }

    const int samples = supersample * supersample;
    const float invSamples = 1.0f / static_cast<float>(samples);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            double ar = 0.0;
            double ag = 0.0;
            double ab = 0.0;
            double aa = 0.0;
            for (int dy = 0; dy < supersample; ++dy) {
                const std::size_t row =
                    (static_cast<std::size_t>(y) * supersample + static_cast<std::size_t>(dy)) * ssize;
                for (int dx = 0; dx < supersample; ++dx) {
                    const std::size_t idx =
                        (row + static_cast<std::size_t>(x) * supersample + static_cast<std::size_t>(dx)) * 4;
                    ar += color[idx + 0];
                    ag += color[idx + 1];
                    ab += color[idx + 2];
                    aa += color[idx + 3];
                }
            }
            ar *= invSamples;
            ag *= invSamples;
            ab *= invSamples;
            aa *= invSamples;
            const double a = Clamp01(aa);
            std::uint8_t* pixel =
                out.bgraPremultiplied.data() + (static_cast<std::size_t>(y) * size + x) * 4;
            if (a <= 0.0) {
                pixel[0] = pixel[1] = pixel[2] = pixel[3] = 0u;
                continue;
            }
            const double straightR = ar / a;
            const double straightG = ag / a;
            const double straightB = ab / a;
            pixel[0] = ToByte(ToSrgb(straightB) * a * 255.0);
            pixel[1] = ToByte(ToSrgb(straightG) * a * 255.0);
            pixel[2] = ToByte(ToSrgb(straightR) * a * 255.0);
            pixel[3] = ToByte(a * 255.0);
        }
    }

    return ErrorCode::None;
}

} // namespace preview3d::provider