#include "SandboxTestSupport.h"
#include "import_broker/ImportSession.h"
#include "import_broker/SharedSection.h"
#include "model_core/MaterialPayload.h"
#include "model_core/PixelFormats.h"
#include "model_core/VertexLayouts.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

struct ScratchObj {
    std::filesystem::path directory;
    std::filesystem::path obj;

    ScratchObj()
    {
        directory = std::filesystem::temp_directory_path() /
            (L"Preview3D-obj-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()));
        REQUIRE(std::filesystem::create_directory(directory));
        obj = directory / L"model.obj";
    }
    ~ScratchObj() { std::error_code error; std::filesystem::remove_all(directory, error); }
    void Write(const std::filesystem::path& path, const std::string& bytes)
    {
        std::ofstream output(path, std::ios::binary);
        output.write(bytes.data(), std::streamsize(bytes.size()));
        REQUIRE(output.good());
    }
};

import_broker::ImportSessionRequest Request(const std::filesystem::path& path)
{
    import_broker::ImportSessionRequest request;
    request.workerExePath = sandbox_test_support::WorkerExePath();
    request.sourcePath = path.wstring();
    request.format = import_broker::ImportFormat::Obj;
    request.generationId = 401;
    request.sectionByteCapacity = import_broker::kImportSectionBytes;
    request.maxChunkCount = 1024;
    request.maxChunkBatchesPerGeneration = 64;
    request.maxChunksPerGeneration = 4096;
    request.maxSidecarRequestsPerGeneration = 64;
    request.maxSidecarFileBytes = 256ull * 1024 * 1024;
    return request;
}

} // namespace

TEST_CASE("OBJ and MTL normalize polygons attributes materials and brokered textures", "[obj][mtl]")
{
    ScratchObj source;
    source.Write(source.obj,
        "mtllib material.mtl\n"
        "o colored_quad\n"
        "v 0 0 0 1 0 0\n"
        "v 1 0 0 0 1 0\n"
        "v 1 1 0 0 0 1\n"
        "v 0 1 0 1 1 1\n"
        "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
        "usemtl painted\n"
        "s 1\n"
        "f 1/1 2/2 3/3 4/4\n");
    source.Write(source.directory / L"material.mtl",
        "newmtl painted\n"
        "Kd 0.2 0.4 0.6\n"
        "Ke 0.1 0.2 0.3\n"
        "d 0.75\n"
        "map_Kd gray.png\n"
        "map_Bump gray.png\n");
    std::filesystem::copy_file(std::filesystem::path(PREVIEW3D_TEST_ASSETS_DIR) / L"textures" / L"gray.png",
                               source.directory / L"gray.png");

    const auto result = import_broker::RunImportSession(Request(source.obj));
    CAPTURE(result.stage, result.errorCode, result.errorPhase, result.chunks.size());
    REQUIRE(result.ok);
    const import_broker::ValidatedChunk* mesh = nullptr;
    const import_broker::ValidatedChunk* material = nullptr;
    uint32_t imageCount = 0;
    for (const auto& chunk : result.chunks) {
        CHECK(chunk.scene.format == model_core::SourceFormatId::Obj);
        if (chunk.descriptor.topology == model_core::ChunkTopology::TriangleList) mesh = &chunk;
        if (chunk.descriptor.topology == model_core::ChunkTopology::Material) material = &chunk;
        if (chunk.descriptor.topology == model_core::ChunkTopology::Image) ++imageCount;
    }
    REQUIRE(mesh);
    CHECK(mesh->descriptor.indexCount == 6);
    CHECK(mesh->descriptor.vertexCount == 6);
    CHECK(mesh->descriptor.vertexLayoutId == uint32_t(model_core::VertexLayoutId::PositionNormalUv0TangentColor_F32));
    CHECK((mesh->descriptor.geometryFlags & model_core::kGeometryHasUv0) != 0);
    CHECK((mesh->descriptor.geometryFlags & model_core::kGeometryHasColors) != 0);
    REQUIRE(material);
    REQUIRE(material->payload.size() == sizeof(model_core::MaterialPayload));
    model_core::MaterialPayload payload{};
    std::memcpy(&payload, material->payload.data(), sizeof(payload));
    CHECK(payload.baseColorFactor[0] == Catch::Approx(0.2f));
    CHECK(payload.baseColorFactor[1] == Catch::Approx(0.4f));
    CHECK(payload.baseColorFactor[2] == Catch::Approx(0.6f));
    CHECK(payload.baseColorFactor[3] == Catch::Approx(0.75f));
    CHECK(payload.alphaMode == uint32_t(model_core::AlphaModeId::Blend));
    // OBJ texture V coordinates are authored bottom-left while the D3D12
    // samplers expect top-left, so the normalized material must flip V.
    CHECK((payload.flags & model_core::kMaterialFlagFlipV) != 0);
    CHECK(imageCount == 2);
    CHECK(material->descriptor.dependencyCount == 2);
    CHECK(mesh->descriptor.dependencyIds[0] == material->descriptor.chunkId);
}

