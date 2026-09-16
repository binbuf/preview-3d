#include "SyntheticSceneGenerator.h"

#include "model_core/Checksum.h"
#include "model_core/ControlProtocol.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"
#include "model_core/GeometryBounds.h"

#include <array>
#include <cstring>

namespace import_worker {

namespace {

using namespace model_core;

// Unit cube centered at the origin, 4 vertices per face (flat per-face
// normals, deliberately not shared/indexed-normal), 6 faces in a fixed
// -Z/+Z/-Y/+Y/-X/+X order. Vertex 0 is the -Z face's first corner:
// (-0.5,-0.5,-0.5), normal (0,0,-1), uv (0,0).
std::array<VertexPositionNormalUv0F32, 24> BuildCubeVertices()
{
    constexpr float h = 0.5f;
    // clang-format off
    return { {
        // -Z
        {-h,-h,-h,  0, 0,-1,  0,0}, { h,-h,-h,  0, 0,-1,  1,0}, { h, h,-h,  0, 0,-1,  1,1}, {-h, h,-h,  0, 0,-1,  0,1},
        // +Z
        {-h,-h, h,  0, 0, 1,  0,0}, { h,-h, h,  0, 0, 1,  1,0}, { h, h, h,  0, 0, 1,  1,1}, {-h, h, h,  0, 0, 1,  0,1},
        // -Y
        {-h,-h,-h,  0,-1, 0,  0,0}, { h,-h,-h,  0,-1, 0,  1,0}, { h,-h, h,  0,-1, 0,  1,1}, {-h,-h, h,  0,-1, 0,  0,1},
        // +Y
        {-h, h,-h,  0, 1, 0,  0,0}, { h, h,-h,  0, 1, 0,  1,0}, { h, h, h,  0, 1, 0,  1,1}, {-h, h, h,  0, 1, 0,  0,1},
        // -X
        {-h,-h,-h, -1, 0, 0,  0,0}, {-h, h,-h, -1, 0, 0,  1,0}, {-h, h, h, -1, 0, 0,  1,1}, {-h,-h, h, -1, 0, 0,  0,1},
        // +X
        { h,-h,-h,  1, 0, 0,  0,0}, { h, h,-h,  1, 0, 0,  1,0}, { h, h, h,  1, 0, 0,  1,1}, { h,-h, h,  1, 0, 0,  0,1},
    } };
    // clang-format on
}

std::array<uint32_t, 36> BuildCubeIndices()
{
    std::array<uint32_t, 36> indices{};
    for (uint32_t face = 0; face < 6; ++face) {
        uint32_t base = face * 4;
        size_t out = face * 6;
        indices[out + 0] = base + 0;
        indices[out + 1] = base + 1;
        indices[out + 2] = base + 2;
        indices[out + 3] = base + 0;
        indices[out + 4] = base + 2;
        indices[out + 5] = base + 3;
    }
    return indices;
}

std::array<VertexPositionOnlyF32, 8> BuildCubeCornerPoints()
{
    constexpr float h = 0.5f;
    // clang-format off
    return { {
        {-h,-h,-h}, {-h,-h, h}, {-h, h,-h}, {-h, h, h},
        { h,-h,-h}, { h,-h, h}, { h, h,-h}, { h, h, h},
    } };
    // clang-format on
}

} // namespace

std::variant<GeneratedSectionInfo, ImportErrorCode> GenerateSyntheticScene(
    std::span<std::byte> destination, uint64_t generationId, uint32_t sceneVariant,
    uint32_t maxChunkCount)
{
    if (sceneVariant != kSceneVariant_CubeAndPointCluster) {
        return ImportErrorCode::InternalImporterFailure;
    }
    if (maxChunkCount < 2) {
        return ImportErrorCode::ResourceLimit;
    }

    auto vertices = BuildCubeVertices();
    auto indices = BuildCubeIndices();
    auto points = BuildCubeCornerPoints();

    constexpr uint64_t kChunk0PayloadBytes = sizeof(vertices) + sizeof(indices);
    constexpr uint64_t kChunk1PayloadBytes = sizeof(points);
    constexpr uint64_t kDescriptorTableBytes = 2 * kChunkDescriptorSize;

    uint64_t chunk0Offset = kSectionHeaderSize + kDescriptorTableBytes;
    uint64_t chunk1Offset = chunk0Offset + kChunk0PayloadBytes;
    uint64_t sectionLength = chunk1Offset + kChunk1PayloadBytes;

    if (sectionLength > destination.size()) {
        return ImportErrorCode::ResourceLimit;
    }

    // Chunk 0 payload: vertices immediately followed by indices.
    std::memcpy(destination.data() + chunk0Offset, vertices.data(), sizeof(vertices));
    std::memcpy(destination.data() + chunk0Offset + sizeof(vertices), indices.data(), sizeof(indices));

    // Chunk 1 payload: the 8 cube corner points.
    std::memcpy(destination.data() + chunk1Offset, points.data(), sizeof(points));

    ChunkDescriptor chunk0{};
    chunk0.sourceRangeOffset = 0;
    chunk0.sourceRangeLength = kChunk0PayloadBytes;
    chunk0.normalizedRangeOffset = chunk0Offset;
    chunk0.normalizedRangeLength = kChunk0PayloadBytes;
    chunk0.topology = ChunkTopology::TriangleList;
    chunk0.indexCount = static_cast<uint32_t>(indices.size());
    chunk0.vertexCount = static_cast<uint32_t>(vertices.size());
    chunk0.vertexLayoutId = static_cast<uint32_t>(VertexLayoutId::PositionNormalUv0_F32);
    chunk0.lodLevel = 0;
    chunk0.chunkId = 1;
    chunk0.byteSize = kChunk0PayloadBytes;
    chunk0.dependencyCount = 0;
    chunk0.chunkChecksum = WireChecksum64(destination.subspan(chunk0Offset, kChunk0PayloadBytes));

    ChunkDescriptor chunk1{};
    chunk1.sourceRangeOffset = 0;
    chunk1.sourceRangeLength = kChunk1PayloadBytes;
    chunk1.normalizedRangeOffset = chunk1Offset;
    chunk1.normalizedRangeLength = kChunk1PayloadBytes;
    chunk1.topology = ChunkTopology::PointList;
    chunk1.indexCount = 0;
    chunk1.vertexCount = static_cast<uint32_t>(points.size());
    chunk1.vertexLayoutId = static_cast<uint32_t>(VertexLayoutId::PositionOnly_F32);
    chunk1.lodLevel = 1;
    chunk1.chunkId = 2;
    chunk1.byteSize = kChunk1PayloadBytes;
    chunk1.dependencyCount = 1;
    chunk1.dependencyIds[0] = 1; // references chunk0
    chunk1.chunkChecksum = WireChecksum64(destination.subspan(chunk1Offset, kChunk1PayloadBytes));

    SetLocalBounds(chunk0, destination.subspan(size_t(chunk0.normalizedRangeOffset), size_t(chunk0.byteSize)));
    SetLocalBounds(chunk1, destination.subspan(size_t(chunk1.normalizedRangeOffset), size_t(chunk1.byteSize)));
    std::memcpy(destination.data() + kSectionHeaderSize, &chunk0, sizeof(chunk0));
    std::memcpy(destination.data() + kSectionHeaderSize + kChunkDescriptorSize, &chunk1, sizeof(chunk1));

    SectionHeader header{};
    header.magic = kSectionMagic;
    header.protocolVersion = kCurrentProtocolVersion;
    header.generationId = generationId;
    header.scene.generationId = generationId;
    header.sectionLength = sectionLength;
    header.chunkCount = 2;
    header.reserved = 0;
    header.sectionChecksum
        = WireChecksum64(destination.subspan(kSectionHeaderSize, sectionLength - kSectionHeaderSize));

    std::memcpy(destination.data(), &header, sizeof(header));

    GeneratedSectionInfo info;
    info.chunkCount = 2;
    info.sectionBytesWritten = sectionLength;
    return info;
}

} // namespace import_worker
