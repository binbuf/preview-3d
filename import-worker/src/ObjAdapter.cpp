#include "ObjAdapter.h"

#include "BoundedChunkWriter.h"
#include "ImageFormatSniff.h"
#include "SidecarFileClient.h"
#include "TextureTranscodeAdapter.h"
#include "UfbxMaterialConversion.h"
#include "WebpDecodeAdapter.h"
#include "WicImageDecodeAdapter.h"
#include "model_core/MaterialPayload.h"
#include "model_core/PixelFormats.h"
#include "model_core/TierALimits.h"
#include "model_core/VertexLayouts.h"
#include "ufbx.h"

#include <algorithm>
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

struct BrokerContext {
    SidecarFileClient* sidecars = nullptr;
    const TextureDecodeOptions* textureOptions = nullptr;
    ImportErrorCode error = ImportErrorCode::None;
    uint32_t textureWarnings = 0;
    uint32_t optionalWarnings = 0;
    uint64_t encodedBytes = 0;
    uint64_t decodedBytes = 0;
    uint64_t decodedPixels = 0;
};

struct BrokerStream {
    SidecarFileClient::Result source;
    std::span<const std::byte> bytes;
    size_t offset = 0;
};

size_t ReadBrokerStream(void* user, void* data, size_t size)
{
    auto& stream = *static_cast<BrokerStream*>(user);
    const size_t count = (std::min)(size, stream.bytes.size() - stream.offset);
    if (count) std::memcpy(data, stream.bytes.data() + stream.offset, count);
    stream.offset += count;
    return count;
}

bool SkipBrokerStream(void* user, size_t size)
{
    auto& stream = *static_cast<BrokerStream*>(user);
    if (size > stream.bytes.size() - stream.offset) return false;
    stream.offset += size;
    return true;
}

uint64_t SizeBrokerStream(void* user)
{
    return static_cast<BrokerStream*>(user)->bytes.size();
}

void CloseBrokerStream(void* user) { delete static_cast<BrokerStream*>(user); }

bool OpenBrokeredFile(void* user, ufbx_stream* stream, const char* path, size_t pathLength,
                      const ufbx_open_file_info* info)
{
    auto& context = *static_cast<BrokerContext*>(user);
    if (!context.sidecars || !info || info->type != UFBX_OPEN_FILE_OBJ_MTL || !path || !pathLength) {
        return false;
    }
    if (context.textureOptions && context.textureOptions->Cancelled()) {
        context.error = ImportErrorCode::Cancelled;
        return false;
    }
    auto result = context.sidecars->RequestSidecarBytes(
        std::string(path, pathLength), kTierBAllSourceBytes, true);
    if (!result.mapping) {
        if (result.errorCode == ImportErrorCode::UnsafeReference ||
            result.errorCode == ImportErrorCode::FileChanged ||
            result.errorCode == ImportErrorCode::ImportProtocolViolation ||
            result.errorCode == ImportErrorCode::ResourceLimit ||
            result.errorCode == ImportErrorCode::AggregateSourceLimit) {
            context.error = result.errorCode;
        } else {
            context.optionalWarnings = (std::min)(64u, context.optionalWarnings + 1);
        }
        return false;
    }
    auto* brokerStream = new (std::nothrow) BrokerStream{std::move(result), {}, 0};
    if (!brokerStream) {
        context.error = ImportErrorCode::OutOfMemory;
        return false;
    }
    brokerStream->bytes = brokerStream->source.mapping->Bytes();
    stream->read_fn = ReadBrokerStream;
    stream->skip_fn = SkipBrokerStream;
    stream->size_fn = SizeBrokerStream;
    stream->close_fn = CloseBrokerStream;
    stream->user = brokerStream;
    return true;
}

ufbx_progress_result CheckProgress(void* user, const ufbx_progress*)
{
    const auto& context = *static_cast<const BrokerContext*>(user);
    return context.textureOptions && context.textureOptions->Cancelled()
        ? UFBX_PROGRESS_CANCEL : UFBX_PROGRESS_CONTINUE;
}

