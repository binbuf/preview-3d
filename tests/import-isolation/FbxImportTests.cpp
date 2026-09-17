#include "SandboxTestSupport.h"
#include "import_broker/ImportSession.h"
#include "import_broker/SharedSection.h"
#include "model_core/MaterialPayload.h"
#include "model_core/VertexLayouts.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct ScratchFbx {
    std::filesystem::path directory;
    std::filesystem::path path;

    ScratchFbx()
    {
        directory = std::filesystem::temp_directory_path() /
            (L"Preview3D-fbx-" + std::to_wstring(GetCurrentProcessId()) + L"-"
             + std::to_wstring(GetTickCount64()));
        REQUIRE(std::filesystem::create_directory(directory));
        path = directory / L"model.fbx";
    }
    ~ScratchFbx()
    {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
    void Write(std::span<const std::byte> bytes)
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        REQUIRE(output.good());
    }
    void Write(std::string_view bytes) { Write(std::as_bytes(std::span(bytes))); }
};

std::vector<std::byte> ReadFixture(std::string_view name)
{
    const auto path = std::filesystem::path(PREVIEW3D_FBX_FIXTURES_DIR) / name;
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    std::vector<char> raw((std::istreambuf_iterator<char>(input)), {});
    if (!name.ends_with(".base64")) {
        const auto bytes = std::as_bytes(std::span(raw));
        return {bytes.begin(), bytes.end()};
    }
    auto digit = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<std::byte> decoded;
    uint32_t accumulator = 0, bits = 0;
    for (unsigned char c : raw) {
        if (c == '=') break;
        const int value = digit(c);
        if (value < 0) continue;
        accumulator = (accumulator << 6) | uint32_t(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(std::byte((accumulator >> bits) & 0xffu));
        }
    }
    REQUIRE_FALSE(decoded.empty());
    return decoded;
}

std::string ReadAsciiFixture(std::string_view name)
{
    const auto bytes = ReadFixture(name);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void ReplaceOnce(std::string& text, std::string_view needle, std::string_view replacement)
{
    const size_t offset = text.find(needle);
    REQUIRE(offset != std::string::npos);
    text.replace(offset, needle.size(), replacement);
}

import_broker::ImportSessionRequest Request(const std::filesystem::path& path,
                                             uint64_t generation = 501)
{
    import_broker::ImportSessionRequest request;
    request.workerExePath = sandbox_test_support::WorkerExePath();
    request.sourcePath = path.wstring();
    request.format = import_broker::ImportFormat::Fbx;
    request.generationId = generation;
    request.sectionByteCapacity = import_broker::kImportSectionBytes;
    request.maxChunkCount = 1024;
    request.maxChunkBatchesPerGeneration = 256;
    request.maxChunksPerGeneration = 20'000;
    return request;
}

uint32_t Count(const import_broker::ImportSessionResult& result, model_core::ChunkTopology topology)
{
    return static_cast<uint32_t>(std::count_if(result.chunks.begin(), result.chunks.end(),
        [topology](const import_broker::ValidatedChunk& chunk) {
            return chunk.descriptor.topology == topology;
        }));
}

} // namespace

TEST_CASE("ASCII FBX preserves hierarchy and shares static mesh geometry across instances",
          "[fbx][hierarchy][instances]")
{
    ScratchFbx source;
    source.Write(ReadFixture("hierarchy-instances-pivots-ascii.fbx"));
    const auto result = import_broker::RunImportSession(Request(source.path));
    CAPTURE(result.stage, result.errorCode, result.errorPhase, result.chunks.size(), result.batchCount);
    REQUIRE(result.ok);
    REQUIRE(Count(result, model_core::ChunkTopology::TriangleList) > 0);
    REQUIRE(Count(result, model_core::ChunkTopology::Node) > 1);
    REQUIRE(Count(result, model_core::ChunkTopology::MeshInstance) > 1);
    CHECK(Count(result, model_core::ChunkTopology::Material) == 1);

    uint32_t firstGeometry = 0, matchingInstances = 0;
    bool transformedNode = false;
    for (const auto& chunk : result.chunks) {
        CHECK(chunk.scene.format == model_core::SourceFormatId::Fbx);
        CHECK(chunk.scene.upAxis == model_core::UpAxisId::Y);
        CHECK(chunk.scene.metersPerUnit == 1.0);
        if (chunk.descriptor.topology == model_core::ChunkTopology::TriangleList && !firstGeometry)
            firstGeometry = chunk.descriptor.chunkId;
        if (chunk.descriptor.topology == model_core::ChunkTopology::MeshInstance) {
            model_core::MeshInstancePayload instance{};
            std::memcpy(&instance, chunk.payload.data(), sizeof(instance));
            matchingInstances += instance.geometryChunkId == firstGeometry;
        }
        if (chunk.descriptor.topology == model_core::ChunkTopology::Node) {
            model_core::NodePayload node{};
            std::memcpy(&node, chunk.payload.data(), sizeof(node));
            transformedNode |= node.localTransform[12] != 0.0 || node.localTransform[13] != 0.0
                || node.localTransform[14] != 0.0;
        }
    }
    CHECK(firstGeometry != 0);
    CHECK(matchingInstances >= 2);
    CHECK(transformedNode);
}

