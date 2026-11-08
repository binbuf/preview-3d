// T08 compatibility readers and the closed fallback policy.
//
// These cases run the real broker/worker path. The native family is attempted
// first; only the closed eligible failures may run exactly one Assimp
// compatibility attempt in the same sandbox. Security, containment and
// resource failures must never retry, and a forced compatibility reader must
// work without a native attempt.

#include "SandboxTestSupport.h"
#include "../../interactive-viewer/src/app/D3D12ImportBridge.h"
#include "import_broker/ImportSession.h"
#include "import_broker/ReaderFallback.h"
#include "import_broker/SharedSection.h"
#include "model_core/ReaderCatalog.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cwchar>
#include <string>

namespace {

using import_broker::ImportFormat;
using import_broker::ReaderFallbackRoute;
using model_core::ImportErrorCode;

import_broker::ImportSessionRequest Request(const wchar_t* fixture, ImportFormat format,
                                            uint64_t generationId)
{
    import_broker::ImportSessionRequest request{};
    request.workerExePath = sandbox_test_support::WorkerExePath();
    request.sourcePath = std::wstring(PREVIEW3D_ASSIMP_FIXTURES_DIR) + fixture;
    request.format = format;
    request.generationId = generationId;
    request.sectionByteCapacity = import_broker::kImportSectionBytes;
    request.maxChunkCount = 4096;
    request.maxChunkBatchesPerGeneration = 64;
    request.maxChunksPerGeneration = 16384;
    request.maxSidecarRequestsPerGeneration = 64;
    request.maxSidecarFileBytes = 256ull * 1024 * 1024;
    return request;
}

import_broker::ImportSessionRequest AssimpRequest(const wchar_t* fixture,
                                                   model_core::AssimpFormat format,
                                                   uint64_t generationId)
{
    auto request = Request(fixture, ImportFormat::Assimp, generationId);
    request.assimpFormat = format;
    return request;
}

import_broker::ImportSessionRequest FallbackRequest(const wchar_t* fixture, ImportFormat format,
                                                    ReaderFallbackRoute route,
                                                    uint64_t generationId)
{
    auto request = Request(fixture, format, generationId);
    request.readerFallback = route;
    return request;
}

struct GeometryCounts {
    uint64_t triangles = 0;
    uint64_t lines = 0;
    uint64_t points = 0;
};

GeometryCounts CountGeometry(const import_broker::ImportSessionResult& result)
{
    GeometryCounts counts;
    for (const auto& chunk : result.chunks) {
        switch (chunk.descriptor.topology) {
        case model_core::ChunkTopology::TriangleList:
            counts.triangles += chunk.descriptor.indexCount / 3;
            break;
        case model_core::ChunkTopology::LineList:
            counts.lines += chunk.descriptor.indexCount / 2;
            break;
        case model_core::ChunkTopology::PointList:
            counts.points += chunk.descriptor.vertexCount;
            break;
        default:
            break;
        }
    }
    return counts;
}

void CheckFallbackOpened(const wchar_t* fixture, ImportFormat format,
                         ReaderFallbackRoute route, ImportErrorCode nativeError,
                         const char* nativeReader, const char* compatibilityReader,
                         uint64_t generationId)
{
    CAPTURE(fixture);
    // The native route alone must fail with the expected eligible error.
    const auto native = import_broker::RunImportSession(Request(fixture, format, generationId));
    CAPTURE(native.stage, native.errorCode, native.chunks.size());
    REQUIRE_FALSE(native.ok);
    CHECK(native.errorCode == nativeError);

    const auto result = import_broker::RunImportSession(
        FallbackRequest(fixture, format, route, generationId + 1));
    CAPTURE(result.stage, result.errorCode, result.chunks.size());
    REQUIRE(result.ok);
    CHECK(result.usedCompatibilityReader);
    CHECK(result.compatibilityFallbackAttempted);
    CHECK(result.readerFallbackAttemptCount == 1);
    CHECK(result.nativeReaderName == nativeReader);
    CHECK(result.compatibilityReaderName == compatibilityReader);
    CHECK(result.finalReaderName == compatibilityReader);
    CHECK(result.readerFallbackDiagnostic.find(nativeReader) != std::string::npos);
    CHECK(result.readerFallbackDiagnostic.find(compatibilityReader) != std::string::npos);
    CHECK(result.readerFallbackDiagnostic.find("final reader") != std::string::npos);
    const GeometryCounts geometry = CountGeometry(result);
    CHECK(geometry.triangles + geometry.lines + geometry.points > 0);
}

} // namespace

