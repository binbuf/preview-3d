// T06 presentation defaults: the format catalog is the single declaration of
// a family's view-state profile, the PLY point-cloud split is a pure function
// of the validated summary, and the broker's OBJ Y-up rule is exercised without
// launching a worker.

#include "model_core/FormatCatalog.h"
#include "model_core/SceneMetadataRules.h"
#include "InfoPanel.h"
#include "D3D12ViewerPath.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

using namespace model_core;

namespace {

const FormatDescriptor& Descriptor(FormatId id)
{
    const FormatDescriptor* descriptor = FindById(id);
    REQUIRE(descriptor != nullptr);
    return *descriptor;
}

} // namespace

TEST_CASE("Catalog declares deterministic per-format presentation profiles",
          "[presentation][format-catalog]")
{
    // PBR families.
    for (const FormatId id : { FormatId::Gltf, FormatId::Glb, FormatId::Fbx, FormatId::Obj,
                               FormatId::Usda, FormatId::Usdc, FormatId::Usdz, FormatId::Usd,
                               FormatId::Ply, FormatId::AsciiPly })
        CHECK(Descriptor(id).presentation == PresentationProfile::StudioPbr);

    // CAD/print families.
    for (const FormatId id : { FormatId::Stl, FormatId::AsciiStl, FormatId::ThreeMf,
                               FormatId::Step })
        CHECK(Descriptor(id).presentation == PresentationProfile::StudioCad);

    // OBJ is declared Y-up even though the file itself has no up-axis marker.
    CHECK(Descriptor(FormatId::Obj).defaultUpAxis == UpAxisId::Y);
    // No format silently claims a length unit it does not have.
    CHECK(Descriptor(FormatId::Obj).unitPolicy == UnitPolicy::Unspecified);
    CHECK(Descriptor(FormatId::Ply).unitPolicy == UnitPolicy::Unspecified);
}

TEST_CASE("Point-only PLY selects the point preset and meshes keep Studio PBR",
          "[presentation][ply]")
{
    const auto& ply = Descriptor(FormatId::Ply);
    CHECK(ResolvePresentationProfile(ply, { 1000, 0 }) == PresentationProfile::PointCloud);
    // A point cloud with any triangle is a mesh: never force the point preset.
    CHECK(ResolvePresentationProfile(ply, { 1000, 12 }) == PresentationProfile::StudioPbr);
    CHECK(ResolvePresentationProfile(ply, { 0, 12 }) == PresentationProfile::StudioPbr);
    CHECK(ResolvePresentationProfile(ply, { 0, 0 }) == PresentationProfile::StudioPbr);

    // The same summary must never change a non-PLY family's declared profile.
    CHECK(ResolvePresentationProfile(Descriptor(FormatId::Stl), { 1000, 0 })
          == PresentationProfile::StudioCad);
    CHECK(ResolvePresentationProfile(Descriptor(FormatId::Gltf), { 1000, 0 })
          == PresentationProfile::StudioPbr);
    CHECK(ResolvePresentationProfile(Descriptor(FormatId::Step), { 1000, 0 })
          == PresentationProfile::StudioCad);
}