TEST_CASE("OBJ missing MTL remains visible with a bounded warning", "[obj][mtl][recovery]")
{
    ScratchObj source;
    source.Write(source.obj,
        "mtllib missing.mtl\nusemtl missing\n"
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    const auto result = import_broker::RunImportSession(Request(source.obj));
    CAPTURE(result.stage, result.errorCode, result.chunks.size());
    REQUIRE(result.ok);
    bool geometry = false, status = false;
    for (const auto& chunk : result.chunks) {
        geometry |= chunk.descriptor.topology == model_core::ChunkTopology::TriangleList;
        status |= chunk.descriptor.topology == model_core::ChunkTopology::ImportStatus;
    }
    CHECK(geometry);
    CHECK(status);
}

TEST_CASE("OBJ sidecars cannot escape the primary directory", "[obj][mtl][security]")
{
    ScratchObj source;
    source.Write(source.obj,
        "mtllib ../outside.mtl\n"
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    const auto result = import_broker::RunImportSession(Request(source.obj));
    REQUIRE_FALSE(result.ok);
    CHECK(result.stage == import_broker::ImportStage::WorkerReportedError);
    CHECK(result.errorCode == model_core::ImportErrorCode::UnsafeReference);
    CHECK(result.errorPhase == model_core::ImportFailurePhase::Sidecars);
}

TEST_CASE("OBJ material texture references cannot escape the primary directory", "[obj][mtl][security]")
{
    ScratchObj source;
    source.Write(source.obj,
        "mtllib material.mtl\nusemtl painted\n"
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    source.Write(source.directory / L"material.mtl",
        "newmtl painted\nKd 1 1 1\nmap_Kd ../outside.png\n");
    const auto result = import_broker::RunImportSession(Request(source.obj));
    REQUIRE_FALSE(result.ok);
    CHECK(result.stage == import_broker::ImportStage::WorkerReportedError);
    CHECK(result.errorCode == model_core::ImportErrorCode::UnsafeReference);
    CHECK(result.errorPhase == model_core::ImportFailurePhase::Sidecars);
}

TEST_CASE("OBJ objects and material groups remain separate with generated normals", "[obj][mtl]")
{
    ScratchObj source;
    source.Write(source.obj,
        "mtllib material.mtl\n"
        "o first\n"
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "usemtl red\ns 1\nf -3 -2 -1\n"
        "o second\n"
        "v 0 0 1\nv 1 0 1\nv 0 1 1\n"
        "usemtl green\ns off\nf -3 -2 -1\n");
    source.Write(source.directory / L"material.mtl",
        "newmtl red\nKd 1 0 0\n"
        "newmtl green\nKd 0 1 0\n");

    const auto result = import_broker::RunImportSession(Request(source.obj));
    CAPTURE(result.stage, result.errorCode, result.chunks.size());
    REQUIRE(result.ok);
    uint32_t meshes = 0, materials = 0;
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology == model_core::ChunkTopology::Material) {
            ++materials;
            continue;
        }
        if (chunk.descriptor.topology != model_core::ChunkTopology::TriangleList) continue;
        ++meshes;
        REQUIRE(chunk.descriptor.vertexCount == 3);
        REQUIRE(chunk.payload.size() >= 3 * sizeof(model_core::VertexPositionNormalUv0TangentColorF32));
        const auto* vertices = reinterpret_cast<const model_core::VertexPositionNormalUv0TangentColorF32*>(
            chunk.payload.data());
        for (uint32_t vertex = 0; vertex < 3; ++vertex) {
            CHECK(vertices[vertex].nx == Catch::Approx(0.0f).margin(1e-5));
            CHECK(vertices[vertex].ny == Catch::Approx(0.0f).margin(1e-5));
            CHECK(vertices[vertex].nz == Catch::Approx(1.0f).margin(1e-5));
        }
    }
    CHECK(meshes == 2);
    CHECK(materials == 2);
}

TEST_CASE("OBJ normalization crosses a bounded output window progressively", "[obj][progressive]")
{
    ScratchObj source;
    std::string obj;
    for (unsigned triangle = 0; triangle < 50; ++triangle) {
        const unsigned first = triangle * 3 + 1;
        obj += "v " + std::to_string(triangle) + " 0 0\n";
        obj += "v " + std::to_string(triangle) + " 1 0\n";
        obj += "v " + std::to_string(triangle) + " 0 1\n";
        obj += "f " + std::to_string(first) + " " + std::to_string(first + 1) + " " +
               std::to_string(first + 2) + "\n";
    }
    source.Write(source.obj, obj);
    auto request = Request(source.obj);
    request.sectionByteCapacity = 4096;
    request.maxChunkCount = 8;
    const auto result = import_broker::RunImportSession(request);
    CAPTURE(result.stage, result.errorCode, result.batchCount, result.chunks.size());
    REQUIRE(result.ok);
    CHECK(result.batchCount > 1);
    uint32_t indices = 0;
    for (const auto& chunk : result.chunks)
        if (chunk.descriptor.topology == model_core::ChunkTopology::TriangleList)
            indices += chunk.descriptor.indexCount;
    CHECK(indices == 150);
}

TEST_CASE("Malformed and empty OBJ files fail without poisoning the next import", "[obj][recovery]")
{
    ScratchObj bad;
    bad.Write(bad.obj, "this is not wavefront geometry\n");
    auto result = import_broker::RunImportSession(Request(bad.obj));
    REQUIRE_FALSE(result.ok);
    CHECK((result.errorCode == model_core::ImportErrorCode::MalformedData ||
           result.errorCode == model_core::ImportErrorCode::EmptyGeometry));

    ScratchObj good;
    good.Write(good.obj, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    result = import_broker::RunImportSession(Request(good.obj));
    REQUIRE(result.ok);
}
