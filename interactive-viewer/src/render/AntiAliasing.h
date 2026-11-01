#pragma once

#include <cstdint>

// T33 edge anti-aliasing policy.
//
// The product default is a single-sample HDR scene target plus a resolve-pass
// spatial filter. It is depth-aware: neighbouring pixels that are not a
// geometry/coverage discontinuity only receive a deliberately small
// texture-detail blend, so authored lettering and engravings stay legible. The
// pick target is a separate R32_UINT MRT and is never sampled by this pass, and
// the D3D11On12/Direct2D UI overlay is recorded after the resolve, so neither
// instance ids nor text are averaged.
//
// Why not MSAA: the opaque material pass writes scene colour + pick id in one
// MRT set, and D3D12 requires every bound render target to use the same sample
// count. Resolving an R32_UINT id target would average ids, which is forbidden.
// A bounded 4x MSAA would therefore require splitting the pick pass and the OIT
// transparent targets, and it still would not filter specular shading aliasing.
// Bounded SSAA is retained as a deterministic validation reference in the
// render-quality lane (2x window, box reduced), not as the default path.
namespace viewer_aa {

enum class Mode : uint32_t {
    Off = 0,
    DepthAwareFxaa = 1,
    Unavailable = 2,
};

enum class FallbackReason : uint32_t {
    None = 0,
    ForcedOff = 1,          // Debug A/B control, not a product state
    Unsupported = 2,        // depth SRV or resolve pipeline unavailable
};

// One luma/depth/coverage query at the centre plus four axial neighbours, then
// two full composite taps along the detected edge. This is the bounded tap
// count the shader executes; it does not grow with viewport size.
constexpr uint32_t kTapCount = 7;
// The pass adds no render target; it reads the existing scene, depth, OIT and
// AO resources in the resolve.
constexpr uint64_t kAdditionalTargetBytes = 0;

struct Parameters {
    bool enabled = true;
    // Display-space luma contrast below which the neighbourhood is left alone.
    float lumaThreshold = 0.045f;
    // Relative `1 - depth` discontinuity that marks a geometry silhouette or
    // an occlusion gap. The depth key is linear for orthographic and monotonic
    // for perspective, so one relative rule covers both projections.
    float depthRelativeThreshold = 0.01f;
    // Blending weights are capped well below 0.5 so a one-pixel wire is
    // softened, not erased. The texture weight is intentionally low.
    float geometricWeight = 0.55f;
    float textureWeight = 0.12f;
    // Neighbour coverage (opaque alpha plus OIT revealage complement) that
    // marks a transparent silhouette. It lets a glass edge receive the same
    // geometric treatment even though transparent surfaces do not write depth.
    float coverageThreshold = 0.15f;
};

float Luma(float red, float green, float blue) noexcept;

// `depth` is the raw D32 sample in [0,1]. Convert it to a monotonic "distance
// key" so a near silhouette and a far background produce a real difference.
float DepthKey(float depth) noexcept;
bool IsDepthDiscontinuity(float centreDepth, float neighbourDepth,
                          float relativeThreshold) noexcept;
bool IsCoverageDiscontinuity(float centreCoverage, float neighbourCoverage,
                             float threshold) noexcept;

// Deterministic CPU mirror of the blend weight used by the resolve shader.
float EdgeBlendWeight(float contrast, bool depthEdge, bool coverageEdge,
                      const Parameters& parameters) noexcept;

// 0 off, 1 active, 2 unavailable. A requested-off debug lane is reported as
// Off with FallbackReason::ForcedOff by the caller.
Mode EffectiveMode(bool requested, bool depthAvailable) noexcept;

} // namespace viewer_aa
