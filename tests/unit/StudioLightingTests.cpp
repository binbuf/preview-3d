// T28: Studio environment calibration and automatic presentation selection.
//
// These checks are pure CPU. The same bake the renderer uploads is measured for
// neutrality, highlight breadth, dark-reflection floor, deterministic identity
// and bounded residency; the direct/IBL split comes from the same rig constants
// the shader receives. The GPU lane in tests/render-quality/studio_lighting.py
// proves the rendered pixels match this contract.

#include "StudioEnvironment.h"
#include "Presentation.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace studio_ibl;
using namespace viewer_presentation;

namespace {

model_core::MaterialPayload Material(float metallic, float roughness, uint32_t flags = 0,
                                     uint32_t alphaMode = uint32_t(model_core::AlphaModeId::Opaque),
                                     float alpha = 1.0f, float transmission = 0.0f)
{
    model_core::MaterialPayload material{};
    material.baseColorFactor[0] = 0.8f;
    material.baseColorFactor[1] = 0.8f;
    material.baseColorFactor[2] = 0.8f;
    material.baseColorFactor[3] = alpha;
    material.metallicFactor = metallic;
    material.roughnessFactor = roughness;
    material.flags = flags;
    material.alphaMode = alphaMode;
    material.transmissionFactor = transmission;
    return material;
}

} // namespace

TEST_CASE("Studio environment bake is deterministic, versioned and bounded",
          "[studio-lighting][studio-ibl]")
{
    const auto first = BakeStudioEnvironment();
    const auto second = BakeStudioEnvironment();
    REQUIRE(first.base.size == 128);
    REQUIRE(first.MipCount() == 6);
    CHECK(first.version == kStudioEnvironmentVersion);
    CHECK(first.variant == StudioEnvironmentVariant::ProductProcedural);
    CHECK(first.identityHash == second.identityHash);
    CHECK(first.residentBytes == second.residentBytes);
    // Cube (R32G32B32A32, 6 faces, mips) + the 64^2 RG32F DFG LUT. The whole
    // environment must stay well inside the existing budget.
    CHECK(first.residentBytes < 8ull * 1024 * 1024);
    CHECK(first.residentBytes > 1024 * 1024);
    for (uint32_t face = 0; face < 6; ++face) {
        REQUIRE(first.base.faces[face].size() == second.base.faces[face].size());
        CHECK(std::memcmp(first.base.faces[face].data(), second.base.faces[face].data(),
                          first.base.faces[face].size() * sizeof(Rgb)) == 0);
    }
}

TEST_CASE("Studio environment meets the measured radiometric calibration",
          "[studio-lighting][studio-ibl]")
{
    const auto bake = BakeStudioEnvironment();
    const auto metrics = MeasureStudioEnvironment(bake);

    CHECK(metrics.averageLuminance > 0.20f);
    CHECK(metrics.averageLuminance < 1.50f);
    CHECK(metrics.frontLuminance > 0.50f);
    // The shadowed hemisphere is lit but clearly darker than the key side.
    CHECK(metrics.backLuminance < metrics.frontLuminance * 0.60f);
    CHECK(metrics.backLuminance > 0.02f);
    // Reference-white diffuse response stays neutral.
    CHECK(metrics.neutralChroma <= kStudioNeutralChromaLimit);
    // Broad softboxes, not a point highlight: a measurable but bounded share of
    // the sphere exceeds four times the average radiance.
    CHECK(metrics.highlightFraction >= 0.005f);
    CHECK(metrics.highlightFraction <= 0.20f);
    // Dark paint/enamel still sees a useful reflection floor.
    CHECK(metrics.darkFloorLuminance >= 0.01f);
    CHECK(metrics.peakLuminance > 4.0f);

    // The world-stable IBL and camera-relative rig both contribute under the
    // shipping default camera direction, and neither lobe vanishes on any
    // principal axis. The exact share is camera/normal dependent; the report
    // records the measured probe values rather than pretending one constant
    // ratio fits every surface.
    const Vec3 defaultFacing{ 0.423293f, -0.83207f, 0.358444f };
    const auto split = EvaluateStudioDiffuseSplit(bake, defaultFacing);
    CHECK(split.iblLuminance > 0.05f);
    CHECK(split.directLuminance > 0.02f);
    CHECK(split.directShare >= kStudioDirectShareMin);
    CHECK(split.directShare <= kStudioDirectShareMax);
    for (const Vec3 normal : {Vec3{0.0f, 0.0f, 1.0f},
                              Vec3{0.5f, 0.0f, 0.866f},
                              Vec3{-0.5f, 0.0f, 0.866f},
                              Vec3{0.0f, 0.5f, 0.866f},
                              Vec3{0.0f, -0.5f, 0.866f}}) {
        const auto axis = EvaluateStudioDiffuseSplit(bake, normal);
        CHECK(axis.directShare >= kStudioDirectShareMin);
        CHECK(axis.directShare <= kStudioDirectShareMax);
        CHECK(axis.iblLuminance > 0.0f);
        CHECK(axis.directLuminance > 0.0f);
    }
    // A surface turned away from the rig has no direct term, but the world IBL
    // still supplies ambient light.
    const auto back = EvaluateStudioDiffuseSplit(bake, {0.0f, 0.0f, -1.0f});
    CHECK(back.directLuminance == 0.0f);
    CHECK(back.iblLuminance > 0.0f);
    CHECK(kStudioDirectKeyIntensity > kStudioDirectFillIntensity);
    CHECK(kStudioDirectFillIntensity > kStudioDirectRimIntensity);
    CHECK(kStudioDirectRimIntensity > 0.0f);
}