TEST_CASE("Binary FBX triangulates and emits generated normalized attributes",
          "[fbx][binary][triangulation][normals]")
{
    ScratchFbx source;
    source.Write(ReadFixture("cube-binary.fbx.base64"));
    const auto result = import_broker::RunImportSession(Request(source.path, 502));
    CAPTURE(result.stage, result.errorCode, result.chunks.size());
    REQUIRE(result.ok);
    uint64_t triangles = 0;
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology != model_core::ChunkTopology::TriangleList) continue;
        CHECK(chunk.descriptor.vertexLayoutId == uint32_t(
            model_core::VertexLayoutId::PositionNormalUv0TangentColor_F32));
        CHECK((chunk.descriptor.geometryFlags & model_core::kGeometryReusableInstanceSource) != 0);
        triangles += chunk.descriptor.indexCount / 3;
        const auto* vertices = reinterpret_cast<const model_core::VertexPositionNormalUv0TangentColorF32*>(
            chunk.payload.data());
        for (uint32_t index = 0; index < chunk.descriptor.vertexCount; ++index) {
            const float length = std::sqrt(vertices[index].nx * vertices[index].nx
                                           + vertices[index].ny * vertices[index].ny
                                           + vertices[index].nz * vertices[index].nz);
            CHECK(length > 0.99f);
            CHECK(length < 1.01f);
        }
    }
    CHECK(triangles == 12);
}

TEST_CASE("FBX preserves UV and vertex-color attributes", "[fbx][attributes][color][uv]")
{
    std::string ascii = ReadAsciiFixture("hierarchy-instances-pivots-ascii.fbx");
    ReplaceOnce(ascii, "\t\tLayerElementMaterial: 0 {",
        "\t\tLayerElementColor: 0 {\n"
        "\t\t\tVersion: 101\n\t\t\tName: \"displayColor\"\n"
        "\t\t\tMappingInformationType: \"ByPolygonVertex\"\n"
        "\t\t\tReferenceInformationType: \"Direct\"\n"
        "\t\t\tColors: *96 {\n"
        "\t\t\t\ta: 1,0,0,1,0,1,0,1,0,0,1,1,1,1,0,1,"
        "1,0,1,1,0,1,1,1,0.25,0.5,0.75,1,1,0.5,0.25,1,"
        "1,0,0,1,0,1,0,1,0,0,1,1,1,1,0,1,"
        "1,0,1,1,0,1,1,1,0.25,0.5,0.75,1,1,0.5,0.25,1,"
        "1,0,0,1,0,1,0,1,0,0,1,1,1,1,0,1,"
        "1,0,1,1,0,1,1,1,0.25,0.5,0.75,1,1,0.5,0.25,1\n"
        "\t\t\t}\n\t\t}\n"
        "\t\tLayerElementMaterial: 0 {");
    ReplaceOnce(ascii,
        "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementUV\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n",
        "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementUV\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n"
        "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementColor\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n");
    ScratchFbx source;
    source.Write(ascii);
    const auto result = import_broker::RunImportSession(Request(source.path, 509));
    CAPTURE(result.stage, result.errorCode);
    REQUIRE(result.ok);
    bool foundAttributes = false, foundNonWhite = false;
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology != model_core::ChunkTopology::TriangleList) continue;
        foundAttributes |= (chunk.descriptor.geometryFlags & model_core::kGeometryHasUv0)
            && (chunk.descriptor.geometryFlags & model_core::kGeometryHasColors);
        const auto* vertices = reinterpret_cast<const model_core::VertexPositionNormalUv0TangentColorF32*>(
            chunk.payload.data());
        for (uint32_t index = 0; index < chunk.descriptor.vertexCount; ++index)
            foundNonWhite |= vertices[index].r != 1.0f || vertices[index].g != 1.0f
                || vertices[index].b != 1.0f;
    }
    CHECK(foundAttributes);
    CHECK(foundNonWhite);
}

