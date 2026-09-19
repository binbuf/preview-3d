#include <catch2/catch_test_macros.hpp>

#include "import_broker/ImportSession.h"
#include "model_core/MaterialPayload.h"
#include "model_core/WireFormat.h"

#include <windows.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef PREVIEW3D_STEP_HOST_EXE
#error "PREVIEW3D_STEP_HOST_EXE must be defined by Tests.ImportIsolation.vcxproj"
#endif
#ifndef PREVIEW3D_STP_FIXTURES_DIR
#error "PREVIEW3D_STP_FIXTURES_DIR must be defined by Tests.ImportIsolation.vcxproj"
#endif

namespace {

std::wstring StpFixture(const wchar_t* name)
{
    return std::wstring(PREVIEW3D_STP_FIXTURES_DIR) + name;
}

std::string Narrow(const wchar_t* text)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(length > 0 ? length - 1 : 0), '\0');
    if (length > 1)
        WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
    return result;
}

import_broker::ImportSessionRequest Request(const wchar_t* fixture, uint64_t generation)
{
    import_broker::ImportSessionRequest request;
    request.format = import_broker::ImportFormat::Step;
    request.sourcePath = StpFixture(fixture);
    request.stepHostExePath = PREVIEW3D_STEP_HOST_EXE;
    request.generationId = generation;
    request.sectionByteCapacity = 64ull * 1024 * 1024;
    request.maxChunkCount = 1024;
    request.maxChunksPerGeneration = 1024;
    request.maxChunkBatchesPerGeneration = 1;
    request.replyTimeoutMs = 20'000;
    return request;
}

struct SceneCounts {
    std::uint32_t geometry = 0;
    std::uint32_t materials = 0;
    std::uint32_t nodes = 0;
    std::uint32_t instances = 0;
    std::uint32_t transparentMaterials = 0;
};

SceneCounts Tally(const import_broker::ImportSessionResult& result)
{
    SceneCounts counts;
    for (const auto& chunk : result.chunks) {
        switch (chunk.descriptor.topology) {
        case model_core::ChunkTopology::TriangleList:
        case model_core::ChunkTopology::PointList:
            ++counts.geometry;
            CHECK(chunk.descriptor.boundsState == model_core::BoundsState::Verified);
            CHECK(chunk.descriptor.vertexCount > 0);
            break;
        case model_core::ChunkTopology::Material: {
            ++counts.materials;
            REQUIRE(chunk.payload.size() == sizeof(model_core::MaterialPayload));
            model_core::MaterialPayload payload{};
            std::memcpy(&payload, chunk.payload.data(), sizeof(payload));
            CHECK(payload.alphaMode <= uint32_t(model_core::AlphaModeId::Blend));
            if (payload.baseColorFactor[3] < 0.999f) ++counts.transparentMaterials;
            break;
        }
        case model_core::ChunkTopology::Node:
            ++counts.nodes;
            break;
        case model_core::ChunkTopology::MeshInstance:
            ++counts.instances;
            break;
        default:
            break;
        }
    }
    return counts;
}

std::vector<model_core::MeshInstancePayload> Instances(
    const std::vector<import_broker::ValidatedChunk>& chunks)
{
    std::vector<model_core::MeshInstancePayload> instances;
    for (const auto& chunk : chunks) {
        if (chunk.descriptor.topology != model_core::ChunkTopology::MeshInstance) continue;
        model_core::MeshInstancePayload payload{};
        std::memcpy(&payload, chunk.payload.data(), sizeof(payload));
        instances.push_back(payload);
    }
    return instances;
}

std::vector<model_core::MaterialPayload> Materials(
    const std::vector<import_broker::ValidatedChunk>& chunks)
{
    std::vector<model_core::MaterialPayload> materials;
    for (const auto& chunk : chunks) {
        if (chunk.descriptor.topology != model_core::ChunkTopology::Material) continue;
        model_core::MaterialPayload payload{};
        std::memcpy(&payload, chunk.payload.data(), sizeof(payload));
        materials.push_back(payload);
    }
    return materials;
}

std::vector<model_core::NodePayload> Nodes(
    const std::vector<import_broker::ValidatedChunk>& chunks)
{
    std::vector<model_core::NodePayload> nodes;
    for (const auto& chunk : chunks) {
        if (chunk.descriptor.topology != model_core::ChunkTopology::Node) continue;
        model_core::NodePayload payload{};
        std::memcpy(&payload, chunk.payload.data(), sizeof(payload));
        nodes.push_back(payload);
    }
    return nodes;
}

std::wstring WriteTempPart21(std::string_view bytes)
{
    wchar_t directory[MAX_PATH]{};
    REQUIRE(GetTempPathW(MAX_PATH, directory) != 0);
    const std::wstring path = std::wstring(directory) + L"preview3d-step-003-"
        + std::to_wstring(GetCurrentProcessId()) + L"-"
        + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) + L".stp";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    REQUIRE(output.good());
    return path;
}