TEST_CASE("the reader fallback policy is a closed eligible set", "[reader-fallback]")
{
    using E = ImportErrorCode;
    for (uint32_t value = 0; value <= uint32_t(E::TessellationFailed); ++value) {
        const auto code = static_cast<E>(value);
        const bool expected = code == E::UnsupportedFormat
            || code == E::UnsupportedRequiredFeature || code == E::EmptyGeometry;
        CAPTURE(value);
        CHECK(import_broker::IsReaderFallbackEligibleError(code) == expected);
    }

    const struct {
        ReaderFallbackRoute route;
        const char* nativeReader;
        const char* compatibilityReader;
        model_core::AssimpFormat hint;
    } routes[] = {
        { ReaderFallbackRoute::GltfAssimp, "gltf", "gltf-assimp",
          model_core::AssimpFormat::Gltf },
        { ReaderFallbackRoute::ObjAssimp, "obj", "obj-assimp",
          model_core::AssimpFormat::Obj },
        { ReaderFallbackRoute::FbxAssimp, "fbx", "fbx-assimp",
          model_core::AssimpFormat::Fbx },
        { ReaderFallbackRoute::ThreeMfAssimp, "3mf", "3mf-assimp",
          model_core::AssimpFormat::ThreeMf },
    };
    for (const auto& route : routes) {
        CAPTURE(route.nativeReader);
        CHECK(import_broker::IsKnownReaderFallbackRoute(route.route));
        const auto names = import_broker::ReaderFallbackNamesFor(route.route);
        REQUIRE(names.nativeReader != nullptr);
        REQUIRE(names.compatibilityReader != nullptr);
        CHECK(std::string(names.nativeReader) == route.nativeReader);
        CHECK(std::string(names.compatibilityReader) == route.compatibilityReader);
        CHECK(import_broker::AssimpFormatForReaderFallback(route.route) == route.hint);
        CHECK(model_core::IsKnownAssimpFormat(route.hint));

        // The reader table and the worker discriminator stay in sync.
        const auto* reader = model_core::FindReaderByName(route.compatibilityReader);
        REQUIRE(reader != nullptr);
        CHECK(reader->isCompatibility);
        model_core::AssimpFormat hint = model_core::AssimpFormat::Dae;
        REQUIRE(model_core::AssimpFormatForCompatibilityReader(*reader, hint));
        CHECK(hint == route.hint);
        CHECK(model_core::CompatibilityReaderForFamily(reader->family) == reader);
        const auto* native = model_core::FindReaderByName(route.nativeReader);
        REQUIRE(native != nullptr);
        CHECK_FALSE(native->isCompatibility);
        CHECK(reader->score < native->score);
    }
    CHECK_FALSE(import_broker::IsKnownReaderFallbackRoute(
        static_cast<ReaderFallbackRoute>(99)));

    // The app-layer bridge maps each shipped compatibility reader to its
    // closed SourceFormat and each native family to its closed fallback route.
    const struct {
        const char* reader;
        d3d12_import_bridge::SourceFormat source;
        d3d12_import_bridge::SourceFormat nativeSource;
        ReaderFallbackRoute route;
        ImportErrorCode eligible;
    } bridgeRoutes[] = {
        { "gltf-assimp", d3d12_import_bridge::SourceFormat::AssimpGltf,
          d3d12_import_bridge::SourceFormat::Glb, ReaderFallbackRoute::GltfAssimp,
          ImportErrorCode::UnsupportedRequiredFeature },
        { "obj-assimp", d3d12_import_bridge::SourceFormat::AssimpObj,
          d3d12_import_bridge::SourceFormat::Obj, ReaderFallbackRoute::ObjAssimp,
          ImportErrorCode::EmptyGeometry },
        { "fbx-assimp", d3d12_import_bridge::SourceFormat::AssimpFbx,
          d3d12_import_bridge::SourceFormat::Fbx, ReaderFallbackRoute::FbxAssimp,
          ImportErrorCode::EmptyGeometry },
        { "3mf-assimp", d3d12_import_bridge::SourceFormat::AssimpThreeMf,
          d3d12_import_bridge::SourceFormat::ThreeMf, ReaderFallbackRoute::ThreeMfAssimp,
          ImportErrorCode::UnsupportedRequiredFeature },
    };
    for (const auto& item : bridgeRoutes) {
        CAPTURE(item.reader);
        const auto* reader = model_core::FindReaderByName(item.reader);
        REQUIRE(reader != nullptr);
        CHECK(d3d12_import_bridge::SourceFormatForReader(*reader, nullptr) == item.source);
        CHECK(d3d12_import_bridge::ReaderFallbackForSource(item.nativeSource) == item.route);
        CHECK(d3d12_import_bridge::ReaderFallbackForSource(item.source)
              == ReaderFallbackRoute::None);
        CHECK(d3d12_import_bridge::CompatibilityReaderForFailedSource(
                  item.nativeSource, item.eligible) == reader);
        CHECK(d3d12_import_bridge::CompatibilityReaderForFailedSource(
                  item.nativeSource, ImportErrorCode::ResourceLimit) == nullptr);
        CHECK(d3d12_import_bridge::CompatibilityReaderForFailedSource(
                  item.nativeSource, ImportErrorCode::UnsafeReference) == nullptr);
        CHECK(d3d12_import_bridge::CompatibilityReaderForFailedSource(
                  item.source, item.eligible) == nullptr);
    }
    CHECK(d3d12_import_bridge::ReaderFallbackForSource(
              d3d12_import_bridge::SourceFormat::Usd) == ReaderFallbackRoute::None);
}

