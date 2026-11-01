// T30: one world-space Directional sun for shading, gizmo and shadow input.
//
// The renderer, nav gizmo and ground projection all consume
// viewer_lighting::DirectionalSunDirection. These checks pin the documented
// Z-up convention, the world->camera transform used by the shader, the gizmo
// bearing and the shadow projection so a later lighting/shadow task cannot
// silently drift back to a camera-relative light.

#include "LightRig.h"
#include "GroundShadow.h"
#include "NavGizmo.h"
#include "D3D12ViewerPath.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <DirectXMath.h>

#include <cmath>
#include <string>

using Catch::Approx;
using namespace DirectX;

namespace {

XMVECTOR OrientationFromYawPitch(float yawDegrees, float pitchDegrees)
{
    const XMVECTOR worldUp = XMVectorSet(0, 0, 1, 0);
    XMVECTOR orientation = XMQuaternionRotationAxis(worldUp, XMConvertToRadians(yawDegrees));
    const XMVECTOR right = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), orientation);
    return XMQuaternionNormalize(XMQuaternionMultiply(
        orientation, XMQuaternionRotationAxis(right, XMConvertToRadians(pitchDegrees))));
}

void SunCpu(float az, float el, XMVECTOR& light)
{
    float direction[3]{};
    viewer_lighting::DirectionalSunDirection(az, el, direction);
    light = XMVectorSet(direction[0], direction[1], direction[2], 0.0f);
}

// Mirror of NavGizmo's yaw-only bearing frame, intentionally written
// independently so the test does not just call the production helper.
void HorizontalFrame(XMVECTOR orientation, XMVECTOR& right, XMVECTOR& forward)
{
    const XMVECTOR up = XMVectorSet(0, 0, 1, 0);
    const XMVECTOR q = XMQuaternionNormalize(orientation);
    const XMVECTOR cameraForward = XMVector3Rotate(XMVectorSet(0, 0, -1, 0), q);
    const XMVECTOR cameraRight = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), q);
    const XMVECTOR forwardFlat = cameraForward - up * XMVectorGetZ(cameraForward);
    const XMVECTOR rightFlat = cameraRight - up * XMVectorGetZ(cameraRight);
    if (XMVectorGetX(XMVector3LengthSq(forwardFlat)) >= XMVectorGetX(XMVector3LengthSq(rightFlat)))
    {
        forward = XMVector3Normalize(forwardFlat);
        right = XMVector3Normalize(XMVector3Cross(forward, up));
    }
    else
    {
        right = XMVector3Normalize(rightFlat);
        forward = XMVector3Normalize(XMVector3Cross(up, right));
    }
}

} // namespace

TEST_CASE("The Directional sun uses the documented Z-up azimuth/elevation convention",
          "[directional-light]")
{
    float direction[3]{};

    viewer_lighting::DirectionalSunDirection(0.0f, 0.0f, direction);
    CHECK(direction[0] == Approx(1.0f).margin(1e-5));
    CHECK(direction[1] == Approx(0.0f).margin(1e-5));
    CHECK(direction[2] == Approx(0.0f).margin(1e-5));

    viewer_lighting::DirectionalSunDirection(0.25f, 0.0f, direction);
    CHECK(direction[0] == Approx(0.0f).margin(1e-5));
    CHECK(direction[1] == Approx(1.0f).margin(1e-5));

    viewer_lighting::DirectionalSunDirection(0.5f, 0.0f, direction);
    CHECK(direction[0] == Approx(-1.0f).margin(1e-5));
    CHECK(direction[1] == Approx(0.0f).margin(1e-5));

    viewer_lighting::DirectionalSunDirection(0.75f, 0.0f, direction);
    CHECK(direction[1] == Approx(-1.0f).margin(1e-5));

    // Elevation raises the world +Z component; the zenith is +Z, independent
    // of azimuth. Every result is a finite unit vector.
    for (float elevation : {0.0f, 0.2f, 0.7f, 1.2f, viewer_lighting::kMaxSunElevation})
    {
        for (float azimuth : {0.0f, 0.13f, 0.37f, 0.62f, 0.99f})
        {
            viewer_lighting::DirectionalSunDirection(azimuth, elevation, direction);
            const float length = std::sqrt(direction[0] * direction[0]
                + direction[1] * direction[1] + direction[2] * direction[2]);
            CHECK(length == Approx(1.0f).margin(1e-5));
            CHECK(std::isfinite(direction[0]));
            CHECK(std::isfinite(direction[1]));
            CHECK(std::isfinite(direction[2]));
            CHECK(direction[2] == Approx(std::sin(elevation)).margin(1e-5));
        }
    }
    viewer_lighting::DirectionalSunDirection(0.4f, 100.0f, direction);
    CHECK(direction[2] == Approx(std::sin(viewer_lighting::kMaxSunElevation)).margin(1e-5));

    // Non-finite and wrapped inputs stay contained instead of propagating NaN.
    viewer_lighting::DirectionalSunDirection(std::nanf(""), 0.4f, direction);
    CHECK((std::isfinite(direction[0]) && std::isfinite(direction[1])
           && std::isfinite(direction[2])));
    viewer_lighting::DirectionalSunDirection(1.25f, 0.4f, direction);
    CHECK(direction[0] == Approx(std::cos(0.25f * 6.2831853f) * std::cos(0.4f)).margin(1e-4));
}

