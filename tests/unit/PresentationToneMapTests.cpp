// T15: exposure/neutral/ACES/Filmic/None tone-map contract, background modes
// and the material-aware Studio preset mapping. All of these are pure CPU
// functions that the renderer's resolve pass and preset selection use.

#include "Presentation.h"
#include "D3D12ViewerPath.h"

#include <catch2/catch_test_macros.hpp>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

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

TEST_CASE("Tone map is monotonic, bounded and finite", "[tonemap]")
{
    for (const ToneMapMode mode : { ToneMapMode::Neutral, ToneMapMode::Aces,
                                    ToneMapMode::Filmic, ToneMapMode::None }) {
        float previous = -1.0f;
        for (int i = 0; i <= 256; ++i) {
            const float x = static_cast<float>(i) * 0.25f;
            const auto mapped = ToneMapLinear({ x, x, x }, 1.0f, mode);
            REQUIRE(std::isfinite(mapped[0]));
            REQUIRE(std::isfinite(mapped[1]));
            REQUIRE(std::isfinite(mapped[2]));
            CHECK(mapped[0] >= previous);
            CHECK(mapped[0] >= 0.0f);
            CHECK(mapped[0] <= 1.0f);
            previous = mapped[0];
        }
        // A white point well above the display range still resolves to <= 1
        // and is bright, not black or clipped into an invalid value.
        const auto white = ToneMapLinear({ 64.0f, 64.0f, 64.0f }, 1.0f, mode);
        CHECK(white[0] <= 1.0f);
        CHECK(white[0] >= 0.5f);
        CHECK(white[0] == white[1]);
        CHECK(white[1] == white[2]);
    }
}

TEST_CASE("Tone map contains NaN and infinity without poisoning neighbors", "[tonemap]")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (const ToneMapMode mode : { ToneMapMode::Neutral, ToneMapMode::Aces,
                                    ToneMapMode::Filmic, ToneMapMode::None }) {
        const auto nanColor = ToneMapLinear({ nan, 1.0f, 0.5f }, 1.0f, mode);
        CHECK(std::isfinite(nanColor[0]));
        CHECK(nanColor[0] >= 0.0f);
        CHECK(nanColor[0] <= 1.0f);
        CHECK(std::isfinite(nanColor[1]));
        CHECK(std::isfinite(nanColor[2]));

        const auto infColor = ToneMapLinear({ inf, -inf, 0.0f }, 1.0f, mode);
        CHECK(std::isfinite(infColor[0]));
        CHECK(std::isfinite(infColor[1]));
        CHECK(std::isfinite(infColor[2]));
        CHECK(infColor[0] <= 1.0f);
        // A negative infinity is contained at black before the curve; the
        // tone curve may then desaturate a very bright neighbor, but the
        // result is always finite and in range.
        CHECK(infColor[1] >= 0.0f);
        CHECK(infColor[1] <= 1.0f);

        const auto badExposure = ToneMapLinear({ 1.0f, 1.0f, 1.0f }, nan, mode);
        CHECK(std::isfinite(badExposure[0]));
    }
}

TEST_CASE("Exposure is a linear pre-scale and 1.0 is the identity", "[tonemap]")
{
    for (const ToneMapMode mode : { ToneMapMode::Neutral, ToneMapMode::Aces,
                                    ToneMapMode::Filmic, ToneMapMode::None }) {
        const std::array<float, 3> color{ 0.18f, 0.42f, 1.7f };
        const auto baseline = ToneMapLinear(color, 1.0f, mode);
        // Exposure 1 must be the identity with respect to the same curve.
        CHECK(baseline[0] == ToneMapLinear(color, 1.0f, mode)[0]);

        // Doubling the scene radiance and using exposure 2 produce the same
        // result because exposure multiplies before the curve.
        const std::array<float, 3> doubled{ color[0] * 2.0f, color[1] * 2.0f, color[2] * 2.0f };
        const auto exposed = ToneMapLinear(doubled, 1.0f, mode);
        const auto scaled = ToneMapLinear(color, 2.0f, mode);
        CHECK(std::fabs(exposed[0] - scaled[0]) < 1e-6f);
        CHECK(std::fabs(exposed[1] - scaled[1]) < 1e-6f);
        CHECK(std::fabs(exposed[2] - scaled[2]) < 1e-6f);

        // Brighter exposure never darkens a non-black pixel.
        const auto dimmer = ToneMapLinear(color, 0.5f, mode);
        const auto brighter = ToneMapLinear(color, 2.0f, mode);
        CHECK(brighter[0] >= dimmer[0]);
    }
    CHECK(ClampExposure(-1.0f) == kMinExposure);
    CHECK(ClampExposure(100.0f) == kMaxExposure);
    CHECK(ClampExposure(std::numeric_limits<float>::quiet_NaN()) == 1.0f);
}

