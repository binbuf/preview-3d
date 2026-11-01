#pragma once

// T32 scale-aware screen-space ambient occlusion, CPU reference.
//
// The GPU pass lives in D3D12ViewerPath.cpp. This module owns only the
// projection-independent parameters, the bounded target budget and the small
// compositing helpers the unit suite can pin without a device. Keeping the
// numbers here prevents the shader and the CPU evidence from drifting.
//
// AO is applied to the *indirect* (environment/IBL and compact-shader ambient)
// contribution only. Direct lights, emission, transparent reflection and the
// tone-mapped final image are never multiplied by the mask.

#include <cstdint>

namespace viewer_ao
{
// Review modes, mirroring the toolbar's Studio/Clay/Directional order.
enum class ReviewMode : uint32_t
{
    Studio = 0,
    Clay = 1,
    Directional = 2,
};

// A fixed, bounded kernel. The GPU shader uses the same count; it never grows
// with resolution or scene complexity.
inline constexpr uint32_t kSampleCount = 12;
inline constexpr uint32_t kMaxPixelRadius = 48;
// Spreads the normalized horizon sum into a visible cavity mask. It is a
// property of the mask, not an exposure change; the mode strength still
// scales it independently.
inline constexpr float kOcclusionGain = 2.2f;
// The bilateral edge filter is a fixed 5x5 window.
inline constexpr uint32_t kFilterRadius = 2;
// Conservative 64 KiB granularity for the public target estimate.
inline constexpr uint64_t kAllocationGranularity = 64ull * 1024ull;

struct Parameters
{
    // Applied to the normalized occlusion sum. Clay is deliberately stronger
    // and slightly wider because it is a geometry-review mode; Studio and
    // Directional stay restrained so material response is not hidden.
    float strength = 0.50f;
    // World radius as a fraction of the scene's fitted radius.
    float radiusFraction = 0.030f;
    // Bounds applied after the fraction, also relative to scene radius.
    float radiusMinFraction = 0.010f;
    float radiusMaxFraction = 0.120f;
    // Screen-space cap keeps work bounded at close zoom.
    float pixelRadiusMax = 40.0f;
    // Normal-facing bias for the horizon test.
    float bias = 0.030f;
    uint32_t sampleCount = kSampleCount;
};

// The defaults are per review mode, not per asset. An unknown mode falls back
// to Studio so a future mode cannot silently inherit Clay's stronger mask.
Parameters ParametersForMode(uint32_t mode) noexcept;

// Scene scale is the fitted world-space radius (half the transformed bounds
// diagonal). A zero/degenerate scale still returns a finite radius.
float WorldRadius(const Parameters& parameters, float sceneScale) noexcept;

// Converts the world radius into a bounded pixel radius using the measured
// world-space size of one pixel. Works for perspective, orthographic and the
// blended projection; no perspective-only assumption is made.
float PixelRadius(float worldRadius, float worldPerPixel, float maxPixels) noexcept;

// Converts a normalized [0,1] occlusion sum into the multiplier applied to
// indirect light. 1 means unoccluded.
float FactorFromOcclusion(float normalizedOcclusion, float strength) noexcept;

// Applies the AO multiplier to the indirect share only:
//   result = max(scene - indirect * (1 - ao), 0)
float CompositeIndirect(float scene, float indirect, float ao) noexcept;

// Authored ORM AO is already folded into the indirect term. Screen AO is
// composed with a bounded soft maximum rather than a second full multiply, so
// a strongly authored cavity cannot become near-black when screen AO is also
// present:
//   authored * (1 - strength * (1 - screen))
float CombineAuthoredOcclusion(float authored, float screen, float strength) noexcept;

bool TargetSupported(uint32_t width, uint32_t height) noexcept;

// One RGBA16F indirect target plus two R8 AO targets (raw and filtered),
// rounded up to the driver's allocation granularity.
uint64_t TargetBytesForExtent(uint32_t width, uint32_t height) noexcept;

} // namespace viewer_ao