TEST_CASE("The shader world-to-camera transform is the inverse camera rotation",
          "[directional-light]")
{
    for (float yaw : {0.0f, 30.0f, 90.0f, 215.0f})
    {
        for (float pitch : {0.0f, 35.0f, 90.0f})
        {
            const XMVECTOR orientation = OrientationFromYawPitch(yaw, pitch);
            XMFLOAT4 quaternion{};
            XMStoreFloat4(&quaternion, orientation);
            const XMVECTOR lightWorld =
                XMVector3Normalize(XMVectorSet(0.35f, -0.62f, 0.70f, 0.0f));
            XMFLOAT3 world{};
            XMStoreFloat3(&world, lightWorld);

            float cameraDirection[3]{};
            viewer_lighting::CameraFromWorldDirection(&world.x, &quaternion.x,
                                                      cameraDirection);
            const XMVECTOR reconstructed =
                XMVector3Rotate(XMVectorSet(cameraDirection[0], cameraDirection[1],
                                            cameraDirection[2], 0.0f), orientation);
            CHECK(XMVectorGetX(reconstructed) == Approx(world.x).margin(1e-5));
            CHECK(XMVectorGetY(reconstructed) == Approx(world.y).margin(1e-5));
            CHECK(XMVectorGetZ(reconstructed) == Approx(world.z).margin(1e-5));
        }
    }
}

TEST_CASE("The gizmo sun bearing is the same world direction the shader receives",
          "[directional-light]")
{
    NavGizmo gizmo;
    gizmo.UpdateLayout(800, 600, 40, 40, 1.0f);
    const auto geometry = gizmo.ComputeDraw(XMQuaternionIdentity());
    REQUIRE(geometry.outerRadius > 0.0f);

    const struct { float yaw, pitch; } cameras[] = {
        {0.0f, 0.0f}, {0.0f, 90.0f}, {30.0f, 70.0f}, {215.0f, 35.0f},
    };
    for (const auto& camera : cameras)
    {
        const XMVECTOR orientation = OrientationFromYawPitch(camera.yaw, camera.pitch);
        XMFLOAT4 quaternion{};
        XMStoreFloat4(&quaternion, orientation);
        XMVECTOR right{}, forward{};
        HorizontalFrame(orientation, right, forward);

        for (float azimuth : {0.0f, 0.2f, 0.5f, 0.75f})
        {
            for (float elevation : {0.0f, 0.45f, 1.1f})
            {
                XMVECTOR lightWorld{};
                SunCpu(azimuth, elevation, lightWorld);
                const auto sun = gizmo.ComputeSun(orientation, azimuth, elevation);
                REQUIRE(sun.visible);

                const float horizontal = std::sqrt(
                    XMVectorGetX(lightWorld) * XMVectorGetX(lightWorld)
                    + XMVectorGetY(lightWorld) * XMVectorGetY(lightWorld));
                const XMVECTOR flat = horizontal > 1e-6f
                    ? XMVectorSet(XMVectorGetX(lightWorld) / horizontal,
                                  XMVectorGetY(lightWorld) / horizontal, 0.0f, 0.0f)
                    : XMVectorZero();
                const float radius = geometry.outerRadius * std::cos(
                    std::min(std::max(elevation, 0.0f), viewer_lighting::kMaxSunElevation));
                const float expectedX = XMVectorGetX(XMVector3Dot(flat, right)) * radius;
                const float expectedY = -XMVectorGetX(XMVector3Dot(flat, forward)) * radius;
                CHECK(sun.x == Approx(expectedX).margin(1e-4));
                CHECK(sun.y == Approx(expectedY).margin(1e-4));

                float cameraDirection[3]{};
                XMFLOAT3 world{};
                XMStoreFloat3(&world, lightWorld);
                viewer_lighting::CameraFromWorldDirection(&world.x, &quaternion.x,
                                                          cameraDirection);
                CHECK(sun.depth == Approx(cameraDirection[2]).margin(1e-5));
            }
        }
    }
}

