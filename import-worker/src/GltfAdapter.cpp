#include "GltfAdapter.h"

#include "model_core/Checksum.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"
#include "platform/CheckedMath.h"

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>

#include <cstring>
#include <filesystem>
#include <vector>

namespace import_worker {

namespace {

using namespace model_core;
using platform::CheckedAdd;
using platform::CheckedMultiply;

// No full Tier-A hard-limits table enforcement yet (.docs/design/03-file-formats-and-ingestion.md)
// -- a later refinement once real large fixtures are being tested. These
// are the sanity caps for this slice.
constexpr size_t kMaxVertices = 1'000'000;
constexpr size_t kMaxIndices = 3'000'000;
constexpr int kMaxNodeDepth = 256;

// One fully-assembled chunk's worth of data, held in memory before the
// total section size is known and everything is written out in one pass --
// mirrors SyntheticSceneGenerator's "compute everything, check once, then
// write sequentially, header last" structure.
struct PendingChunk {
    std::vector<VertexPositionNormalUv0F32> vertices;
    std::vector<uint32_t> indices;
};

// This codebase's ImportErrorCode enum is a deliberately small subset of
// the full taxonomy in .docs/design/03-file-formats-and-ingestion.md --
// every fastgltf-reported failure (bad JSON, missing/unrecognized
// extension, bad GLB container, etc.) maps to MalformedData until that enum
// is expanded with finer-grained codes, a known simplification for this
// slice.
ImportErrorCode MapFastgltfError(fastgltf::Error)
{
    return ImportErrorCode::MalformedData;
}

// Builds T*R*S from a node's TRS fields via fastgltf::math's own
// translate/rotate/scale free functions, each confirmed (by reading
// math.hpp directly) to post-multiply the new transform onto the input --
// starting from identity and applying translate, then rotate, then scale in
// that order yields exactly T*R*S, the standard composition. This avoids
// depending on fquat::asMatrix()'s return type/element-access syntax or any
// guessed composition order.
fastgltf::math::fmat4x4 LocalTransform(const fastgltf::Node& node)
{
    if (const auto* trs = std::get_if<fastgltf::TRS>(&node.transform)) {
        fastgltf::math::fmat4x4 local(1.0f);
        local = fastgltf::math::translate(local, trs->translation);
        local = fastgltf::math::rotate(local, trs->rotation);
        local = fastgltf::math::scale(local, trs->scale);
        return local;
    }
    return std::get<fastgltf::math::fmat4x4>(node.transform);
}

// Manual accessor validation, mandatory before every templated fastgltf
// accessor read: fastgltf's iterateAccessor*/copyFromAccessor validate only
// accessor.type via a runtime assert, compiled out under NDEBUG (Release),
// and never independently validate componentType at all. A wrong-typed
// accessor here is a signal of a corrupt/hostile file, not an
// optional-feature gap -- never trust the library's own assert-based
// checks in this worker.
bool ValidateVec3FloatAccessor(const fastgltf::Accessor& accessor)
{
    return accessor.type == fastgltf::AccessorType::Vec3
        && accessor.componentType == fastgltf::ComponentType::Float;
}

bool ValidateVec2FloatAccessor(const fastgltf::Accessor& accessor)
{
    return accessor.type == fastgltf::AccessorType::Vec2
        && accessor.componentType == fastgltf::ComponentType::Float;
}

// iterateAccessor<uint32_t> against an UnsignedByte/UnsignedShort accessor
// is fastgltf's designed up-conversion behavior, not an unsafe read --
// accept any of the three legitimate index component types.
bool ValidateIndexAccessor(const fastgltf::Accessor& accessor)
{
    return accessor.type == fastgltf::AccessorType::Scalar
        && (accessor.componentType == fastgltf::ComponentType::UnsignedByte
            || accessor.componentType == fastgltf::ComponentType::UnsignedShort
            || accessor.componentType == fastgltf::ComponentType::UnsignedInt);
}

// Generates flat per-triangle normals: accumulates each triangle's face
// normal (cross product of two edges) into its three vertices, normalizes
// once at the end. Same algorithm interactive-viewer's Model.cpp uses
// (GenerateNormals), written fresh here -- no linkage to interactive-viewer.
void GenerateFlatNormals(PendingChunk& chunk)
{
    std::vector<fastgltf::math::fvec3> accum(chunk.vertices.size(),
                                              fastgltf::math::fvec3(0.0f, 0.0f, 0.0f));
    for (size_t i = 0; i + 2 < chunk.indices.size(); i += 3) {
        uint32_t i0 = chunk.indices[i];
        uint32_t i1 = chunk.indices[i + 1];
        uint32_t i2 = chunk.indices[i + 2];
        fastgltf::math::fvec3 p0(chunk.vertices[i0].px, chunk.vertices[i0].py, chunk.vertices[i0].pz);
        fastgltf::math::fvec3 p1(chunk.vertices[i1].px, chunk.vertices[i1].py, chunk.vertices[i1].pz);
        fastgltf::math::fvec3 p2(chunk.vertices[i2].px, chunk.vertices[i2].py, chunk.vertices[i2].pz);
        fastgltf::math::fvec3 faceNormal = fastgltf::math::cross(p1 - p0, p2 - p0);
        accum[i0] += faceNormal;
        accum[i1] += faceNormal;
        accum[i2] += faceNormal;
    }
    for (size_t i = 0; i < chunk.vertices.size(); ++i) {
        fastgltf::math::fvec3 n = accum[i];
        float lengthSquared = fastgltf::math::dot(n, n);
        fastgltf::math::fvec3 normalized = lengthSquared > 1e-12f
            ? fastgltf::math::normalize(n)
            : fastgltf::math::fvec3(0.0f, 0.0f, 1.0f);
        chunk.vertices[i].nx = normalized.x();
        chunk.vertices[i].ny = normalized.y();
        chunk.vertices[i].nz = normalized.z();
    }
}

struct WalkState {
    const fastgltf::Asset& asset;
    std::vector<uint8_t> visitState;
    std::vector<PendingChunk> chunks;
    size_t totalVertices = 0;
    size_t totalIndices = 0;
    uint32_t maxChunkCount = 0;
    ImportErrorCode error = ImportErrorCode::None;
};

bool ConvertPrimitive(WalkState& state, const fastgltf::Primitive& primitive,
                       const fastgltf::math::fmat4x4& world, const fastgltf::math::fmat3x3& normalMatrix)
{
    if (primitive.type != fastgltf::PrimitiveType::Triangles) {
        return true; // skip, not fatal -- mirrors Model.cpp's leniency for non-triangle primitives
    }

    auto positionIt = primitive.findAttribute("POSITION");
    if (positionIt == primitive.attributes.end()) {
        return true; // skip, not fatal
    }

    if (state.chunks.size() >= state.maxChunkCount) {
        state.error = ImportErrorCode::ResourceLimit;
        return false;
    }

    const fastgltf::Accessor& positionAccessor = state.asset.accessors[positionIt->accessorIndex];
    if (!ValidateVec3FloatAccessor(positionAccessor)) {
        state.error = ImportErrorCode::MalformedData;
        return false;
    }
    if (positionAccessor.count > kMaxVertices
        || positionAccessor.count > kMaxVertices - state.totalVertices) {
        state.error = ImportErrorCode::ResourceLimit;
        return false;
    }

    if (!primitive.indicesAccessor.has_value()) {
        // Options::GenerateMeshIndices is always requested, so an
        // index-less primitive should never reach here.
        state.error = ImportErrorCode::InternalImporterFailure;
        return false;
    }
    const fastgltf::Accessor& indexAccessor = state.asset.accessors[*primitive.indicesAccessor];
    if (!ValidateIndexAccessor(indexAccessor)) {
        state.error = ImportErrorCode::MalformedData;
        return false;
    }
    if (indexAccessor.count % 3 != 0) {
        state.error = ImportErrorCode::MalformedData;
        return false;
    }
    if (indexAccessor.count > kMaxIndices || indexAccessor.count > kMaxIndices - state.totalIndices) {
        state.error = ImportErrorCode::ResourceLimit;
        return false;
    }

    auto normalIt = primitive.findAttribute("NORMAL");
    bool hasNormal = normalIt != primitive.attributes.end();
    if (hasNormal) {
        const fastgltf::Accessor& normalAccessor = state.asset.accessors[normalIt->accessorIndex];
        if (!ValidateVec3FloatAccessor(normalAccessor) || normalAccessor.count != positionAccessor.count) {
            state.error = ImportErrorCode::MalformedData;
            return false;
        }
    }

    auto uvIt = primitive.findAttribute("TEXCOORD_0");
    bool hasUv = uvIt != primitive.attributes.end();
    if (hasUv) {
        const fastgltf::Accessor& uvAccessor = state.asset.accessors[uvIt->accessorIndex];
        if (!ValidateVec2FloatAccessor(uvAccessor) || uvAccessor.count != positionAccessor.count) {
            state.error = ImportErrorCode::MalformedData;
            return false;
        }
    }

    PendingChunk chunk;
    chunk.vertices.resize(positionAccessor.count);

    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
        state.asset, positionAccessor, [&](fastgltf::math::fvec3 pos, size_t idx) {
            fastgltf::math::fvec4 worldPos
                = world * fastgltf::math::fvec4(pos.x(), pos.y(), pos.z(), 1.0f);
            chunk.vertices[idx].px = worldPos.x();
            chunk.vertices[idx].py = worldPos.y();
            chunk.vertices[idx].pz = worldPos.z();
        });