TEST_CASE("OBJ accepts the Y-up default while keeping units unspecified",
          "[presentation][orientation]")
{
    CHECK(SceneUnitsAndUpAxisAllowed(SourceFormatId::Obj, UpAxisId::Y, 0.0));
    // A worker that still reports Unknown remains compatible.
    CHECK(SceneUnitsAndUpAxisAllowed(SourceFormatId::Obj, UpAxisId::Unknown, 0.0));
    // OBJ never authorizes a different axis or an invented metre factor.
    CHECK_FALSE(SceneUnitsAndUpAxisAllowed(SourceFormatId::Obj, UpAxisId::Z, 0.0));
    CHECK_FALSE(SceneUnitsAndUpAxisAllowed(SourceFormatId::Obj, UpAxisId::X, 0.0));
    CHECK_FALSE(SceneUnitsAndUpAxisAllowed(SourceFormatId::Obj, UpAxisId::Y, 1.0));

    // Neighbouring format rules stay exactly as strict as before.
    CHECK(SceneUnitsAndUpAxisAllowed(SourceFormatId::Gltf, UpAxisId::Y, 1.0));
    CHECK_FALSE(SceneUnitsAndUpAxisAllowed(SourceFormatId::Gltf, UpAxisId::Y, 0.0));
    CHECK(SceneUnitsAndUpAxisAllowed(SourceFormatId::Ply, UpAxisId::Unknown, 0.0));
    CHECK_FALSE(SceneUnitsAndUpAxisAllowed(SourceFormatId::Ply, UpAxisId::Y, 0.0));
    CHECK(SceneUnitsAndUpAxisAllowed(SourceFormatId::Fbx, UpAxisId::Y, 1.0));
    CHECK_FALSE(SceneUnitsAndUpAxisAllowed(SourceFormatId::Fbx, UpAxisId::Z, 1.0));
    CHECK(SceneUnitsAndUpAxisAllowed(SourceFormatId::Usda, UpAxisId::Z, 0.01));
    CHECK_FALSE(SceneUnitsAndUpAxisAllowed(SourceFormatId::Usda, UpAxisId::Unknown, 0.01));
    CHECK(SceneUnitsAndUpAxisAllowed(SourceFormatId::Step, UpAxisId::Unknown, 0.001));
    CHECK_FALSE(SceneUnitsAndUpAxisAllowed(SourceFormatId::Step, UpAxisId::Y, 0.001));
    CHECK_FALSE(SceneUnitsAndUpAxisAllowed(SourceFormatId::Step, UpAxisId::Unknown, 0.0));

    // T08: IGES normalizes geometry to the OCCT system millimetre, so its
    // post-transfer factor must be positive; BREP carries no unit at all and
    // must report the explicit zero rather than a guessed scale.
    CHECK(SceneUnitsAndUpAxisAllowed(SourceFormatId::Iges, UpAxisId::Unknown, 0.001));
    CHECK_FALSE(SceneUnitsAndUpAxisAllowed(SourceFormatId::Iges, UpAxisId::Y, 0.001));
    CHECK_FALSE(SceneUnitsAndUpAxisAllowed(SourceFormatId::Iges, UpAxisId::Unknown, 0.0));
    CHECK(SceneUnitsAndUpAxisAllowed(SourceFormatId::Brep, UpAxisId::Unknown, 0.0));
    CHECK_FALSE(SceneUnitsAndUpAxisAllowed(SourceFormatId::Brep, UpAxisId::Z, 0.0));
    CHECK_FALSE(SceneUnitsAndUpAxisAllowed(SourceFormatId::Brep, UpAxisId::Unknown, 0.001));

    // T10 compatibility formats report the exporter-family up axis and never
    // invent a length unit.
    CHECK(SceneUnitsAndUpAxisAllowed(SourceFormatId::Dae, UpAxisId::Y, 0.0));
    CHECK(SceneUnitsAndUpAxisAllowed(SourceFormatId::ThreeDs, UpAxisId::Z, 0.0));
    CHECK(SceneUnitsAndUpAxisAllowed(SourceFormatId::Off, UpAxisId::Unknown, 0.0));
    CHECK_FALSE(SceneUnitsAndUpAxisAllowed(SourceFormatId::Dae, UpAxisId::Y, 1.0));
    CHECK_FALSE(SceneUnitsAndUpAxisAllowed(SourceFormatId::Amf, UpAxisId::Y, 0.0));
}

TEST_CASE("Info panel reports the active preset and source orientation",
          "[presentation][info-panel]")
{
    ModelData metadata;
    metadata.source.format = model_core::SourceFormatId::Ply;
    metadata.source.upAxis = model_core::UpAxisId::Unknown;
    metadata.source.metersPerUnit = 0.0;
    metadata.pointCount = 128;
    metadata.triangleCount = 0;

    // No explicit preset yet: the panel resolves the catalog recommendation.
    auto sections = BuildInfoPanelSections(metadata, false);
    REQUIRE_FALSE(sections.empty());
    CHECK(sections.back().title == L"Presentation");
    CHECK(sections.back().rows[0].value == L"Point cloud");
    CHECK(sections.back().rows[1].value == L"Unknown");
    CHECK(sections.back().rows[2].value == L"Unspecified");

    // A mesh PLY keeps the PBR default even with prominent point data.
    metadata.triangleCount = 4;
    sections = BuildInfoPanelSections(metadata, false);
    CHECK(sections.back().rows[0].value == L"Studio PBR");

    // An already-applied CAD preset wins over the catalog recommendation.
    metadata.source.format = model_core::SourceFormatId::Stl;
    sections = BuildInfoPanelSections(metadata, false, GroundAxis::Automatic,
        model_core::PresentationProfile::StudioCad);
    CHECK(sections.back().rows[0].value == L"Studio CAD");
}

TEST_CASE("Studio PBR uses ORM R as ambient occlusion without darkening direct specular",
          "[texture-ao]")
{
    const char* source = D3D12ViewerPath::TexturedPixelShaderSource();
    REQUIRE(source != nullptr);
    const std::string shader(source);
    // ORM convention: R is AO, G roughness, B metallic.
    CHECK(shader.find("occlusion=OcclusionFactor(mr.r,gMaterialLobes0.z)") != std::string::npos);
    CHECK(shader.find("metallic*=mr.b") != std::string::npos);
    CHECK(shader.find("roughness*=mr.g") != std::string::npos);
    // Indirect diffuse/environment terms are scaled by AO. T14 replaced the
    // analytic view-space gradient with the world-stable SH/cubemap IBL.
    CHECK(shader.find("EvaluateStudioIrradiance(worldN)*envIntensity*diffuseScale*occlusion")
          != std::string::npos);
    CHECK(shader.find("SampleStudioSpecular(worldN,worldView,roughness)") != std::string::npos);
    CHECK(shader.find("*envIntensity*specularScale*occlusion") != std::string::npos);
    // The direct key light is not AO-scaled: its call has no `*occlusion`
    // argument, so metals keep clean direct specular.
    const auto direct = shader.find("EvaluateLight(n,viewDir,normalize(float3(.45f,-.55f,.70f))");
    REQUIRE(direct != std::string::npos);
    const auto lineEnd = shader.find(';', direct);
    REQUIRE(lineEnd != std::string::npos);
    CHECK(shader.substr(direct, lineEnd - direct).find("occlusion") == std::string::npos);
}
