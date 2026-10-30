// T17: line/strip/fan/point topology. Covers the end-to-end adapters
// (glTF strip/fan/lines/points, OBJ line/point fallback), the LineList wire
// contract, and the validator's odd-count/out-of-range rejection.

#include "SandboxTestSupport.h"
#include "import_broker/ImportSession.h"
#include "import_broker/SharedSectionValidator.h"
#include "model_core/Checksum.h"
#include "model_core/GeometryBounds.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct ScratchFile {
    std::filesystem::path directory;
    explicit ScratchFile(const wchar_t* extension)
    {
        directory = std::filesystem::temp_directory_path()
            / (L"Preview3D-topology-" + std::to_wstring(GetCurrentProcessId()) + L"-"
               + std::to_wstring(GetTickCount64()));
        REQUIRE(std::filesystem::create_directory(directory));
        path = directory / (std::wstring(L"model") + extension);
    }
    ~ScratchFile() { std::error_code error; std::filesystem::remove_all(directory, error); }
    std::filesystem::path path;
    void Write(const std::string& bytes) const
    {
        std::ofstream output(path, std::ios::binary);
        output.write(bytes.data(), std::streamsize(bytes.size()));
        REQUIRE(output.good());
    }
};

std::string Base64(const std::vector<std::byte>& bytes)
{
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint32_t a = std::to_integer<uint32_t>(bytes[i]);
        const uint32_t b = i + 1 < bytes.size() ? std::to_integer<uint32_t>(bytes[i + 1]) : 0;
        const uint32_t c = i + 2 < bytes.size() ? std::to_integer<uint32_t>(bytes[i + 2]) : 0;
        output.push_back(alphabet[a >> 2]);
        output.push_back(alphabet[((a & 3) << 4) | (b >> 4)]);
        output.push_back(i + 1 < bytes.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=');
        output.push_back(i + 2 < bytes.size() ? alphabet[c & 63] : '=');
    }
    return output;
}

struct MeshData {
    std::vector<float> positions;
    std::vector<uint16_t> indices;
    float minimum[3]{};
    float maximum[3]{};
};

MeshData Mesh(std::vector<float> positions, std::vector<uint16_t> indices)
{
    MeshData data;
    data.positions = std::move(positions);
    data.indices = std::move(indices);
    for (unsigned axis = 0; axis < 3; ++axis) {
        data.minimum[axis] = data.positions[axis];
        data.maximum[axis] = data.positions[axis];
    }
    for (size_t i = 0; i < data.positions.size(); i += 3)
        for (unsigned axis = 0; axis < 3; ++axis) {
            data.minimum[axis] = (std::min)(data.minimum[axis], data.positions[i + axis]);
            data.maximum[axis] = (std::max)(data.maximum[axis], data.positions[i + axis]);
        }
    return data;
}

std::string GltfJson(const MeshData& mesh, uint32_t mode)
{
    std::vector<std::byte> binary;
    for (float value : mesh.positions) {
        std::byte bytes[sizeof(float)];
        std::memcpy(bytes, &value, sizeof(value));
        binary.insert(binary.end(), std::begin(bytes), std::end(bytes));
    }
    const size_t indexOffset = binary.size();
    for (uint16_t value : mesh.indices) {
        std::byte bytes[sizeof(uint16_t)];
        std::memcpy(bytes, &value, sizeof(value));
        binary.insert(binary.end(), std::begin(bytes), std::end(bytes));
    }
    const std::string buffer = Base64(binary);
    char json[2048]{};
    std::snprintf(json, sizeof(json),
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"byteLength\":%zu,\"uri\":\"data:application/octet-stream;base64,%s\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":%zu,\"target\":34962},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34963}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":%zu,\"type\":\"VEC3\","
        "\"min\":[%g,%g,%g],\"max\":[%g,%g,%g]},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":%zu,\"type\":\"SCALAR\"}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},"
        "\"indices\":1,\"mode\":%u}]}],"
        "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}",
        binary.size(), buffer.c_str(), indexOffset,
        indexOffset, mesh.indices.size() * sizeof(uint16_t),
        mesh.positions.size() / 3,
        mesh.minimum[0], mesh.minimum[1], mesh.minimum[2],
        mesh.maximum[0], mesh.maximum[1], mesh.maximum[2],
        mesh.indices.size(), mode);
    return json;
}