ImportErrorCode MapLoadError(const ufbx_error& error)
{
    switch (error.type) {
    case UFBX_ERROR_EMPTY_FILE: return ImportErrorCode::EmptyGeometry;
    case UFBX_ERROR_OUT_OF_MEMORY: return ImportErrorCode::OutOfMemory;
    case UFBX_ERROR_MEMORY_LIMIT:
    case UFBX_ERROR_ALLOCATION_LIMIT: return ImportErrorCode::ScratchLimit;
    case UFBX_ERROR_CANCELLED: return ImportErrorCode::Cancelled;
    case UFBX_ERROR_EXTERNAL_FILE_NOT_FOUND: return ImportErrorCode::FileUnavailable;
    default: return ImportErrorCode::MalformedData;
    }
}

bool Finite(double value) { return std::isfinite(value); }
float SafeFloat(double value, float fallback = 0.0f)
{
    return Finite(value) && value >= -std::numeric_limits<float>::max()
        && value <= std::numeric_limits<float>::max() ? static_cast<float>(value) : fallback;
}
float Saturate(double value, float fallback = 1.0f)
{
    return Finite(value) ? static_cast<float>(std::clamp(value, 0.0, 1.0)) : fallback;
}

const ufbx_texture* FileTexture(const ufbx_texture* texture)
{
    if (!texture) return nullptr;
    if (texture->type == UFBX_TEXTURE_FILE) return texture;
    return texture->file_textures.count ? texture->file_textures.data[0] : nullptr;
}

const ufbx_texture* MapTexture(const ufbx_material_map& primary,
                               const ufbx_material_map* fallback = nullptr)
{
    if (primary.texture_enabled && primary.texture) return FileTexture(primary.texture);
    if (fallback && fallback->texture_enabled && fallback->texture) return FileTexture(fallback->texture);
    return nullptr;
}

struct DecodedImage {
    PixelFormatId format = PixelFormatId::Unknown;
    ColorSpaceId space = ColorSpaceId::Linear;
    uint32_t width = 0, height = 0, levels = 0;
    std::vector<std::byte> pixels;
};

