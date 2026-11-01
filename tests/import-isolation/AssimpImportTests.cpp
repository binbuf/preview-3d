#include "SandboxTestSupport.h"
#include "import_broker/ImportSession.h"
#include "import_broker/SharedSection.h"
#include "model_core/MaterialPayload.h"
#include "model_core/PixelFormats.h"
#include "model_core/VertexLayouts.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

namespace {

using model_core::AssimpFormat;

import_broker::ImportSessionRequest Request(const wchar_t* fixture, AssimpFormat format,
                                            uint64_t generationId = 701)
{
    import_broker::ImportSessionRequest request;
    request.workerExePath = sandbox_test_support::WorkerExePath();
    request.sourcePath = std::wstring(PREVIEW3D_ASSIMP_FIXTURES_DIR) + fixture;
    request.format = import_broker::ImportFormat::Assimp;
    request.assimpFormat = format;
    request.generationId = generationId;
    request.sectionByteCapacity = import_broker::kImportSectionBytes;
    request.maxChunkCount = 4096;
    request.maxChunkBatchesPerGeneration = 64;
    request.maxChunksPerGeneration = 16384;
    request.maxSidecarRequestsPerGeneration = 64;
    request.maxSidecarFileBytes = 256ull * 1024 * 1024;
    return request;
}

struct SceneSummary {
    uint64_t triangles = 0;
    uint32_t materials = 0;
    uint32_t images = 0;
    uint32_t status = 0;
    uint32_t textureWarnings = 0;
    uint32_t optionalWarnings = 0;
    model_core::SourceFormatId format = model_core::SourceFormatId::Unknown;
    model_core::UpAxisId upAxis = model_core::UpAxisId::Unknown;
    double metersPerUnit = -1.0;
};

SceneSummary Summarize(const import_broker::ImportSessionResult& result)
{
    SceneSummary summary;
    for (const auto& chunk : result.chunks) {
        summary.format = chunk.scene.format;
        summary.upAxis = chunk.scene.upAxis;
        summary.metersPerUnit = chunk.scene.metersPerUnit;
        if (chunk.descriptor.topology == model_core::ChunkTopology::TriangleList) {
            summary.triangles += chunk.descriptor.indexCount / 3;
        } else if (chunk.descriptor.topology == model_core::ChunkTopology::Material) {
            ++summary.materials;
        } else if (chunk.descriptor.topology == model_core::ChunkTopology::Image) {
            ++summary.images;
        } else if (chunk.descriptor.topology == model_core::ChunkTopology::ImportStatus) {
            ++summary.status;
            REQUIRE(chunk.payload.size() == sizeof(model_core::ImportStatusPayload));
            model_core::ImportStatusPayload payload{};
            std::memcpy(&payload, chunk.payload.data(), sizeof(payload));
            summary.textureWarnings = payload.textureWarnings;
            summary.optionalWarnings = payload.optionalFeatureWarnings;
        }
    }
    return summary;
}

void CheckPositive(const wchar_t* fixture, AssimpFormat format,
                   model_core::SourceFormatId wire, model_core::UpAxisId upAxis,
                   uint64_t expectedTriangles, uint32_t expectedMaterials)
{
    CAPTURE(fixture);
    const auto result = import_broker::RunImportSession(Request(fixture, format));
    CAPTURE(result.stage, result.errorCode, result.errorPhase, result.chunks.size());
    REQUIRE(result.ok);
    const SceneSummary summary = Summarize(result);
    CHECK(summary.format == wire);
    CHECK(summary.upAxis == upAxis);
    CHECK(summary.metersPerUnit == 0.0);
    CHECK(summary.triangles == expectedTriangles);
    CHECK(summary.materials == expectedMaterials);
    for (const auto& chunk : result.chunks) {
        CHECK(chunk.descriptor.byteSize <= chunk.payload.size());
        if (chunk.descriptor.topology == model_core::ChunkTopology::TriangleList) {
            CHECK(chunk.descriptor.boundsState == model_core::BoundsState::Verified);
        }
    }
}

} // namespace

TEST_CASE("Assimp DAE 3DS OFF and AMF positive fixtures open", "[assimp]")
{
    CheckPositive(L"box.dae", AssimpFormat::Dae, model_core::SourceFormatId::Dae,
                  model_core::UpAxisId::Y, 12, 1);
    CheckPositive(L"box.3ds", AssimpFormat::ThreeDs, model_core::SourceFormatId::ThreeDs,
                  model_core::UpAxisId::Z, 12, 1);
    CheckPositive(L"box.off", AssimpFormat::Off, model_core::SourceFormatId::Off,
                  model_core::UpAxisId::Y, 12, 1);
    CheckPositive(L"box.amf", AssimpFormat::Amf, model_core::SourceFormatId::Amf,
                  model_core::UpAxisId::Z, 12, 1);
}