import_broker::ImportSessionRequest Request(const std::filesystem::path& path,
                                            import_broker::ImportFormat format)
{
    import_broker::ImportSessionRequest request;
    request.workerExePath = sandbox_test_support::WorkerExePath();
    request.sourcePath = path.wstring();
    request.format = format;
    request.generationId = 517;
    request.sectionByteCapacity = import_broker::kImportSectionBytes;
    request.maxChunkCount = 1024;
    request.maxChunkBatchesPerGeneration = 64;
    request.maxChunksPerGeneration = 4096;
    request.maxSidecarRequestsPerGeneration = 64;
    request.maxSidecarFileBytes = 256ull * 1024 * 1024;
    return request;
}

const import_broker::ValidatedChunk* FirstGeometry(const import_broker::ImportSessionResult& result)
{
    for (const auto& chunk : result.chunks)
        if (model_core::IsGeometryTopology(chunk.descriptor.topology)) return &chunk;
    return nullptr;
}

// Hand-built sections exercise the validator without a live worker.
struct ChunkSpec {
    model_core::ChunkDescriptor descriptor{};
    std::vector<std::byte> payload;
};

std::vector<std::byte> BuildSection(std::vector<ChunkSpec> chunks, uint64_t generationId)
{
    using namespace model_core;
    uint64_t offset = kSectionHeaderSize + chunks.size() * kChunkDescriptorSize;
    for (auto& chunk : chunks) {
        chunk.descriptor.normalizedRangeOffset = offset;
        chunk.descriptor.normalizedRangeLength = chunk.payload.size();
        chunk.descriptor.byteSize = chunk.payload.size();
        if (IsGeometryTopology(chunk.descriptor.topology))
            SetLocalBounds(chunk.descriptor, chunk.payload);
        offset += chunk.payload.size();
    }
    std::vector<std::byte> section(offset, std::byte{0});
    for (auto& chunk : chunks) {
        if (!chunk.payload.empty())
            std::memcpy(section.data() + chunk.descriptor.normalizedRangeOffset,
                        chunk.payload.data(), chunk.payload.size());
        chunk.descriptor.chunkChecksum = WireChecksum64(std::span<const std::byte>(
            section.data() + chunk.descriptor.normalizedRangeOffset, chunk.payload.size()));
    }
    for (size_t i = 0; i < chunks.size(); ++i)
        std::memcpy(section.data() + kSectionHeaderSize + i * kChunkDescriptorSize,
                    &chunks[i].descriptor, sizeof(ChunkDescriptor));
    SectionHeader header{};
    header.magic = kSectionMagic;
    header.protocolVersion = kCurrentProtocolVersion;
    header.generationId = generationId;
    header.scene.generationId = generationId;
    header.scene.format = SourceFormatId::Gltf;
    header.scene.upAxis = UpAxisId::Y;
    header.scene.metersPerUnit = 1.0;
    header.sectionLength = section.size();
    header.chunkCount = uint32_t(chunks.size());
    header.sectionChecksum = WireChecksum64(std::span<const std::byte>(
        section.data() + kSectionHeaderSize, section.size() - kSectionHeaderSize));
    std::memcpy(section.data(), &header, sizeof(header));
    return section;
}

ChunkSpec LineChunk(std::vector<uint32_t> indices, uint32_t vertexCount = 4)
{
    ChunkSpec chunk;
    chunk.descriptor.topology = model_core::ChunkTopology::LineList;
    chunk.descriptor.chunkId = 1;
    chunk.descriptor.vertexCount = vertexCount;
    chunk.descriptor.indexCount = uint32_t(indices.size());
    chunk.descriptor.vertexLayoutId = uint32_t(model_core::VertexLayoutId::PositionOnly_F32);
    chunk.descriptor.boundsState = model_core::BoundsState::Verified;
    chunk.payload.resize(size_t(vertexCount) * sizeof(model_core::VertexPositionOnlyF32));
    auto* vertices = reinterpret_cast<model_core::VertexPositionOnlyF32*>(chunk.payload.data());
    for (uint32_t i = 0; i < vertexCount; ++i) vertices[i] = {float(i), 0.0f, 0.0f};
    const auto* indexBytes = reinterpret_cast<const std::byte*>(indices.data());
    chunk.payload.insert(chunk.payload.end(), indexBytes,
                         indexBytes + indices.size() * sizeof(uint32_t));
    return chunk;
}

} // namespace