TEST_CASE("a native-rejected glTF opens through gltf-assimp", "[reader-fallback]")
{
    CheckFallbackOpened(L"required-extension.gltf", ImportFormat::Gltf,
                        ReaderFallbackRoute::GltfAssimp, ImportErrorCode::UnsupportedRequiredFeature,
                        "gltf", "gltf-assimp", 810);
}

TEST_CASE("a native-rejected 3MF opens through 3mf-assimp", "[reader-fallback]")
{
    CheckFallbackOpened(L"required-extension.3mf", ImportFormat::ThreeMf,
                        ReaderFallbackRoute::ThreeMfAssimp,
                        ImportErrorCode::UnsupportedRequiredFeature,
                        "3mf", "3mf-assimp", 812);
}

TEST_CASE("native-rejected OBJ and FBX open through their compatibility readers",
          "[reader-fallback]")
{
    CheckFallbackOpened(L"missing-vt.obj", ImportFormat::Obj,
                        ReaderFallbackRoute::ObjAssimp, ImportErrorCode::EmptyGeometry,
                        "obj", "obj-assimp", 814);
    CheckFallbackOpened(L"line-only.fbx", ImportFormat::Fbx,
                        ReaderFallbackRoute::FbxAssimp, ImportErrorCode::EmptyGeometry,
                        "fbx", "fbx-assimp", 816);
}

TEST_CASE("security failures never retry the compatibility reader", "[reader-fallback]")
{
    const auto result = import_broker::RunImportSession(
        FallbackRequest(L"unsafe-buffer.gltf", ImportFormat::Gltf,
                        ReaderFallbackRoute::GltfAssimp, 820));
    CAPTURE(result.stage, result.errorCode, result.chunks.size());
    REQUIRE_FALSE(result.ok);
    CHECK(result.errorCode == ImportErrorCode::UnsafeReference);
    CHECK_FALSE(result.compatibilityFallbackAttempted);
    CHECK_FALSE(result.usedCompatibilityReader);
    CHECK(result.readerFallbackAttemptCount == 0);
    CHECK(result.nativeReaderName == "gltf");
    CHECK(result.finalReaderName == "gltf");
    CHECK(result.readerFallbackDiagnostic.find("not attempted") != std::string::npos);
}