TEST_CASE("FBX normalizes authored axes units pivots and mirrored transforms",
          "[fbx][axes][units][transforms]")
{
    struct Fixture { const char* name; bool binary; } fixtures[]{
        {"z-up-binary.fbx.base64", true},
        {"negative-scale-pivots-ascii.fbx", false},
    };
    for (const auto& fixture : fixtures) {
        ScratchFbx source;
        source.Write(ReadFixture(fixture.name));
        const auto result = import_broker::RunImportSession(Request(source.path, 510));
        CAPTURE(fixture.name, result.stage, result.errorCode);
        REQUIRE(result.ok);
        bool nonIdentity = false, mirrored = false;
        for (const auto& chunk : result.chunks) {
            CHECK(chunk.scene.upAxis == model_core::UpAxisId::Y);
            CHECK(chunk.scene.metersPerUnit == 1.0);
            if (chunk.descriptor.topology != model_core::ChunkTopology::Node) continue;
            model_core::NodePayload node{};
            std::memcpy(&node, chunk.payload.data(), sizeof(node));
            nonIdentity |= node.localTransform[0] != 1.0 || node.localTransform[5] != 1.0
                || node.localTransform[10] != 1.0 || node.localTransform[12] != 0.0
                || node.localTransform[13] != 0.0 || node.localTransform[14] != 0.0;
            const double determinant = node.localTransform[0]
                * (node.localTransform[5] * node.localTransform[10]
                   - node.localTransform[6] * node.localTransform[9])
                - node.localTransform[1]
                * (node.localTransform[4] * node.localTransform[10]
                   - node.localTransform[6] * node.localTransform[8])
                + node.localTransform[2]
                * (node.localTransform[4] * node.localTransform[9]
                   - node.localTransform[5] * node.localTransform[8]);
            mirrored |= determinant < 0.0;
        }
        CHECK(nonIdentity);
        if (!fixture.binary) CHECK(mirrored);
    }
}

TEST_CASE("FBX geometry crosses small shared sections in progressive batches", "[fbx][progressive]")
{
    ScratchFbx source;
    source.Write(ReadFixture("hierarchy-instances-pivots-ascii.fbx"));
    auto request = Request(source.path, 503);
    request.sectionByteCapacity = 4096;
    request.maxChunkCount = 8;
    const auto result = import_broker::RunImportSession(request);
    CAPTURE(result.stage, result.errorCode, result.batchCount, result.chunks.size());
    REQUIRE(result.ok);
    CHECK(result.batchCount > 1);
    CHECK(Count(result, model_core::ChunkTopology::MeshInstance) > 1);
}

TEST_CASE("FBX deformation is rejected until the static-pose task", "[fbx][deformation]")
{
    for (const char* fixture : {"linear-skin-binary.fbx.base64", "shape-animation-binary.fbx.base64"}) {
        ScratchFbx source;
        source.Write(ReadFixture(fixture));
        const auto result = import_broker::RunImportSession(Request(source.path, 504));
        CAPTURE(fixture, result.stage, result.errorCode);
        REQUIRE_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::UnsupportedRequiredFeature);
        CHECK(result.errorPhase == model_core::ImportFailurePhase::Geometry);
    }
}

