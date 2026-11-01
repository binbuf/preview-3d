#pragma once

// T16 ground contact shadow and ambient-occlusion reference math.
//
// The shipping path is the analytic "blob" slice described by the task: the
// model's verified bounds are projected onto a ground plane along a stable key
// direction, and a soft elliptical mask is composited into the linear HDR
// scene. There is no extra depth pass, so this module owns every piece of the
// placement/projection/bias math the GPU shader mirrors, plus the ORM AO
// formula. It has no Direct3D dependency beyond plain float arrays.

#include <model_core/MaterialPayload.h>

#include <cstdint>

namespace viewer_ground_shadow {

// Stable Studio key direction in the viewer's Z-up world (towards the light).
// It matches the Studio rig's design direction; the ground shadow is a
// presentation cue, not a physically tracked shadow-map light. Directional
// lighting passes its own world-space sun from `viewer_lighting::
// DirectionalSunDirection` instead, so the visible sun, lit face and shadow
// input share one direction. T31 must keep that interface.
constexpr float kKeyLightDirection[3] = { 0.45f, -0.55f, 0.70f };

// Ground sits just below the verified bounds; the grid sits further below so
// the two coplanar layers cannot z-fight.
constexpr float kGroundDepthBiasFraction = 0.0015f;
constexpr float kGridDepthBiasFraction = 0.012f;

constexpr float kDefaultSoftness = 0.50f;
constexpr float kDefaultOpacity = 0.78f;
// The analytic projection sees the whole box, including its top. A pure
// projection reads as a broad floor darkening rather than contact, so the
// footprint is biased back toward the model's base by this fraction. It stays
// a light-space projection (tilt still shifts and stretches it) but the dense
// core sits at the contact point.
constexpr float kContactBias = 0.15f;
// The soft rim extends beyond the model's base so the contact remains visible
// where the model itself covers the centre of the footprint.
constexpr float kContactMarginFraction = 0.60f;
// A grazing key would project an unbounded footprint; clamp its elevation and
// the resulting extent.
constexpr float kMinLightElevation = 0.08f;
constexpr float kMaxExtentInRadii = 6.0f;

// T31 geometry-derived contact shadow. A fitted orthographic light frustum
// renders the model's triangles into a bounded depth target; the ground layer
// samples that target with a comparison sampler. The bounds only size the
// frustum and the ground quad -- they are never the shadow silhouette.
constexpr uint32_t kShadowMapResolution = 2048;
constexpr uint32_t kShadowMapFallbackResolution = 1024;
constexpr float kShadowFrustumPadding = 0.08f;
// Depth bias in texels, converted to normalized light-space depth by
// FitShadowFrustum. The rasterizer applies its own slope-scaled bias.
constexpr float kShadowDepthBiasTexels = 2.0f;
// Approximate transmission attenuation. Clear glass is not an opaque caster:
// the depth pass still records it (so it can occlude other casters), but the
// resolved shadow contribution is scaled into [floor, 1]. This is a bounded
// approximation, not caustics or volume absorption.
constexpr float kShadowTransmissionScale = 0.60f;
constexpr float kShadowTransmissionFloor = 0.40f;

// How a triangle participates in the shadow depth pass. BLEND without the
// transmissive flag is a coverage/decal surface and casts no hard silhouette;
// MASK cutouts keep their authored alpha test. A double-sided or mirrored mesh
// is still a valid caster because the pass culls nothing.
enum class ShadowCasterKind : uint32_t
{
    None = 0,
    Opaque = 1,
    Masked = 2,
    Transmissive = 3,
};

struct ShadowFrustum
{
    bool valid = false;
    // Row-major world -> light clip matrix (DirectX row-vector convention).
    float matrix[16]{};
    float extent[2]{}; // light-space width/height covered by the map
    float nearZ = 0.0f;
    float farZ = 1.0f;
    float texelWorld = 0.0f;
    float depthBias = 0.0f; // normalized depth units
    uint32_t resolution = 0;
};

// Root constants mirrored by the contact-shadow ground pixel shader.
struct ShadowConstants
{
    float ground[4]{};     // center.xy, plane z, half size (camera relative)
    float parameters[4]{}; // opacity, pcf radius in texels, enabled, unused
    float texel[4]{};      // 1/width, 1/height, depth bias, unused
    float matrix[16]{};    // camera-relative -> light clip
};

struct Bounds
{
    float minimum[3]{};
    float maximum[3]{};
};

struct GroundPlane
{
    bool valid = false;
    float center[3]{}; // world-space plane center
    float radius = 0.0f; // half the bounds diagonal
    float z = 0.0f;
    float depthBias = 0.0f; // positive distance below bounds.minimum[2]
};

struct ShadowFootprint
{
    bool valid = false;
    float center[2]{}; // world-space ground-plane X/Y
    float radius[2]{}; // soft ellipse semi-axes
    float softness = kDefaultSoftness;
    float opacity = kDefaultOpacity;
};

// Root constants mirrored by the ground pixel shader.
struct Constants
{
    float ground[4]{};      // center.xy, plane z, half size (camera relative)
    float shadow[4]{};      // center.xy, radii.xy (camera relative)
    float parameters[4]{};  // opacity, softness, highlight strength, unused
    float color[4]{};       // display-space highlight rgb, center alpha
};

// T21a: the illuminated ground treatment is independent of the contact shadow.
// `highlightStrength` of 0 disables it while leaving the shadow untouched.
Constants BuildConstants(const GroundPlane& plane, const ShadowFootprint& footprint,
                         const float cameraTarget[3],
                         bool highlightEnabled = true,
                         const float highlightColor[3] = nullptr,
                         bool shadowEnabled = true) noexcept;

// Display-space highlight tint for a backdrop (subtle additive lift).
void HighlightColor(const float backdropDisplay[3], float out[3]) noexcept;

// Row-major world-from-model matrix applied to the eight bounds corners.
Bounds TransformBounds(const Bounds& bounds, const float worldFromModel[16]) noexcept;

// Places a plane just below bounds.minimum[2]. Invalid/non-finite bounds return
// an invalid plane instead of guessing.
GroundPlane PlaceGroundPlane(const Bounds& bounds) noexcept;

// Projects the eight bounds corners onto the plane along `lightDirection`
// (towards the light). The resulting ellipse is the soft contact footprint.
ShadowFootprint ProjectShadow(const Bounds& bounds, const GroundPlane& plane,
                              const float lightDirection[3],
                              float opacity = kDefaultOpacity) noexcept;

// Soft elliptical mask in [0,1]. Outside the ellipse it is exactly zero; the
// inner (1 - softness) fraction is fully opaque.
float ShadowMask(float x, float y, const ShadowFootprint& footprint) noexcept;

// Fits a square orthographic light frustum around the verified world bounds.
// The light-space centre is snapped to whole texels so tiny bound changes and
// camera motion cannot make the shadow crawl. Invalid bounds or a degenerate
// light direction return an invalid frustum instead of guessing.
ShadowFrustum FitShadowFrustum(const Bounds& worldBounds, const float lightDirection[3],
                               uint32_t resolution) noexcept;

// Rewrites a world -> light-clip matrix so it consumes camera-relative
// positions: (world - cameraTarget) * out == world * matrix.
void ShadowMatrixForCameraTarget(const float matrix[16], const float cameraTarget[3],
                                 float out[16]) noexcept;

// Ground quad constants for the geometry-derived path. The quad still spans a
// bounded region around the fitted footprint; pixels outside the shadow map
// resolve to lit.
ShadowConstants BuildShadowConstants(const GroundPlane& plane, const ShadowFootprint& footprint,
                                     const ShadowFrustum& frustum,
                                     const float cameraTarget[3],
                                     float opacity = kDefaultOpacity) noexcept;

// Classifies a triangle for the depth-only caster pass. `completeVertex` means
// the mesh can sample its authored base-colour alpha for a MASK cutout.
ShadowCasterKind ClassifyShadowCaster(uint32_t alphaMode, uint32_t materialFlags,
                                     bool completeVertex) noexcept;

// Bounded approximate transmission attenuation in [floor, 1].
float TransmissionShadowAttenuation(float transmission) noexcept;

// glTF occlusion strength: 1 + strength * (sample - 1). A missing map uses
// sample = 1, and an absent strength uses 1.0.
float OcclusionFactor(float sampledOcclusion, float strength) noexcept;

} // namespace viewer_ground_shadow
