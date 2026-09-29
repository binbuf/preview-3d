#pragma once

// SPIKE-8a (T02): a product-owned CPU tile rasterizer prototype.
//
// This is deliberately dependency-free: no GPU device, no D3D, no third-party
// renderer, no COM. It takes triangle and point lists and writes a
// premultiplied BGRA buffer. T03 wires this same Render() entry point into its
// Shell-surrogate probe. The production tile rasterizer (T15) is expected to
// replace this implementation behind the same contract, not to reuse the
// prototype code as-is.
//
// Contract implemented here (design/05-thumbnail-provider.md, "CPU renderer"):
//  - transparent canvas with a soft neutral contact shadow;
//  - fixed isometric view framed from verified bounds with ~7% margin;
//  - double-precision model/view transforms, near-plane clip, depth test;
//  - ambient plus two fixed lights; opaque/masked triangles; a weighted-opaque
//    approximation for transparency; round depth-tested point splats;
//  - linear-space downsample, converted to premultiplied BGRA.

#include <cstdint>
#include <span>
#include <vector>

namespace thumbnail_rasterizer
{

struct Vec3
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct LinearRgb
{
    float r = 0.5f;
    float g = 0.5f;
    float b = 0.5f;
};

struct Triangle
{
    Vec3 p[3];
    Vec3 n[3]; // used only when hasNormals is set; otherwise a flat normal is derived
    LinearRgb color[3];
    float alpha = 1.0f;     // 0..1 material alpha; <1 uses the weighted-opaque approximation
    float maskCutoff = 0.5f;
    std::uint8_t flags = 0; // bit0 hasNormals, bit1 masked, bit2 doubleSided
};

constexpr std::uint8_t kTriangleHasNormals = 0x1;
constexpr std::uint8_t kTriangleMasked = 0x2;
constexpr std::uint8_t kTriangleDoubleSided = 0x4;

struct Point
{
    Vec3 p;
    LinearRgb color;
    float radius = 0.0f; // world-space radius; <=0 selects a bounds-derived default
    float alpha = 1.0f;
};

struct GeometryView
{
    std::span<const Triangle> triangles;
    std::span<const Point> points;
};

struct Options
{
    int size = 256;      // returned bitmap is size x size
    int supersample = 2; // >=1 internal supersampling factor
    double marginFraction = 0.07;
    bool floorShadow = true;
};

struct Image
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> bgraPremultiplied; // top-down, 4 bytes per pixel
};

enum class Status
{
    Ok,
    NoGeometry,
    InvalidOptions,
    Cancelled,
};

// Cooperative cancellation check, polled at bounded raster intervals.
using CancelFn = bool (*)(void* userData);

struct Bounds
{
    Vec3 min;
    Vec3 max;
    bool valid = false;
};

Bounds ComputeBounds(const GeometryView& geometry);

Status Render(const GeometryView& geometry, const Options& options, Image& out,
              CancelFn cancel = nullptr, void* cancelUser = nullptr);

} // namespace thumbnail_rasterizer