TEST_CASE("glTF triangle strips and fans normalize to complete indexed triangles",
          "[topology][gltf]")
{
    // Strip: 0 1 2 3 -> (0,1,2),(2,1,3)
    const MeshData strip = Mesh({0,0,0, 1,0,0, 0,1,0, 1,1,0}, {0,1,2,3});
    const MeshData fan = Mesh({0,0,0, 1,0,0, 1,1,0, 0,1,0}, {0,1,2,3});
    for (const auto& [mesh, mode] : {std::pair{strip, 5u}, std::pair{fan, 6u}}) {
        ScratchFile source(L".gltf");
        source.Write(GltfJson(mesh, mode));
        const auto result = import_broker::RunImportSession(
            Request(source.path, import_broker::ImportFormat::Gltf));
        CAPTURE(result.stage, result.errorCode, result.chunks.size());
        REQUIRE(result.ok);
        const auto* geometry = FirstGeometry(result);
        REQUIRE(geometry);
        CHECK(geometry->descriptor.topology == model_core::ChunkTopology::TriangleList);
        CHECK(geometry->descriptor.indexCount == 6);
        CHECK(geometry->descriptor.vertexCount >= 4);
        CHECK(geometry->descriptor.indexCount % 3 == 0);
        // Every triangle index resolves inside the chunk's own vertex range.
        const auto stride = model_core::VertexStrideForLayout(model_core::VertexLayoutId::PositionNormalUv0TangentColor_F32);
        const auto* indices = reinterpret_cast<const uint32_t*>(
            geometry->payload.data() + size_t(geometry->descriptor.vertexCount) * stride);
        for (uint32_t i = 0; i < geometry->descriptor.indexCount; ++i)
            CHECK(indices[i] < geometry->descriptor.vertexCount);
        CHECK(geometry->descriptor.localMin[0] == 0.0f);
        CHECK(geometry->descriptor.localMax[0] == 1.0f);
        CHECK(geometry->descriptor.localMax[1] == 1.0f);
    }
}

TEST_CASE("glTF line strip and loop produce LineList instead of EmptyGeometry",
          "[topology][gltf]")
{
    const MeshData line = Mesh({0,0,0, 1,0,0, 1,1,0, 0,1,0}, {0,1,2,3});
    const std::array<std::pair<uint32_t, uint32_t>, 2> modes{{{3, 3}, {2, 4}}}; // strip, loop
    for (const auto& [mode, expectedSegments] : modes) {
        ScratchFile source(L".gltf");
        source.Write(GltfJson(line, mode));
        const auto result = import_broker::RunImportSession(
            Request(source.path, import_broker::ImportFormat::Gltf));
        CAPTURE(mode, result.stage, result.errorCode);
        REQUIRE(result.ok);
        const auto* geometry = FirstGeometry(result);
        REQUIRE(geometry);
        CHECK(geometry->descriptor.topology == model_core::ChunkTopology::LineList);
        CHECK(geometry->descriptor.indexCount == expectedSegments * 2);
        CHECK(geometry->descriptor.indexCount % 2 == 0);
        CHECK(geometry->descriptor.localMax[0] == 1.0f);
    }
}

TEST_CASE("glTF LINES and POINTS preserve their topology", "[topology][gltf]")
{
    const MeshData data = Mesh({0,0,0, 1,0,0, 0,1,0, 1,1,0}, {0,1,2,3});
    {
        ScratchFile source(L".gltf");
        source.Write(GltfJson(data, 1)); // LINES
        const auto result = import_broker::RunImportSession(
            Request(source.path, import_broker::ImportFormat::Gltf));
        REQUIRE(result.ok);
        const auto* geometry = FirstGeometry(result);
        REQUIRE(geometry);
        CHECK(geometry->descriptor.topology == model_core::ChunkTopology::LineList);
        CHECK(geometry->descriptor.indexCount == 4);
    }
    {
        ScratchFile source(L".gltf");
        source.Write(GltfJson(data, 0)); // POINTS
        const auto result = import_broker::RunImportSession(
            Request(source.path, import_broker::ImportFormat::Gltf));
        REQUIRE(result.ok);
        const auto* geometry = FirstGeometry(result);
        REQUIRE(geometry);
        CHECK(geometry->descriptor.topology == model_core::ChunkTopology::PointList);
        CHECK(geometry->descriptor.indexCount == 0);
        CHECK(geometry->descriptor.vertexCount == 4);
    }
}