TEST_CASE("Directional and Studio shadows consume the documented light directions",
          "[directional-light][ground-shadow]")
{
    using namespace viewer_ground_shadow;
    Bounds bounds{};
    bounds.minimum[0] = -1.0f; bounds.minimum[1] = -1.0f; bounds.minimum[2] = 0.0f;
    bounds.maximum[0] = 1.0f; bounds.maximum[1] = 1.0f; bounds.maximum[2] = 2.0f;
    const GroundPlane plane = PlaceGroundPlane(bounds);
    REQUIRE(plane.valid);

    // Front-lit sun from -Y: the footprint shifts away from the light (+Y).
    float frontSun[3]{};
    viewer_lighting::DirectionalSunDirection(0.75f, 0.45f, frontSun);
    const ShadowFootprint front = ProjectShadow(bounds, plane, frontSun);
    REQUIRE(front.valid);
    CHECK(front.center[1] > plane.center[1] + 0.05f);

    // The same azimuth from behind the model produces the opposite shift.
    float backSun[3]{};
    viewer_lighting::DirectionalSunDirection(0.25f, 0.45f, backSun);
    const ShadowFootprint back = ProjectShadow(bounds, plane, backSun);
    REQUIRE(back.valid);
    CHECK(back.center[1] < plane.center[1] - 0.05f);

    // Gold/steel and CAD relief still use the same analytic interface in the
    // non-Directional modes; kKeyLightDirection remains that stable input.
    const ShadowFootprint studio = ProjectShadow(bounds, plane, kKeyLightDirection);
    REQUIRE(studio.valid);
    CHECK(studio.center[0] < plane.center[0]);
    CHECK(studio.center[1] > plane.center[1]);
}

TEST_CASE("The Directional lobes, gizmo and shadow share one world sun in the shader",
          "[directional-light]")
{
    const char* source = D3D12ViewerPath::TexturedPixelShaderSource();
    REQUIRE(source != nullptr);
    const std::string shader(source);
    CHECK(shader.find("gSunDirection") != std::string::npos);
    CHECK(shader.find("float3 lightDir=normalize(gSunDirection.xyz);")
          != std::string::npos);
    // The old camera-relative azimuth/elevation branch must not survive in the
    // material shader. `gLighting.y` is only allowed as inert state now.
    CHECK(shader.find("float az=gLighting.y, el=clamp(gLighting.z") == std::string::npos);
    // The shader's shading vectors are already in the fixed world frame;
    // applying the camera quaternion here again would make the sun orbit.
    CHECK(shader.find("RotateCameraFromWorld") == std::string::npos);
    CHECK(shader.find("baseDirect=EvaluateLightSplit(n,viewDir,lightDir") != std::string::npos);
    CHECK(shader.find("coatDirect=EvaluateClearcoatLight(coatN,viewDir,lightDir") != std::string::npos);
}