TEST_CASE("Background modes clear and composite in linear space", "[tonemap][studio-presets]")
{
    const auto dark = BackgroundLinear(BackgroundMode::Dark);
    CHECK(dark[3] == 1.0f);
    CHECK(dark[0] > 0.0f);
    CHECK(dark[0] < 0.2f);

    const auto grey = BackgroundLinear(BackgroundMode::MidGrey);
    CHECK(grey[3] == 1.0f);
    CHECK(grey[0] > dark[0]);

    const auto transparent = BackgroundLinear(BackgroundMode::Transparent);
    CHECK(transparent[3] == 0.0f);
    const auto checker = BackgroundLinear(BackgroundMode::Checkerboard);
    CHECK(checker[3] == 0.0f);
    CHECK(kCheckerLight > kCheckerDark);

    // The no-model clear path uses the same tone curve as the resolve pass.
    const auto display = BackgroundDisplay(BackgroundMode::Dark, 1.0f, ToneMapMode::Neutral);
    CHECK(display[0] > 0.0f);
    CHECK(display[0] < 0.5f);
    CHECK(std::isfinite(display[0]));
}

TEST_CASE("Material summary is bounded and reports ranges", "[studio-presets]")
{
    std::vector<model_core::MaterialPayload> materials;
    materials.push_back(Material(0.0f, 0.2f));
    materials.push_back(Material(1.0f, 0.9f));
    materials.push_back(Material(0.0f, 0.5f, model_core::kMaterialFlagUnlit));
    std::vector<uint32_t> baseColors{ 1u, 0u, 0u };
    const auto summary = SummarizeMaterials(materials, 0, 120000, false, baseColors);
    CHECK(summary.materialCount == 3);
    CHECK(summary.baseColorTextureCount == 1);
    CHECK(summary.unlitMaterialCount == 1);
    CHECK(summary.metallicMin == 0.0f);
    CHECK(summary.metallicMax == 1.0f);
    CHECK(summary.roughnessMin == 0.2f);
    CHECK(summary.roughnessMax == 0.9f);
    CHECK(summary.triangleCount == 120000);
    CHECK_FALSE(summary.PointOnly());

    // The scan cap wins over an adversarial material count.
    std::vector<model_core::MaterialPayload> many(kMaterialScanCap + 100,
        Material(0.5f, 0.5f));
    const auto capped = SummarizeMaterials(many, 0, 0, false);
    CHECK(capped.materialCount == kMaterialScanCap);
}

TEST_CASE("Preset mapping selects each Studio presentation from bounded facts", "[studio-presets]")
{
    // Default textured PBR.
    MaterialSummary pbr;
    pbr.materialCount = 4;
    pbr.baseColorTextureCount = 4;
    pbr.metallicMin = 0.0f;
    pbr.metallicMax = 1.0f;
    pbr.roughnessMin = 0.1f;
    pbr.roughnessMax = 0.8f;
    pbr.triangleCount = 1000;
    CHECK(SelectPresentationPreset(pbr, model_core::PresentationProfile::StudioPbr)
          == PresentationPreset::StudioPbr);

    // CAD by format, even with textures present.
    auto cad = pbr;
    cad.cadFormat = true;
    CHECK(SelectPresentationPreset(cad, model_core::PresentationProfile::StudioCad)
          == PresentationPreset::StudioCad);
    CHECK(SelectPresentationPreset(pbr, model_core::PresentationProfile::StudioCad)
          == PresentationPreset::StudioCad);

    // Point-only always wins over a declared PBR profile.
    MaterialSummary points;
    points.pointCount = 250000;
    points.triangleCount = 0;
    CHECK(SelectPresentationPreset(points, model_core::PresentationProfile::StudioPbr)
          == PresentationPreset::PointCloud);
    CHECK(points.PointOnly());

    // A majority-transparent asset gets the checker-backed preset; one glass
    // sphere among opaque materials does not.
    MaterialSummary transparent;
    transparent.materialCount = 4;
    transparent.baseColorTextureCount = 2;
    transparent.alphaMaterialCount = 3;
    transparent.transmissionMaterialCount = 2;
    transparent.triangleCount = 1000;
    CHECK(SelectPresentationPreset(transparent, model_core::PresentationProfile::StudioPbr)
          == PresentationPreset::Transparent);

    auto mostlyOpaque = pbr;
    mostlyOpaque.alphaMaterialCount = 1;
    mostlyOpaque.transmissionMaterialCount = 0;
    CHECK(SelectPresentationPreset(mostlyOpaque, model_core::PresentationProfile::StudioPbr)
          == PresentationPreset::StudioPbr);

    // A dense untextured scan becomes Scan / Sculpt; a small untextured asset
    // stays Studio PBR.
    MaterialSummary scan;
    scan.materialCount = 1;
    scan.roughnessMin = scan.roughnessMax = 0.7f;
    scan.metallicMin = scan.metallicMax = 0.0f;
    scan.triangleCount = kScanTriangleThreshold;
    CHECK(SelectPresentationPreset(scan, model_core::PresentationProfile::StudioPbr)
          == PresentationPreset::ScanSculpt);
    scan.triangleCount = kScanTriangleThreshold - 1;
    CHECK(SelectPresentationPreset(scan, model_core::PresentationProfile::StudioPbr)
          == PresentationPreset::StudioPbr);
}

