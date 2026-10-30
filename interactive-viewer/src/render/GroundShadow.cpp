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

Constants BuildConstants(const GroundPlane& plane, const ShadowFootprint& footprint,
                         const float cameraTarget[3]) noexcept
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
    constants.color[0] = 0.160f;
    constants.color[1] = 0.170f;
    constants.color[2] = 0.190f;
    constants.color[3] = 0.90f;
    return constants;
}

} // namespace viewer_ground_shadow
