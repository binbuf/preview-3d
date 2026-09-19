#include "StepHostImport.h"

#include "StepPart21Preflight.h"

#include "model_core/Checksum.h"
#include "model_core/GeometryBounds.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"

#include <array>
#include <cmath>
#include <cstring>
#include <optional>

namespace step_host {
namespace {

using namespace model_core;

// Unit cube centered at the origin, 4 vertices per face (flat per-face
// normals), 6 faces in a fixed -Z/+Z/-Y/+Y/-X/+X order. Identical geometry to
// SyntheticSceneGenerator.cpp so the STEP route exercises the same validated
// geometry path the fast worker already proves.
std::array<VertexPositionNormalUv0F32, 24> BuildCubeVertices()
{
    constexpr float h = 0.5f;
    // clang-format off
    return { {
        {-h,-h,-h,  0, 0,-1,  0,0}, { h,-h,-h,  0, 0,-1,  1,0}, { h, h,-h,  0, 0,-1,  1,1}, {-h, h,-h,  0, 0,-1,  0,1},
        {-h,-h, h,  0, 0, 1,  0,0}, { h,-h, h,  0, 0, 1,  1,0}, { h, h, h,  0, 0, 1,  1,1}, {-h, h, h,  0, 0, 1,  0,1},
        {-h,-h,-h,  0,-1, 0,  0,0}, { h,-h,-h,  0,-1, 0,  1,0}, { h,-h, h,  0,-1, 0,  1,1}, {-h,-h, h,  0,-1, 0,  0,1},
        {-h, h,-h,  0, 1, 0,  0,0}, { h, h,-h,  0, 1, 0,  1,0}, { h, h, h,  0, 1, 0,  1,1}, {-h, h, h,  0, 1, 0,  0,1},
        {-h,-h,-h, -1, 0, 0,  0,0}, {-h, h,-h, -1, 0, 0,  1,0}, {-h, h, h, -1, 0, 0,  1,1}, {-h,-h, h, -1, 0, 0,  0,1},
        { h,-h,-h,  1, 0, 0,  0,0}, { h, h,-h,  1, 0, 0,  1,0}, { h, h, h,  1, 0, 0,  1,1}, { h,-h, h,  1, 0, 0,  0,1},
    } };
    // clang-format on
}

std::array<std::uint32_t, 36> BuildCubeIndices()
{
    std::array<std::uint32_t, 36> indices{};
    for (std::uint32_t face = 0; face < 6; ++face) {
        const std::uint32_t base = face * 4;
        const std::size_t out = face * 6;
        indices[out + 0] = base + 0;
        indices[out + 1] = base + 1;
        indices[out + 2] = base + 2;
        indices[out + 3] = base + 0;
        indices[out + 4] = base + 2;
        indices[out + 5] = base + 3;
    }
    return indices;
}

// Writes a bounded, fully validated synthetic scene: one reusable cube
// geometry, one visible node, and one mesh instance. This is the STEP-002
// placeholder behind the admission boundary; STEP-003 replaces the synthetic
// payload with the accepted XDE traversal.
std::optional<std::pair<std::uint32_t, std::uint64_t>> WriteSyntheticScene(
    std::span<std::byte> destination, std::uint64_t generationId, std::uint32_t maxChunkCount)
{
    const auto vertices = BuildCubeVertices();
    const auto indices = BuildCubeIndices();
    const std::uint64_t geometryBytes = sizeof(vertices) + sizeof(indices);
    const std::uint64_t descriptorTableBytes = 3 * kChunkDescriptorSize;
    const std::uint64_t geometryOffset = kSectionHeaderSize + descriptorTableBytes;
    const std::uint64_t nodeOffset = geometryOffset + geometryBytes;
    const std::uint64_t instanceOffset = nodeOffset + sizeof(NodePayload);
    const std::uint64_t sectionLength = instanceOffset + sizeof(MeshInstancePayload);

    if (maxChunkCount < 3 || sectionLength > destination.size()) return std::nullopt;

    std::memcpy(destination.data() + geometryOffset, vertices.data(), sizeof(vertices));
    std::memcpy(destination.data() + geometryOffset + sizeof(vertices), indices.data(), sizeof(indices));

    ChunkDescriptor geometry{};
    geometry.sourceRangeOffset = 0;
    geometry.sourceRangeLength = geometryBytes;
    geometry.normalizedRangeOffset = geometryOffset;
    geometry.normalizedRangeLength = geometryBytes;
    geometry.topology = ChunkTopology::TriangleList;
    geometry.indexCount = static_cast<std::uint32_t>(indices.size());
    geometry.vertexCount = static_cast<std::uint32_t>(vertices.size());
    geometry.vertexLayoutId = static_cast<std::uint32_t>(VertexLayoutId::PositionNormalUv0_F32);
    geometry.lodLevel = kFineLod;
    geometry.chunkId = 1;
    geometry.byteSize = geometryBytes;
    geometry.dependencyCount = 0;
    geometry.meshId = 1;
    geometry.nodeId = 0;
    geometry.geometryFlags = 0;
    geometry.boundsState = BoundsState::Verified;
    geometry.chunkChecksum = WireChecksum64(destination.subspan(geometryOffset, geometryBytes));
    SetLocalBounds(geometry, destination.subspan(static_cast<std::size_t>(geometryOffset),
                                                  static_cast<std::size_t>(geometryBytes)));

    NodePayload node{};
    node.nodeId = 2;
    node.parentNodeId = 0;
    node.flags = kSceneRecordVisible;
    node.localTransform[0] = 1.0; node.localTransform[5] = 1.0;
    node.localTransform[10] = 1.0; node.localTransform[15] = 1.0;
    std::memcpy(destination.data() + nodeOffset, &node, sizeof(node));

    MeshInstancePayload instance{};
    instance.instanceId = 3;
    instance.nodeId = 2;
    instance.geometryChunkId = 1;
    instance.materialChunkId = 0;
    instance.flags = kSceneRecordVisible;
    for (unsigned axis = 0; axis < 3; ++axis) {
        instance.worldMin[axis] = geometry.origin[axis] + geometry.localMin[axis];
        instance.worldMax[axis] = geometry.origin[axis] + geometry.localMax[axis];
    }
    std::memcpy(destination.data() + instanceOffset, &instance, sizeof(instance));

    ChunkDescriptor nodeChunk{};
    nodeChunk.topology = ChunkTopology::Node;
    nodeChunk.chunkId = 2;
    nodeChunk.normalizedRangeOffset = nodeOffset;
    nodeChunk.normalizedRangeLength = sizeof(node);
    nodeChunk.byteSize = sizeof(node);
    nodeChunk.chunkChecksum = WireChecksum64(destination.subspan(nodeOffset, sizeof(node)));

    ChunkDescriptor instanceChunk{};
    instanceChunk.topology = ChunkTopology::MeshInstance;
    instanceChunk.chunkId = 3;
    instanceChunk.normalizedRangeOffset = instanceOffset;
    instanceChunk.normalizedRangeLength = sizeof(instance);
    instanceChunk.byteSize = sizeof(instance);
    instanceChunk.dependencyIds[0] = 1;
    instanceChunk.dependencyIds[2] = 2;
    instanceChunk.dependencyCount = 2;
    instanceChunk.chunkChecksum = WireChecksum64(destination.subspan(instanceOffset, sizeof(instance)));

    std::memcpy(destination.data() + kSectionHeaderSize, &geometry, sizeof(geometry));
    std::memcpy(destination.data() + kSectionHeaderSize + kChunkDescriptorSize, &nodeChunk, sizeof(nodeChunk));
    std::memcpy(destination.data() + kSectionHeaderSize + 2 * kChunkDescriptorSize, &instanceChunk, sizeof(instanceChunk));

    SectionHeader header{};
    header.magic = kSectionMagic;
    header.protocolVersion = kCurrentProtocolVersion;
    header.generationId = generationId;
    header.sectionLength = sectionLength;
    header.chunkCount = 3;
    header.reserved = 0;
    header.scene.generationId = generationId;
    header.scene.format = SourceFormatId::Step;
    header.scene.upAxis = UpAxisId::Unknown;
    header.scene.metersPerUnit = 1.0;
    header.scene.meshCount = 1;
    header.scene.nodeCount = 1;
    header.scene.animationCount = 0;
    header.scene.skinCount = 0;
    header.scene.boneCount = 0;
    header.scene.reserved = 0;
    header.sectionChecksum = WireChecksum64(
        destination.subspan(kSectionHeaderSize, static_cast<std::size_t>(sectionLength - kSectionHeaderSize)));
    std::memcpy(destination.data(), &header, sizeof(header));

    return std::make_pair(std::uint32_t(3), sectionLength);
}

bool IsCancelled(HANDLE cancellationEvent)
{
    return cancellationEvent && cancellationEvent != INVALID_HANDLE_VALUE
        && WaitForSingleObject(cancellationEvent, 0) == WAIT_OBJECT_0;
}

} // namespace

model_core::ImportErrorCode MapPreflightStatus(std::uint32_t status)
{
    switch (static_cast<StepPreflightStatus>(status)) {
    case StepPreflightStatus::Ok:
        return model_core::ImportErrorCode::None;
    case StepPreflightStatus::NotPart21:
    case StepPreflightStatus::MalformedSyntax:
    case StepPreflightStatus::DuplicateEntity:
        return model_core::ImportErrorCode::MalformedData;
    case StepPreflightStatus::UnsupportedEncoding:
        return model_core::ImportErrorCode::UnsupportedEncoding;
    case StepPreflightStatus::EntityLimit:
    case StepPreflightStatus::ReferenceLimit:
    case StepPreflightStatus::DepthLimit:
    case StepPreflightStatus::RecordLengthLimit:
    case StepPreflightStatus::StringLengthLimit:
    case StepPreflightStatus::SourceLimit:
        return model_core::ImportErrorCode::ResourceLimit;
    case StepPreflightStatus::ExternalDocument:
        return model_core::ImportErrorCode::UnsupportedRequiredFeature;
    case StepPreflightStatus::ReadFailure:
        return model_core::ImportErrorCode::FileUnavailable;
    }
    return model_core::ImportErrorCode::InternalImporterFailure;
}

StepImportResult RunStepHostImport(const model_core::ParseStepFileRequest& request,
                                   std::span<std::byte> section, HANDLE sourceHandle,
                                   HANDLE cancellationEvent)
{
    StepImportResult result;
    if (!request.generationId || !request.sourceFileHandleValue || !request.sectionHandleValue
        || request.sectionByteCapacity < sizeof(SectionHeader)
        || section.size() < static_cast<std::size_t>(request.sectionByteCapacity)
        || !request.maxChunkCount) {
        result.errorCode = model_core::ImportErrorCode::ImportProtocolViolation;
        return result;
    }
    if (IsCancelled(cancellationEvent)) {
        result.errorCode = model_core::ImportErrorCode::Cancelled;
        return result;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(sourceHandle, &size) || size.QuadPart <= 0) {
        result.errorCode = model_core::ImportErrorCode::FileUnavailable;
        return result;
    }

    const StepPreflightResult preflight
        = StepPreflightHandle(sourceHandle, static_cast<std::uint64_t>(size.QuadPart));
    if (!preflight.ok()) {
        result.errorCode = MapPreflightStatus(static_cast<std::uint32_t>(preflight.status));
        return result;
    }
    if (IsCancelled(cancellationEvent)) {
        result.errorCode = model_core::ImportErrorCode::Cancelled;
        return result;
    }

    // STEP-003 replaces this synthetic payload with the accepted XDE traversal.
    const auto scene = WriteSyntheticScene(section, request.generationId, request.maxChunkCount);
    if (!scene) {
        result.errorCode = model_core::ImportErrorCode::ResourceLimit;
        return result;
    }
    result.chunkCount = scene->first;
    result.sectionBytesWritten = scene->second;
    return result;
}

} // namespace step_host