std::optional<DecodedImage> DecodeTexture(BrokerContext& context, const ufbx_texture* texture,
                                          ColorSpaceId space, TextureSemantic semantic)
{
    texture = FileTexture(texture);
    if (!texture) return std::nullopt;
    std::optional<std::vector<std::byte>> owned;
    std::span<const std::byte> encoded;
    if (texture->content.data && texture->content.size) {
        encoded = {static_cast<const std::byte*>(texture->content.data), texture->content.size};
    } else {
        const ufbx_string path = texture->relative_filename.length
            ? texture->relative_filename : texture->filename;
        if (!path.data || !path.length || !context.sidecars) {
            context.textureWarnings = (std::min)(64u, context.textureWarnings + 1);
            return std::nullopt;
        }
        const uint64_t remaining = context.encodedBytes < context.textureOptions->maxEncodedBytes
            ? context.textureOptions->maxEncodedBytes - context.encodedBytes : 0;
        auto result = context.sidecars->RequestSidecarBytes(std::string(path.data, path.length), remaining);
        if (!result.bytes) {
            if (result.errorCode == ImportErrorCode::UnsafeReference ||
                result.errorCode == ImportErrorCode::FileChanged ||
                result.errorCode == ImportErrorCode::ImportProtocolViolation ||
                result.errorCode == ImportErrorCode::ResourceLimit ||
                result.errorCode == ImportErrorCode::AggregateSourceLimit) {
                context.error = result.errorCode;
            } else {
                context.textureWarnings = (std::min)(64u, context.textureWarnings + 1);
            }
            return std::nullopt;
        }
        owned = std::move(result.bytes);
        encoded = *owned;
    }
    if (encoded.empty() || encoded.size() > context.textureOptions->maxEncodedBytes - context.encodedBytes) {
        context.textureWarnings = (std::min)(64u, context.textureWarnings + 1);
        return std::nullopt;
    }
    context.encodedBytes += encoded.size();
    TextureDecodeOptions options = *context.textureOptions;
    options.semantic = semantic;
    options.maxDecodedBytes = (std::min)(options.maxDecodedBytes,
        kMaxAggregateTextureBytes - context.decodedBytes);
    options.maxPixels = (std::min)(options.maxPixels,
        kMaxAggregateTexturePixels - context.decodedPixels);
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
    if (context.textureOptions->Cancelled()) {
        context.error = ImportErrorCode::Cancelled;
        return std::nullopt;
    }
    if (image.pixels.empty()) {
        context.textureWarnings = (std::min)(64u, context.textureWarnings + 1);
        return std::nullopt;
    }
    const uint64_t pixels = uint64_t(image.width) * image.height;
    if (image.pixels.size() > kMaxAggregateTextureBytes - context.decodedBytes ||
        pixels > kMaxAggregateTexturePixels - context.decodedPixels) {
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
    if (!writer.Add(descriptor, ChunkBytes(header), image.pixels)) return 0;
    return descriptor.chunkId;
}

uint32_t ResolveImage(BoundedChunkWriter& writer, BrokerContext& context,
                      const ufbx_texture* texture, ColorSpaceId space, TextureSemantic semantic,
                      std::unordered_map<uint64_t, uint32_t>& cache)
{
    texture = FileTexture(texture);
    if (!texture) return 0;
    const uint64_t key = (uint64_t(reinterpret_cast<uintptr_t>(texture)) >> 3)
        ^ (uint64_t(space) << 61) ^ (uint64_t(semantic) << 58);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    auto image = DecodeTexture(context, texture, space, semantic);
    if (!image) {
        if (context.error != ImportErrorCode::None) return 0;
        DecodedImage fallback;
        fallback.format = PixelFormatId::RGBA8_UNORM; fallback.space = space;
        fallback.width = fallback.height = semantic == TextureSemantic::Color ? 2u : 1u;
        fallback.levels = 1; fallback.pixels.resize(size_t(fallback.width) * fallback.height * 4, std::byte{255});
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

bool EmitMaterials(const ufbx_scene& scene, BoundedChunkWriter& writer, BrokerContext& context,
                   std::unordered_map<const ufbx_material*, uint32_t>& materialIds)
{
    std::unordered_map<uint64_t, uint32_t> imageIds;
    for (size_t index = 0; index < scene.materials.count; ++index) {
        if (context.textureOptions->Cancelled()) { context.error = ImportErrorCode::Cancelled; return false; }
        const ufbx_material* material = scene.materials.data[index];
        MaterialPayload payload = ConvertUfbxMaterial(*material, false, context.optionalWarnings);
        const ufbx_texture* base = MapTexture(material->pbr.base_color, &material->fbx.diffuse_color);
        const ufbx_texture* normal = MapTexture(material->pbr.normal_map, &material->fbx.normal_map);
        if (!normal) normal = MapTexture(material->fbx.bump);
        const ufbx_texture* emissive = MapTexture(material->pbr.emission_color, &material->fbx.emission_color);
        const ufbx_texture* roughness = MapTexture(material->pbr.roughness);
        const ufbx_texture* metalness = MapTexture(material->pbr.metalness);
        uint32_t images[4]{};
        images[0] = ResolveImage(writer, context, base, ColorSpaceId::Srgb, TextureSemantic::Color, imageIds);
        if (context.error != ImportErrorCode::None || (base && !images[0])) return false;
        if (roughness && roughness == metalness) {
            images[1] = ResolveImage(writer, context, roughness, ColorSpaceId::Linear, TextureSemantic::Data, imageIds);
            if (context.error != ImportErrorCode::None || !images[1]) return false;
        } else if (roughness || metalness) {
            // The normalized wire format has one packed metallic/roughness slot.
            // Distinct scalar MTL maps remain a bounded optional-feature warning.
            context.optionalWarnings = (std::min)(64u, context.optionalWarnings + 1);
        }
        images[2] = ResolveImage(writer, context, normal, ColorSpaceId::Linear, TextureSemantic::Normal, imageIds);
        if (context.error != ImportErrorCode::None || (normal && !images[2])) return false;
        images[3] = ResolveImage(writer, context, emissive, ColorSpaceId::Srgb, TextureSemantic::Emissive, imageIds);
        if (context.error != ImportErrorCode::None || (emissive && !images[3])) return false;
        if (base && base->has_uv_transform) {
            payload.uvOffset[0] = SafeFloat(base->uv_transform.translation.x);
            payload.uvOffset[1] = SafeFloat(base->uv_transform.translation.y);
            payload.uvScale[0] = SafeFloat(base->uv_transform.scale.x, 1.0f);
            payload.uvScale[1] = SafeFloat(base->uv_transform.scale.y, 1.0f);
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

bool Normalize(ufbx_vec3 value, float out[3])
{
    const double length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    if (!Finite(length) || length <= 1e-20) { out[0] = 0; out[1] = 0; out[2] = 1; return false; }
    out[0] = float(value.x / length); out[1] = float(value.y / length); out[2] = float(value.z / length);
    return true;
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
    float tangent[3]{1, 0, 0}, handedness = 1.0f;
    if (std::abs(determinant) > 1e-12f) {
        const float inverse = 1.0f / determinant;
        for (unsigned axis = 0; axis < 3; ++axis) tangent[axis] = (e1[axis] * dv2 - e2[axis] * dv1) * inverse;
        const float length = std::sqrt(tangent[0] * tangent[0] + tangent[1] * tangent[1] + tangent[2] * tangent[2]);
        if (length > 1e-12f) for (float& value : tangent) value /= length;
        const float bitangent[3]{(e2[0] * du1 - e1[0] * du2) * inverse,
                                 (e2[1] * du1 - e1[1] * du2) * inverse,
                                 (e2[2] * du1 - e1[2] * du2) * inverse};
        const float cross[3]{vertices[0].ny * tangent[2] - vertices[0].nz * tangent[1],
                             vertices[0].nz * tangent[0] - vertices[0].nx * tangent[2],
                             vertices[0].nx * tangent[1] - vertices[0].ny * tangent[0]};
        handedness = cross[0] * bitangent[0] + cross[1] * bitangent[1] + cross[2] * bitangent[2] < 0 ? -1.0f : 1.0f;
    }
    for (unsigned i = 0; i < 3; ++i) {
        vertices[i].tx = tangent[0]; vertices[i].ty = tangent[1];
        vertices[i].tz = tangent[2]; vertices[i].tw = handedness;
    }
}

struct GeometryChunk {
    ChunkDescriptor descriptor{};
    std::vector<VertexPositionNormalUv0TangentColorF32> vertices;
    std::vector<uint32_t> indices;
    bool hasOrigin = false;
};

bool AddVertex(GeometryChunk& chunk, const ufbx_mesh& mesh, const ufbx_node& node, uint32_t sourceIndex)
{
    if (sourceIndex >= mesh.num_indices) return false;
    auto position = ufbx_transform_position(&node.geometry_to_world,
        ufbx_get_vertex_vec3(&mesh.vertex_position, sourceIndex));
    auto normal = ufbx_transform_direction(&node.geometry_to_world,
        ufbx_get_vertex_vec3(&mesh.vertex_normal, sourceIndex));
    if (!Finite(position.x) || !Finite(position.y) || !Finite(position.z) ||
        !Finite(normal.x) || !Finite(normal.y) || !Finite(normal.z)) return false;
    if (!chunk.hasOrigin) {
        chunk.descriptor.origin[0] = position.x; chunk.descriptor.origin[1] = position.y;
        chunk.descriptor.origin[2] = position.z; chunk.hasOrigin = true;
    }
    VertexPositionNormalUv0TangentColorF32 vertex{};
    vertex.px = SafeFloat(position.x - chunk.descriptor.origin[0]);
    vertex.py = SafeFloat(position.y - chunk.descriptor.origin[1]);
    vertex.pz = SafeFloat(position.z - chunk.descriptor.origin[2]);
    float normalized[3]; Normalize(normal, normalized);
    vertex.nx = normalized[0]; vertex.ny = normalized[1]; vertex.nz = normalized[2];
    if (mesh.vertex_uv.exists) {
        const auto uv = ufbx_get_vertex_vec2(&mesh.vertex_uv, sourceIndex);
        if (!Finite(uv.x) || !Finite(uv.y)) return false;
        vertex.u = SafeFloat(uv.x); vertex.v = SafeFloat(uv.y);
    }
    vertex.tx = 1; vertex.tw = 1;
    vertex.r = vertex.g = vertex.b = vertex.a = 1;
    if (mesh.vertex_color.exists) {
        const auto color = ufbx_get_vertex_vec4(&mesh.vertex_color, sourceIndex);
        if (!Finite(color.x) || !Finite(color.y) || !Finite(color.z) || !Finite(color.w)) return false;
        vertex.r = Saturate(color.x); vertex.g = Saturate(color.y);
        vertex.b = Saturate(color.z); vertex.a = Saturate(color.w);
    }
    for (unsigned axis = 0; axis < 3; ++axis) {
        const float value = (&vertex.px)[axis];
        if (chunk.vertices.empty()) chunk.descriptor.localMin[axis] = chunk.descriptor.localMax[axis] = value;
        else { chunk.descriptor.localMin[axis] = (std::min)(chunk.descriptor.localMin[axis], value);
               chunk.descriptor.localMax[axis] = (std::max)(chunk.descriptor.localMax[axis], value); }
    }
    chunk.indices.push_back(uint32_t(chunk.vertices.size()));
    chunk.vertices.push_back(vertex);
    return true;
}

bool FlushGeometry(BoundedChunkWriter& writer, GeometryChunk& chunk, uint32_t materialId,
                   uint32_t meshId, uint32_t nodeId, uint32_t sourceFirst,
                   bool hasUv, bool hasColor)
{
    if (chunk.indices.empty()) return true;
    chunk.descriptor.topology = ChunkTopology::TriangleList;
    chunk.descriptor.vertexLayoutId = uint32_t(VertexLayoutId::PositionNormalUv0TangentColor_F32);
    chunk.descriptor.vertexCount = uint32_t(chunk.vertices.size());
    chunk.descriptor.indexCount = uint32_t(chunk.indices.size());
    chunk.descriptor.chunkId = writer.NextId();
    chunk.descriptor.meshId = meshId; chunk.descriptor.nodeId = nodeId;
    chunk.descriptor.boundsState = BoundsState::Verified;
    chunk.descriptor.geometryFlags = kGeometryDeindexed
        | (hasUv ? kGeometryHasUv0 : 0u) | (hasColor ? kGeometryHasColors : 0u);
    chunk.descriptor.sourceRangeOffset = (uint64_t(meshId) << 32) | sourceFirst;
    chunk.descriptor.sourceRangeLength = chunk.indices.size();
    if (materialId) { chunk.descriptor.dependencyIds[0] = materialId; chunk.descriptor.dependencyCount = 1; }
    return writer.Add(chunk.descriptor, ChunkBytes(chunk.vertices), ChunkBytes(chunk.indices));
}

bool EmitGeometry(const ufbx_scene& scene, BoundedChunkWriter& writer, BrokerContext& context,
                  const std::unordered_map<const ufbx_material*, uint32_t>& materialIds,
                  uint32_t chunkTriangleLimit)
{
    uint64_t totalTriangles = 0, totalVertices = 0;
    uint32_t meshOrdinal = 0;
    for (size_t nodeIndex = 0; nodeIndex < scene.nodes.count; ++nodeIndex) {
        const ufbx_node* node = scene.nodes.data[nodeIndex];
        if (!node->mesh) continue;
        if (meshOrdinal >= kTierBObjectLimit) { context.error = ImportErrorCode::ResourceLimit; return false; }
        const ufbx_mesh& mesh = *node->mesh;
        if (mesh.num_triangles > kTierBTriangleLimit - totalTriangles ||
            mesh.num_indices > kTierBIndexLimit || mesh.max_face_triangles > kTierBTriangleLimit) {
            context.error = ImportErrorCode::ResourceLimit; return false;
        }
        for (size_t partIndex = 0; partIndex < mesh.material_parts.count; ++partIndex) {
            const ufbx_mesh_part& part = mesh.material_parts.data[partIndex];
            const ufbx_material* material = part.index < node->materials.count ? node->materials.data[part.index] : nullptr;
            const auto materialIt = materialIds.find(material);
            const uint32_t materialId = materialIt == materialIds.end() ? 0 : materialIt->second;
            GeometryChunk chunk;
            uint32_t sourceFirst = 0, sourceCursor = 0;
            std::vector<uint32_t> triangles((std::max)(size_t(3), mesh.max_face_triangles * 3));
            for (size_t faceOffset = 0; faceOffset < part.face_indices.count; ++faceOffset) {
                if ((faceOffset & 1023) == 0 && context.textureOptions->Cancelled()) {
                    context.error = ImportErrorCode::Cancelled; return false;
                }
                const uint32_t faceIndex = part.face_indices.data[faceOffset];
                if (faceIndex >= mesh.faces.count) { context.error = ImportErrorCode::MalformedData; return false; }
                const ufbx_face face = mesh.faces.data[faceIndex];
                if (face.num_indices < 3) continue;
                const uint32_t triangleCount = ufbx_triangulate_face(triangles.data(), triangles.size(), &mesh, face);
                if (triangleCount != face.num_indices - 2) { context.error = ImportErrorCode::MalformedData; return false; }
                for (uint32_t triangle = 0; triangle < triangleCount; ++triangle) {
                    if (chunk.indices.size() / 3 >= chunkTriangleLimit) {
                        if (!FlushGeometry(writer, chunk, materialId, meshOrdinal, uint32_t(nodeIndex), sourceFirst,
                                           mesh.vertex_uv.exists, mesh.vertex_color.exists)) return false;
                        chunk = {}; sourceFirst = sourceCursor;
                    }
                    const size_t firstVertex = chunk.vertices.size();
                    for (unsigned corner = 0; corner < 3; ++corner) {
                        if (!AddVertex(chunk, mesh, *node, triangles[triangle * 3 + corner])) {
                            context.error = ImportErrorCode::MalformedData; return false;
                        }
                    }
                    GenerateTriangleTangent(chunk.vertices.data() + firstVertex);
                    sourceCursor += 3; ++totalTriangles; totalVertices += 3;
                    if (totalTriangles > kTierBTriangleLimit || totalVertices > kTierBVertexLimit) {
                        context.error = ImportErrorCode::ResourceLimit; return false;
                    }
                }
            }
            if (!FlushGeometry(writer, chunk, materialId, meshOrdinal, uint32_t(nodeIndex), sourceFirst,
                               mesh.vertex_uv.exists, mesh.vertex_color.exists)) return false;
        }
        ++meshOrdinal;
    }
    if (!totalTriangles) { context.error = ImportErrorCode::EmptyGeometry; return false; }
    return true;
}

} // namespace

std::variant<ObjImportResult, ImportErrorCode> ImportObj(
    std::span<const std::byte> sourceBytes, std::span<std::byte> destination,
    uint64_t generationId, uint32_t maxChunkCount, SidecarFileClient& sidecars,
    ChunkBatchSink* batchSink, const TextureDecodeOptions& textureOptions)
{
    if (sourceBytes.empty()) return ImportErrorCode::EmptyGeometry;
    if (sourceBytes.size() > kTierBPrimarySourceBytes) return ImportErrorCode::PrimarySourceLimit;
    const uint64_t scratchLimit = TierBScratchLimit();
    if (!scratchLimit) return ImportErrorCode::ScratchLimit;
    BrokerContext context{&sidecars, &textureOptions};
    ufbx_load_opts options{};
    options.file_format = UFBX_FILE_FORMAT_OBJ;
    options.filename = {"document.obj", 12};
    options.load_external_files = true;
    options.ignore_missing_external_files = true;
    options.generate_missing_normals = true;
    options.normalize_normals = true;
    options.index_error_handling = UFBX_INDEX_ERROR_HANDLING_ABORT_LOADING;
    options.unicode_error_handling = UFBX_UNICODE_ERROR_HANDLING_ABORT_LOADING;
    options.node_depth_limit = 256;
    options.temp_allocator.memory_limit = size_t(scratchLimit / 2);
    options.result_allocator.memory_limit = size_t(scratchLimit / 2);
    options.temp_allocator.allocation_limit = 1'000'000;
    options.result_allocator.allocation_limit = 1'000'000;
    options.open_file_cb.fn = OpenBrokeredFile;
    options.open_file_cb.user = &context;
    options.progress_cb.fn = CheckProgress;
    options.progress_cb.user = &context;
    options.progress_interval_hint = 1u << 20;
    ufbx_error loadError{};
    ScenePtr scene(ufbx_load_memory(sourceBytes.data(), sourceBytes.size(), &options, &loadError));
    if (context.error != ImportErrorCode::None) return context.error;
    if (!scene) return MapLoadError(loadError);
    context.optionalWarnings = (std::min)(64u,
        context.optionalWarnings + uint32_t((std::min)(size_t(64), scene->metadata.warnings.count)));
    if (scene->meshes.count > kTierBObjectLimit || scene->materials.count > kTierBMaterialLimit ||
        scene->nodes.count > kTierBObjectLimit) return ImportErrorCode::ResourceLimit;

    SceneMetadata metadata{};
    metadata.generationId = generationId;
    metadata.format = SourceFormatId::Obj;
    metadata.meshCount = uint32_t(scene->meshes.count);
    metadata.nodeCount = uint32_t(scene->nodes.count);
    metadata.upAxis = UpAxisId::Unknown;
    BoundedChunkWriter writer(destination, generationId, maxChunkCount, metadata, batchSink);
    constexpr uint64_t kNormalizedTriangleBytes =
        3 * sizeof(VertexPositionNormalUv0TangentColorF32) + 3 * sizeof(uint32_t);
    if (destination.size() <= kSectionHeaderSize + kChunkDescriptorSize)
        return ImportErrorCode::ResourceLimit;
    const uint32_t chunkTriangleLimit = uint32_t((std::min<uint64_t>)(kChunkTriangles,
        (destination.size() - kSectionHeaderSize - kChunkDescriptorSize) / kNormalizedTriangleBytes));
    if (!chunkTriangleLimit) return ImportErrorCode::ResourceLimit;
    std::unordered_map<const ufbx_material*, uint32_t> materialIds;
    if (!EmitMaterials(*scene, writer, context, materialIds))
        return context.error != ImportErrorCode::None ? context.error : writer.Error();
    if (!EmitGeometry(*scene, writer, context, materialIds, chunkTriangleLimit))
        return context.error != ImportErrorCode::None ? context.error : writer.Error();
    if (context.textureWarnings) {
        ChunkDescriptor descriptor{}; descriptor.topology = ChunkTopology::TextureWarning;
        descriptor.chunkId = writer.NextId();
        if (!writer.Add(descriptor, ChunkBytes(context.textureWarnings))) return writer.Error();
    }
    if (context.textureWarnings || context.optionalWarnings) {
        ImportStatusPayload status{};
        status.textureWarnings = context.textureWarnings;
        status.optionalFeatureWarnings = context.optionalWarnings;
        ChunkDescriptor descriptor{}; descriptor.topology = ChunkTopology::ImportStatus;
        descriptor.chunkId = writer.NextId();
        if (!writer.Add(descriptor, ChunkBytes(status))) return writer.Error();
    }
    if (!writer.Complete()) return writer.Error();
    return ObjImportResult{writer.Count(), writer.Length()};
}

} // namespace import_worker