TEST_CASE("resource and malformed failures never retry the compatibility reader",
          "[reader-fallback]")
{
    SECTION("resource limit") {
        const auto result = import_broker::RunImportSession(
            FallbackRequest(L"resource-dag.gltf", ImportFormat::Gltf,
                            ReaderFallbackRoute::GltfAssimp, 822));
        CAPTURE(result.stage, result.errorCode);
        REQUIRE_FALSE(result.ok);
        CHECK(result.errorCode == ImportErrorCode::ResourceLimit);
        CHECK_FALSE(result.compatibilityFallbackAttempted);
        CHECK(result.readerFallbackAttemptCount == 0);
        CHECK(result.finalReaderName == "gltf");
    }
    SECTION("malformed data") {
        const auto result = import_broker::RunImportSession(
            FallbackRequest(L"malformed.gltf", ImportFormat::Gltf,
                            ReaderFallbackRoute::GltfAssimp, 823));
        CAPTURE(result.stage, result.errorCode);
        REQUIRE_FALSE(result.ok);
        CHECK(result.errorCode == ImportErrorCode::MalformedData);
        CHECK_FALSE(result.compatibilityFallbackAttempted);
        CHECK(result.readerFallbackAttemptCount == 0);
        CHECK(result.finalReaderName == "gltf");
    }
}

TEST_CASE("only one compatibility attempt is made per generation", "[reader-fallback]")
{
    // The native failure is eligible (required extension) and the compatibility
    // attempt fails too (its external buffer is missing). The broker must not
    // try a second reader, and the final reader is still the compatibility one.
    const auto result = import_broker::RunImportSession(
        FallbackRequest(L"required-extension-missing-buffer.gltf", ImportFormat::Gltf,
                        ReaderFallbackRoute::GltfAssimp, 824));
    CAPTURE(result.stage, result.errorCode, result.chunks.size());
    REQUIRE_FALSE(result.ok);
    CHECK(result.compatibilityFallbackAttempted);
    CHECK_FALSE(result.usedCompatibilityReader);
    CHECK(result.readerFallbackAttemptCount == 1);
    CHECK(result.nativeReaderName == "gltf");
    CHECK(result.finalReaderName == "gltf-assimp");
    CHECK(result.readerFallbackDiagnostic.find("tried once") != std::string::npos);
    CHECK(result.readerFallbackDiagnostic.find("also failed") != std::string::npos);
}

TEST_CASE("a forced compatibility reader works without the native attempt", "[reader-fallback]")
{
    const auto gltf = import_broker::RunImportSession(
        AssimpRequest(L"required-extension.gltf", model_core::AssimpFormat::Gltf, 830));
    CAPTURE(gltf.stage, gltf.errorCode, gltf.chunks.size());
    REQUIRE(gltf.ok);
    CHECK_FALSE(gltf.compatibilityFallbackAttempted);
    CHECK_FALSE(gltf.usedCompatibilityReader);
    CHECK(gltf.readerFallbackAttemptCount == 0);
    CHECK(CountGeometry(gltf).triangles == 1);

    const auto obj = import_broker::RunImportSession(
        AssimpRequest(L"box.obj", model_core::AssimpFormat::Obj, 831));
    CAPTURE(obj.stage, obj.errorCode, obj.chunks.size());
    REQUIRE(obj.ok);
    CHECK(CountGeometry(obj).triangles == 12);

    const auto fbx = import_broker::RunImportSession(
        AssimpRequest(L"box.fbx", model_core::AssimpFormat::Fbx, 832));
    CAPTURE(fbx.stage, fbx.errorCode, fbx.chunks.size());
    REQUIRE(fbx.ok);
    CHECK(CountGeometry(fbx).triangles == 12);

    const auto threeMf = import_broker::RunImportSession(
        AssimpRequest(L"required-extension.3mf", model_core::AssimpFormat::ThreeMf, 833));
    CAPTURE(threeMf.stage, threeMf.errorCode, threeMf.chunks.size());
    REQUIRE(threeMf.ok);
    CHECK(CountGeometry(threeMf).triangles == 1);
}

