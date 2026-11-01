#include "AntiAliasing.h"

#include <algorithm>
#include <cmath>

namespace viewer_aa {

float Luma(float red, float green, float blue) noexcept
{
    return 0.2126f * red + 0.7152f * green + 0.0722f * blue;
}

float DepthKey(float depth) noexcept
{
    return std::clamp(1.0f - depth, 0.0f, 1.0f);
}

bool IsDepthDiscontinuity(float centreDepth, float neighbourDepth,
                          float relativeThreshold) noexcept
{
    const float centre = DepthKey(centreDepth);
    const float neighbour = DepthKey(neighbourDepth);
    const float threshold = (std::max)(0.0005f, centre * relativeThreshold);
    return std::abs(centre - neighbour) > threshold;
}

bool IsCoverageDiscontinuity(float centreCoverage, float neighbourCoverage,
                             float threshold) noexcept
{
    return std::abs(centreCoverage - neighbourCoverage) > threshold;
}

float EdgeBlendWeight(float contrast, bool depthEdge, bool coverageEdge,
                      const Parameters& parameters) noexcept
{
    if (!parameters.enabled || !(contrast > parameters.lumaThreshold))
        return 0.0f;
    // saturate((contrast - threshold) / (1 - threshold)) keeps a barely visible
    // edge from receiving a full-strength blend.
    const float reach = std::clamp(
        (contrast - parameters.lumaThreshold) /
            (std::max)(1e-4f, 1.0f - parameters.lumaThreshold), 0.0f, 1.0f);
    const float weight = (depthEdge || coverageEdge)
        ? parameters.geometricWeight : parameters.textureWeight;
    return weight * reach;
}

Mode EffectiveMode(bool requested, bool depthAvailable) noexcept
{
    if (!requested) return Mode::Off;
    return depthAvailable ? Mode::DepthAwareFxaa : Mode::Unavailable;
}

} // namespace viewer_aa