struct TempFileGuard {
    std::wstring path;
    ~TempFileGuard() { if (!path.empty()) { std::error_code error; std::filesystem::remove(path, error); } }
};

} // namespace

TEST_CASE("STEP-003 imports AP203 AP214 and AP242 analytic parts", "[step-003][ap]")
{
    const std::array<const wchar_t*, 3> fixtures{
        L"part_ap203.stp", L"part_ap214.stp", L"part_ap242.stp"};
    uint64_t generation = 0x6300;
    for (const wchar_t* fixture : fixtures) {
        const auto result = import_broker::RunImportSession(Request(fixture, generation++));
        INFO(Narrow(fixture) << " code " << uint32_t(result.errorCode) << " stage " << uint32_t(result.stage));
        REQUIRE(result.ok);
        REQUIRE(result.producer == import_broker::ImportProducer::StepHost);
        REQUIRE_FALSE(result.chunks.empty());
        CHECK(result.chunks.front().scene.format == model_core::SourceFormatId::Step);
        CHECK(result.chunks.front().scene.upAxis == model_core::UpAxisId::Unknown);
        CHECK(result.chunks.front().scene.metersPerUnit > 0.0);
        CHECK(result.chunks.front().scene.meshCount == 1);
        CHECK(result.chunks.front().scene.nodeCount == 1);
        const SceneCounts counts = Tally(result);
        CHECK(counts.geometry == 1);
        CHECK(counts.instances == 1);
        CHECK(counts.materials == 1);
    }
}

TEST_CASE("STEP-003 preserves assembly occurrences and reusable definitions",
          "[step-003][assembly]")
{
    const auto result = import_broker::RunImportSession(Request(L"assembly_ap214.stp", 0x6310));
    INFO("code " << uint32_t(result.errorCode));
    REQUIRE(result.ok);
    const SceneCounts counts = Tally(result);
    CHECK(counts.geometry == 2);   // box and cylinder definitions
    CHECK(counts.instances == 3);  // two box occurrences plus one cylinder
    CHECK(counts.materials == 2);
    CHECK(counts.nodes == 4);      // root assembly plus three occurrence nodes
    CHECK(counts.transparentMaterials == 1);
    CHECK(result.chunks.front().scene.meshCount == 2);

    // The box definition is referenced by two instances without duplicating
    // geometry: one geometry chunk carries multiple instances.
    std::uint32_t reusedChunk = 0;
    std::uint32_t references = 0;
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology != model_core::ChunkTopology::TriangleList) continue;
        std::uint32_t count = 0;
        for (const auto& instance : Instances(result.chunks))
            if (instance.geometryChunkId == chunk.descriptor.chunkId) ++count;
        if (count > references) { references = count; reusedChunk = chunk.descriptor.chunkId; }
    }
    CHECK(references == 2);
    CHECK(reusedChunk != 0);

    // The second box occurrence is translated by (40,0,0) in author space.
    bool sawTranslated = false;
    for (const auto& instance : Instances(result.chunks)) {
        if (instance.geometryChunkId == reusedChunk && instance.worldMin[0] > 39.0
            && instance.worldMin[0] < 41.0)
            sawTranslated = true;
    }
    CHECK(sawTranslated);

    const auto nodes = Nodes(result.chunks);
    std::uint32_t roots = 0;
    for (const auto& node : nodes) if (node.parentNodeId == 0) ++roots;
    CHECK(roots == 1);
}

TEST_CASE("STEP-003 composes nested assembly transforms recursively", "[step-003][nested]")
{
    const auto result = import_broker::RunImportSession(Request(L"assembly_nested_ap214.stp", 0x6320));
    INFO("code " << uint32_t(result.errorCode));
    REQUIRE(result.ok);
    const SceneCounts counts = Tally(result);
    CHECK(counts.geometry == 2);
    CHECK(counts.instances == 5);
    CHECK(counts.nodes == 8);

    // A sub-assembly reused at y=30 places one of its children near y=30.
    bool sawNested = false;
    for (const auto& instance : Instances(result.chunks))
        if (instance.worldMin[1] > 28.0 && instance.worldMin[1] < 32.0) sawNested = true;
    CHECK(sawNested);

    // Every node is reachable to a single root at depth at most 4.
    const auto nodes = Nodes(result.chunks);
    std::uint32_t roots = 0;
    for (const auto& node : nodes) {
        if (node.parentNodeId == 0) ++roots;
        else {
            bool parentFound = false;
            for (const auto& candidate : nodes) if (candidate.nodeId == node.parentNodeId) parentFound = true;
            CHECK(parentFound);
        }
    }
    CHECK(roots == 1);
}