    if (hasUv) {
        const fastgltf::Accessor& uvAccessor = state.asset.accessors[uvIt->accessorIndex];
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(
            state.asset, uvAccessor, [&](fastgltf::math::fvec2 uv, size_t idx) {
                chunk.vertices[idx].u = uv.x();
                chunk.vertices[idx].v = uv.y();
            });
    } else {
        for (auto& v : chunk.vertices) {
            v.u = 0.0f;
            v.v = 0.0f;
        }
    }

    chunk.indices.resize(indexAccessor.count);
    fastgltf::iterateAccessorWithIndex<uint32_t>(
        state.asset, indexAccessor,
        [&](uint32_t index, size_t idx) { chunk.indices[idx] = index; });

    for (uint32_t index : chunk.indices) {
        if (index >= chunk.vertices.size()) {
            state.error = ImportErrorCode::MalformedData;
            return false;
        }
    }

    if (hasNormal) {
        const fastgltf::Accessor& normalAccessor = state.asset.accessors[normalIt->accessorIndex];
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
            state.asset, normalAccessor, [&](fastgltf::math::fvec3 n, size_t idx) {
                fastgltf::math::fvec3 worldNormal = fastgltf::math::normalize(normalMatrix * n);
                chunk.vertices[idx].nx = worldNormal.x();
                chunk.vertices[idx].ny = worldNormal.y();
                chunk.vertices[idx].nz = worldNormal.z();
            });
    } else {
        GenerateFlatNormals(chunk);
    }

