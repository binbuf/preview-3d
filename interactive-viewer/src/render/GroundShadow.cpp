#include "GroundShadow.h"

#include <algorithm>
#include <cmath>

namespace viewer_ground_shadow {
namespace {

bool Finite(float value) noexcept
{
    return std::isfinite(value);
}

float Length3(float x, float y, float z) noexcept
{
    return std::sqrt(x * x + y * y + z * z);
}

// Smooth 0..1 ramp with zero derivative at both ends (Hermite).
float SmoothStep(float edge0, float edge1, float value) noexcept
{
    if (edge1 <= edge0) return value < edge0 ? 0.0f : 1.0f;
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

bool ValidBounds(const Bounds& bounds) noexcept
{
    for (int i = 0; i < 3; ++i) {
        if (!Finite(bounds.minimum[i]) || !Finite(bounds.maximum[i])) return false;
        if (bounds.minimum[i] > bounds.maximum[i]) return false;
    }
    return true;
}

} // namespace

Bounds TransformBounds(const Bounds& bounds, const float worldFromModel[16]) noexcept
{
    Bounds result{};
    if (!ValidBounds(bounds) || worldFromModel == nullptr) return result;

    float minimum[3] = { 3.402823466e+38F, 3.402823466e+38F, 3.402823466e+38F };
    float maximum[3] = { -3.402823466e+38F, -3.402823466e+38F, -3.402823466e+38F };
    for (int corner = 0; corner < 8; ++corner) {
        const float x = bounds.minimum[0] + ((corner & 1) ? bounds.maximum[0] - bounds.minimum[0] : 0.0f);
        const float y = bounds.minimum[1] + ((corner & 2) ? bounds.maximum[1] - bounds.minimum[1] : 0.0f);
        const float z = bounds.minimum[2] + ((corner & 4) ? bounds.maximum[2] - bounds.minimum[2] : 0.0f);
        // Row-major, row-vector convention: out = v * matrix (DirectXMath).
        const float tx = x * worldFromModel[0] + y * worldFromModel[4] + z * worldFromModel[8]
            + worldFromModel[12];
        const float ty = x * worldFromModel[1] + y * worldFromModel[5] + z * worldFromModel[9]
            + worldFromModel[13];
        const float tz = x * worldFromModel[2] + y * worldFromModel[6] + z * worldFromModel[10]
            + worldFromModel[14];
        if (!Finite(tx) || !Finite(ty) || !Finite(tz)) return result;
        const float transformed[3] = { tx, ty, tz };
        for (int axis = 0; axis < 3; ++axis) {
            minimum[axis] = std::min(minimum[axis], transformed[axis]);
            maximum[axis] = std::max(maximum[axis], transformed[axis]);
        }
    }
    for (int axis = 0; axis < 3; ++axis) {
        result.minimum[axis] = minimum[axis];
        result.maximum[axis] = maximum[axis];
    }
    return result;
}

GroundPlane PlaceGroundPlane(const Bounds& bounds) noexcept
{
    GroundPlane plane;
    if (!ValidBounds(bounds)) return plane;

    const float extentX = bounds.maximum[0] - bounds.minimum[0];
    const float extentY = bounds.maximum[1] - bounds.minimum[1];
    const float extentZ = bounds.maximum[2] - bounds.minimum[2];
    plane.radius = std::max(0.001f, Length3(extentX, extentY, extentZ) * 0.5f);
    if (!Finite(plane.radius)) return GroundPlane{};

    plane.depthBias = plane.radius * kGroundDepthBiasFraction;
    plane.center[0] = (bounds.minimum[0] + bounds.maximum[0]) * 0.5f;
    plane.center[1] = (bounds.minimum[1] + bounds.maximum[1]) * 0.5f;
    plane.z = bounds.minimum[2] - plane.depthBias;
    plane.center[2] = plane.z;
    plane.valid = true;
    return plane;
}

ShadowFootprint ProjectShadow(const Bounds& bounds, const GroundPlane& plane,
                              const float lightDirection[3],
                              float opacity) noexcept
{
    ShadowFootprint footprint;
    if (!plane.valid || !ValidBounds(bounds) || lightDirection == nullptr) return footprint;

    float lightX = lightDirection[0];
    float lightY = lightDirection[1];
    float lightZ = std::max(lightDirection[2], kMinLightElevation);
    const float length = Length3(lightX, lightY, lightZ);
    if (!Finite(length) || length <= 0.0f) return footprint;
    lightX /= length;
    lightY /= length;
    lightZ /= length;

    float minimumX = 3.402823466e+38F, minimumY = 3.402823466e+38F;
    float maximumX = -3.402823466e+38F, maximumY = -3.402823466e+38F;
    for (int corner = 0; corner < 8; ++corner) {
        const float x = bounds.minimum[0] + ((corner & 1) ? bounds.maximum[0] - bounds.minimum[0] : 0.0f);
        const float y = bounds.minimum[1] + ((corner & 2) ? bounds.maximum[1] - bounds.minimum[1] : 0.0f);
        const float z = bounds.minimum[2] + ((corner & 4) ? bounds.maximum[2] - bounds.minimum[2] : 0.0f);
        const float travel = (z - plane.z) / lightZ;
        const float projectedX = x - lightX * travel;
        const float projectedY = y - lightY * travel;
        if (!Finite(projectedX) || !Finite(projectedY)) return footprint;
        minimumX = std::min(minimumX, projectedX);
        minimumY = std::min(minimumY, projectedY);
        maximumX = std::max(maximumX, projectedX);
        maximumY = std::max(maximumY, projectedY);
    }

    const float limit = std::max(plane.radius * kMaxExtentInRadii, 0.001f);
    const float projectedCenterX = (minimumX + maximumX) * 0.5f;
    const float projectedCenterY = (minimumY + maximumY) * 0.5f;
    // Bias the full projected box back toward the model's base footprint.
    float centerX = plane.center[0] + (projectedCenterX - plane.center[0]) * kContactBias;
    float centerY = plane.center[1] + (projectedCenterY - plane.center[1]) * kContactBias;
    centerX = std::clamp(centerX, plane.center[0] - limit, plane.center[0] + limit);
    centerY = std::clamp(centerY, plane.center[1] - limit, plane.center[1] + limit);
    const float baseRadiusX = (bounds.maximum[0] - bounds.minimum[0]) * 0.5f;
    const float baseRadiusY = (bounds.maximum[1] - bounds.minimum[1]) * 0.5f;
    const float margin = plane.radius * kContactMarginFraction;
    const float minimumRadius = plane.radius * 0.05f;
    const float projectedRadiusX = (maximumX - minimumX) * 0.5f;
    const float projectedRadiusY = (maximumY - minimumY) * 0.5f;
    const float radiusX = std::clamp(baseRadiusX + (projectedRadiusX - baseRadiusX) * kContactBias
            + margin, minimumRadius, limit);
    const float radiusY = std::clamp(baseRadiusY + (projectedRadiusY - baseRadiusY) * kContactBias
            + margin, minimumRadius, limit);

    footprint.center[0] = centerX;
    footprint.center[1] = centerY;
    footprint.radius[0] = radiusX;
    footprint.radius[1] = radiusY;
    footprint.softness = kDefaultSoftness;
    footprint.opacity = std::clamp(Finite(opacity) ? opacity : kDefaultOpacity, 0.0f, 1.0f);
    footprint.valid = true;
    return footprint;
}

float ShadowMask(float x, float y, const ShadowFootprint& footprint) noexcept
{
    if (!footprint.valid || footprint.radius[0] <= 0.0f || footprint.radius[1] <= 0.0f)
        return 0.0f;
    const float dx = (x - footprint.center[0]) / footprint.radius[0];
    const float dy = (y - footprint.center[1]) / footprint.radius[1];
    const float distance = std::sqrt(dx * dx + dy * dy);
    const float softness = std::clamp(footprint.softness, 0.001f, 1.0f);
    const float inner = 1.0f - softness;
    if (distance <= inner) return footprint.opacity;
    if (distance >= 1.0f) return 0.0f;
    return footprint.opacity * (1.0f - SmoothStep(inner, 1.0f, distance));
}

float OcclusionFactor(float sampledOcclusion, float strength) noexcept
{
    const float sample = std::clamp(Finite(sampledOcclusion) ? sampledOcclusion : 1.0f, 0.0f, 1.0f);
    const float amount = std::clamp(Finite(strength) ? strength : 1.0f, 0.0f, 1.0f);
    return std::clamp(1.0f + amount * (sample - 1.0f), 0.0f, 1.0f);
}

namespace {

float Dot3(const float a[3], const float b[3]) noexcept
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void Cross3(const float a[3], const float b[3], float out[3]) noexcept
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

void Normalize3(float v[3]) noexcept
{
    const float length = Length3(v[0], v[1], v[2]);
    if (length > 0.0f) {
        v[0] /= length;
        v[1] /= length;
        v[2] /= length;
    }
}

// Row-major, row-vector product: out = a * b.
void Multiply4x4(const float a[16], const float b[16], float out[16]) noexcept
{
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            float value = 0.0f;
            for (int k = 0; k < 4; ++k)
                value += a[row * 4 + k] * b[k * 4 + column];
            out[row * 4 + column] = value;
        }
    }
}

} // namespace