TEST_CASE("Malformed and cancelled FBX requests do not poison pooled recovery", "[fbx][recovery][pool]")
{
    import_broker::PrepareImportWorkerPoolAsync(sandbox_test_support::WorkerExePath());
    ScratchFbx malformed;
    auto truncated = ReadFixture("cube-binary.fbx.base64");
    truncated.resize(truncated.size() / 2);
    malformed.Write(truncated);
    auto badRequest = Request(malformed.path, 505);
    badRequest.useWorkerPool = true;
    auto result = import_broker::RunImportSession(badRequest);
    REQUIRE_FALSE(result.ok);
    CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    const uint32_t worker = result.workerProcessId;

    ScratchFbx progressive;
    progressive.Write(ReadFixture("hierarchy-instances-pivots-ascii.fbx"));
    std::atomic_bool cancelled = false;
    auto cancelledRequest = Request(progressive.path, 506);
    cancelledRequest.useWorkerPool = true;
    cancelledRequest.sectionByteCapacity = 4096;
    cancelledRequest.maxChunkCount = 8;
    cancelledRequest.isCancelled = [&] { return cancelled.load(); };
    cancelledRequest.onBatch = [&](std::vector<import_broker::ValidatedChunk>&& chunks) {
        REQUIRE_FALSE(chunks.empty());
        cancelled.store(true);
    };
    result = import_broker::RunImportSession(cancelledRequest);
    REQUIRE_FALSE(result.ok);
    CHECK(result.errorCode == model_core::ImportErrorCode::Cancelled);
    CHECK(result.workerProcessId == worker);

    ScratchFbx valid;
    valid.Write(ReadFixture("cube-binary.fbx.base64"));
    auto validRequest = Request(valid.path, 507);
    validRequest.useWorkerPool = true;
    result = import_broker::RunImportSession(validRequest);
    CAPTURE(result.stage, result.errorCode);
    REQUIRE(result.ok);
    CHECK(result.workerProcessId == worker);
}

TEST_CASE("FBX parser is forced and cannot be spoofed by extension", "[fbx][security][format]")
{
    ScratchFbx source;
    source.Write("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    const auto result = import_broker::RunImportSession(Request(source.path, 508));
    REQUIRE_FALSE(result.ok);
    CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
}

TEST_CASE("FBX hierarchy depth is bounded before normalization", "[fbx][limits][depth]")
{
    std::string ascii = ReadAsciiFixture("hierarchy-instances-pivots-ascii.fbx");
    ReplaceOnce(ascii, "Definitions:  {\n\tVersion: 100\n\tCount: 9",
                       "Definitions:  {\n\tVersion: 100\n\tCount: 269");
    ReplaceOnce(ascii, "\tObjectType: \"Model\" {\n\t\tCount: 3",
                       "\tObjectType: \"Model\" {\n\t\tCount: 263");
    std::string models, connections;
    uint64_t parent = 0;
    for (uint64_t index = 0; index < 260; ++index) {
        const uint64_t id = 8'000'000 + index;
        models += "\tModel: " + std::to_string(id) + ", \"Model::deep" + std::to_string(index)
            + "\", \"Null\" {\n\t\tVersion: 232\n\t}\n";
        connections += "\tC: \"OO\"," + std::to_string(id) + "," + std::to_string(parent) + "\n";
        parent = id;
    }
    ReplaceOnce(ascii, "\n}\n\n; Object connections", "\n" + models + "}\n\n; Object connections");
    ReplaceOnce(ascii, "\n}\n;Takes section", "\n" + connections + "}\n;Takes section");
    ScratchFbx source;
    source.Write(ascii);
    const auto result = import_broker::RunImportSession(Request(source.path, 511));
    REQUIRE_FALSE(result.ok);
    CHECK(result.errorCode == model_core::ImportErrorCode::ResourceLimit);
}

TEST_CASE("FBX node counts are bounded before scene-record allocation", "[fbx][limits][count]")
{
    std::string ascii = ReadAsciiFixture("hierarchy-instances-pivots-ascii.fbx");
    ReplaceOnce(ascii, "Definitions:  {\n\tVersion: 100\n\tCount: 9",
                       "Definitions:  {\n\tVersion: 100\n\tCount: 50009");
    ReplaceOnce(ascii, "\tObjectType: \"Model\" {\n\t\tCount: 3",
                       "\tObjectType: \"Model\" {\n\t\tCount: 50003");
    std::string models;
    models.reserve(4 * 1024 * 1024);
    for (uint64_t index = 0; index < 50'000; ++index) {
        const uint64_t id = 9'000'000 + index;
        models += "\tModel: " + std::to_string(id) + ", \"Model::bounded\", \"Null\" {\n"
            "\t\tVersion: 232\n\t}\n";
    }
    ReplaceOnce(ascii, "\n}\n\n; Object connections", "\n" + models + "}\n\n; Object connections");
    ScratchFbx source;
    source.Write(ascii);
    const auto result = import_broker::RunImportSession(Request(source.path, 512));
    REQUIRE_FALSE(result.ok);
    CHECK(result.errorCode == model_core::ImportErrorCode::ResourceLimit);
}