TEST_CASE("OBJ line-only and point-only documents open with the right topology",
          "[topology][obj]")
{
    {
        ScratchFile source(L".obj");
        source.Write("v 0 0 0\nv 1 0 0\nv 1 1 0\nl 1 2 3\n");
        const auto result = import_broker::RunImportSession(
            Request(source.path, import_broker::ImportFormat::Obj));
        CAPTURE(result.stage, result.errorCode);
        REQUIRE(result.ok);
        const auto* geometry = FirstGeometry(result);
        REQUIRE(geometry);
        CHECK(geometry->descriptor.topology == model_core::ChunkTopology::LineList);
        CHECK(geometry->descriptor.indexCount == 4);
    }
    {
        ScratchFile source(L".obj");
        source.Write("v 0 0 0\nv 1 0 0\np 1 2\n");
        const auto result = import_broker::RunImportSession(
            Request(source.path, import_broker::ImportFormat::Obj));
        CAPTURE(result.stage, result.errorCode);
        REQUIRE(result.ok);
        const auto* geometry = FirstGeometry(result);
        REQUIRE(geometry);
        CHECK(geometry->descriptor.topology == model_core::ChunkTopology::PointList);
        CHECK(geometry->descriptor.vertexCount == 2);
        CHECK(geometry->descriptor.indexCount == 0);
    }
}

TEST_CASE("Odd-count and out-of-range line data fails typed", "[topology][gltf][malformed]")
{
    {
        // LINES with an odd index count is not a complete pair list.
        ScratchFile source(L".gltf");
        source.Write(GltfJson(Mesh({0,0,0, 1,0,0, 0,1,0}, {0,1,2}), 1));
        const auto result = import_broker::RunImportSession(
            Request(source.path, import_broker::ImportFormat::Gltf));
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    }
    {
        // An OBJ line referencing a nonexistent vertex is typed, not empty.
        ScratchFile source(L".obj");
        source.Write("v 0 0 0\nv 1 0 0\nl 1 9\n");
        const auto result = import_broker::RunImportSession(
            Request(source.path, import_broker::ImportFormat::Obj));
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    }
}

TEST_CASE("SharedSectionValidator accepts LineList and rejects hostile variants",
          "[topology][validator]")
{
    constexpr uint64_t generation = 91;
    auto validate = [](const std::vector<std::byte>& section) {
        return import_broker::ValidateAndCopySection(
            std::span<const std::byte>(section.data(), section.size()), generation, 64, nullptr,
            false, nullptr, 0, 0, nullptr);
    };
    {
        const auto section = BuildSection({LineChunk({0, 1, 1, 2, 2, 3})}, generation);
        const auto result = validate(section);
        CAPTURE(result.errorCode);
        REQUIRE(result.ok);
        REQUIRE(result.chunks.size() == 1);
        CHECK(result.chunks[0].descriptor.topology == model_core::ChunkTopology::LineList);
        CHECK(result.chunks[0].descriptor.indexCount == 6);
        CHECK(result.chunks[0].descriptor.localMin[0] == 0.0f);
        CHECK(result.chunks[0].descriptor.localMax[0] == 3.0f);
    }
    {
        const auto section = BuildSection({LineChunk({0, 1, 2})}, generation);
        const auto result = validate(section);
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    }
    {
        const auto section = BuildSection({LineChunk({0, 1, 7, 2})}, generation);
        const auto result = validate(section);
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    }
    {
        // A LineList whose payload is one index short of its declared count.
        auto chunk = LineChunk({0, 1, 1, 2});
        chunk.descriptor.indexCount = 6;
        const auto section = BuildSection({chunk}, generation);
        const auto result = validate(section);
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    }
}
