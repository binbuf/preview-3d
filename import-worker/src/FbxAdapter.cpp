#include "FbxAdapter.h"

#include "BoundedChunkWriter.h"
#include "model_core/GeometryBounds.h"
#include "model_core/MaterialPayload.h"
#include "model_core/TierALimits.h"
#include "model_core/VertexLayouts.h"
#include "ufbx.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

namespace import_worker {
namespace {

using namespace model_core;

struct SceneDeleter { void operator()(ufbx_scene* scene) const noexcept { ufbx_free_scene(scene); } };
using ScenePtr = std::unique_ptr<ufbx_scene, SceneDeleter>;

FbxImportOutcome Fail(ImportErrorCode code,
                      ImportFailurePhase phase = ImportFailurePhase::Geometry)
{
    return FbxImportFailure{code, phase};
}

ufbx_progress_result CheckProgress(void* user, const ufbx_progress*)
{
    return static_cast<const FbxImportOptions*>(user)->Cancelled()
        ? UFBX_PROGRESS_CANCEL : UFBX_PROGRESS_CONTINUE;
}

bool DenyExternalFile(void*, ufbx_stream*, const char*, size_t, const ufbx_open_file_info*)
{
    return false;
}

ImportErrorCode MapLoadError(const ufbx_error& error)
{
    switch (error.type) {
    case UFBX_ERROR_EMPTY_FILE: return ImportErrorCode::EmptyGeometry;
    case UFBX_ERROR_OUT_OF_MEMORY: return ImportErrorCode::OutOfMemory;
    case UFBX_ERROR_MEMORY_LIMIT:
    case UFBX_ERROR_ALLOCATION_LIMIT: return ImportErrorCode::ScratchLimit;
    case UFBX_ERROR_CANCELLED: return ImportErrorCode::Cancelled;
    case UFBX_ERROR_NODE_DEPTH_LIMIT: return ImportErrorCode::ResourceLimit;
    default: return ImportErrorCode::MalformedData;
    }
}

bool Finite(double value)
{
    return std::isfinite(value) && std::abs(value) <= 1.0e30;
}

bool ToFloat(double value, float& result)
{
    if (!Finite(value) || value < -(std::numeric_limits<float>::max)()
        || value > (std::numeric_limits<float>::max)()) return false;
    result = static_cast<float>(value);
    return std::isfinite(result);
}

bool Normalize(ufbx_vec3 value, float out[3])
{
    const double length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    if (!Finite(length) || length <= 1.0e-20) return false;
    return ToFloat(value.x / length, out[0]) && ToFloat(value.y / length, out[1])
        && ToFloat(value.z / length, out[2]);
}

float Saturate(double value)
{
    return static_cast<float>(std::clamp(value, 0.0, 1.0));
}

void GenerateTriangleTangent(VertexPositionNormalUv0TangentColorF32* vertices)
{
    const float e1[3]{vertices[1].px - vertices[0].px, vertices[1].py - vertices[0].py,
                      vertices[1].pz - vertices[0].pz};
    const float e2[3]{vertices[2].px - vertices[0].px, vertices[2].py - vertices[0].py,
                      vertices[2].pz - vertices[0].pz};
    const float du1 = vertices[1].u - vertices[0].u, dv1 = vertices[1].v - vertices[0].v;
    const float du2 = vertices[2].u - vertices[0].u, dv2 = vertices[2].v - vertices[0].v;
    const float determinant = du1 * dv2 - du2 * dv1;
    float tangent[3]{1.0f, 0.0f, 0.0f}, handedness = 1.0f;
    if (std::abs(determinant) > 1.0e-12f) {
        const float inverse = 1.0f / determinant;
        for (uint32_t axis = 0; axis < 3; ++axis)
            tangent[axis] = (e1[axis] * dv2 - e2[axis] * dv1) * inverse;
        const float length = std::sqrt(tangent[0] * tangent[0] + tangent[1] * tangent[1]
                                       + tangent[2] * tangent[2]);
        if (length > 1.0e-12f)
            for (float& value : tangent) value /= length;
        const float bitangent[3]{(e2[0] * du1 - e1[0] * du2) * inverse,
                                 (e2[1] * du1 - e1[1] * du2) * inverse,
                                 (e2[2] * du1 - e1[2] * du2) * inverse};
        const float cross[3]{vertices[0].ny * tangent[2] - vertices[0].nz * tangent[1],
                             vertices[0].nz * tangent[0] - vertices[0].nx * tangent[2],
                             vertices[0].nx * tangent[1] - vertices[0].ny * tangent[0]};
        handedness = cross[0] * bitangent[0] + cross[1] * bitangent[1]
            + cross[2] * bitangent[2] < 0.0f ? -1.0f : 1.0f;
    }
    for (uint32_t index = 0; index < 3; ++index) {
        vertices[index].tx = tangent[0]; vertices[index].ty = tangent[1];
        vertices[index].tz = tangent[2]; vertices[index].tw = handedness;
    }
}

void MatrixToWire(const ufbx_matrix& source, double destination[16])
{
    std::fill(destination, destination + 16, 0.0);
    destination[0] = source.m00; destination[1] = source.m10; destination[2] = source.m20;
    destination[4] = source.m01; destination[5] = source.m11; destination[6] = source.m21;
    destination[8] = source.m02; destination[9] = source.m12; destination[10] = source.m22;
    destination[12] = source.m03; destination[13] = source.m13; destination[14] = source.m23;
    destination[15] = 1.0;
}

bool ValidMatrix(const double matrix[16])
{
    for (uint32_t index = 0; index < 16; ++index)
        if (!Finite(matrix[index])) return false;
    const double determinant =
        matrix[0] * (matrix[5] * matrix[10] - matrix[6] * matrix[9])
        - matrix[1] * (matrix[4] * matrix[10] - matrix[6] * matrix[8])
        + matrix[2] * (matrix[4] * matrix[9] - matrix[5] * matrix[8]);
    return Finite(determinant) && std::abs(determinant) >= 1.0e-18;
}

bool SameMatrix(const ufbx_matrix& left, const ufbx_matrix& right)
{
    for (uint32_t index = 0; index < 12; ++index)
        if (left.v[index] != right.v[index]) return false;
    return true;
}

double MatrixDeterminant(const ufbx_matrix& matrix)
{
    return matrix.m00 * (matrix.m11 * matrix.m22 - matrix.m12 * matrix.m21)
        - matrix.m01 * (matrix.m10 * matrix.m22 - matrix.m12 * matrix.m20)
        + matrix.m02 * (matrix.m10 * matrix.m21 - matrix.m11 * matrix.m20);
}

struct GeometryRecord {
    uint32_t chunkId = 0;
    uint32_t partIndex = 0;
    ChunkDescriptor descriptor{};
};

struct MeshVariant {
    const ufbx_mesh* mesh = nullptr;
    ufbx_matrix geometryToNode{};
    std::vector<const ufbx_node*> nodes;
    std::vector<GeometryRecord> geometry;
};

struct GeometryChunk {
    ChunkDescriptor descriptor{};
    std::vector<VertexPositionNormalUv0TangentColorF32> vertices;
    std::vector<uint32_t> indices;
    bool hasOrigin = false;
};

bool AddVertex(GeometryChunk& chunk, const ufbx_mesh& mesh, const ufbx_matrix& transform,
               const ufbx_matrix& normalTransform, uint32_t sourceIndex)
{
    if (sourceIndex >= mesh.num_indices) return false;
    const ufbx_vec3 position = ufbx_transform_position(
        &transform, ufbx_get_vertex_vec3(&mesh.vertex_position, sourceIndex));
    const ufbx_vec3 normal = ufbx_transform_direction(
        &normalTransform, ufbx_get_vertex_vec3(&mesh.vertex_normal, sourceIndex));
    if (!Finite(position.x) || !Finite(position.y) || !Finite(position.z)
        || !Finite(normal.x) || !Finite(normal.y) || !Finite(normal.z)) return false;

    if (!chunk.hasOrigin) {
        chunk.descriptor.origin[0] = position.x;
        chunk.descriptor.origin[1] = position.y;
        chunk.descriptor.origin[2] = position.z;
        chunk.hasOrigin = true;
    }
    VertexPositionNormalUv0TangentColorF32 vertex{};
    if (!ToFloat(position.x - chunk.descriptor.origin[0], vertex.px)
        || !ToFloat(position.y - chunk.descriptor.origin[1], vertex.py)
        || !ToFloat(position.z - chunk.descriptor.origin[2], vertex.pz)) return false;
    float normalized[3]{};
    if (!Normalize(normal, normalized)) return false;
    vertex.nx = normalized[0]; vertex.ny = normalized[1]; vertex.nz = normalized[2];
    if (mesh.vertex_uv.exists) {
        const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh.vertex_uv, sourceIndex);
        if (!ToFloat(uv.x, vertex.u) || !ToFloat(uv.y, vertex.v)) return false;
    }
    vertex.tx = vertex.tw = 1.0f;
    vertex.r = vertex.g = vertex.b = vertex.a = 1.0f;
    if (mesh.vertex_color.exists) {
        const ufbx_vec4 color = ufbx_get_vertex_vec4(&mesh.vertex_color, sourceIndex);
        if (!Finite(color.x) || !Finite(color.y) || !Finite(color.z) || !Finite(color.w)) return false;
        vertex.r = Saturate(color.x); vertex.g = Saturate(color.y);
        vertex.b = Saturate(color.z); vertex.a = Saturate(color.w);
    }
    if (chunk.vertices.size() >= UINT32_MAX) return false;
    chunk.indices.push_back(static_cast<uint32_t>(chunk.vertices.size()));
    chunk.vertices.push_back(vertex);
    return true;
}

bool FlushGeometry(BoundedChunkWriter& writer, GeometryChunk& chunk, uint32_t meshId,
                   uint32_t partIndex, uint32_t sourceFirst, bool hasUv, bool hasColor,
                   GeometryRecord& record)
{
    if (chunk.indices.empty()) return true;
    auto& descriptor = chunk.descriptor;
    descriptor.topology = ChunkTopology::TriangleList;
    descriptor.vertexLayoutId = uint32_t(VertexLayoutId::PositionNormalUv0TangentColor_F32);
    descriptor.vertexCount = static_cast<uint32_t>(chunk.vertices.size());
    descriptor.indexCount = static_cast<uint32_t>(chunk.indices.size());
    descriptor.chunkId = writer.NextId();
    descriptor.meshId = meshId;
    descriptor.boundsState = BoundsState::Verified;
    descriptor.geometryFlags = kGeometryDeindexed | kGeometryReusableInstanceSource
        | (hasUv ? kGeometryHasUv0 : 0u) | (hasColor ? kGeometryHasColors : 0u);
    descriptor.sourceRangeOffset = (uint64_t(meshId) << 32) | sourceFirst;
    descriptor.sourceRangeLength = descriptor.indexCount;
    if (!SetLocalBounds(descriptor, ChunkBytes(chunk.vertices))) return false;
    if (!writer.Add(descriptor, ChunkBytes(chunk.vertices), ChunkBytes(chunk.indices))) return false;
    record.chunkId = descriptor.chunkId;
    record.partIndex = partIndex;
    record.descriptor = descriptor;
    return true;
}

bool EmitVariant(BoundedChunkWriter& writer, MeshVariant& variant, uint32_t meshOrdinal,
                 uint32_t chunkTriangleLimit, const FbxImportOptions& options,
                 uint64_t& normalizedTriangles, uint64_t& normalizedVertices,
                 ImportErrorCode& error)
{
    const ufbx_mesh& mesh = *variant.mesh;
    const ufbx_matrix normalTransform = ufbx_matrix_for_normals(&variant.geometryToNode);
    const double geometryDeterminant = MatrixDeterminant(variant.geometryToNode);
    if (!Finite(geometryDeterminant) || std::abs(geometryDeterminant) < 1.0e-18) {
        error = ImportErrorCode::MalformedData; return false;
    }
    const bool reverseWinding = mesh.reversed_winding != (geometryDeterminant < 0.0);
    uint32_t sourceCursor = 0;
    for (size_t partOffset = 0; partOffset < mesh.material_parts.count; ++partOffset) {
        const ufbx_mesh_part& part = mesh.material_parts.data[partOffset];
        if (part.index >= kTierBMaterialLimit) { error = ImportErrorCode::ResourceLimit; return false; }
        GeometryChunk chunk;
        chunk.vertices.reserve(size_t(chunkTriangleLimit) * 3);
        chunk.indices.reserve(size_t(chunkTriangleLimit) * 3);
        uint32_t sourceFirst = sourceCursor;
        for (size_t faceOffset = 0; faceOffset < part.face_indices.count; ++faceOffset) {
            if ((faceOffset & 255u) == 0 && options.Cancelled()) {
                error = ImportErrorCode::Cancelled; return false;
            }
            const uint32_t faceIndex = part.face_indices.data[faceOffset];
            if (faceIndex >= mesh.faces.count) { error = ImportErrorCode::MalformedData; return false; }
            if (mesh.face_hole.count && mesh.face_hole.data[faceIndex]) continue;
            const ufbx_face face = mesh.faces.data[faceIndex];
            if (face.num_indices < 3) continue;
            const uint64_t required = uint64_t(face.num_indices - 2) * 3;
            if (required > uint64_t(chunkTriangleLimit) * 3 || required > SIZE_MAX) {
                error = ImportErrorCode::ResourceLimit; return false;
            }
            std::vector<uint32_t> triangles(static_cast<size_t>(required));
            const uint32_t triangleCount = ufbx_triangulate_face(
                triangles.data(), triangles.size(), &mesh, face);
            if (triangleCount != face.num_indices - 2) {
                error = ImportErrorCode::MalformedData; return false;
            }
            for (uint32_t triangle = 0; triangle < triangleCount; ++triangle) {
                if (chunk.indices.size() / 3 >= chunkTriangleLimit) {
                    GeometryRecord record;
                    if (!FlushGeometry(writer, chunk, meshOrdinal, part.index, sourceFirst,
                                       mesh.vertex_uv.exists, mesh.vertex_color.exists, record)) {
                        error = options.Cancelled() ? ImportErrorCode::Cancelled
                            : writer.Error() == ImportErrorCode::None
                                ? ImportErrorCode::MalformedData : writer.Error();
                        return false;
                    }
                    variant.geometry.push_back(record);
                    chunk = {};
                    chunk.vertices.reserve(size_t(chunkTriangleLimit) * 3);
                    chunk.indices.reserve(size_t(chunkTriangleLimit) * 3);
                    sourceFirst = sourceCursor;
                }
                uint32_t corners[3]{triangles[triangle * 3], triangles[triangle * 3 + 1],
                                    triangles[triangle * 3 + 2]};
                if (reverseWinding) std::swap(corners[1], corners[2]);
                const size_t firstVertex = chunk.vertices.size();
                for (uint32_t corner : corners) {
                    if (!AddVertex(chunk, mesh, variant.geometryToNode, normalTransform, corner)) {
                        error = ImportErrorCode::MalformedData; return false;
                    }
                }
                GenerateTriangleTangent(chunk.vertices.data() + firstVertex);
                sourceCursor += 3;
                ++normalizedTriangles;
                normalizedVertices += 3;
                if (normalizedTriangles > kTierBTriangleLimit
                    || normalizedVertices > kTierBVertexLimit) {
                    error = ImportErrorCode::ResourceLimit; return false;
                }
            }
        }
        if (!chunk.indices.empty()) {
            GeometryRecord record;
            if (!FlushGeometry(writer, chunk, meshOrdinal, part.index, sourceFirst,
                               mesh.vertex_uv.exists, mesh.vertex_color.exists, record)) {
                error = options.Cancelled() ? ImportErrorCode::Cancelled
                    : writer.Error() == ImportErrorCode::None
                        ? ImportErrorCode::MalformedData : writer.Error();
                return false;
            }
            variant.geometry.push_back(record);
        }
    }
    return true;
}

bool TransformBounds(const ChunkDescriptor& geometry, const double world[16],
                     double minimum[3], double maximum[3])
{
    std::fill(minimum, minimum + 3, (std::numeric_limits<double>::max)());
    std::fill(maximum, maximum + 3, -(std::numeric_limits<double>::max)());
    for (uint32_t corner = 0; corner < 8; ++corner) {
        const double point[3]{
            geometry.origin[0] + (corner & 1 ? geometry.localMax[0] : geometry.localMin[0]),
            geometry.origin[1] + (corner & 2 ? geometry.localMax[1] : geometry.localMin[1]),
            geometry.origin[2] + (corner & 4 ? geometry.localMax[2] : geometry.localMin[2]),
        };
        for (uint32_t axis = 0; axis < 3; ++axis) {
            const double value = point[0] * world[axis] + point[1] * world[4 + axis]
                + point[2] * world[8 + axis] + world[12 + axis];
            if (!Finite(value)) return false;
            minimum[axis] = (std::min)(minimum[axis], value);
            maximum[axis] = (std::max)(maximum[axis], value);
        }
    }
    return true;
}

} // namespace