TEST_CASE("Assimp X3D DirectX and DXF fixtures open through the same adapter", "[assimp]")
{
    CheckPositive(L"box.x3d", AssimpFormat::X3d, model_core::SourceFormatId::X3d,
                  model_core::UpAxisId::Y, 12, 1);
    CheckPositive(L"box.x", AssimpFormat::DirectX, model_core::SourceFormatId::DirectX,
                  model_core::UpAxisId::Y, 12, 1);
    // Assimp's DXF support is limited to 3DFACE/polyline entities. The fixture
    // is a four-face tetrahedron shell, so only the typed triangle count is
    // asserted.
    const auto dxf = import_broker::RunImportSession(
        Request(L"triangle.dxf", AssimpFormat::Dxf, 702));
    CAPTURE(dxf.stage, dxf.errorCode, dxf.chunks.size());
    REQUIRE(dxf.ok);
    const SceneSummary summary = Summarize(dxf);
    CHECK(summary.format == model_core::SourceFormatId::Dxf);
    CHECK(summary.upAxis == model_core::UpAxisId::Z);
    CHECK(summary.triangles >= 4);
}

TEST_CASE("Assimp missing texture degrades with bounded warnings", "[assimp][recovery]")
{
    auto request = Request(L"missing-texture.dae", AssimpFormat::Dae, 703);
    const auto result = import_broker::RunImportSession(request);
    CAPTURE(result.stage, result.errorCode, result.chunks.size());
    REQUIRE(result.ok);
    const SceneSummary summary = Summarize(result);
    CHECK(summary.triangles == 12);
    CHECK(summary.materials == 1);
    // The missing map is replaced by the product fallback image and reported
    // once through the bounded status record.
    CHECK(summary.images == 1);
    CHECK(summary.textureWarnings >= 1);
    CHECK(summary.status == 1);
    CHECK(result.missingSidecarReferencesUtf8.size() == 1);
}

TEST_CASE("Assimp texture traversal is rejected before any open", "[assimp][security]")
{
    const auto result = import_broker::RunImportSession(
        Request(L"traversal-texture.dae", AssimpFormat::Dae, 704));
    CAPTURE(result.stage, result.errorCode, result.errorPhase);
    REQUIRE_FALSE(result.ok);
    CHECK(result.errorCode == model_core::ImportErrorCode::UnsafeReference);
    CHECK(result.errorPhase == model_core::ImportFailurePhase::Sidecars);
}

TEST_CASE("Assimp malformed and truncated fixtures fail typed", "[assimp][security]")
{
    const struct {
        const wchar_t* fixture;
        AssimpFormat format;
    } cases[] = {
        {L"truncated.dae", AssimpFormat::Dae},
        {L"truncated.3ds", AssimpFormat::ThreeDs},
        {L"malformed.off", AssimpFormat::Off},
        {L"malformed.amf", AssimpFormat::Amf},
    };
    for (const auto& item : cases) {
        CAPTURE(item.fixture);
        const auto result = import_broker::RunImportSession(
            Request(item.fixture, item.format, 705));
        CAPTURE(result.stage, result.errorCode, result.chunks.size());
        REQUIRE_FALSE(result.ok);
        CHECK(result.errorCode != model_core::ImportErrorCode::None);
        CHECK(result.stage != import_broker::ImportStage::Completed);
    }
}

TEST_CASE("Assimp tiny caps reject an otherwise valid scene typed", "[assimp][security]")
{
    auto request = Request(L"over-budget.off", AssimpFormat::Off, 706);
    request.assimpTinyLimitsForTesting = true;
    const auto result = import_broker::RunImportSession(request);
    CAPTURE(result.stage, result.errorCode, result.chunks.size());
    REQUIRE_FALSE(result.ok);
    CHECK(result.errorCode == model_core::ImportErrorCode::ResourceLimit);
}

TEST_CASE("Assimp sidecar requests are capped by the broker", "[assimp][security]")
{
    auto request = Request(L"missing-texture.dae", AssimpFormat::Dae, 707);
    request.maxSidecarRequestsPerGeneration = 0;
    const auto result = import_broker::RunImportSession(request);
    CAPTURE(result.stage, result.errorCode, result.chunks.size());
    REQUIRE_FALSE(result.ok);
    CHECK(result.errorCode == model_core::ImportErrorCode::ResourceLimit);
}
