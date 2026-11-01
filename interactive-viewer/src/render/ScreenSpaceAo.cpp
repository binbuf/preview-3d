#include "ScreenSpaceAo.h"

#include <algorithm>
#include <cmath>

namespace viewer_ao
{
namespace
{
uint64_t RoundToGranularity(uint64_t bytes) noexcept
{
    return ((bytes + kAllocationGranularity - 1) / kAllocationGranularity)
        * kAllocationGranularity;
}
} // namespace

Parameters ParametersForMode(uint32_t mode) noexcept
{
    Parameters parameters;
    switch (static_cast<ReviewMode>(mode))
    {
    case ReviewMode::Clay:
        // Geometry review: cavities and contact matter more than material
        // response. Stronger and slightly wider, but still bounded.
        parameters.strength = 0.85f;
        parameters.radiusFraction = 0.140f;
        parameters.radiusMinFraction = 0.020f;
        parameters.radiusMaxFraction = 0.300f;
        parameters.pixelRadiusMax = 48.0f;
        parameters.bias = 0.025f;
        break;
    case ReviewMode::Directional:
        // One coherent sun already gives strong form; AO stays conservative so
        // it cannot be mistaken for a missing shadow.
        parameters.strength = 0.55f;
        parameters.radiusFraction = 0.090f;
        parameters.radiusMinFraction = 0.015f;
        parameters.radiusMaxFraction = 0.240f;
        parameters.pixelRadiusMax = 36.0f;
        parameters.bias = 0.030f;
        break;
    case ReviewMode::Studio:
    default:
        parameters.strength = 0.60f;
        parameters.radiusFraction = 0.100f;
        parameters.radiusMinFraction = 0.018f;
        parameters.radiusMaxFraction = 0.260f;
        parameters.pixelRadiusMax = 40.0f;
        parameters.bias = 0.030f;
        break;
    }
    return parameters;
}

float WorldRadius(const Parameters& parameters, float sceneScale) noexcept
{
    const float scale = std::max(sceneScale, 1e-4f);
    const float minimum = scale * std::max(parameters.radiusMinFraction, 0.0f);
    const float maximum = scale * std::max(parameters.radiusMaxFraction,
                                            parameters.radiusMinFraction);
    const float wanted = scale * std::max(parameters.radiusFraction, 0.0f);
    const float clamped = std::clamp(wanted, minimum, std::max(minimum, maximum));
    return std::max(clamped, 1e-5f);
}

float PixelRadius(float worldRadius, float worldPerPixel, float maxPixels) noexcept
{
    if (!(worldRadius > 0.0f) || !(worldPerPixel > 0.0f)) return 1.0f;
    const float pixels = worldRadius / worldPerPixel;
    if (!std::isfinite(pixels)) return 1.0f;
    return std::clamp(pixels, 1.0f, std::max(1.0f, maxPixels));
}

float FactorFromOcclusion(float normalizedOcclusion, float strength) noexcept
{
    // The gain spreads a typical 0.2-0.5 normalized horizon sum into a
    // visible but restrained cavity darkening; the final clamp keeps it a
    // mask, not a global exposure change.
    const float occlusion = std::clamp(normalizedOcclusion, 0.0f, 1.0f);
    const float scaled = occlusion * std::clamp(strength, 0.0f, 1.0f)
        * kOcclusionGain;
    return std::clamp(1.0f - scaled, 0.0f, 1.0f);
}

float CompositeIndirect(float scene, float indirect, float ao) noexcept
{
    const float mask = std::clamp(ao, 0.0f, 1.0f);
    const float ambient = std::max(indirect, 0.0f);
    return std::max(scene - ambient * (1.0f - mask), 0.0f);
}

float CombineAuthoredOcclusion(float authored, float screen, float strength) noexcept
{
    const float authoredMask = std::clamp(authored, 0.0f, 1.0f);
    const float screenMask = std::clamp(screen, 0.0f, 1.0f);
    const float weight = std::clamp(strength, 0.0f, 1.0f);
    return std::clamp(authoredMask * (1.0f - weight * (1.0f - screenMask)), 0.0f, 1.0f);
}

bool TargetSupported(uint32_t width, uint32_t height) noexcept
{
    return width != 0 && height != 0 && uint64_t(width) * uint64_t(height) <= 16ull * 1024 * 1024;
}

uint64_t TargetBytesForExtent(uint32_t width, uint32_t height) noexcept
{
    if (width == 0 || height == 0) return 0;
    const uint64_t pixels = uint64_t(width) * uint64_t(height);
    // RGBA16F indirect + R8 raw + R8 filtered.
    return RoundToGranularity(pixels * 8ull)
        + RoundToGranularity(pixels)
        + RoundToGranularity(pixels);
}

} // namespace viewer_ao
