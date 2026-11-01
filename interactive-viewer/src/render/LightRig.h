#pragma once

// T30: the single documented Directional-light transform.
//
// The viewer's world is right-handed and Z-up. This is the same frame the
// ground-axis correction produces: source XY/YZ/XZ up axes are rotated onto
// positive world Z before geometry, bounds, grid and ground shadow are drawn.
// A Directional sun is therefore a *world* direction:
//
//   azimuth    normalized 0..1 around +Z; 0 = +X, 0.25 = +Y, 0.5 = -X,
//              0.75 = -Y. It is the same value the HUD and gizmo expose as
//              0..360 degrees (azimuth * 360).
//   elevation  radians above the horizon, clamped to [0, kMaxSunElevation].
//   direction  unit vector towards the light:
//              (cos(az)cos(el), sin(az)cos(el), sin(el)).
//
// The vertex pipeline subtracts the camera origin (double precision) but
// keeps positions and normals in this same Z-up world frame; the camera
// rotation is applied by the view/projection matrix. The object shader, the
// nav gizmo and the ground shadow all consume this one world direction, so a
// camera orbit moves the view around a stationary sun and a sun move changes
// the lit face, highlight, gizmo bearing and shadow together. The gizmo's
// screen projection additionally uses the camera orientation for its depth
// and bearing; that is presentation only and never changes the world sun.
//
// Keep this header Direct3D-free: it is used by the renderer, the gizmo and
// the CPU unit tests.

#include <algorithm>
#include <cmath>

namespace viewer_lighting
{
inline constexpr float kMaxSunElevation = 1.4835f;

// Writes a unit vector towards the sun. Non-finite or out-of-range inputs are
// contained: the azimuth wraps into [0,1), the elevation is clamped, and a
// degenerate result falls back to the +Z zenith rather than producing NaNs.
inline void DirectionalSunDirection(float normalizedAzimuth, float elevationRadians,
                                    float out[3]) noexcept
{
    if (out == nullptr) return;
    float azimuth = std::isfinite(normalizedAzimuth) ? normalizedAzimuth : 0.0f;
    azimuth -= std::floor(azimuth);
    float elevation = std::isfinite(elevationRadians) ? elevationRadians : 0.0f;
    elevation = std::min(std::max(elevation, 0.0f), kMaxSunElevation);

    const float horizontal = std::cos(elevation);
    const float radians = azimuth * 6.28318530717958647692f;
    out[0] = std::cos(radians) * horizontal;
    out[1] = std::sin(radians) * horizontal;
    out[2] = std::sin(elevation);

    const float length = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (!std::isfinite(length) || length <= 1e-6f)
    {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 1.0f;
        return;
    }
    const float inverse = 1.0f / length;
    out[0] *= inverse;
    out[1] *= inverse;
    out[2] *= inverse;
}

// Rotates a world-space direction into camera space for the given
// world-from-camera orientation quaternion (x, y, z, w). This is the inverse
// of the shader's RotateWorldFromCamera and the CPU mirror used by tests and
// by the nav gizmo's screen projection.
inline void CameraFromWorldDirection(const float world[3], const float worldFromCamera[4],
                                     float out[3]) noexcept
{
    if (world == nullptr || worldFromCamera == nullptr || out == nullptr) return;
    const float qx = -worldFromCamera[0];
    const float qy = -worldFromCamera[1];
    const float qz = -worldFromCamera[2];
    const float qw = worldFromCamera[3];
    // v + 2 * q x (q x v + w * v), the standard unit-quaternion rotation.
    const float tx = qy * world[2] - qz * world[1] + qw * world[0];
    const float ty = qz * world[0] - qx * world[2] + qw * world[1];
    const float tz = qx * world[1] - qy * world[0] + qw * world[2];
    out[0] = world[0] + 2.0f * (qy * tz - qz * ty);
    out[1] = world[1] + 2.0f * (qz * tx - qx * tz);
    out[2] = world[2] + 2.0f * (qx * ty - qy * tx);
}

} // namespace viewer_lighting