ShadowFrustum FitShadowFrustum(const Bounds& worldBounds, const float lightDirection[3],
                               uint32_t resolution) noexcept
{
    ShadowFrustum frustum;
    if (!ValidBounds(worldBounds) || lightDirection == nullptr || resolution == 0) return frustum;

    float light[3] = { lightDirection[0], lightDirection[1], lightDirection[2] };
    // A grazing sun would make the projection unbounded; clamp it the same way
    // the analytic fallback does, but keep the shadow's world bearing.
    light[2] = std::max(light[2], kMinLightElevation);
    Normalize3(light);
    if (!Finite(light[0]) || !Finite(light[1]) || !Finite(light[2])) return frustum;
    if (light[0] == 0.0f && light[1] == 0.0f && light[2] == 0.0f) return frustum;

    // Light camera looks along -light. Build a right-handed basis that stays
    // finite when the light is near vertical.
    const float zaxis[3] = { -light[0], -light[1], -light[2] };
    const float upCandidate[3] = { 0.0f, 0.0f, 1.0f };
    const float upVertical[3] = { 0.0f, 1.0f, 0.0f };
    const float* up = std::abs(zaxis[2]) > 0.9f ? upVertical : upCandidate;
    float xaxis[3]{};
    Cross3(up, zaxis, xaxis);
    Normalize3(xaxis);
    float yaxis[3]{};
    Cross3(zaxis, xaxis, yaxis);
    Normalize3(yaxis);
    if (!Finite(xaxis[0]) || !Finite(yaxis[0]) || !Finite(zaxis[0])) return frustum;

    float minimumX = 3.402823466e+38F, minimumY = 3.402823466e+38F;
    float minimumZ = 3.402823466e+38F;
    float maximumX = -3.402823466e+38F, maximumY = -3.402823466e+38F;
    float maximumZ = -3.402823466e+38F;
    for (int corner = 0; corner < 8; ++corner) {
        const float point[3] = {
            worldBounds.minimum[0] + ((corner & 1) ? worldBounds.maximum[0] - worldBounds.minimum[0] : 0.0f),
            worldBounds.minimum[1] + ((corner & 2) ? worldBounds.maximum[1] - worldBounds.minimum[1] : 0.0f),
            worldBounds.minimum[2] + ((corner & 4) ? worldBounds.maximum[2] - worldBounds.minimum[2] : 0.0f),
        };
        const float x = Dot3(point, xaxis);
        const float y = Dot3(point, yaxis);
        const float z = Dot3(point, zaxis);
        if (!Finite(x) || !Finite(y) || !Finite(z)) return ShadowFrustum{};
        minimumX = std::min(minimumX, x); maximumX = std::max(maximumX, x);
        minimumY = std::min(minimumY, y); maximumY = std::max(maximumY, y);
        minimumZ = std::min(minimumZ, z); maximumZ = std::max(maximumZ, z);
    }

    const float rawExtent = std::max(maximumX - minimumX, maximumY - minimumY);
    const float padding = std::max(rawExtent * kShadowFrustumPadding, 1e-6f);
    const float extent = std::max(rawExtent + padding * 2.0f, 1e-4f);
    const float texel = extent / static_cast<float>(resolution);
    if (!Finite(extent) || !Finite(texel) || texel <= 0.0f) return ShadowFrustum{};

    // Snap the covered square to whole texels. The extent itself is fixed by
    // the verified bounds, so a sub-texel bounds change (streamed LOD, a
    // different decomposition of the same assembly) reuses the same raster.
    const float centerX = std::round(((minimumX + maximumX) * 0.5f) / texel) * texel;
    const float centerY = std::round(((minimumY + maximumY) * 0.5f) / texel) * texel;
    const float depthPadding = std::max((maximumZ - minimumZ) * kShadowFrustumPadding, 1e-5f);
    const float nearZ = minimumZ - depthPadding;
    const float farZ = maximumZ + depthPadding;
    const float depthRange = std::max(farZ - nearZ, 1e-5f);

    const float inverseExtent = 1.0f / extent;
    const float rotation[16] = {
        xaxis[0], yaxis[0], zaxis[0], 0.0f,
        xaxis[1], yaxis[1], zaxis[1], 0.0f,
        xaxis[2], yaxis[2], zaxis[2], 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    const float projection[16] = {
        2.0f * inverseExtent, 0.0f, 0.0f, 0.0f,
        0.0f, 2.0f * inverseExtent, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f / depthRange, 0.0f,
        -centerX * 2.0f * inverseExtent, -centerY * 2.0f * inverseExtent,
        -nearZ / depthRange, 1.0f,
    };
    Multiply4x4(rotation, projection, frustum.matrix);
    frustum.extent[0] = extent;
    frustum.extent[1] = extent;
    frustum.nearZ = nearZ;
    frustum.farZ = farZ;
    frustum.texelWorld = texel;
    frustum.depthBias = std::clamp(kShadowDepthBiasTexels * texel / depthRange, 1e-4f, 0.01f);
    frustum.resolution = resolution;
    frustum.valid = true;
    return frustum;
}

void ShadowMatrixForCameraTarget(const float matrix[16], const float cameraTarget[3],
                                 float out[16]) noexcept
{
    if (matrix == nullptr || out == nullptr) return;
    const float target[3] = {
        cameraTarget ? cameraTarget[0] : 0.0f,
        cameraTarget ? cameraTarget[1] : 0.0f,
        cameraTarget ? cameraTarget[2] : 0.0f,
    };
    for (int column = 0; column < 4; ++column) {
        out[12 + column] = matrix[12 + column]
            + target[0] * matrix[column]
            + target[1] * matrix[4 + column]
            + target[2] * matrix[8 + column];
    }
    for (int index = 0; index < 12; ++index) out[index] = matrix[index];
}

ShadowConstants BuildShadowConstants(const GroundPlane& plane, const ShadowFootprint& footprint,
                                     const ShadowFrustum& frustum,
                                     const float cameraTarget[3],
                                     float opacity) noexcept
{
    ShadowConstants constants;
    if (!frustum.valid) return constants;
    const float targetX = cameraTarget ? cameraTarget[0] : 0.0f;
    const float targetY = cameraTarget ? cameraTarget[1] : 0.0f;
    const float targetZ = cameraTarget ? cameraTarget[2] : 0.0f;
    const float shadowOffsetX = footprint.center[0] - plane.center[0];
    const float shadowOffsetY = footprint.center[1] - plane.center[1];
    const float shadowReach = std::max(footprint.radius[0], footprint.radius[1])
        + std::sqrt(shadowOffsetX * shadowOffsetX + shadowOffsetY * shadowOffsetY);
    const GroundPlane planeCopy = plane;
    constants.ground[0] = planeCopy.center[0] - targetX;
    constants.ground[1] = planeCopy.center[1] - targetY;
    constants.ground[2] = planeCopy.z - targetZ;
    constants.ground[3] = std::max(planeCopy.radius * 4.0f, shadowReach * 1.5f);
    constants.parameters[0] = std::clamp(Finite(opacity) ? opacity : kDefaultOpacity, 0.0f, 1.0f);
    constants.parameters[1] = 1.0f; // 3x3 PCF radius
    constants.parameters[2] = 1.0f;
    constants.texel[0] = 1.0f / frustum.extent[0];
    constants.texel[1] = 1.0f / frustum.extent[1];
    constants.texel[2] = frustum.depthBias;
    const float target[3] = { targetX, targetY, targetZ };
    ShadowMatrixForCameraTarget(frustum.matrix, target, constants.matrix);
    return constants;
}

ShadowCasterKind ClassifyShadowCaster(uint32_t alphaMode, uint32_t materialFlags,
                                     bool completeVertex) noexcept
{
    const uint32_t blend = static_cast<uint32_t>(model_core::AlphaModeId::Blend);
    const uint32_t mask = static_cast<uint32_t>(model_core::AlphaModeId::Mask);
    // A BLEND surface is coverage, not a solid occluder. Treating its average
    // alpha as a depth would paint an unexplained solid silhouette.
    if (alphaMode == blend) return ShadowCasterKind::None;
    if ((materialFlags & model_core::kMaterialFlagTransmissive) != 0)
        return ShadowCasterKind::Transmissive;
    if (alphaMode == mask) {
        // Without the complete-vertex path there is no authored base-colour
        // alpha to test; guessing opaque would invent a solid cutout shadow.
        return completeVertex ? ShadowCasterKind::Masked : ShadowCasterKind::None;
    }
    return ShadowCasterKind::Opaque;
}

float TransmissionShadowAttenuation(float transmission) noexcept
{
    const float value = std::clamp(Finite(transmission) ? transmission : 0.0f, 0.0f, 1.0f);
    return std::clamp(1.0f - kShadowTransmissionScale * value,
                      kShadowTransmissionFloor, 1.0f);
}


void HighlightColor(const float backdropDisplay[3], float out[3]) noexcept
{
    constexpr float lift = 0.055f;
    for (int i = 0; i < 3; ++i) {
        const float base = backdropDisplay ? backdropDisplay[i] : 0.0f;
        out[i] = std::clamp((Finite(base) ? base : 0.0f) + lift, 0.0f, 1.0f);
    }
}

Constants BuildConstants(const GroundPlane& plane, const ShadowFootprint& footprint,
                         const float cameraTarget[3], bool highlightEnabled,
                         const float highlightColor[3], bool shadowEnabled) noexcept
{
    const float targetX = cameraTarget ? cameraTarget[0] : 0.0f;
    const float targetY = cameraTarget ? cameraTarget[1] : 0.0f;
    const float targetZ = cameraTarget ? cameraTarget[2] : 0.0f;
    Constants constants;
    constants.ground[0] = plane.center[0] - targetX;
    constants.ground[1] = plane.center[1] - targetY;
    constants.ground[2] = plane.z - targetZ;
    const float shadowOffsetX = footprint.center[0] - plane.center[0];
    const float shadowOffsetY = footprint.center[1] - plane.center[1];
    const float shadowReach = std::max(footprint.radius[0], footprint.radius[1])
        + std::sqrt(shadowOffsetX * shadowOffsetX + shadowOffsetY * shadowOffsetY);
    constants.ground[3] = std::max(plane.radius * 5.0f, shadowReach * 1.6f);
    constants.shadow[0] = footprint.center[0] - targetX;
    constants.shadow[1] = footprint.center[1] - targetY;
    constants.shadow[2] = footprint.radius[0];
    constants.shadow[3] = footprint.radius[1];
    constants.parameters[0] = footprint.opacity;
    constants.parameters[1] = footprint.softness;
    // T21a: independent highlight strength. The contact shadow keeps using
    // parameters[0]/[1] even when the highlight is off.
    constants.parameters[2] = highlightEnabled ? 1.0f : 0.0f;
    // T21a: the shadow toggle is independent of the highlight. parameters[3]
    // is 0 when the user turned shadows off, even if the ground quad is still
    // drawn for the highlight.
    constants.parameters[3] = shadowEnabled ? 1.0f : 0.0f;
    float tint[3] = { 0.235f, 0.240f, 0.270f };
    if (highlightColor) { tint[0] = highlightColor[0]; tint[1] = highlightColor[1]; tint[2] = highlightColor[2]; }
    constants.color[0] = std::clamp(Finite(tint[0]) ? tint[0] : 0.0f, 0.0f, 1.0f);
    constants.color[1] = std::clamp(Finite(tint[1]) ? tint[1] : 0.0f, 0.0f, 1.0f);
    constants.color[2] = std::clamp(Finite(tint[2]) ? tint[2] : 0.0f, 0.0f, 1.0f);
    // Peak coverage of the local highlight. The shader applies the radial fade.
    constants.color[3] = 0.55f;
    return constants;
}

} // namespace viewer_ground_shadow