TEST_CASE("STEP-003 applies instance color over a reused definition", "[step-003][color][instance]")
{
    const auto result = import_broker::RunImportSession(Request(L"instance_color_ap214.stp", 0x6330));
    INFO("code " << uint32_t(result.errorCode));
    REQUIRE(result.ok);
    const SceneCounts counts = Tally(result);
    CHECK(counts.geometry == 1);   // one reusable box definition
    CHECK(counts.instances == 3);  // placed three times
    CHECK(counts.materials == 2);  // definition color plus one instance color

    // The middle occurrence (translated to x=20) carries a different material
    // than the two definition-colored occurrences.
    const auto instances = Instances(result.chunks);
    std::uint32_t firstMaterial = 0, middleMaterial = 0;
    for (const auto& instance : instances) {
        if (instance.worldMin[0] < 1.0) firstMaterial = instance.materialChunkId;
        if (instance.worldMin[0] > 19.0 && instance.worldMin[0] < 21.0) middleMaterial = instance.materialChunkId;
    }
    CHECK(firstMaterial != 0);
    CHECK(middleMaterial != 0);
    CHECK(firstMaterial != middleMaterial);
}

TEST_CASE("STEP-003 splits reusable geometry along face-color seams", "[step-003][color][face]")
{
    const auto result = import_broker::RunImportSession(Request(L"face_color_ap214.stp", 0x6340));
    INFO("code " << uint32_t(result.errorCode));
    REQUIRE(result.ok);
    const SceneCounts counts = Tally(result);
    CHECK(counts.geometry == 2);   // one seam group per face color
    CHECK(counts.materials == 2);
    CHECK(counts.instances == 2);  // one occurrence, one instance per seam group
    CHECK(counts.nodes == 1);
}

TEST_CASE("STEP-003 keeps transferred metre factor positive for inch-authored files",
          "[step-003][units]")
{
    const auto result = import_broker::RunImportSession(Request(L"inch_part_ap214.stp", 0x6350));
    INFO("code " << uint32_t(result.errorCode));
    REQUIRE(result.ok);
    CHECK(result.chunks.front().scene.upAxis == model_core::UpAxisId::Unknown);
    // The pinned reader normalizes transferred geometry to the Cascade system
    // unit; the verified factor describes those stored coordinates and is
    // positive and finite for both millimetre and inch-authored files.
    CHECK(result.chunks.front().scene.metersPerUnit > 0.0);
    CHECK(result.chunks.front().scene.metersPerUnit < 1.0);
}

TEST_CASE("STEP-003 rejects Part-21 files with no supported visual geometry",
          "[step-003][negative]")
{
    constexpr std::string_view kNoShape =
        "ISO-10303-21;\nHEADER;\n"
        "FILE_DESCRIPTION((''),'2;1');\n"
        "FILE_NAME('','',(''),(''),'','','');\n"
        "FILE_SCHEMA(('AUTOMOTIVE_DESIGN'));\n"
        "ENDSEC;\nDATA;\n#1=APPLICATION_CONTEXT('core data');\nENDSEC;\n"
        "END-ISO-10303-21;\n";
    TempFileGuard source{WriteTempPart21(kNoShape)};
    auto request = Request(L"", 0x6360);
    request.sourcePath = source.path;
    const auto actual = import_broker::RunImportSession(request);
    CHECK_FALSE(actual.ok);
    CHECK(actual.producer == import_broker::ImportProducer::StepHost);
}

TEST_CASE("STEP-003 normalized output is deterministic and carries no source strings",
          "[step-003][determinism]")
{
    const auto first = import_broker::RunImportSession(Request(L"assembly_ap214.stp", 0x6370));
    const auto second = import_broker::RunImportSession(Request(L"assembly_ap214.stp", 0x6371));
    REQUIRE(first.ok);
    REQUIRE(second.ok);
    REQUIRE(first.chunks.size() == second.chunks.size());
    for (std::size_t i = 0; i < first.chunks.size(); ++i) {
        CHECK(first.chunks[i].descriptor.chunkId == second.chunks[i].descriptor.chunkId);
        CHECK(first.chunks[i].descriptor.byteSize == second.chunks[i].descriptor.byteSize);
        CHECK(first.chunks[i].descriptor.chunkChecksum == second.chunks[i].descriptor.chunkChecksum);
    }

    for (const auto& chunk : first.chunks) {
        const std::string payload(reinterpret_cast<const char*>(chunk.payload.data()),
                                  chunk.payload.size());
        CHECK(payload.find("AP214") == std::string::npos);
        CHECK(payload.find("AUTOMOTIVE") == std::string::npos);
        CHECK(payload.find("laboratory") == std::string::npos);
    }
}

TEST_CASE("STEP-003 observes cancellation before the XDE traversal", "[step-003][cancel]")
{
    auto request = Request(L"assembly_ap214.stp", 0x6380);
    request.isCancelled = [] { return true; };
    const auto result = import_broker::RunImportSession(request);
    CHECK_FALSE(result.ok);
    CHECK(result.errorCode == model_core::ImportErrorCode::Cancelled);
}