FbxImportOutcome ImportFbx(std::span<const std::byte> sourceBytes,
                           std::span<std::byte> destination, uint64_t generationId,
                           uint32_t maxChunkCount, ChunkBatchSink* batchSink,
                           const FbxImportOptions& importOptions)
{
    if (sourceBytes.empty()) return Fail(ImportErrorCode::EmptyGeometry);
    if (sourceBytes.size() > kTierBPrimarySourceBytes)
        return Fail(ImportErrorCode::PrimarySourceLimit);
    if (importOptions.Cancelled()) return Fail(ImportErrorCode::Cancelled);
    const uint64_t scratchLimit = TierBScratchLimit();
    if (!scratchLimit || scratchLimit / 2 > SIZE_MAX) return Fail(ImportErrorCode::ScratchLimit);

    ufbx_load_opts options{};
    options.file_format = UFBX_FILE_FORMAT_FBX;
    options.no_format_from_content = true;
    options.no_format_from_extension = true;
    options.filename = {"document.fbx", 12};
    options.ignore_animation = true;
    options.ignore_embedded = true;
    options.evaluate_skinning = false;
    options.evaluate_caches = false;
    options.load_external_files = false;
    options.ignore_missing_external_files = true;
    options.skip_skin_vertices = true;
    options.strict = true;
    options.force_single_thread_ascii_parsing = true;
    options.generate_missing_normals = true;
    options.normalize_normals = true;
    options.normalize_tangents = true;
    options.index_error_handling = UFBX_INDEX_ERROR_HANDLING_ABORT_LOADING;
    options.unicode_error_handling = UFBX_UNICODE_ERROR_HANDLING_ABORT_LOADING;
    options.node_depth_limit = kMaxSceneHierarchyDepth;
    options.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_PRESERVE;
    options.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_HELPER_NODES;
    options.pivot_handling = UFBX_PIVOT_HANDLING_RETAIN;
    options.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    options.target_axes = ufbx_axes_right_handed_y_up;
    options.target_unit_meters = 1.0;
    options.temp_allocator.memory_limit = static_cast<size_t>(scratchLimit / 2);
    options.result_allocator.memory_limit = static_cast<size_t>(scratchLimit / 2);
    options.temp_allocator.allocation_limit = 1'000'000;
    options.result_allocator.allocation_limit = 1'000'000;
    options.open_file_cb.fn = DenyExternalFile;
    options.progress_cb.fn = CheckProgress;
    options.progress_cb.user = const_cast<FbxImportOptions*>(&importOptions);
    options.progress_interval_hint = 1u << 20;

    ufbx_error loadError{};
    ScenePtr scene(ufbx_load_memory(sourceBytes.data(), sourceBytes.size(), &options, &loadError));
    if (!scene) return Fail(MapLoadError(loadError));
    if (importOptions.Cancelled()) return Fail(ImportErrorCode::Cancelled);
    if (scene->metadata.file_format != UFBX_FILE_FORMAT_FBX)
        return Fail(ImportErrorCode::UnsupportedFormat);
    if (scene->nodes.count > kTierBObjectLimit || scene->meshes.count > kTierBObjectLimit
        || scene->materials.count > kTierBMaterialLimit)
        return Fail(ImportErrorCode::ResourceLimit);
    if (scene->skin_deformers.count || scene->blend_deformers.count)
        return Fail(ImportErrorCode::UnsupportedRequiredFeature);

    uint64_t sourceTriangles = 0, sourceVertices = 0;
    for (const ufbx_mesh* mesh : scene->meshes) {
        if (!mesh || mesh->num_triangles > kTierBTriangleLimit - sourceTriangles
            || mesh->num_vertices > kTierBVertexLimit - sourceVertices
            || mesh->num_indices > kTierBIndexLimit
            || mesh->max_face_triangles > kChunkTriangles)
            return Fail(ImportErrorCode::ResourceLimit);
        sourceTriangles += mesh->num_triangles;
        sourceVertices += mesh->num_vertices;
    }
    const bool omittedGeometry = scene->cache_deformers.count || scene->cache_files.count
        || scene->constraints.count || scene->nurbs_curves.count || scene->nurbs_surfaces.count
        || scene->procedural_geometries.count;
    if (!sourceTriangles)
        return Fail(omittedGeometry ? ImportErrorCode::UnsupportedRequiredFeature
                                    : ImportErrorCode::EmptyGeometry);

    SceneMetadata metadata{};
    metadata.generationId = generationId;
    metadata.format = SourceFormatId::Fbx;
    metadata.upAxis = UpAxisId::Y;
    metadata.metersPerUnit = 1.0;
    metadata.meshCount = static_cast<uint32_t>(scene->meshes.count);
    metadata.nodeCount = static_cast<uint32_t>(scene->nodes.count);
    BoundedChunkWriter writer(destination, generationId, maxChunkCount, metadata, batchSink);
    if (destination.size() <= kSectionHeaderSize + kChunkDescriptorSize)
        return Fail(ImportErrorCode::ResourceLimit);
    constexpr uint64_t triangleBytes = 3 * sizeof(VertexPositionNormalUv0TangentColorF32)
        + 3 * sizeof(uint32_t);
    const uint32_t chunkTriangleLimit = static_cast<uint32_t>((std::min<uint64_t>)(
        kChunkTriangles, (destination.size() - kSectionHeaderSize - kChunkDescriptorSize)
        / triangleBytes));
    if (!chunkTriangleLimit) return Fail(ImportErrorCode::ResourceLimit);

    MaterialPayload neutral{};
    neutral.baseColorFactor[0] = neutral.baseColorFactor[1] = neutral.baseColorFactor[2] = 0.8f;
    neutral.baseColorFactor[3] = 1.0f;
    neutral.roughnessFactor = 1.0f;
    neutral.uvScale[0] = neutral.uvScale[1] = 1.0f;
    neutral.alphaMode = uint32_t(AlphaModeId::Opaque);
    neutral.alphaCutoff = 0.5f;
    neutral.flags = kMaterialFlagDoubleSided;
    ChunkDescriptor materialDescriptor{};
    materialDescriptor.topology = ChunkTopology::Material;
    materialDescriptor.chunkId = writer.NextId();
    if (!writer.Add(materialDescriptor, ChunkBytes(neutral))) return Fail(writer.Error());
    const uint32_t neutralMaterialId = materialDescriptor.chunkId;

    std::vector<MeshVariant> variants;
    variants.reserve(scene->nodes.count);
    for (const ufbx_node* node : scene->nodes) {
        if (!node || !node->mesh || !node->mesh->num_triangles) continue;
        auto found = std::find_if(variants.begin(), variants.end(), [&](const MeshVariant& variant) {
            return variant.mesh == node->mesh && SameMatrix(variant.geometryToNode, node->geometry_to_node);
        });
        if (found == variants.end()) {
            if (variants.size() >= kTierBObjectLimit) return Fail(ImportErrorCode::ResourceLimit);
            variants.push_back({node->mesh, node->geometry_to_node});
            found = std::prev(variants.end());
        }
        found->nodes.push_back(node);
    }

    uint64_t normalizedTriangles = 0, normalizedVertices = 0;
    ImportErrorCode emitError = ImportErrorCode::None;
    for (MeshVariant& variant : variants) {
        const uint32_t meshOrdinal = static_cast<uint32_t>(variant.mesh->typed_id);
        if (meshOrdinal >= kTierBObjectLimit
            || !EmitVariant(writer, variant, meshOrdinal, chunkTriangleLimit, importOptions,
                            normalizedTriangles, normalizedVertices, emitError))
            return Fail(emitError == ImportErrorCode::None ? ImportErrorCode::ResourceLimit : emitError);
    }
    if (!normalizedTriangles) return Fail(ImportErrorCode::EmptyGeometry);

    std::vector<const ufbx_node*> orderedNodes(scene->nodes.begin(), scene->nodes.end());
    std::stable_sort(orderedNodes.begin(), orderedNodes.end(), [](const ufbx_node* left,
                                                                  const ufbx_node* right) {
        return left->node_depth < right->node_depth;
    });
    const uint32_t firstNodeId = writer.NextId();
    if (uint64_t(firstNodeId) + orderedNodes.size() > UINT32_MAX)
        return Fail(ImportErrorCode::ChunkCatalogLimit);
    std::unordered_map<const ufbx_node*, uint32_t> nodeIds;
    nodeIds.reserve(orderedNodes.size());
    for (size_t index = 0; index < orderedNodes.size(); ++index)
        nodeIds.emplace(orderedNodes[index], firstNodeId + static_cast<uint32_t>(index));
    for (const ufbx_node* node : orderedNodes) {
        if (importOptions.Cancelled()) return Fail(ImportErrorCode::Cancelled);
        NodePayload payload{};
        payload.nodeId = nodeIds.at(node);
        if (node->parent) {
            const auto parent = nodeIds.find(node->parent);
            if (parent == nodeIds.end()) return Fail(ImportErrorCode::MalformedData);
            payload.parentNodeId = parent->second;
        }
        payload.flags = node->visible ? kSceneRecordVisible : 0;
        MatrixToWire(node->node_to_parent, payload.localTransform);
        if (!ValidMatrix(payload.localTransform)) return Fail(ImportErrorCode::MalformedData);
        if (!writer.AddNode(payload))
            return Fail(importOptions.Cancelled() ? ImportErrorCode::Cancelled : writer.Error());
    }

    uint64_t instanceCount = 0;
    for (const MeshVariant& variant : variants) {
        for (const ufbx_node* node : variant.nodes) {
            double world[16]{};
            MatrixToWire(node->node_to_world, world);
            if (!ValidMatrix(world)) return Fail(ImportErrorCode::MalformedData);
            for (const GeometryRecord& geometry : variant.geometry) {
                if (++instanceCount > kTierBObjectLimit) return Fail(ImportErrorCode::ResourceLimit);
                if (importOptions.Cancelled()) return Fail(ImportErrorCode::Cancelled);
                MeshInstancePayload payload{};
                payload.instanceId = writer.NextId();
                payload.nodeId = nodeIds.at(node);
                payload.geometryChunkId = geometry.chunkId;
                payload.materialChunkId = neutralMaterialId;
                payload.flags = kSceneRecordVisible;
                if (!TransformBounds(geometry.descriptor, world, payload.worldMin, payload.worldMax))
                    return Fail(ImportErrorCode::MalformedData);
                if (!writer.AddInstance(payload))
                    return Fail(importOptions.Cancelled() ? ImportErrorCode::Cancelled : writer.Error());
            }
        }
    }

    const uint32_t optionalWarnings = static_cast<uint32_t>((std::min)(
        size_t(64), scene->metadata.warnings.count + size_t(omittedGeometry ? 1 : 0)));
    if (optionalWarnings) {
        ImportStatusPayload status{};
        status.optionalFeatureWarnings = optionalWarnings;
        ChunkDescriptor descriptor{};
        descriptor.topology = ChunkTopology::ImportStatus;
        descriptor.chunkId = writer.NextId();
        if (!writer.Add(descriptor, ChunkBytes(status))) return Fail(writer.Error());
    }
    if (!writer.Complete()) return Fail(writer.Error());
    return FbxImportResult{writer.Count(), writer.Length()};
}

} // namespace import_worker