    state.totalVertices += chunk.vertices.size();
    state.totalIndices += chunk.indices.size();
    state.chunks.push_back(std::move(chunk));
    return true;
}

// Walks the node tree from `nodeIndex`, accumulating world transforms and
// converting every triangle-mesh primitive found. Depth-capped and
// cycle-detected (3-state: 0=unvisited, 1=visiting, 2=done) -- only a true
// cycle (revisiting a node with state 1) is an error; a DAG diamond (two
// parents sharing a child) is legally reprocessed, matching
// interactive-viewer's existing Model.cpp::VisitNode semantics.
bool VisitNode(WalkState& state, size_t nodeIndex, const fastgltf::math::fmat4x4& parentWorld,
               int depth)
{
    if (depth > kMaxNodeDepth) {
        state.error = ImportErrorCode::MalformedData;
        return false;
    }
    if (nodeIndex >= state.asset.nodes.size()) {
        state.error = ImportErrorCode::MalformedData;
        return false;
    }
    if (state.visitState[nodeIndex] == 1) {
        state.error = ImportErrorCode::MalformedData; // true cycle
        return false;
    }
    state.visitState[nodeIndex] = 1;

    const fastgltf::Node& node = state.asset.nodes[nodeIndex];
    fastgltf::math::fmat4x4 local = LocalTransform(node);
    fastgltf::math::fmat4x4 world = parentWorld * local;

    if (node.meshIndex.has_value()) {
        if (*node.meshIndex >= state.asset.meshes.size()) {
            state.error = ImportErrorCode::MalformedData;
            return false;
        }
        const fastgltf::Mesh& mesh = state.asset.meshes[*node.meshIndex];
        fastgltf::math::fmat3x3 normalMatrix
            = fastgltf::math::transpose(fastgltf::math::inverse(fastgltf::math::fmat3x3(world)));

        for (const fastgltf::Primitive& primitive : mesh.primitives) {
            if (!ConvertPrimitive(state, primitive, world, normalMatrix)) {
                return false;
            }
        }
    }

    for (size_t child : node.children) {
        if (!VisitNode(state, child, world, depth + 1)) {
            return false;
        }
    }

    state.visitState[nodeIndex] = 2;
    return true;
}

} // namespace

