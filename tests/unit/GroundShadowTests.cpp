// T16: analytic ground contact shadow, ORM ambient occlusion and preset/toggle
// arbitration. The renderer uses these exact functions, so the CPU contract and
// the GPU shader cannot drift silently.

#include "GroundShadow.h"
#include "GroundAxis.h"
#include "Presentation.h"
#include "D3D12ViewerPath.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstring>
#include <string>

using Catch::Approx;
using namespace viewer_ground_shadow;

namespace {

Bounds Box(float minX, float minY, float minZ, float maxX, float maxY, float maxZ)
{
    Bounds bounds;
    bounds.minimum[0] = minX; bounds.minimum[1] = minY; bounds.minimum[2] = minZ;
    bounds.maximum[0] = maxX; bounds.maximum[1] = maxY; bounds.maximum[2] = maxZ;
    return bounds;
}

float RadiusOf(const Bounds& bounds)
{
    const float dx = bounds.maximum[0] - bounds.minimum[0];
    const float dy = bounds.maximum[1] - bounds.minimum[1];
    const float dz = bounds.maximum[2] - bounds.minimum[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f;
}

} // namespace

TEST_CASE("Ground plane sits below verified bounds and above the grid",
          "[ground-shadow]")
{
    const Bounds bounds = Box(-1.0f, -2.0f, -3.0f, 1.0f, 2.0f, 3.0f);
    const GroundPlane plane = PlaceGroundPlane(bounds);
    REQUIRE(plane.valid);
    CHECK(plane.z < bounds.minimum[2]);
    CHECK(plane.depthBias > 0.0f);
    CHECK(plane.depthBias == Approx(RadiusOf(bounds) * kGroundDepthBiasFraction));
    CHECK(plane.center[0] == Approx(0.0f));
    CHECK(plane.center[1] == Approx(0.0f));
    // The grid must remain a distinct, lower layer.
    const float gridZ = bounds.minimum[2] - RadiusOf(bounds) * kGridDepthBiasFraction;
    CHECK(plane.z > gridZ);

    // A Y-up source is rotated into the viewer's Z-up frame before grounding.
    DirectX::XMFLOAT4X4 transform{};
    DirectX::XMStoreFloat4x4(&transform,
        GroundAxisTransform(GroundAxis::Y, model_core::UpAxisId::Unknown, false, false));
    const Bounds yUp = Box(-1.0f, 0.0f, -2.0f, 1.0f, 4.0f, 2.0f);
    const Bounds grounded = TransformBounds(yUp, &transform._11);
    CHECK(grounded.minimum[2] == Approx(0.0f));
    CHECK(grounded.maximum[2] == Approx(4.0f));
    const GroundPlane groundedPlane = PlaceGroundPlane(grounded);
    REQUIRE(groundedPlane.valid);
    CHECK(groundedPlane.z < grounded.minimum[2]);

    // Every ground axis (and the inverted direction) places the plane below
    // that axis's transformed minimum, not the source minimum.
    for (const GroundAxis axis : { GroundAxis::X, GroundAxis::Y, GroundAxis::Z }) {
        for (const bool inverted : { false, true }) {
            DirectX::XMFLOAT4X4 axisTransform{};
            DirectX::XMStoreFloat4x4(&axisTransform,
                GroundAxisTransform(axis, model_core::UpAxisId::Unknown, false, inverted));
            const Bounds axisBounds = TransformBounds(yUp, &axisTransform._11);
            const GroundPlane axisPlane = PlaceGroundPlane(axisBounds);
            REQUIRE(axisPlane.valid);
            CHECK(axisPlane.z < axisBounds.minimum[2]);
            CHECK(axisPlane.center[0] >= axisBounds.minimum[0]);
            CHECK(axisPlane.center[0] <= axisBounds.maximum[0]);
            CHECK(axisPlane.center[1] >= axisBounds.minimum[1]);
            CHECK(axisPlane.center[1] <= axisBounds.maximum[1]);
        }
    }

    // Millimetre and kilometre scales stay finite and proportionally biased.
    for (const float scale : { 0.001f, 1.0f, 1000.0f }) {
        const Bounds scaled = Box(-scale, -scale, 0.0f, scale, scale, 2.0f * scale);
        const GroundPlane p = PlaceGroundPlane(scaled);
        REQUIRE(p.valid);
        CHECK(std::isfinite(p.z));
        CHECK(p.z < scaled.minimum[2]);
        CHECK(p.depthBias > 0.0f);
        CHECK(p.depthBias < 0.01f * scale);
    }

    // Degenerate input never guesses a plane.
    CHECK_FALSE(PlaceGroundPlane(Box(1, 0, 0, -1, 0, 0)).valid);
    CHECK_FALSE(PlaceGroundPlane(Box(0, 0, 0, std::nanf(""), 1, 1)).valid);
}

TEST_CASE("Analytic shadow projection follows the key light and stays bounded",
          "[ground-shadow]")
{
    const Bounds bounds = Box(-1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 2.0f);
    const GroundPlane plane = PlaceGroundPlane(bounds);
    REQUIRE(plane.valid);

    const float vertical[3] = { 0.0f, 0.0f, 1.0f };
    const ShadowFootprint down = ProjectShadow(bounds, plane, vertical);
    REQUIRE(down.valid);
    CHECK(down.center[0] == Approx(0.0f).margin(1e-5));
    CHECK(down.center[1] == Approx(0.0f).margin(1e-5));
    CHECK(down.radius[0] == Approx(1.0f + plane.radius * kContactMarginFraction).margin(1e-4));
    CHECK(down.radius[1] == Approx(1.0f + plane.radius * kContactMarginFraction).margin(1e-4));
    CHECK(ShadowMask(0.0f, 0.0f, down) == Approx(down.opacity));
    CHECK(ShadowMask(down.center[0] + down.radius[0] * 1.01f, 0.0f, down) == 0.0f);

    const float tilted[3] = { 0.45f, -0.55f, 0.70f };
    const ShadowFootprint angled = ProjectShadow(bounds, plane, tilted);
    REQUIRE(angled.valid);
    // The light comes from +X/-Y, so the footprint shifts to -X/+Y.
    CHECK(angled.center[0] < down.center[0]);
    CHECK(angled.center[1] > down.center[1]);
    CHECK(angled.radius[0] >= down.radius[0] - 1e-4f);
    CHECK(angled.radius[1] >= down.radius[1] - 1e-4f);
    // Soft edges: the inner core is opaque and the rim fades to zero.
    const float edgeX = angled.center[0] + angled.radius[0]
        * (1.0f - angled.softness * 0.5f);
    const float rim = ShadowMask(edgeX, angled.center[1], angled);
    CHECK(rim > 0.0f);
    CHECK(rim < angled.opacity);

    // A grazing light is clamped to a bounded footprint instead of stretching
    // to infinity, and a missing light never produces a footprint.
    const float grazing[3] = { 1.0f, 0.0f, 0.001f };
    const ShadowFootprint bounded = ProjectShadow(bounds, plane, grazing);
    REQUIRE(bounded.valid);
    CHECK(bounded.radius[0] <= plane.radius * kMaxExtentInRadii + 1e-3f);
    CHECK(bounded.radius[1] <= plane.radius * kMaxExtentInRadii + 1e-3f);
    CHECK_FALSE(ProjectShadow(bounds, plane, nullptr).valid);

    // Camera-relative root constants subtract the camera target.
    const float target[3] = { 4.0f, -2.0f, 0.5f };
    const Constants constants = BuildConstants(plane, angled, target);
    CHECK(constants.ground[0] == Approx(plane.center[0] - 4.0f));
    CHECK(constants.ground[1] == Approx(plane.center[1] + 2.0f));
    CHECK(constants.ground[2] == Approx(plane.z - 0.5f));
    CHECK(constants.shadow[0] == Approx(angled.center[0] - 4.0f));
    CHECK(constants.shadow[2] == Approx(angled.radius[0]));
    CHECK(constants.parameters[1] == Approx(angled.softness));
}

TEST_CASE("Ground highlight and contact shadow are independent constants",
          "[ground-shadow][appearance]")
{
    const Bounds bounds = Box(-1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 2.0f);
    const GroundPlane plane = PlaceGroundPlane(bounds);
    REQUIRE(plane.valid);
    const ShadowFootprint footprint = ProjectShadow(bounds, plane, kKeyLightDirection);
    REQUIRE(footprint.valid);
    const float target[3] = { 0.0f, 0.0f, 0.0f };
    const float backdrop[3] = { 0x1C / 255.0f, 0x1C / 255.0f, 0x1E / 255.0f };
    float tint[3]{};
    HighlightColor(backdrop, tint);
    CHECK(tint[0] > backdrop[0]);
    CHECK(tint[2] > backdrop[2]);

    const Constants on = BuildConstants(plane, footprint, target, true, tint);
    const Constants off = BuildConstants(plane, footprint, target, false, tint);
    const Constants noShadow = BuildConstants(plane, footprint, target, true, tint, false);
    // The highlight flag changes only the highlight channel, never the shadow.
    CHECK(on.parameters[2] == 1.0f);
    CHECK(off.parameters[2] == 0.0f);
    CHECK(on.parameters[0] == off.parameters[0]);
    CHECK(on.color[0] == Approx(tint[0]));
    // The shadow flag is independent too: highlight on, shadow off.
    CHECK(on.parameters[3] == 1.0f);
    CHECK(noShadow.parameters[3] == 0.0f);
    CHECK(noShadow.parameters[2] == 1.0f);
    CHECK(noShadow.parameters[0] == on.parameters[0]);
}

TEST_CASE("Authored ambient occlusion honours strength and leaves direct specular clean",
          "[ground-shadow]")
{
    CHECK(OcclusionFactor(1.0f, 1.0f) == Approx(1.0f));
    CHECK(OcclusionFactor(0.25f, 1.0f) == Approx(0.25f));
    CHECK(OcclusionFactor(0.25f, 0.5f) == Approx(0.625f));
    CHECK(OcclusionFactor(0.25f, 0.0f) == Approx(1.0f));
    CHECK(OcclusionFactor(0.0f, 1.0f) == Approx(0.0f));
    CHECK(OcclusionFactor(std::nanf(""), 1.0f) == Approx(1.0f));

    const char* objectSource = D3D12ViewerPath::TexturedPixelShaderSource();
    REQUIRE(objectSource != nullptr);
    const std::string shader(objectSource);
    // T22: MR red is not AO. Only the independent occlusion slot contributes.
    CHECK(shader.find("occlusion=OcclusionFactor(SampleSlot(gOcclusion,aoUv).r,gMaterialLobes0.z)")
          != std::string::npos);
    CHECK(shader.find("OcclusionFactor(mr.r") == std::string::npos);
    CHECK(shader.find("float OcclusionFactor(float sample,float strength)") != std::string::npos);
    // Direct key/fill/rim calls must not receive an AO factor; only indirect
    // diffuse and environment reflection are scaled.
    for (const char* direct : {
            "EvaluateLightSplit(n,viewDir,normalize(float3(.45f,-.55f,.70f))",
            "EvaluateLightSplit(n,viewDir,normalize(float3(-.70f,.25f,.38f))",
            "EvaluateLightSplit(n,viewDir,normalize(float3(.18f,.76f,.28f))" }) {
        const auto start = shader.find(direct);
        REQUIRE(start != std::string::npos);
        const auto end = shader.find(';', start);
        REQUIRE(end != std::string::npos);
        CHECK(shader.substr(start, end - start).find("occlusion") == std::string::npos);
    }
}

TEST_CASE("Ground shadow preset defaults and toggle arbitration are independent of the grid",
          "[ground-shadow]")
{
    using viewer_presentation::PresentationPreset;
    using viewer_presentation::SettingsForPreset;

    const auto cad = SettingsForPreset(PresentationPreset::StudioCad);
    CHECK(cad.shadowEnabled);
    CHECK(cad.gridVisible);

    const auto pbr = SettingsForPreset(PresentationPreset::StudioPbr);
    CHECK(pbr.shadowEnabled);
    CHECK(pbr.gridVisible);

    const auto scan = SettingsForPreset(PresentationPreset::ScanSculpt);
    CHECK(scan.shadowEnabled);
    CHECK_FALSE(scan.gridVisible);

    const auto transparent = SettingsForPreset(PresentationPreset::Transparent);
    CHECK_FALSE(transparent.shadowEnabled);
    CHECK_FALSE(transparent.gridVisible);

    const auto points = SettingsForPreset(PresentationPreset::PointCloud);
    CHECK_FALSE(points.shadowEnabled);
    CHECK_FALSE(points.gridVisible);

    // A user toggle wins over a later preset; without one the preset decides.
    CHECK(viewer_presentation::ResolveShadowVisible(false, false, true));
    CHECK_FALSE(viewer_presentation::ResolveShadowVisible(true, false, true));
    CHECK(viewer_presentation::ResolveShadowVisible(true, true, false));
    // Toggling the shadow never touches the preset's grid choice.
    auto toggled = cad;
    toggled.shadowEnabled = !toggled.shadowEnabled;
    CHECK(toggled.gridVisible == cad.gridVisible);
}

TEST_CASE("Ground-shadow shader is an independent compilable layer", "[ground-shadow]")
{
    const char* source = D3D12ViewerPath::GroundPixelShaderSource();
    REQUIRE(source != nullptr);
    const std::string shader(source);
    // No grid uniforms or grid draw dependency: the layer stands alone.
    CHECK(shader.find("Grid") == std::string::npos);
    CHECK(shader.find("gShadow") != std::string::npos);
    CHECK(shader.find("smoothstep") != std::string::npos);
    // T21a: the highlight flag and the shadow flag are separate channels.
    CHECK(shader.find("gShadowParams.z") != std::string::npos);
    CHECK(shader.find("gShadowParams.w") != std::string::npos);

    Microsoft::WRL::ComPtr<ID3DBlob> compiled;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr,
                                  "PSMain", "ps_5_1", D3DCOMPILE_ENABLE_STRICTNESS, 0,
                                  &compiled, &errors);
    if (FAILED(hr) && errors) {
        INFO(static_cast<const char*>(errors->GetBufferPointer()));
    }
    CHECK(SUCCEEDED(hr));
    CHECK(compiled != nullptr);
}
