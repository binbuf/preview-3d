#include "StudioEnvironment.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace studio_ibl {
namespace {

constexpr float kPi = 3.14159265358979323846f;

Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Mul(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 Normalize(const Vec3& v)
{
    const float length = std::sqrt(Dot(v, v));
    if (length <= 1e-12f) return {0.0f, 0.0f, 1.0f};
    return Mul(v, 1.0f / length);
}

Rgb Add(const Rgb& a, const Rgb& b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
Rgb Mul(const Rgb& a, float s) { return {a.r * s, a.g * s, a.b * s}; }
Rgb Lerp(const Rgb& a, const Rgb& b, float t)
{
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
float SmoothStep(float edge0, float edge1, float x)
{
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// One rectangular softbox in direction space. `axis` points at the centre of
// the panel; `right`/`up` span its plane. halfX/halfY are tangent-plane half
// extents, so the panel subtends a finite solid angle. The feather softens the
// edges without changing the centre radiance.
Rgb Softbox(const Vec3& direction, const Vec3& axis, float halfX, float halfY,
            float feather, const Rgb& radiance)
{
    const float along = Dot(direction, axis);
    if (along <= 0.02f) return {};
    const Vec3 right = Normalize(Cross({0.0f, 0.0f, 1.0f}, axis));
    const Vec3 up = Normalize(Cross(axis, right));
    if (Dot(right, right) < 0.5f || Dot(up, up) < 0.5f) return {};
    const float x = Dot(direction, right) / along;
    const float y = Dot(direction, up) / along;
    const float maskX = 1.0f - SmoothStep(halfX - feather, halfX + feather, std::fabs(x));
    const float maskY = 1.0f - SmoothStep(halfY - feather, halfY + feather, std::fabs(y));
    const float fade = SmoothStep(0.02f, 0.20f, along);
    return Mul(radiance, maskX * maskY * fade);
}

Vec3 SoftboxAxis(float azimuth, float elevation)
{
    const float ce = std::cos(elevation);
    return Normalize({ce * std::cos(azimuth), ce * std::sin(azimuth), std::sin(elevation)});
}

float RadicalInverse(uint32_t bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

Vec3 ImportanceSampleGgx(float xi1, float xi2, const Vec3& normal, float roughness)
{
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float phi = 2.0f * kPi * xi1;
    const float cosTheta = std::sqrt(std::max(0.0f, (1.0f - xi2) / (1.0f + (a2 - 1.0f) * xi2)));
    const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
    const Vec3 h{std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta};
    const Vec3 reference = std::fabs(normal.z) < 0.999f ? Vec3{0.0f, 0.0f, 1.0f}
                                                        : Vec3{1.0f, 0.0f, 0.0f};
    const Vec3 tangent = Normalize(Cross(reference, normal));
    const Vec3 bitangent = Cross(normal, tangent);
    return Normalize(Add(Add(Mul(tangent, h.x), Mul(bitangent, h.y)), Mul(normal, h.z)));
}

void ShBasis(const Vec3& n, float basis[9])
{
    basis[0] = 0.282095f;
    basis[1] = 0.488603f * n.y;
    basis[2] = 0.488603f * n.z;
    basis[3] = 0.488603f * n.x;
    basis[4] = 1.092548f * n.x * n.y;
    basis[5] = 1.092548f * n.y * n.z;
    basis[6] = 0.315392f * (3.0f * n.z * n.z - 1.0f);
    basis[7] = 1.092548f * n.x * n.z;
    basis[8] = 0.546274f * (n.x * n.x - n.y * n.y);
}

uint32_t MipExtent(uint32_t base, uint32_t level)
{
    return std::max(1u, base >> level);
}

void BuildCube(CubeImage& image, uint32_t size,
               const std::function<Rgb(const Vec3&)>& radiance)
{
    image.size = size;
    for (uint32_t face = 0; face < 6; ++face) {
        image.faces[face].resize(static_cast<size_t>(size) * size);
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                image.faces[face][static_cast<size_t>(y) * size + x] =
                    radiance(CubemapDirection(face, x, y, size));
            }
        }
    }
}

ShCoefficients ProjectIrradiance(const CubeImage& base)
{
    ShCoefficients result;
    const float deltaOmega = 4.0f * kPi / (6.0f * static_cast<float>(base.size) * base.size);
    for (uint32_t face = 0; face < 6; ++face) {
        const auto& texels = base.faces[face];
        for (uint32_t y = 0; y < base.size; ++y) {
            for (uint32_t x = 0; x < base.size; ++x) {
                const Vec3 dir = CubemapDirection(face, x, y, base.size);
                float basis[9];
                ShBasis(dir, basis);
                const Rgb radiance = texels[static_cast<size_t>(y) * base.size + x];
                for (uint32_t i = 0; i < 9; ++i) {
                    // Band weights: L0 keeps pi/pi, L1 2pi/3 / pi, L2 pi/4 / pi.
                    const float band = i == 0 ? 1.0f : (i < 4 ? 2.0f / 3.0f : 0.25f);
                    result.values[i] = Add(result.values[i],
                                           Mul(radiance, basis[i] * band * deltaOmega));
                }
            }
        }
    }
    return result;
}

} // namespace

Vec3 CubemapDirection(uint32_t face, uint32_t x, uint32_t y, uint32_t size)
{
    const float u = 2.0f * (static_cast<float>(x) + 0.5f) / static_cast<float>(size) - 1.0f;
    const float v = 2.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(size) - 1.0f;
    switch (face)
    {
    case 0: return Normalize({1.0f, -v, -u});   // +X
    case 1: return Normalize({-1.0f, -v, u});   // -X
    case 2: return Normalize({u, 1.0f, v});     // +Y
    case 3: return Normalize({u, -1.0f, -v});   // -Y
    case 4: return Normalize({u, -v, 1.0f});    // +Z
    default: return Normalize({-u, -v, -1.0f}); // -Z
    }
}

Rgb ProceduralStudioRadiance(const Vec3& direction)
{
    const Vec3 dir = Normalize(direction);
    const float up = std::clamp(dir.z * 0.5f + 0.5f, 0.0f, 1.0f);
    Rgb color = Lerp({0.030f, 0.029f, 0.027f}, {0.165f, 0.166f, 0.175f},
                     SmoothStep(0.0f, 0.5f, up));
    color = Lerp(color, {0.52f, 0.54f, 0.58f}, SmoothStep(0.5f, 1.0f, up));

    // Key softbox: high-right-front, the main highlight source.
    color = Add(color, Softbox(dir, SoftboxAxis(0.72f, 0.62f), 0.34f, 0.22f, 0.07f,
                               {9.0f, 8.6f, 7.9f}));
    // Fill softbox: low-left, broad and cool.
    color = Add(color, Softbox(dir, SoftboxAxis(-2.20f, 0.22f), 0.40f, 0.30f, 0.10f,
                               {2.2f, 2.4f, 2.9f}));
    // Rim strip behind the subject, tall and narrow.
    color = Add(color, Softbox(dir, SoftboxAxis(2.85f, 0.95f), 0.18f, 0.38f, 0.06f,
                               {5.4f, 5.5f, 5.8f}));
    // Overhead panel; a visible ceiling reflection on polished metals.
    color = Add(color, Softbox(dir, SoftboxAxis(0.0f, 1.35f), 0.58f, 0.16f, 0.08f,
                               {1.7f, 1.75f, 1.8f}));
    return color;
}

EnvironmentBake BakeStudioEnvironment(const BakeOptions& options)
{
    BakeOptions settings = options;
    settings.faceSize = std::clamp(settings.faceSize, 4u, 256u);
    // Keep the cubemap a power of two so the mip chain is exact.
    uint32_t power = 4;
    while (power * 2u <= settings.faceSize) power *= 2u;
    settings.faceSize = power;

    uint32_t fullChain = 1;
    for (uint32_t extent = settings.faceSize; extent > 1; extent >>= 1) ++fullChain;
    settings.specularMipCount = std::clamp(settings.specularMipCount, 1u, fullChain);
    settings.specularSampleCount = std::clamp(settings.specularSampleCount, 4u, 256u);

    EnvironmentBake bake;
    BuildCube(bake.base, settings.faceSize, ProceduralStudioRadiance);
    bake.specularMips.push_back(bake.base);

    for (uint32_t level = 1; level < settings.specularMipCount; ++level)
    {
        const uint32_t size = MipExtent(settings.faceSize, level);
        const float roughness = settings.specularMipCount > 1
            ? static_cast<float>(level) / static_cast<float>(settings.specularMipCount - 1)
            : 1.0f;
        CubeImage mip;
        mip.size = size;
        const uint32_t samples = settings.specularSampleCount;
        for (uint32_t face = 0; face < 6; ++face)
        {
            mip.faces[face].resize(static_cast<size_t>(size) * size);
            for (uint32_t y = 0; y < size; ++y)
            {
                for (uint32_t x = 0; x < size; ++x)
                {
                    const Vec3 normal = CubemapDirection(face, x, y, size);
                    Rgb sum;
                    float weight = 0.0f;
                    for (uint32_t sample = 0; sample < samples; ++sample)
                    {
                        const float xi1 = (static_cast<float>(sample) + 0.5f) /
                                          static_cast<float>(samples);
                        const float xi2 = RadicalInverse(sample);
                        const Vec3 halfVector = ImportanceSampleGgx(xi1, xi2, normal, roughness);
                        const float nDotH = std::max(0.0f, Dot(normal, halfVector));
                        const Vec3 light = Normalize(Sub(Mul(halfVector, 2.0f * nDotH), normal));
                        const float nDotL = std::max(0.0f, Dot(normal, light));
                        if (nDotL <= 0.0f) continue;
                        sum = Add(sum, Mul(ProceduralStudioRadiance(light), nDotL));
                        weight += nDotL;
                    }
                    mip.faces[face][static_cast<size_t>(y) * size + x] =
                        weight > 1e-6f ? Mul(sum, 1.0f / weight) : ProceduralStudioRadiance(normal);
                }
            }
        }
        bake.specularMips.push_back(std::move(mip));
    }

    bake.irradiance = ProjectIrradiance(bake.base);
    return bake;
}

float RoughnessToMip(float roughness, uint32_t mipCount)
{
    if (mipCount <= 1) return 0.0f;
    return std::clamp(roughness, 0.0f, 1.0f) * static_cast<float>(mipCount - 1);
}

Rgb EvaluateIrradiance(const ShCoefficients& coefficients, const Vec3& normal)
{
    float basis[9];
    ShBasis(Normalize(normal), basis);
    Rgb result;
    for (uint32_t i = 0; i < 9; ++i)
        result = Add(result, Mul(coefficients.values[i], basis[i]));
    return {std::max(0.0f, result.r), std::max(0.0f, result.g), std::max(0.0f, result.b)};
}

Rgb ReferenceIrradianceDivPi(const Vec3& normal, uint32_t sampleCount)
{
    const Vec3 n = Normalize(normal);
    const uint32_t count = std::max(1u, sampleCount);
    Rgb sum;
    for (uint32_t i = 0; i < count; ++i)
    {
        // Fibonacci sphere: deterministic, near-uniform coverage.
        const float z = 1.0f - (2.0f * static_cast<float>(i) + 1.0f) / static_cast<float>(count);
        const float radius = std::sqrt(std::max(0.0f, 1.0f - z * z));
        const float phi = static_cast<float>(i) * 2.39996322972865332f;
        const Vec3 direction{radius * std::cos(phi), radius * std::sin(phi), z};
        const float nDotL = std::max(0.0f, Dot(n, direction));
        if (nDotL <= 0.0f) continue;
        // ∫ L cosθ dω / π over the sphere with uniform samples of measure 4π.
        sum = Add(sum, Mul(ProceduralStudioRadiance(direction), nDotL * 4.0f / count));
    }
    return sum;
}

float Luminance(const Rgb& color)
{
    return 0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b;
}

std::array<float, 3> NeutralToneMapLinear(const std::array<float, 3>& linear, float exposure)
{
    float color[3] = {
        std::max(0.0f, linear[0] * exposure),
        std::max(0.0f, linear[1] * exposure),
        std::max(0.0f, linear[2] * exposure),
    };
    const float startCompression = 0.8f - 0.04f;
    const float desaturation = 0.15f;

    const float minimum = std::min(color[0], std::min(color[1], color[2]));
    const float offset = minimum < 0.08f ? minimum - 6.25f * minimum * minimum : 0.04f;
    for (float& channel : color) channel -= offset;

    const float peak = std::max(color[0], std::max(color[1], color[2]));
    if (peak < startCompression)
        return {color[0], color[1], color[2]};

    const float distance = 1.0f - startCompression;
    const float newPeak = 1.0f - distance * distance / (peak + distance - startCompression);
    const float scale = newPeak / std::max(peak, 1e-6f);
    for (float& channel : color) channel *= scale;

    const float grey = 1.0f - 1.0f / (desaturation * (peak - newPeak) + 1.0f);
    for (float& channel : color) channel = channel + (newPeak - channel) * grey;
    return {std::clamp(color[0], 0.0f, 1.0f), std::clamp(color[1], 0.0f, 1.0f),
            std::clamp(color[2], 0.0f, 1.0f)};
}

std::array<float, 3> NeutralToneMap(const std::array<float, 3>& linear, float exposure)
{
    auto mapped = NeutralToneMapLinear(linear, exposure);
    auto encode = [](float value) {
        return value <= 0.0031308f ? value * 12.92f
                                   : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
    };
    return {std::clamp(encode(mapped[0]), 0.0f, 1.0f),
            std::clamp(encode(mapped[1]), 0.0f, 1.0f),
            std::clamp(encode(mapped[2]), 0.0f, 1.0f)};
}

} // namespace studio_ibl