std::variant<GltfImportResult, ImportErrorCode> ImportGltf(std::span<const std::byte> sourceGlbBytes,
                                                             std::span<std::byte> destination,
                                                             uint64_t generationId,
                                                             uint32_t maxChunkCount)
{
    auto dataBufferResult
        = fastgltf::GltfDataBuffer::FromBytes(sourceGlbBytes.data(), sourceGlbBytes.size());
    if (!dataBufferResult) {
        return MapFastgltfError(dataBufferResult.error());
    }

    // Extensions::None: a file requiring one fails cleanly with
    // MissingExtensions/UnknownRequiredExtension rather than being parsed.
    // Never LoadExternalBuffers/LoadExternalImages: the worker has no path
    // authority. LoadGLBBuffers is deprecated in 0.9.0 (now default
    // behaviour) and deliberately not passed.
    fastgltf::Parser parser(fastgltf::Extensions::None);
    auto assetResult = parser.loadGltfBinary(dataBufferResult.get(), std::filesystem::path{},
                                              fastgltf::Options::GenerateMeshIndices);
    if (!assetResult) {
        return MapFastgltfError(assetResult.error());
    }
    const fastgltf::Asset& asset = assetResult.get();

    if (asset.scenes.empty()) {
        return ImportErrorCode::MalformedData;
    }
    size_t sceneIndex = asset.defaultScene.value_or(0);
    if (sceneIndex >= asset.scenes.size()) {
        return ImportErrorCode::MalformedData;
    }

    WalkState state{ asset };
    state.visitState.assign(asset.nodes.size(), 0);
    state.maxChunkCount = maxChunkCount;

    fastgltf::math::fmat4x4 identity(1.0f);
    for (size_t nodeIndex : asset.scenes[sceneIndex].nodeIndices) {
        if (!VisitNode(state, nodeIndex, identity, 0)) {
            return state.error;
        }
    }

    if (state.chunks.empty()) {
        return ImportErrorCode::MalformedData; // no supported geometry found
    }

    // Compute layout and total size before writing anything -- mirrors
    // SyntheticSceneGenerator's "compute everything, check once, then write
    // sequentially, header last" structure.
    auto descriptorTableBytes
        = CheckedMultiply(static_cast<uint64_t>(state.chunks.size()), kChunkDescriptorSize);
    if (!descriptorTableBytes) {
        return ImportErrorCode::ResourceLimit;
    }
    auto headerAndTable = CheckedAdd(kSectionHeaderSize, *descriptorTableBytes);
    if (!headerAndTable) {
        return ImportErrorCode::ResourceLimit;
    }

    std::vector<uint64_t> payloadOffsets(state.chunks.size());
    std::vector<uint64_t> payloadSizes(state.chunks.size());
    uint64_t offset = *headerAndTable;
    for (size_t i = 0; i < state.chunks.size(); ++i) {
        uint64_t vertexBytes = static_cast<uint64_t>(state.chunks[i].vertices.size())
            * sizeof(VertexPositionNormalUv0F32);
        uint64_t indexBytes = static_cast<uint64_t>(state.chunks[i].indices.size()) * sizeof(uint32_t);
        auto payloadSize = CheckedAdd(vertexBytes, indexBytes);
        auto nextOffset = payloadSize ? CheckedAdd(offset, *payloadSize) : std::nullopt;
        if (!payloadSize || !nextOffset) {
            return ImportErrorCode::ResourceLimit;
        }
        payloadOffsets[i] = offset;
        payloadSizes[i] = *payloadSize;
        offset = *nextOffset;
    }
    uint64_t sectionLength = offset;

    if (sectionLength > destination.size()) {
        return ImportErrorCode::ResourceLimit;
    }

    for (size_t i = 0; i < state.chunks.size(); ++i) {
        const PendingChunk& chunk = state.chunks[i];
        uint64_t vertexBytes = chunk.vertices.size() * sizeof(VertexPositionNormalUv0F32);

        std::memcpy(destination.data() + payloadOffsets[i], chunk.vertices.data(), vertexBytes);
        std::memcpy(destination.data() + payloadOffsets[i] + vertexBytes, chunk.indices.data(),
                    chunk.indices.size() * sizeof(uint32_t));

        ChunkDescriptor descriptor{};
        descriptor.sourceRangeOffset = 0;
        descriptor.sourceRangeLength = 0;
        descriptor.normalizedRangeOffset = payloadOffsets[i];
        descriptor.normalizedRangeLength = payloadSizes[i];
        descriptor.topology = ChunkTopology::TriangleList;
        descriptor.indexCount = static_cast<uint32_t>(chunk.indices.size());
        descriptor.vertexCount = static_cast<uint32_t>(chunk.vertices.size());
        descriptor.vertexLayoutId = static_cast<uint32_t>(VertexLayoutId::PositionNormalUv0_F32);
        descriptor.lodLevel = 0;
        descriptor.chunkId = static_cast<uint32_t>(i + 1);
        descriptor.byteSize = payloadSizes[i];
        descriptor.dependencyCount = 0;
        descriptor.chunkChecksum = Fnv1a64(destination.subspan(payloadOffsets[i], payloadSizes[i]));

        std::memcpy(destination.data() + kSectionHeaderSize + i * kChunkDescriptorSize, &descriptor,
                    sizeof(descriptor));
    }

    SectionHeader header{};
    header.magic = kSectionMagic;
    header.protocolVersion = kCurrentProtocolVersion;
    header.generationId = generationId;
    header.sectionLength = sectionLength;
    header.chunkCount = static_cast<uint32_t>(state.chunks.size());
    header.reserved = 0;
    header.sectionChecksum
        = Fnv1a64(destination.subspan(kSectionHeaderSize, sectionLength - kSectionHeaderSize));

    std::memcpy(destination.data(), &header, sizeof(header));

    GltfImportResult result;
    result.chunkCount = static_cast<uint32_t>(state.chunks.size());
    result.sectionBytesWritten = sectionLength;
    return result;
}

} // namespace import_worker