TEST_CASE("Product procedural bake beats the neutral reference on the T28 criteria",
          "[studio-lighting][studio-ibl]")
{
    const auto product = MeasureStudioEnvironment(BakeStudioEnvironment());
    BakeOptions referenceOptions;
    referenceOptions.variant = StudioEnvironmentVariant::NeutralReference;
    const auto reference = MeasureStudioEnvironment(BakeStudioEnvironment(referenceOptions));

    // Both are neutral enough; the shipping bake keeps the warmer/cooler panel
    // separation that makes form readable while staying inside the limit.
    CHECK(product.neutralChroma <= kStudioNeutralChromaLimit);
    CHECK(reference.neutralChroma <= kStudioNeutralChromaLimit);
    // Shipping bake has the broader readable highlight.
    CHECK(product.highlightFraction > reference.highlightFraction);
    // And the higher dark-reflection floor that preserves black materials.
    CHECK(product.darkFloorLuminance > reference.darkFloorLuminance);
    // A larger bake is not automatically better: 256 texels/face costs 4x the
    // residency and changes the measured response by less than the calibration
    // tolerance. The selected default is the 128-texel bake.
    BakeOptions high;
    high.faceSize = 256;
    high.specularMipCount = 8;
    high.specularSampleCount = 64;
    const auto highBake = BakeStudioEnvironment(high);
    const auto highMetrics = MeasureStudioEnvironment(highBake);
    CHECK(highBake.residentBytes > BakeStudioEnvironment().residentBytes * 3);
    CHECK(std::fabs(highMetrics.neutralChroma - product.neutralChroma) < 0.01f);
    CHECK(std::fabs(highMetrics.frontLuminance - product.frontLuminance)
          < product.frontLuminance * 0.02f);
}

TEST_CASE("Presentation contribution uses geometry and authored material presence",
          "[studio-lighting][studio-presets]")
{
    std::vector<model_core::MaterialPayload> materials;
    materials.push_back(Material(0.0f, 0.4f));
    materials.push_back(Material(0.0f, 0.05f, model_core::kMaterialFlagTransmissive,
                                 uint32_t(model_core::AlphaModeId::Opaque), 1.0f, 1.0f));
    materials.push_back(Material(1.0f, 0.2f));
    std::vector<uint64_t> triangles{ 98000, 1000, 1000 };
    std::vector<uint32_t> maps{ 1u, 0u, 1u };
    std::vector<uint32_t> baseColors{ 10u, 0u, 11u };
    auto summary = SummarizeMaterials(materials, 0, 100000, false, baseColors,
                                      triangles, maps);
    CHECK(summary.contributedTriangleCount == 100000);
    CHECK(summary.transparentTriangleCount == 1000);
    CHECK(summary.TransparentShare() < 0.02f);
    CHECK(SelectPresentationPreset(summary, model_core::PresentationProfile::StudioPbr)
          == PresentationPreset::StudioPbr);

    // A transparent majority of the contributed surface selects the checker.
    triangles = { 1000, 98000, 1000 };
    summary = SummarizeMaterials(materials, 0, 100000, false, baseColors, triangles, maps);
    CHECK(SelectPresentationPreset(summary, model_core::PresentationProfile::StudioPbr)
          == PresentationPreset::Transparent);

    // A dense authored, untextured mesh is not a scan: only the absence of a
    // material catalog makes an un-authored dense surface scan-like.
    std::vector<model_core::MaterialPayload> authored{ Material(0.0f, 0.7f) };
    std::vector<uint64_t> authoredTriangles{ 500000 };
    const auto pbr = SummarizeMaterials(authored, 0, 500000, false, {}, authoredTriangles);
    CHECK(SelectPresentationPreset(pbr, model_core::PresentationProfile::StudioPbr)
          == PresentationPreset::StudioPbr);
    const auto scan = SummarizeMaterials({}, 0, 500000, false, {});
    CHECK(SelectPresentationPreset(scan, model_core::PresentationProfile::StudioPbr)
          == PresentationPreset::ScanSculpt);

    // CAD and point-only still win over contribution.
    auto cad = pbr;
    cad.cadFormat = true;
    CHECK(SelectPresentationPreset(cad, model_core::PresentationProfile::StudioCad)
          == PresentationPreset::StudioCad);
    MaterialSummary points;
    points.pointCount = 1000;
    points.triangleCount = 0;
    CHECK(SelectPresentationPreset(points, model_core::PresentationProfile::StudioPbr)
          == PresentationPreset::PointCloud);
}

TEST_CASE("Manual presentation override survives preset resolution and keeps appearance",
          "[studio-lighting][studio-presets]")
{
    MaterialSummary transparent;
    transparent.materialCount = 4;
    transparent.contributedTriangleCount = 4000;
    transparent.transparentTriangleCount = 4000;
    CHECK(SelectPresentationPreset(transparent, model_core::PresentationProfile::StudioPbr)
          == PresentationPreset::Transparent);

    const auto manual = ResolvePresetSettings(transparent,
        model_core::PresentationProfile::StudioPbr, true, PresentationPreset::StudioCad);
    CHECK(manual.preset == PresentationPreset::StudioCad);
    // T21a: preset settings never own the global background/highlight choice;
    // the shipping viewer reads those from the persisted preference record.
    const auto settings = SettingsForPreset(PresentationPreset::StudioCad);
    CHECK(settings.exposure == 1.0f);
    CHECK(settings.environmentIntensity > 0.0f);
}
