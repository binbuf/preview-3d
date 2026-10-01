#include "ThumbnailRasterizer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace thumbnail_rasterizer
{
namespace
{

constexpr double kEps = 1e-12;
constexpr std::uint64_t kCancelInterval = 2048;

Vec3 Add(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
Vec3 Sub(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
Vec3 Neg(const Vec3& a) { return { -a.x, -a.y, -a.z }; }
Vec3 Scale(const Vec3& a, double s) { return { a.x * s, a.y * s, a.z * s }; }
double Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
double Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }
Vec3 Normalize(const Vec3& a)
{
    const double len = Length(a);
    return len > kEps ? Scale(a, 1.0 / len) : Vec3{ 0.0, 1.0, 0.0 };
}

bool IsFinite(const Vec3& a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

float ToSrgb(float linear)
{
    const float c = std::clamp(linear, 0.0f, 1.0f);
    return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

LinearRgb ToLinearRgb(float r, float g, float b)
{
    // Treat authored material colors as sRGB and convert to linear for shading
    // and averaging. Deterministic and consistent with the viewer palette.
    auto channel = [](float c) {
        c = std::clamp(c, 0.0f, 1.0f);
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    return { channel(r), channel(g), channel(b) };
}

struct Camera
{
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

struct ClipVertex
{
    Vec3 view;
    Vec3 normal;
    LinearRgb color;
};

ClipVertex LerpVertex(const ClipVertex& a, const ClipVertex& b, double t)
{
    ClipVertex out;
    out.view = Add(a.view, Scale(Sub(b.view, a.view), t));
    out.normal = Add(a.normal, Scale(Sub(b.normal, a.normal), t));
    out.color = { static_cast<float>(a.color.r + (b.color.r - a.color.r) * t),
                  static_cast<float>(a.color.g + (b.color.g - a.color.g) * t),
                  static_cast<float>(a.color.b + (b.color.b - a.color.b) * t) };
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
        if (curIn) out.push_back(cur);
        if (curIn != nxtIn) {
            const double t = dc / (dc - dn);
            out.push_back(LerpVertex(cur, nxt, t));
        }
    }
}

// Scene-space ambient plus two fixed directional lights; returns straight linear RGB.
LinearRgb Shade(const Vec3& normal, const LinearRgb& albedo)
{
    static const Vec3 light1 = Normalize(Vec3{ 0.45, 0.80, 0.62 });
    static const Vec3 light2 = Normalize(Vec3{ -0.62, 0.30, -0.55 });
    const Vec3 n = Normalize(normal);
    const double d1 = (std::max)(0.0, Dot(n, light1));
    const double d2 = (std::max)(0.0, Dot(n, light2));
    const double ambient = 0.30;
    const Vec3 ambientColor{ 0.92, 0.94, 1.0 };
    const Vec3 light1Color{ 1.0, 0.96, 0.90 };
    const Vec3 light2Color{ 0.72, 0.80, 1.0 };
    const double fill = 0.10;

    LinearRgb out;
    out.r = static_cast<float>(albedo.r * (ambient * ambientColor.x + 0.62 * d1 * light1Color.x +
                                           0.34 * d2 * light2Color.x + fill));
    out.g = static_cast<float>(albedo.g * (ambient * ambientColor.y + 0.62 * d1 * light1Color.y +
                                           0.34 * d2 * light2Color.y + fill));
    out.b = static_cast<float>(albedo.b * (ambient * ambientColor.z + 0.62 * d1 * light1Color.z +
                                           0.34 * d2 * light2Color.z + fill));
    return out;
}

struct Pixel
{
    double x;
    double y;
    double depth;
    Vec3 normal;
    LinearRgb color;
    float alpha;
};

double Edge(double ax, double ay, double bx, double by, double px, double py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

void CompositePixel(std::vector<float>& color, std::vector<float>& depth, int ssize, int x, int y,
                    double z, const LinearRgb& shaded, float alpha)
{
    const int idx = y * ssize + x;
    const float a = Clamp01(alpha);
    if (a <= 0.0f) return;
    const float inv = 1.0f - a;
    color[idx * 4 + 0] = shaded.r * a + color[idx * 4 + 0] * inv;
    color[idx * 4 + 1] = shaded.g * a + color[idx * 4 + 1] * inv;
    color[idx * 4 + 2] = shaded.b * a + color[idx * 4 + 2] * inv;
    color[idx * 4 + 3] = a + color[idx * 4 + 3] * inv;
    depth[idx] = static_cast<float>(z);
}

void RasterizeTriangle(const Camera& cam, std::vector<float>& color, std::vector<float>& depth,
                       const ClipVertex& v0, const ClipVertex& v1, const ClipVertex& v2, float opacity)
{
    const double x0 = cam.cx + v0.view.x * cam.pxPerUnit;
    const double y0 = cam.cy - v0.view.y * cam.pxPerUnit;
    const double x1 = cam.cx + v1.view.x * cam.pxPerUnit;
    const double y1 = cam.cy - v1.view.y * cam.pxPerUnit;
    const double x2 = cam.cx + v2.view.x * cam.pxPerUnit;
    const double y2 = cam.cy - v2.view.y * cam.pxPerUnit;
    if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1) ||
        !std::isfinite(x2) || !std::isfinite(y2))
        return;

    const double z0 = (v0.view.z - cam.nearZ) / (cam.farZ - cam.nearZ);
    const double z1 = (v1.view.z - cam.nearZ) / (cam.farZ - cam.nearZ);
    const double z2 = (v2.view.z - cam.nearZ) / (cam.farZ - cam.nearZ);

    const double area = Edge(x0, y0, x1, y1, x2, y2);
    if (std::fabs(area) < kEps) return;
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
    if (px1 < px0 || py1 < py0) return;

    for (int py = py0; py <= py1; ++py) {
        const double sy = py + 0.5;
        for (int px = px0; px <= px1; ++px) {
            const double sx = px + 0.5;
            const double w0 = Edge(x1, y1, x2, y2, sx, sy);
            const double w1 = Edge(x2, y2, x0, y0, sx, sy);
            const double w2 = Edge(x0, y0, x1, y1, sx, sy);
            if (positive) {
                if (w0 < -kEps || w1 < -kEps || w2 < -kEps) continue;
            } else {
                if (w0 > kEps || w1 > kEps || w2 > kEps) continue;
            }
            const double b0 = w0 * invArea;
            const double b1 = w1 * invArea;
            const double b2 = w2 * invArea;
            const double z = b0 * z0 + b1 * z1 + b2 * z2;
            const int idx = py * ssize + px;
            if (z >= depth[idx]) continue;
            const Vec3 n = Add(Scale(v0.normal, b0), Add(Scale(v1.normal, b1), Scale(v2.normal, b2)));
            const LinearRgb c{ static_cast<float>(b0 * v0.color.r + b1 * v1.color.r + b2 * v2.color.r),
                               static_cast<float>(b0 * v0.color.g + b1 * v1.color.g + b2 * v2.color.g),
                               static_cast<float>(b0 * v0.color.b + b1 * v1.color.b + b2 * v2.color.b) };
            LinearRgb shaded = Shade(n, c);
            if (opacity < 1.0f) {
                // Weighted-opaque transparency approximation: blend toward a
                // neutral frost by the material opacity and write an opaque
                // result, so no sorting or true alpha blending is required.
                static const LinearRgb frost = ToLinearRgb(0.82f, 0.84f, 0.86f);
                const float w = 1.0f - opacity;
                shaded.r = shaded.r * opacity + frost.r * w;
                shaded.g = shaded.g * opacity + frost.g * w;
                shaded.b = shaded.b * opacity + frost.b * w;
            }
            CompositePixel(color, depth, ssize, px, py, z, shaded, 1.0f);
        }
    }
}

bool RenderPoints(const Camera& cam, const GeometryView& geometry, double defaultRadius,
                  std::vector<float>& color, std::vector<float>& depth, CancelFn cancel,
                  void* cancelUser, std::uint64_t& work)
{
    const Vec3 towardCamera = Neg(cam.forward);
    const Vec3 ambientNormal = Normalize(towardCamera);
    for (const Point& point : geometry.points) {
        if (!IsFinite(point.p)) continue;
        if (cancel && (++work % kCancelInterval == 0) && cancel(cancelUser)) return false;

        const Vec3 rel = Sub(point.p, cam.center);
        const double vx = Dot(rel, cam.right);
        const double vy = Dot(rel, cam.up);
        const double vz = Dot(rel, cam.forward);
        if (vz < cam.nearZ) continue;
        const double sx = cam.cx + vx * cam.pxPerUnit;
        const double sy = cam.cy - vy * cam.pxPerUnit;
        if (!std::isfinite(sx) || !std::isfinite(sy)) continue;
        const double z = (vz - cam.nearZ) / (cam.farZ - cam.nearZ);
        if (z < 0.0 || z > 1.0) continue;

        const float worldRadius = point.radius > 0.0f ? point.radius : static_cast<float>(defaultRadius);
        double radiusPx = static_cast<double>(worldRadius) * cam.pxPerUnit;
        radiusPx = std::clamp(radiusPx, 1.0, 128.0);

        const LinearRgb shaded = Shade(ambientNormal, point.color);
        const int px0 = (std::max)(0, static_cast<int>(std::floor(sx - radiusPx - 1.0)));
        const int px1 = (std::min)(cam.ssize - 1, static_cast<int>(std::ceil(sx + radiusPx + 1.0)));
        const int py0 = (std::max)(0, static_cast<int>(std::floor(sy - radiusPx - 1.0)));
        const int py1 = (std::min)(cam.ssize - 1, static_cast<int>(std::ceil(sy + radiusPx + 1.0)));
        if (px1 < px0 || py1 < py0) continue;

        for (int py = py0; py <= py1; ++py) {
            const double fy = py + 0.5 - sy;
            for (int px = px0; px <= px1; ++px) {
                const double fx = px + 0.5 - sx;
                const double dist = std::sqrt(fx * fx + fy * fy);
                const double coverage = std::clamp(radiusPx + 0.5 - dist, 0.0, 1.0);
                if (coverage <= 0.0) continue;
                const int idx = py * cam.ssize + px;
                if (z >= depth[idx]) continue;
                CompositePixel(color, depth, cam.ssize, px, py, z, shaded,
                               static_cast<float>(coverage) * Clamp01(point.alpha));
            }
        }
    }
    return true;
}

void RenderFloorShadow(const Camera& cam, const Bounds& bounds, double usable,
                       std::vector<float>& color)
{
    const Vec3 base{ (bounds.min.x + bounds.max.x) * 0.5, bounds.min.y,
                     (bounds.min.z + bounds.max.z) * 0.5 };
    const Vec3 rel = Sub(base, cam.center);
    const double bx = cam.cx + Dot(rel, cam.right) * cam.pxPerUnit;
    const double by = cam.cy - Dot(rel, cam.up) * cam.pxPerUnit;
    const double sigma = (std::max)(1.0, usable * 0.5 * 0.55);
    const double verticalSquash = 0.42;
    const double strength = 0.34;

    const int radius = static_cast<int>(std::ceil(sigma * 3.0));
    const int px0 = (std::max)(0, static_cast<int>(std::floor(bx)) - radius);
    const int px1 = (std::min)(cam.ssize - 1, static_cast<int>(std::ceil(bx)) + radius);
    const int py0 = (std::max)(0, static_cast<int>(std::floor(by)) - radius);
    const int py1 = (std::min)(cam.ssize - 1, static_cast<int>(std::ceil(by)) + radius);

    LinearRgb shadowColor = ToLinearRgb(0.06f, 0.06f, 0.07f);
    const double denom = 2.0 * sigma * sigma;
    for (int py = py0; py <= py1; ++py) {
        const double dy = (py + 0.5 - by) / verticalSquash;
        for (int px = px0; px <= px1; ++px) {
            const double dx = px + 0.5 - bx;
            const double d2 = dx * dx + dy * dy;
            const float a = static_cast<float>(strength * std::exp(-d2 / denom));
            if (a <= 0.0f) continue;
            const int idx = py * cam.ssize + px;
            const float inv = 1.0f - a;
            color[idx * 4 + 0] = shadowColor.b * a + color[idx * 4 + 0] * inv;
            color[idx * 4 + 1] = shadowColor.g * a + color[idx * 4 + 1] * inv;
            color[idx * 4 + 2] = shadowColor.r * a + color[idx * 4 + 2] * inv;
            color[idx * 4 + 3] = a + color[idx * 4 + 3] * inv;
        }
    }
}

} // namespace

Bounds ComputeBounds(const GeometryView& geometry)
{
    Bounds bounds;
    auto include = [&bounds](const Vec3& p) {
        if (!IsFinite(p)) return;
        if (!bounds.valid) {
            bounds.min = bounds.max = p;
            bounds.valid = true;
            return;
        }
        bounds.min.x = (std::min)(bounds.min.x, p.x);
        bounds.min.y = (std::min)(bounds.min.y, p.y);
        bounds.min.z = (std::min)(bounds.min.z, p.z);
        bounds.max.x = (std::max)(bounds.max.x, p.x);
        bounds.max.y = (std::max)(bounds.max.y, p.y);
        bounds.max.z = (std::max)(bounds.max.z, p.z);
    };
    for (const Triangle& triangle : geometry.triangles) {
        include(triangle.p[0]);
        include(triangle.p[1]);
        include(triangle.p[2]);
    }
    for (const Point& point : geometry.points) include(point.p);
    return bounds;
}

Status Render(const GeometryView& geometry, const Options& options, Image& out, CancelFn cancel,
              void* cancelUser)
{
    out = Image{};
    if (options.size < 1 || options.size > 4096 || options.supersample < 1 ||
        options.supersample > 4)
        return Status::InvalidOptions;

    const Bounds bounds = ComputeBounds(geometry);
    if (!bounds.valid) return Status::NoGeometry;

    const int size = options.size;
    const int ss = options.supersample;
    const int ssize = size * ss;

    Camera cam;
    cam.center = Scale(Add(bounds.min, bounds.max), 0.5);
    cam.forward = Normalize(Vec3{ -1.0, -1.0, -1.0 });
    cam.right = Normalize(Cross(Vec3{ 0.0, 1.0, 0.0 }, cam.forward));
    cam.up = Cross(cam.forward, cam.right);
    cam.ssize = ssize;

    double maxPointRadius = 0.0;
    for (const Point& point : geometry.points) {
        if (std::isfinite(point.radius) && point.radius > maxPointRadius)
            maxPointRadius = point.radius;
    }
    const Vec3 extent = Sub(bounds.max, bounds.min);
    const double diag = (std::max)(Length(extent), 1e-9);
    const double defaultRadius = diag * 0.006;
    if (maxPointRadius <= 0.0) maxPointRadius = defaultRadius;

    double minX = std::numeric_limits<double>::infinity();
    double maxX = -std::numeric_limits<double>::infinity();
    double minY = std::numeric_limits<double>::infinity();
    double maxY = -std::numeric_limits<double>::infinity();
    double minZ = std::numeric_limits<double>::infinity();
    double maxZ = -std::numeric_limits<double>::infinity();
    for (int ci = 0; ci < 8; ++ci) {
        const Vec3 corner{ (ci & 1) ? bounds.max.x : bounds.min.x,
                           (ci & 2) ? bounds.max.y : bounds.min.y,
                           (ci & 4) ? bounds.max.z : bounds.min.z };
        const Vec3 rel = Sub(corner, cam.center);
        const double vx = Dot(rel, cam.right);
        const double vy = Dot(rel, cam.up);
        const double vz = Dot(rel, cam.forward);
        minX = (std::min)(minX, vx);
        maxX = (std::max)(maxX, vx);
        minY = (std::min)(minY, vy);
        maxY = (std::max)(maxY, vy);
        minZ = (std::min)(minZ, vz);
        maxZ = (std::max)(maxZ, vz);
    }
    minX -= maxPointRadius;
    maxX += maxPointRadius;
    minY -= maxPointRadius;
    maxY += maxPointRadius;
    minZ -= maxPointRadius;
    maxZ += maxPointRadius;

    const double span = (std::max)(maxZ - minZ, 1e-9);
    double contentExtent = (std::max)((maxX - minX) * 0.5, (maxY - minY) * 0.5);
    if (contentExtent < 1e-9) contentExtent = 1e-9;
    const double margin = std::clamp(options.marginFraction, 0.0, 0.45);
    const double usable = static_cast<double>(ssize) * (1.0 - 2.0 * margin);
    cam.pxPerUnit = (usable * 0.5) / contentExtent;
    cam.cx = static_cast<double>(ssize) * 0.5 - (minX + maxX) * 0.5 * cam.pxPerUnit;
    cam.cy = static_cast<double>(ssize) * 0.5 + (minY + maxY) * 0.5 * cam.pxPerUnit;
    cam.nearZ = minZ - span * 0.05;
    cam.farZ = maxZ + span * 0.05;
    if (cam.farZ - cam.nearZ < kEps) cam.farZ = cam.nearZ + kEps;

    std::vector<float> color(static_cast<std::size_t>(ssize) * ssize * 4, 0.0f);
    std::vector<float> depth(static_cast<std::size_t>(ssize) * ssize,
                             std::numeric_limits<float>::infinity());

    if (options.floorShadow) RenderFloorShadow(cam, bounds, usable, color);

    std::uint64_t work = 0;
    std::vector<ClipVertex> clipBuffer;
    clipBuffer.reserve(4);
    std::vector<ClipVertex> poly;
    poly.reserve(4);

    for (const Triangle& triangle : geometry.triangles) {
        if (cancel && (++work % kCancelInterval == 0) && cancel(cancelUser)) return Status::Cancelled;
        if (!IsFinite(triangle.p[0]) || !IsFinite(triangle.p[1]) || !IsFinite(triangle.p[2]))
            continue;
        const Vec3 geoNormal =
            Normalize(Cross(Sub(triangle.p[1], triangle.p[0]), Sub(triangle.p[2], triangle.p[0])));
        if (Length(Cross(Sub(triangle.p[1], triangle.p[0]), Sub(triangle.p[2], triangle.p[0]))) < kEps)
            continue;
        const bool backface = Dot(geoNormal, cam.forward) > 0.0;
        const bool doubleSided = (triangle.flags & kTriangleDoubleSided) != 0;
        if (backface && !doubleSided) continue;

        float alpha = Clamp01(triangle.alpha);
        if ((triangle.flags & kTriangleMasked) != 0) {
            if (triangle.alpha < triangle.maskCutoff) continue;
            alpha = 1.0f;
        }

        const bool hasNormals = (triangle.flags & kTriangleHasNormals) != 0;
        ClipVertex verts[3];
        const Vec3* positions[3] = { &triangle.p[0], &triangle.p[1], &triangle.p[2] };
        for (int i = 0; i < 3; ++i) {
            const Vec3 rel = Sub(*positions[i], cam.center);
            verts[i].view = { Dot(rel, cam.right), Dot(rel, cam.up), Dot(rel, cam.forward) };
            Vec3 n = hasNormals ? triangle.n[i] : geoNormal;
            if (backface) n = Neg(n);
            verts[i].normal = Normalize(n);
            verts[i].color = triangle.color[i];
        }

        ClipNearPlane(verts, cam.nearZ, clipBuffer);
        if (clipBuffer.size() < 3) continue;

        // Fan-triangulate the clipped polygon against a scratch list so we do
        // not allocate per triangle.
        for (std::size_t i = 1; i + 1 < clipBuffer.size(); ++i) {
            RasterizeTriangle(cam, color, depth, clipBuffer[0], clipBuffer[i], clipBuffer[i + 1],
                              alpha);
        }
    }

    if (!RenderPoints(cam, geometry, defaultRadius, color, depth, cancel, cancelUser, work))
        return Status::Cancelled;

    out.width = size;
    out.height = size;
    out.bgraPremultiplied.assign(static_cast<std::size_t>(size) * size * 4, 0u);
    const int samples = ss * ss;
    const float invSamples = 1.0f / static_cast<float>(samples);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float ar = 0.0f;
            float ag = 0.0f;
            float ab = 0.0f;
            float aa = 0.0f;
            for (int dy = 0; dy < ss; ++dy) {
                const int row = (y * ss + dy) * ssize;
                for (int dx = 0; dx < ss; ++dx) {
                    const int idx = (row + x * ss + dx) * 4;
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
            const float a = Clamp01(aa);
            std::uint8_t* outPixel =
                out.bgraPremultiplied.data() + (static_cast<std::size_t>(y) * size + x) * 4;
            if (a <= 0.0f) {
                outPixel[0] = outPixel[1] = outPixel[2] = outPixel[3] = 0u;
                continue;
            }
            const float straightR = ar / a;
            const float straightG = ag / a;
            const float straightB = ab / a;
            outPixel[0] = static_cast<std::uint8_t>(std::lround(ToSrgb(straightB) * a * 255.0f));
            outPixel[1] = static_cast<std::uint8_t>(std::lround(ToSrgb(straightG) * a * 255.0f));
            outPixel[2] = static_cast<std::uint8_t>(std::lround(ToSrgb(straightR) * a * 255.0f));
            outPixel[3] = static_cast<std::uint8_t>(std::lround(a * 255.0f));
        }
    }
    return Status::Ok;
}

} // namespace thumbnail_rasterizer