TEST_CASE("the app path records the fallback and offers the explicit action",
          "[reader-fallback]")
{
    SECTION("automatic fallback on a default native open") {
        const auto result = d3d12_import_bridge::RunImport(
            d3d12_import_bridge::SourceFormat::Glb,
            std::wstring(PREVIEW3D_ASSIMP_FIXTURES_DIR) + L"required-extension.gltf",
            840);
        CAPTURE(result.errorCode, result.errorStage);
        REQUIRE(result.ok);
        CHECK(result.usedCompatibilityReader);
        CHECK(result.compatibilityFallbackAttempted);
        CHECK(result.readerName.find(L"gltf-assimp") != std::wstring::npos);
        CHECK(result.readerFallbackDiagnostic.find(L"gltf") != std::wstring::npos);
        CHECK(result.readerFallbackDiagnostic.find(L"gltf-assimp") != std::wstring::npos);
    }
    SECTION("a forced native reader is offered the explicit action instead") {
        const auto result = d3d12_import_bridge::RunImport(
            d3d12_import_bridge::SourceFormat::Glb,
            std::wstring(PREVIEW3D_ASSIMP_FIXTURES_DIR) + L"required-extension.gltf",
            841, {}, {}, 64ull * 1024 * 1024, false, 0, {}, {}, {}, {}, {},
            model_core::ImportOptions{}, false, false);
        CAPTURE(result.errorCode, result.errorStage);
        REQUIRE_FALSE(result.ok);
        CHECK_FALSE(result.compatibilityFallbackAttempted);
        CHECK(result.compatibilityReaderStableName == "gltf-assimp");
        CHECK(result.compatibilityReaderName.find(L"gltf-assimp") != std::wstring::npos);
        // The failure text names both readers and the final reader.
        CHECK(result.errorDetails.find(L"gltf") != std::wstring::npos);
        CHECK(result.errorDetails.find(L"gltf-assimp") != std::wstring::npos);
        CHECK(result.errorDetails.find(L"final reader") != std::wstring::npos);
        const std::wstring diagnostics = d3d12_import_bridge::DiagnosticDetails(
            std::wstring(PREVIEW3D_ASSIMP_FIXTURES_DIR) + L"required-extension.gltf", result);
        CHECK(diagnostics.find(L"Reader:") != std::wstring::npos);
        CHECK(diagnostics.find(L"gltf-assimp") != std::wstring::npos);
    }
    SECTION("a forced compatibility reader does not run the native attempt") {
        const auto result = d3d12_import_bridge::RunImport(
            d3d12_import_bridge::SourceFormat::AssimpGltf,
            std::wstring(PREVIEW3D_ASSIMP_FIXTURES_DIR) + L"required-extension.gltf",
            842);
        CAPTURE(result.errorCode, result.errorStage);
        REQUIRE(result.ok);
        CHECK_FALSE(result.compatibilityFallbackAttempted);
        CHECK_FALSE(result.usedCompatibilityReader);
        CHECK(result.readerName.find(L"gltf-assimp") != std::wstring::npos);
    }
    SECTION("a failed compatibility attempt is final and names the final reader") {
        const auto result = d3d12_import_bridge::RunImport(
            d3d12_import_bridge::SourceFormat::Glb,
            std::wstring(PREVIEW3D_ASSIMP_FIXTURES_DIR) + L"required-extension-missing-buffer.gltf",
            843);
        CAPTURE(result.errorCode, result.errorStage);
        REQUIRE_FALSE(result.ok);
        CHECK(result.compatibilityFallbackAttempted);
        CHECK_FALSE(result.usedCompatibilityReader);
        CHECK(result.compatibilityReaderStableName == "gltf-assimp");
        CHECK(result.readerName.find(L"gltf-assimp") != std::wstring::npos);
        CHECK(result.readerFallbackDiagnostic.find(L"also failed") != std::wstring::npos);
    }
}
