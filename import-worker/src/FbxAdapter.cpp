#include "FbxAdapter.h"

#include "BoundedChunkWriter.h"
#include "ImageFormatSniff.h"
#include "SidecarFileClient.h"
#include "TextureTranscodeAdapter.h"
#include "UfbxMaterialConversion.h"
#include "WebpDecodeAdapter.h"
#include "WicImageDecodeAdapter.h"
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
#include <optional>
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
    float tangent[3]{}, handedness = 1.0f;
    if (std::abs(determinant) > 1.0e-12f) {
        const float inverse = 1.0f / determinant;
        for (uint32_t axis = 0; axis < 3; ++axis)
            tangent[axis] = (e1[axis] * dv2 - e2[axis] * dv1) * inverse;
        const float length = std::sqrt(tangent[0] * tangent[0] + tangent[1] * tangent[1]
                                       + tangent[2] * tangent[2]);
        if (length > 1.0e-12f) {
            for (float& value : tangent) value /= length;
        } else {
            tangent[0] = tangent[1] = tangent[2] = 0.0f;
        }
        const float bitangent[3]{(e2[0] * du1 - e1[0] * du2) * inverse,
                                 (e2[1] * du1 - e1[1] * du2) * inverse,
                                 (e2[2] * du1 - e1[2] * du2) * inverse};
        const float cross[3]{vertices[0].ny * tangent[2] - vertices[0].nz * tangent[1],
                             vertices[0].nz * tangent[0] - vertices[0].nx * tangent[2],
                             vertices[0].nx * tangent[1] - vertices[0].ny * tangent[0]};
        handedness = cross[0] * bitangent[0] + cross[1] * bitangent[1]
            + cross[2] * bitangent[2] < 0.0f ? -1.0f : 1.0f;
    }
    if (tangent[0] == 0.0f && tangent[1] == 0.0f && tangent[2] == 0.0f) {
        const float ax = std::abs(vertices[0].nx), ay = std::abs(vertices[0].ny);
        const float az = std::abs(vertices[0].nz);
        const float basis[3]{ax <= ay && ax <= az ? 1.0f : 0.0f,
                             ay < ax && ay <= az ? 1.0f : 0.0f,
                             az < ax && az < ay ? 1.0f : 0.0f};
        tangent[0] = basis[1] * vertices[0].nz - basis[2] * vertices[0].ny;
        tangent[1] = basis[2] * vertices[0].nx - basis[0] * vertices[0].nz;
        tangent[2] = basis[0] * vertices[0].ny - basis[1] * vertices[0].nx;
        const float length = std::sqrt(tangent[0] * tangent[0] + tangent[1] * tangent[1]
                                       + tangent[2] * tangent[2]);
        if (length > 1.0e-12f)
            for (float& value : tangent) value /= length;
        else
            tangent[0] = 1.0f;
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

double MatrixDeterminant(const ufbx_matrix& matrix);

bool ValidMatrix(const ufbx_matrix& matrix)
{
    for (double value : matrix.v)
        if (!Finite(value)) return false;
    const double determinant = MatrixDeterminant(matrix);
    return Finite(determinant) && std::abs(determinant) >= 1.0e-18;
}

bool InheritedVisible(const ufbx_node* node)
{
    for (const ufbx_node* current = node; current; current = current->parent)
        if (!current->visible) return false;
    return true;
}

bool UnsupportedMeshGeometry(const ufbx_mesh& mesh)
{
    return mesh.cache_deformers.count != 0 || mesh.subdivision_preview_levels != 0
        || mesh.subdivision_render_levels != 0 || mesh.subdivision_evaluated
        || mesh.from_tessellated_nurbs;
}

bool PositionTransformToNode(const ufbx_node& node, const ufbx_mesh& mesh,
                             ufbx_matrix& transform)
{
    if (mesh.skinned_is_local) {
        transform = node.geometry_to_node;
    } else {
        if (!ValidMatrix(node.node_to_world)) return false;
        transform = ufbx_matrix_invert(&node.node_to_world);
    }
    return ValidMatrix(transform);
}

bool EvaluatedPositionNormal(const ufbx_mesh& mesh, const ufbx_matrix& transform,
                             const ufbx_matrix& normalTransform, uint32_t sourceIndex,
                             ufbx_vec3& position, ufbx_vec3& normal, bool& normalUsable)
{
    if (sourceIndex >= mesh.num_indices || !mesh.skinned_position.exists) return false;
    position = ufbx_transform_position(
        &transform, ufbx_get_vertex_vec3(&mesh.skinned_position, sourceIndex));
    if (!Finite(position.x) || !Finite(position.y) || !Finite(position.z)) return false;

    normalUsable = false;
    if (mesh.skinned_normal.exists) {
        normal = ufbx_transform_direction(
            &normalTransform, ufbx_get_vertex_vec3(&mesh.skinned_normal, sourceIndex));
        const double length = std::sqrt(normal.x * normal.x + normal.y * normal.y
                                        + normal.z * normal.z);
        normalUsable = Finite(normal.x) && Finite(normal.y) && Finite(normal.z)
            && Finite(length) && length > 1.0e-20;
    }
    return true;
}

void HashValue(uint64_t& hash, uint64_t value)
{
    for (uint32_t shift = 0; shift < 64; shift += 8) {
        hash ^= static_cast<uint8_t>(value >> shift);
        hash *= 1099511628211ull;
    }
}

void HashDouble(uint64_t& hash, double value)
{
    if (value == 0.0) value = 0.0; // canonicalize negative zero
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    HashValue(hash, bits);
}

std::optional<uint64_t> EvaluatedGeometryHash(const ufbx_node& node,
                                              const FbxImportOptions& options)
{
    const ufbx_mesh& mesh = *node.mesh;
    ufbx_matrix transform{};
    if (!PositionTransformToNode(node, mesh, transform)) return std::nullopt;
    const ufbx_matrix normalTransform = ufbx_matrix_for_normals(&transform);
    if (!ValidMatrix(normalTransform)) return std::nullopt;
    uint64_t hash = 1469598103934665603ull;
    HashValue(hash, mesh.typed_id);
    HashValue(hash, mesh.num_indices);
    HashValue(hash, mesh.reversed_winding ? 1u : 0u);
    HashValue(hash, mesh.generated_normals ? 1u : 0u);
    HashValue(hash, mesh.material_parts.count);
    for (const ufbx_mesh_part& part : mesh.material_parts) {
        HashValue(hash, part.index);
        HashValue(hash, part.face_indices.count);
    }
    for (uint32_t index = 0; index < mesh.num_indices; ++index) {
        if ((index & 1023u) == 0 && options.Cancelled()) return std::nullopt;
        ufbx_vec3 position{}, normal{};
        bool normalUsable = false;
        if (!EvaluatedPositionNormal(mesh, transform, normalTransform, index,
                                     position, normal, normalUsable)) return std::nullopt;
        HashDouble(hash, position.x); HashDouble(hash, position.y); HashDouble(hash, position.z);
        HashValue(hash, normalUsable ? 1u : 0u);
        if (normalUsable) {
            HashDouble(hash, normal.x); HashDouble(hash, normal.y); HashDouble(hash, normal.z);
        }
    }
    return hash;
}

bool EquivalentEvaluatedGeometry(const ufbx_node& leftNode, const ufbx_node& rightNode,
                                 const FbxImportOptions& options)
{
    const ufbx_mesh& left = *leftNode.mesh;
    const ufbx_mesh& right = *rightNode.mesh;
    if (left.typed_id != right.typed_id || left.num_indices != right.num_indices
        || left.reversed_winding != right.reversed_winding
        || left.generated_normals != right.generated_normals
        || left.material_parts.count != right.material_parts.count) return false;
    for (size_t index = 0; index < left.material_parts.count; ++index) {
        if (left.material_parts.data[index].index != right.material_parts.data[index].index
            || left.material_parts.data[index].face_indices.count
                != right.material_parts.data[index].face_indices.count) return false;
    }
    ufbx_matrix leftTransform{}, rightTransform{};
    if (!PositionTransformToNode(leftNode, left, leftTransform)
        || !PositionTransformToNode(rightNode, right, rightTransform)) return false;
    const ufbx_matrix leftNormal = ufbx_matrix_for_normals(&leftTransform);
    const ufbx_matrix rightNormal = ufbx_matrix_for_normals(&rightTransform);
    for (uint32_t index = 0; index < left.num_indices; ++index) {
        if ((index & 1023u) == 0 && options.Cancelled()) return false;
        ufbx_vec3 leftPosition{}, leftDirection{}, rightPosition{}, rightDirection{};
        bool leftUsable = false, rightUsable = false;
        if (!EvaluatedPositionNormal(left, leftTransform, leftNormal, index,
                                     leftPosition, leftDirection, leftUsable)
            || !EvaluatedPositionNormal(right, rightTransform, rightNormal, index,
                                        rightPosition, rightDirection, rightUsable)) return false;
        if (std::memcmp(&leftPosition, &rightPosition, sizeof(leftPosition)) != 0
            || leftUsable != rightUsable
            || (leftUsable && std::memcmp(&leftDirection, &rightDirection,
                                          sizeof(leftDirection)) != 0)) return false;
    }
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
    const ufbx_node* representative = nullptr;
    uint64_t evaluatedHash = 0;
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
               const ufbx_matrix& normalTransform,
               uint32_t sourceIndex, bool& normalUsable)
{
    ufbx_vec3 position{}, normal{};
    if (!EvaluatedPositionNormal(mesh, transform, normalTransform, sourceIndex,
                                 position, normal, normalUsable)) return false;

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
    if (normalUsable) {
        float normalized[3]{};
        if (!Normalize(normal, normalized)) return false;
        vertex.nx = normalized[0]; vertex.ny = normalized[1]; vertex.nz = normalized[2];
    }
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
    const ufbx_node& representative = *variant.representative;
    ufbx_matrix positionTransform{};
    if (!PositionTransformToNode(representative, mesh, positionTransform)) {
        error = ImportErrorCode::MalformedData; return false;
    }
    const ufbx_matrix normalTransform = ufbx_matrix_for_normals(&positionTransform);
    const double geometryDeterminant = MatrixDeterminant(positionTransform);
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
                bool normalUsable[3]{};
                uint32_t cornerIndex = 0;
                for (uint32_t corner : corners) {
                    if (!AddVertex(chunk, mesh, positionTransform, normalTransform,
                                   corner, normalUsable[cornerIndex++])) {
                        error = ImportErrorCode::MalformedData; return false;
                    }
                }
                if (!normalUsable[0] || !normalUsable[1] || !normalUsable[2]) {
                    auto* vertices = chunk.vertices.data() + firstVertex;
                    const ufbx_vec3 edge1{vertices[1].px - vertices[0].px,
                                          vertices[1].py - vertices[0].py,
                                          vertices[1].pz - vertices[0].pz};
                    const ufbx_vec3 edge2{vertices[2].px - vertices[0].px,
                                          vertices[2].py - vertices[0].py,
                                          vertices[2].pz - vertices[0].pz};
                    const ufbx_vec3 faceNormal{edge1.y * edge2.z - edge1.z * edge2.y,
                                               edge1.z * edge2.x - edge1.x * edge2.z,
                                               edge1.x * edge2.y - edge1.y * edge2.x};
                    float normalized[3]{};
                    if (!Normalize(faceNormal, normalized)) {
                        error = ImportErrorCode::MalformedData; return false;
                    }
                    for (uint32_t index = 0; index < 3; ++index) {
                        if (normalUsable[index]) continue;
                        vertices[index].nx = normalized[0]; vertices[index].ny = normalized[1];
                        vertices[index].nz = normalized[2];
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

struct DecodedImage {
    PixelFormatId format = PixelFormatId::Unknown;
    ColorSpaceId space = ColorSpaceId::Linear;
    uint32_t width = 0, height = 0, levels = 0;
    std::vector<std::byte> pixels;
};

struct MaterialContext {
    SidecarFileClient* sidecars = nullptr;
    const TextureDecodeOptions* options = nullptr;
    ImportErrorCode error = ImportErrorCode::None;
    uint32_t textureWarnings = 0;
    uint32_t optionalWarnings = 0;
    uint64_t encodedBytes = 0;
    uint64_t decodedBytes = 0;
    uint64_t decodedPixels = 0;
    uint64_t maxDecodedBytes = kMaxAggregateTextureBytes;
    uint64_t maxDecodedPixels = kMaxAggregateTexturePixels;
};

void Warn(uint32_t& warnings)
{
    warnings = (std::min)(64u, warnings + 1);
}

const ufbx_texture* FileTexture(const ufbx_texture* texture, MaterialContext& context)
{
    if (!texture) return nullptr;
    if (texture->type == UFBX_TEXTURE_FILE) return texture;
    // ufbx exposes shader-node leaves here. They are usable only when exactly
    // one file leaf is semantically representative; layered/procedural input
    // otherwise gets the deterministic optional-texture fallback.
    if (texture->file_textures.count == 1 && texture->file_textures.data[0]
        && texture->file_textures.data[0]->type == UFBX_TEXTURE_FILE) {
        Warn(context.optionalWarnings);
        return texture->file_textures.data[0];
    }
    Warn(context.optionalWarnings);
    return nullptr;
}

const ufbx_texture* MapTexture(const ufbx_material_map& primary, MaterialContext& context,
                               const ufbx_material_map* fallback = nullptr)
{
    if (primary.texture_enabled && primary.texture) return FileTexture(primary.texture, context);
    if (fallback && fallback->texture_enabled && fallback->texture)
        return FileTexture(fallback->texture, context);
    return nullptr;
}

// Conservative syntactic mirror of the sidecar policy's containment checks,
// used only to decide whether a reference may be short-circuited by the
// unsupported-format filter below. Anything that looks unsafe must still go to
// the resolver so its rejection stays terminal and fail-closed. The resolver
// remains the authority (its canonical-path containment also catches reparse
// points); this is just a guard so a disallowed extension can never mask an
// unsafe reference.
bool ReferenceLooksUnsafe(std::string_view path)
{
    if (path.empty() || path.front() == '/' || path.front() == '\\') return true;
    if (path.find('\\') != std::string_view::npos
        || path.find(':') != std::string_view::npos) return true;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t end = path.find('/', start);
        const std::string_view component = path.substr(
            start, end == std::string_view::npos ? path.size() - start : end - start);
        if (component == "..") return true;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return false;
}

std::optional<DecodedImage> DecodeTexture(MaterialContext& context, const ufbx_texture* texture,
                                          ColorSpaceId space, TextureSemantic semantic)
{
    std::optional<std::vector<std::byte>> owned;
    std::span<const std::byte> encoded;
    if (texture->content.data && texture->content.size) {
        encoded = {static_cast<const std::byte*>(texture->content.data), texture->content.size};
    } else {
        const ufbx_string path = texture->relative_filename.length
            ? texture->relative_filename : texture->filename;
        if (!path.data || !path.length || !context.sidecars) {
            Warn(context.textureWarnings);
            return std::nullopt;
        }
        // Skip formats no decoder handles (for example the EXR normal/
        // roughness maps some exporters emit). Requesting one would surface the
        // sidecar resolver's disallowed-extension rejection as a fatal
        // UnsafeReference for the whole model; the map is optional, so degrade
        // to a texture warning instead. A reference that looks unsafe is never
        // short-circuited: it still goes to the resolver and fails closed.
        const std::string_view pathView(path.data, path.length);
        if (!ReferenceLooksUnsafe(pathView) && !HasDecodableImageExtension(pathView)) {
            Warn(context.textureWarnings);
            return std::nullopt;
        }
        const uint64_t remaining = context.encodedBytes < context.options->maxEncodedBytes
            ? context.options->maxEncodedBytes - context.encodedBytes : 0;
        auto result = context.sidecars->RequestSidecarBytes(std::string(path.data, path.length), remaining);
        if (!result.bytes) {
            if (result.errorCode == ImportErrorCode::UnsafeReference
                || result.errorCode == ImportErrorCode::FileChanged
                || result.errorCode == ImportErrorCode::ImportProtocolViolation
                || result.errorCode == ImportErrorCode::ResourceLimit
                || result.errorCode == ImportErrorCode::AggregateSourceLimit)
                context.error = result.errorCode;
            else Warn(context.textureWarnings);
            return std::nullopt;
        }
        owned = std::move(result.bytes);
        encoded = *owned;
    }
    if (encoded.empty() || encoded.size() > context.options->maxEncodedBytes - context.encodedBytes) {
        Warn(context.textureWarnings);
        return std::nullopt;
    }
    context.encodedBytes += encoded.size();
    TextureDecodeOptions options = *context.options;
    options.semantic = semantic;
    const uint64_t remainingDecodedBytes = context.decodedBytes < context.maxDecodedBytes
        ? context.maxDecodedBytes - context.decodedBytes : 0;
    const uint64_t remainingDecodedPixels = context.decodedPixels < context.maxDecodedPixels
        ? context.maxDecodedPixels - context.decodedPixels : 0;
    const uint64_t maxDimensionPixels = uint64_t(options.maxDimension) * options.maxDimension;
    const bool aggregateConstrained = remainingDecodedBytes < options.maxDecodedBytes
        || remainingDecodedPixels < (std::min)(options.maxPixels, maxDimensionPixels);
    options.maxDecodedBytes = (std::min)(options.maxDecodedBytes, remainingDecodedBytes);
    options.maxPixels = (std::min)(options.maxPixels, remainingDecodedPixels);
    DecodedImage image;
    image.space = space;
    switch (SniffImageFormat(encoded)) {
    case SniffedImageFormat::Ktx2:
        if (auto decoded = TranscodeKtx2BasisImage(encoded, options)) {
            image.format = decoded->pixelFormat; image.width = decoded->width;
            image.height = decoded->height; image.levels = decoded->mipLevels;
            image.pixels = std::move(decoded->pixelBytes);
        }
        break;
    case SniffedImageFormat::WebP:
        if (auto decoded = DecodeWebpImage(encoded, space, options)) {
            image.format = decoded->pixelFormat; image.space = decoded->colorSpace;
            image.width = decoded->width; image.height = decoded->height;
            image.levels = decoded->mipLevels; image.pixels = std::move(decoded->pixelBytes);
        }
        break;
    case SniffedImageFormat::Png:
    case SniffedImageFormat::Jpeg:
    case SniffedImageFormat::Bmp:
    case SniffedImageFormat::Tiff:
        if (auto decoded = DecodeRasterImageWic(encoded, space, options)) {
            image.format = decoded->pixelFormat; image.space = decoded->colorSpace;
            image.width = decoded->width; image.height = decoded->height;
            image.levels = decoded->mipLevels; image.pixels = std::move(decoded->pixelBytes);
        }
        break;
    default: break;
    }
    if (context.options->Cancelled()) {
        context.error = ImportErrorCode::Cancelled;
        return std::nullopt;
    }
    if (image.pixels.empty()) {
        if (aggregateConstrained) {
            context.error = ImportErrorCode::ResourceLimit;
            return std::nullopt;
        }
        Warn(context.textureWarnings);
        return std::nullopt;
    }
    const uint64_t pixels = uint64_t(image.width) * image.height;
    if (image.pixels.size() > remainingDecodedBytes || pixels > remainingDecodedPixels) {
        context.error = ImportErrorCode::ResourceLimit;
        return std::nullopt;
    }
    context.decodedBytes += image.pixels.size();
    context.decodedPixels += pixels;
    return image;
}

uint32_t EmitImage(BoundedChunkWriter& writer, DecodedImage&& image)
{
    ImagePayloadHeader header{};
    header.pixelFormat = uint32_t(image.format); header.width = image.width;
    header.height = image.height; header.mipLevels = image.levels;
    header.colorSpace = uint32_t(image.space); header.pixelDataByteSize = image.pixels.size();
    ChunkDescriptor descriptor{};
    descriptor.topology = ChunkTopology::Image; descriptor.chunkId = writer.NextId();
    return writer.Add(descriptor, ChunkBytes(header), image.pixels) ? descriptor.chunkId : 0;
}

uint32_t ResolveImage(BoundedChunkWriter& writer, MaterialContext& context,
                      const ufbx_texture* texture, ColorSpaceId space, TextureSemantic semantic,
                      std::unordered_map<uint64_t, uint32_t>& cache)
{
    if (!texture) return 0;
    // Normal maps generate renormalized mips, so they keep their own decode
    // identity. With the same source and color space, base-color, emissive,
    // and data roles decode to identical pixels, so they share one image chunk
    // instead of spending the aggregate texture budget once per slot -- real
    // interior scenes (loft-17) reuse the same bitmap for Diffuse and
    // Emissive across many materials. The source identity is the authored
    // filename, because ufbx exposes one texture object per material map even
    // when every one of them reads the same embedded blob.
    const uint64_t role = semantic == TextureSemantic::Normal ? 1u : 0u;
    const ufbx_string path = texture->relative_filename.length
        ? texture->relative_filename : texture->filename;
    uint64_t identity = uint64_t(reinterpret_cast<uintptr_t>(texture)) >> 3;
    if (path.data && path.length) {
        identity = 1469598103934665603ull;
        for (size_t i = 0; i < path.length; ++i) {
            identity ^= static_cast<unsigned char>(path.data[i]);
            identity *= 1099511628211ull;
        }
    }
    const uint64_t key = identity ^ (uint64_t(space) << 61) ^ (role << 58);
    if (const auto it = cache.find(key); it != cache.end()) return it->second;
    auto image = DecodeTexture(context, texture, space, semantic);
    if (!image) {
        if (context.error != ImportErrorCode::None) return 0;
        DecodedImage fallback;
        fallback.format = PixelFormatId::RGBA8_UNORM; fallback.space = space;
        fallback.width = fallback.height = semantic == TextureSemantic::Color ? 2u : 1u;
        fallback.levels = 1;
        fallback.pixels.resize(size_t(fallback.width) * fallback.height * 4, std::byte{255});
        if (semantic == TextureSemantic::Color) {
            for (unsigned i = 0; i < 4; ++i) for (unsigned c = 0; c < 3; ++c)
                fallback.pixels[i * 4 + c] = std::byte((i == 0 || i == 3) ? 64 : 192);
        } else if (semantic == TextureSemantic::Normal) {
            fallback.pixels[0] = fallback.pixels[1] = std::byte{128};
        } else if (semantic == TextureSemantic::Emissive) {
            fallback.pixels[0] = fallback.pixels[1] = fallback.pixels[2] = std::byte{0};
        }
        image = std::move(fallback);
    }
    const uint32_t id = EmitImage(writer, std::move(*image));
    if (id) cache.emplace(key, id);
    return id;
}

bool EmitMaterials(const ufbx_scene& scene, BoundedChunkWriter& writer, MaterialContext& context,
                   std::unordered_map<const ufbx_material*, uint32_t>& materialIds)
{
    std::unordered_map<uint64_t, uint32_t> imageIds;
    for (const ufbx_material* material : scene.materials) {
        if (!material || context.options->Cancelled()) {
            context.error = ImportErrorCode::Cancelled;
            return false;
        }
        MaterialPayload payload = ConvertUfbxMaterial(*material, true, context.optionalWarnings);
        const ufbx_texture* base = MapTexture(material->pbr.base_color, context, &material->fbx.diffuse_color);
        const ufbx_texture* normal = MapTexture(material->pbr.normal_map, context, &material->fbx.normal_map);
        if (!normal) normal = MapTexture(material->fbx.bump, context);
        const ufbx_texture* emissive = MapTexture(material->pbr.emission_color, context, &material->fbx.emission_color);
        const ufbx_texture* roughness = MapTexture(material->pbr.roughness, context);
        const ufbx_texture* metalness = MapTexture(material->pbr.metalness, context);
        uint32_t images[4]{};
        images[0] = ResolveImage(writer, context, base, ColorSpaceId::Srgb, TextureSemantic::Color, imageIds);
        if (context.error != ImportErrorCode::None || (base && !images[0])) return false;
        if (roughness && roughness == metalness) {
            images[1] = ResolveImage(writer, context, roughness, ColorSpaceId::Linear, TextureSemantic::Data, imageIds);
            if (context.error != ImportErrorCode::None || !images[1]) return false;
        } else if (roughness || metalness) {
            Warn(context.optionalWarnings);
        }
        images[2] = ResolveImage(writer, context, normal, ColorSpaceId::Linear, TextureSemantic::Normal, imageIds);
        if (context.error != ImportErrorCode::None || (normal && !images[2])) return false;
        images[3] = ResolveImage(writer, context, emissive, ColorSpaceId::Srgb, TextureSemantic::Emissive, imageIds);
        if (context.error != ImportErrorCode::None || (emissive && !images[3])) return false;
        if (base && base->has_uv_transform) {
            payload.uvOffset[0] = ToFloat(base->uv_transform.translation.x, payload.uvOffset[0])
                ? payload.uvOffset[0] : 0.0f;
            payload.uvOffset[1] = ToFloat(base->uv_transform.translation.y, payload.uvOffset[1])
                ? payload.uvOffset[1] : 0.0f;
            payload.uvScale[0] = ToFloat(base->uv_transform.scale.x, payload.uvScale[0])
                ? payload.uvScale[0] : 1.0f;
            payload.uvScale[1] = ToFloat(base->uv_transform.scale.y, payload.uvScale[1])
                ? payload.uvScale[1] : 1.0f;
            const auto& q = base->uv_transform.rotation;
            const double rotation = std::atan2(2.0 * (q.w * q.z + q.x * q.y),
                                               1.0 - 2.0 * (q.y * q.y + q.z * q.z));
            if (!ToFloat(rotation, payload.uvRotation)) { payload.uvRotation = 0.0f; Warn(context.optionalWarnings); }
        }
        ChunkDescriptor descriptor{};
        descriptor.topology = ChunkTopology::Material; descriptor.chunkId = writer.NextId();
        for (unsigned slot = 0; slot < 4; ++slot) descriptor.dependencyIds[slot] = images[slot];
        descriptor.dependencyCount = uint32_t(std::count_if(std::begin(images), std::end(images),
            [](uint32_t id) { return id != 0; }));
        if (!writer.Add(descriptor, ChunkBytes(payload))) return false;
        materialIds.emplace(material, descriptor.chunkId);
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
    options.ignore_animation = false;
    // Embedded image blobs are decoded below from their in-memory ufbx blobs;
    // external images remain disabled during load and are brokered explicitly.
    options.ignore_embedded = false;
    options.evaluate_skinning = true;
    options.evaluate_caches = false;
    options.load_external_files = false;
    options.ignore_missing_external_files = true;
    options.skip_skin_vertices = false;
    options.clean_skin_weights = true;
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
    ScenePtr loadedScene(ufbx_load_memory(sourceBytes.data(), sourceBytes.size(), &options, &loadError));
    if (!loadedScene) return Fail(MapLoadError(loadError));
    if (importOptions.Cancelled()) return Fail(ImportErrorCode::Cancelled);
    if (loadedScene->metadata.file_format != UFBX_FILE_FORMAT_FBX)
        return Fail(ImportErrorCode::UnsupportedFormat);
    if (loadedScene->nodes.count > kTierBObjectLimit
        || loadedScene->meshes.count > kTierBObjectLimit
        || loadedScene->materials.count > kTierBMaterialLimit
        || loadedScene->anim_stacks.count > kTierBObjectLimit
        || loadedScene->skin_deformers.count > kTierBObjectLimit
        || loadedScene->bones.count > kTierBObjectLimit)
        return Fail(ImportErrorCode::ResourceLimit);

    uint64_t sourceTriangles = 0, sourceVertices = 0;
    bool hasSubdivision = false;
    for (const ufbx_mesh* mesh : loadedScene->meshes) {
        if (!mesh || mesh->num_triangles > kTierBTriangleLimit - sourceTriangles
            || mesh->num_vertices > kTierBVertexLimit - sourceVertices
            || mesh->num_indices > kTierBIndexLimit
            || mesh->max_face_triangles > kChunkTriangles)
            return Fail(ImportErrorCode::ResourceLimit);
        sourceTriangles += mesh->num_triangles;
        sourceVertices += mesh->num_vertices;
        hasSubdivision |= mesh->subdivision_preview_levels != 0
            || mesh->subdivision_render_levels != 0 || mesh->subdivision_evaluated
            || mesh->from_tessellated_nurbs;
    }
    const bool hasCaches = loadedScene->cache_deformers.count || loadedScene->cache_files.count;
    const bool hasNurbs = loadedScene->nurbs_curves.count || loadedScene->nurbs_surfaces.count
        || loadedScene->nurbs_trim_surfaces.count || loadedScene->nurbs_trim_boundaries.count;
    const bool hasProcedural = loadedScene->procedural_geometries.count != 0;
    const bool hasConstraints = loadedScene->constraints.count != 0;
    const bool omittedGeometry = hasCaches || hasNurbs || hasProcedural || hasSubdivision;

    const ufbx_anim* animation = loadedScene->anim;
    double evaluationTime = 0.0;
    if (loadedScene->anim_stacks.count) {
        const ufbx_anim_stack* first = loadedScene->anim_stacks.data[0];
        if (!first || !first->anim || !Finite(first->time_begin))
            return Fail(ImportErrorCode::MalformedData);
        animation = first->anim;
        evaluationTime = first->time_begin;
    }
    if (!animation) return Fail(ImportErrorCode::MalformedData);
    if (importOptions.Cancelled()) return Fail(ImportErrorCode::Cancelled);
    ufbx_evaluate_opts evaluateOptions{};
    const size_t evaluationAllocatorLimit = importOptions.evaluationAllocatorLimit
        ? importOptions.evaluationAllocatorLimit : static_cast<size_t>(scratchLimit / 2);
    evaluateOptions.temp_allocator.memory_limit = evaluationAllocatorLimit;
    evaluateOptions.result_allocator.memory_limit = evaluationAllocatorLimit;
    evaluateOptions.temp_allocator.allocation_limit = 1'000'000;
    evaluateOptions.result_allocator.allocation_limit = 1'000'000;
    evaluateOptions.evaluate_skinning = true;
    evaluateOptions.evaluate_caches = false;
    evaluateOptions.evaluate_flags = 0;
    evaluateOptions.load_external_files = false;
    evaluateOptions.open_file_cb.fn = DenyExternalFile;
    ufbx_error evaluationError{};
    ScenePtr evaluatedScene(ufbx_evaluate_scene(loadedScene.get(), animation, evaluationTime,
                                                &evaluateOptions, &evaluationError));
    if (!evaluatedScene) return Fail(MapLoadError(evaluationError));
    if (importOptions.Cancelled()) return Fail(ImportErrorCode::Cancelled);
    const ufbx_scene* scene = evaluatedScene.get();

    SceneMetadata metadata{};
    metadata.generationId = generationId;
    metadata.format = SourceFormatId::Fbx;
    metadata.upAxis = UpAxisId::Y;
    metadata.metersPerUnit = 1.0;
    metadata.meshCount = static_cast<uint32_t>(loadedScene->meshes.count);
    metadata.nodeCount = static_cast<uint32_t>(loadedScene->nodes.count);
    metadata.animationCount = static_cast<uint32_t>(loadedScene->anim_stacks.count);
    metadata.skinCount = static_cast<uint32_t>(loadedScene->skin_deformers.count);
    metadata.boneCount = static_cast<uint32_t>(loadedScene->bones.count);
    BoundedChunkWriter writer(destination, generationId, maxChunkCount, metadata, batchSink);
    if (destination.size() <= kSectionHeaderSize + kChunkDescriptorSize)
        return Fail(ImportErrorCode::ResourceLimit);
    constexpr uint64_t triangleBytes = 3 * sizeof(VertexPositionNormalUv0TangentColorF32)
        + 3 * sizeof(uint32_t);
    const uint32_t chunkTriangleLimit = static_cast<uint32_t>((std::min<uint64_t>)(
        kChunkTriangles, (destination.size() - kSectionHeaderSize - kChunkDescriptorSize)
        / triangleBytes));
    if (!chunkTriangleLimit) return Fail(ImportErrorCode::ResourceLimit);

    TextureDecodeOptions defaultTextureOptions;
    defaultTextureOptions.isCancelled = importOptions.isCancelled;
    MaterialContext materialContext;
    materialContext.sidecars = importOptions.sidecars;
    materialContext.options = importOptions.textureOptions ? importOptions.textureOptions : &defaultTextureOptions;
    materialContext.maxDecodedBytes = importOptions.maxAggregateTextureBytes;
    materialContext.maxDecodedPixels = importOptions.maxAggregateTexturePixels;
    std::unordered_map<const ufbx_material*, uint32_t> materialIds;
    if (!EmitMaterials(*scene, writer, materialContext, materialIds)) {
        const ImportErrorCode error = materialContext.error != ImportErrorCode::None
            ? materialContext.error : writer.Error();
        return Fail(error, ImportFailurePhase::Sidecars);
    }

    // Nodes may carry no material binding. Construct its deterministic fallback
    // now, but publish it only if a visible geometry part actually needs it.
    MaterialPayload neutral{};
    neutral.baseColorFactor[0] = neutral.baseColorFactor[1] = neutral.baseColorFactor[2] = 0.8f;
    neutral.baseColorFactor[3] = 1.0f;
    neutral.roughnessFactor = 1.0f;
    neutral.uvScale[0] = neutral.uvScale[1] = 1.0f;
    neutral.alphaMode = uint32_t(AlphaModeId::Opaque);
    neutral.alphaCutoff = 0.5f;
    neutral.flags = kMaterialFlagDoubleSided;
    uint32_t neutralMaterialId = 0;

    std::vector<MeshVariant> variants;
    variants.reserve(scene->nodes.count);
    bool hasSupportedVisibleGeometry = false;
    bool omittedVisibleGeometry = false;
    uint64_t sharingComparisonIndices = 0;
    auto chargeSharingComparison = [&](size_t count) {
        if (count > kTierBIndexLimit - sharingComparisonIndices) return false;
        sharingComparisonIndices += count;
        return true;
    };
    for (const ufbx_node* node : scene->nodes) {
        if (importOptions.Cancelled()) return Fail(ImportErrorCode::Cancelled);
        if (!node || !node->mesh || !node->mesh->num_triangles) continue;
        if (UnsupportedMeshGeometry(*node->mesh)) {
            omittedVisibleGeometry |= InheritedVisible(node);
            continue;
        }
        hasSupportedVisibleGeometry |= InheritedVisible(node);
        if (!chargeSharingComparison(node->mesh->num_indices))
            return Fail(ImportErrorCode::ResourceLimit);
        const auto evaluatedHash = EvaluatedGeometryHash(*node, importOptions);
        if (!evaluatedHash)
            return Fail(importOptions.Cancelled() ? ImportErrorCode::Cancelled
                                                  : ImportErrorCode::MalformedData);
        auto found = variants.end();
        for (auto candidate = variants.begin(); candidate != variants.end(); ++candidate) {
            if (candidate->evaluatedHash != *evaluatedHash) continue;
            if (!chargeSharingComparison(node->mesh->num_indices))
                return Fail(ImportErrorCode::ResourceLimit);
            const bool equivalent = EquivalentEvaluatedGeometry(
                *candidate->representative, *node, importOptions);
            if (importOptions.Cancelled()) return Fail(ImportErrorCode::Cancelled);
            if (equivalent) { found = candidate; break; }
        }
        if (found == variants.end()) {
            if (variants.size() >= kTierBObjectLimit) return Fail(ImportErrorCode::ResourceLimit);
            MeshVariant variant{};
            variant.mesh = node->mesh;
            variant.representative = node;
            variant.evaluatedHash = *evaluatedHash;
            variants.push_back(std::move(variant));
            found = std::prev(variants.end());
        }
        found->nodes.push_back(node);
    }
    if (omittedGeometry && !hasSupportedVisibleGeometry)
        return Fail(ImportErrorCode::UnsupportedRequiredFeature);
    if (omittedVisibleGeometry && !hasSupportedVisibleGeometry)
        return Fail(ImportErrorCode::UnsupportedRequiredFeature);
    if (!sourceTriangles || variants.empty())
        return Fail(omittedGeometry ? ImportErrorCode::UnsupportedRequiredFeature
                                    : ImportErrorCode::EmptyGeometry);

    bool needsNeutralMaterial = false;
    for (const MeshVariant& variant : variants) {
        for (const ufbx_node* node : variant.nodes) {
            for (const ufbx_mesh_part& part : variant.mesh->material_parts) {
                const ufbx_material* material = part.index < node->materials.count
                    ? node->materials.data[part.index] : nullptr;
                if (!materialIds.contains(material)) {
                    needsNeutralMaterial = true;
                    break;
                }
            }
            if (needsNeutralMaterial) break;
        }
        if (needsNeutralMaterial) break;
    }
    if (needsNeutralMaterial) {
        ChunkDescriptor materialDescriptor{};
        materialDescriptor.topology = ChunkTopology::Material;
        materialDescriptor.chunkId = writer.NextId();
        if (!writer.Add(materialDescriptor, ChunkBytes(neutral))) return Fail(writer.Error());
        neutralMaterialId = materialDescriptor.chunkId;
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
                const ufbx_material* material = geometry.partIndex < node->materials.count
                    ? node->materials.data[geometry.partIndex] : nullptr;
                if (const auto it = materialIds.find(material); it != materialIds.end())
                    payload.materialChunkId = it->second;
                else
                    payload.materialChunkId = neutralMaterialId;
                payload.flags = kSceneRecordVisible;
                if (!TransformBounds(geometry.descriptor, world, payload.worldMin, payload.worldMax))
                    return Fail(ImportErrorCode::MalformedData);
                if (!writer.AddInstance(payload))
                    return Fail(importOptions.Cancelled() ? ImportErrorCode::Cancelled : writer.Error());
            }
        }
    }

    const size_t featureWarnings = size_t(hasCaches) + size_t(hasNurbs)
        + size_t(hasProcedural) + size_t(hasSubdivision) + size_t(hasConstraints);
    const uint32_t optionalWarnings = static_cast<uint32_t>((std::min)(size_t(64),
        loadedScene->metadata.warnings.count + featureWarnings + materialContext.optionalWarnings));
    if (optionalWarnings || materialContext.textureWarnings) {
        ImportStatusPayload status{};
        status.optionalFeatureWarnings = optionalWarnings;
        status.textureWarnings = materialContext.textureWarnings;
        ChunkDescriptor descriptor{};
        descriptor.topology = ChunkTopology::ImportStatus;
        descriptor.chunkId = writer.NextId();
        if (!writer.Add(descriptor, ChunkBytes(status))) return Fail(writer.Error());
    }
    if (!writer.Complete()) return Fail(writer.Error());
    return FbxImportResult{writer.Count(), writer.Length()};
}

} // namespace import_worker