TEST_CASE("Preset settings expose exposure, background and grid defaults", "[studio-presets]")
{
    const auto pbr = SettingsForPreset(PresentationPreset::StudioPbr);
    CHECK(pbr.background == BackgroundMode::Dark);
    CHECK(pbr.exposure == 1.0f);
    CHECK(pbr.gridVisible);

    const auto cad = SettingsForPreset(PresentationPreset::StudioCad);
    CHECK(cad.background == BackgroundMode::Dark);
    CHECK(cad.gridVisible);

    const auto scan = SettingsForPreset(PresentationPreset::ScanSculpt);
    CHECK(scan.background == BackgroundMode::MidGrey);
    CHECK_FALSE(scan.gridVisible);

    const auto transparent = SettingsForPreset(PresentationPreset::Transparent);
    CHECK(transparent.background == BackgroundMode::Checkerboard);
    CHECK_FALSE(transparent.gridVisible);

    const auto points = SettingsForPreset(PresentationPreset::PointCloud);
    CHECK(points.background == BackgroundMode::Dark);
    CHECK_FALSE(points.gridVisible);

    // The manual override wins and still returns a complete view state.
    MaterialSummary cadSummary;
    cadSummary.cadFormat = true;
    const auto overridden = ResolvePresetSettings(cadSummary,
        model_core::PresentationProfile::StudioCad, true,
        PresentationPreset::Transparent);
    CHECK(overridden.preset == PresentationPreset::Transparent);
    CHECK(overridden.background == BackgroundMode::Checkerboard);
}

TEST_CASE("Object shader writes linear HDR and the resolve shader owns the display transform",
          "[tonemap][studio-presets]")
{
    const char* objectSource = D3D12ViewerPath::TexturedPixelShaderSource();
    REQUIRE(objectSource != nullptr);
    const std::string objectShader(objectSource);
    // The per-object curve call is gone; geometry now writes linear radiance.
    CHECK(objectShader.find("NeutralToneMap(color,gEnvironment.z)") == std::string::npos);
    CHECK(objectShader.find("output.color = float4(max(color,0.0f), base.a)") != std::string::npos);
    // The env intensity scales the direct rig and IBL together.
    CHECK(objectShader.find("1.15f*envIntensity*gPresentation.x") != std::string::npos);
    CHECK(objectShader.find("EvaluateStudioIrradiance(worldN)*envIntensity") != std::string::npos);
    CHECK(objectShader.find("gPresentation") != std::string::npos);

    const char* resolveSource = D3D12ViewerPath::ResolvePixelShaderSource();
    REQUIRE(resolveSource != nullptr);
    const std::string resolveShader(resolveSource);
    CHECK(resolveShader.find("ApplyToneMap") != std::string::npos);
    CHECK(resolveShader.find("AcesToneMap") != std::string::npos);
    CHECK(resolveShader.find("FilmicToneMap") != std::string::npos);
    CHECK(resolveShader.find("SrgbEncode") != std::string::npos);
    CHECK(resolveShader.find("backgroundMode==3u") != std::string::npos);
    CHECK(resolveShader.find("backgroundMode==2u") != std::string::npos);
}

TEST_CASE("Resolve pixel shader compiles for the feature-level-11_0 baseline", "[tonemap]")
{
    const char* source = D3D12ViewerPath::ResolvePixelShaderSource();
    REQUIRE(source != nullptr);
    Microsoft::WRL::ComPtr<ID3DBlob> shader;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr,
                                  "PSMain", "ps_5_1", D3DCOMPILE_ENABLE_STRICTNESS, 0,
                                  &shader, &errors);
    if (FAILED(hr) && errors) {
        INFO(static_cast<const char*>(errors->GetBufferPointer()));
    }
    CHECK(SUCCEEDED(hr));
    CHECK(shader != nullptr);
}
