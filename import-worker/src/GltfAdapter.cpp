#include "GltfAdapter.h"

#include "DracoDecodeAdapter.h"
#include "TextureTranscodeAdapter.h"

#include "model_core/Checksum.h"
#include "model_core/MaterialPayload.h"
#include "model_core/PixelFormats.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"
#include "platform/CheckedMath.h"

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>

#include <cstring>
#include <filesystem>
#include <optional>
#include <unordered_map>
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

// Aggregate decoded-texture-pixel budget (Tier A), enforced here before any
// cross-process publication -- the first of two independent checks;
// SharedSectionValidator re-enforces the same budget at the trust boundary
// and must never rely on this worker-side accounting alone.
constexpr uint64_t kMaxAggregateDecodedTexturePixels = 1'000'000'000;

// One fully-assembled mesh chunk's worth of data, held in memory before the
// total section size is known and everything is written out in one pass --
// mirrors SyntheticSceneGenerator's "compute everything, check once, then
// write sequentially, header last" structure.
struct PendingChunk {
    std::vector<VertexPositionNormalUv0F32> vertices;
    std::vector<uint32_t> indices;
    std::optional<size_t> pendingMaterialIndex; // index into WalkState::pendingMaterials
};

struct PendingImage {
    PixelFormatId pixelFormat = PixelFormatId::Unknown;
    uint32_t width = 0;
    uint32_t height = 0;
    ColorSpaceId colorSpace = ColorSpaceId::Srgb;
    std::vector<std::byte> pixelBytes; // level 0 only this slice
};

struct PendingMaterial {
    MaterialPayload data{};
    std::optional<size_t> pendingBaseColorImageIndex; // index into WalkState::pendingImages
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
    std::vector<PendingMaterial> pendingMaterials;
    std::unordered_map<size_t, size_t> materialIndexToPendingIndex; // glTF material index -> pendingMaterials index
    std::vector<PendingImage> pendingImages;
    std::unordered_map<size_t, size_t> imageIndexToPendingIndex; // glTF image index -> pendingImages index
    size_t totalVertices = 0;
    size_t totalIndices = 0;
    uint64_t totalDecodedImagePixels = 0;
    uint32_t maxChunkCount = 0;
    ImportErrorCode error = ImportErrorCode::None;
};

// GLB-embedded buffers only -- the worker has no path authority to load an
// external URI. Returns nullopt for any out-of-range index, non-embedded
// data source, or out-of-bounds byteOffset/byteLength.
std::optional<std::span<const std::byte>> ResolveBufferViewBytes(const fastgltf::Asset& asset,
                                                                    size_t bufferViewIndex)
{
    if (bufferViewIndex >= asset.bufferViews.size()) {
        return std::nullopt;
    }
    const fastgltf::BufferView& view = asset.bufferViews[bufferViewIndex];
    if (view.bufferIndex >= asset.buffers.size()) {
        return std::nullopt;
    }
    const fastgltf::Buffer& buffer = asset.buffers[view.bufferIndex];
    const auto* array = std::get_if<fastgltf::sources::Array>(&buffer.data);
    if (array == nullptr) {
        return std::nullopt;
    }
    auto end = CheckedAdd(static_cast<uint64_t>(view.byteOffset), static_cast<uint64_t>(view.byteLength));
    if (!end || *end > array->bytes.size()) {
        return std::nullopt;
    }
    return std::span<const std::byte>(array->bytes.data() + view.byteOffset, view.byteLength);
}

// Resolves (with dedup by glTF image index) the pending-image index for a
// KHR_texture_basisu image, transcoding its embedded KTX2 bytes. Transcode
// failure is soft -- returns nullopt without setting state.error, which the
// caller treats as "no base color texture," never a hard import failure
// (Draco geometry decode is the asymmetric opposite: required geometry
// fails hard). A fatal condition (resource-limit overflow) sets state.error
// and also returns nullopt; callers must check state.error to distinguish
// the two.
std::optional<size_t> ResolveBasisuImage(WalkState& state, size_t imageIndex)
{
    auto existing = state.imageIndexToPendingIndex.find(imageIndex);
    if (existing != state.imageIndexToPendingIndex.end()) {
        return existing->second;
    }

    if (imageIndex >= state.asset.images.size()) {
        return std::nullopt; // soft-fail: malformed texture reference, not a required-geometry problem
    }
    const fastgltf::Image& image = state.asset.images[imageIndex];
    const auto* bufferViewSource = std::get_if<fastgltf::sources::BufferView>(&image.data);
    if (bufferViewSource == nullptr || bufferViewSource->mimeType != fastgltf::MimeType::KTX2) {
        return std::nullopt; // only a GLB-embedded KTX2 bufferView is supported this slice
    }
    auto ktx2Bytes = ResolveBufferViewBytes(state.asset, bufferViewSource->bufferViewIndex);
    if (!ktx2Bytes) {
        return std::nullopt;
    }

    auto transcoded = TranscodeKtx2BasisImage(*ktx2Bytes);
    if (!transcoded) {
        return std::nullopt;
    }

    auto pixelCount = CheckedMultiply(static_cast<uint64_t>(transcoded->width),
                                       static_cast<uint64_t>(transcoded->height));
    auto newTotal = pixelCount ? CheckedAdd(state.totalDecodedImagePixels, *pixelCount) : std::nullopt;
    if (!pixelCount || !newTotal || *newTotal > kMaxAggregateDecodedTexturePixels) {
        state.error = ImportErrorCode::ResourceLimit;
        return std::nullopt;
    }
    state.totalDecodedImagePixels = *newTotal;

    if (state.chunks.size() + state.pendingMaterials.size() + state.pendingImages.size()
        >= state.maxChunkCount) {
        state.error = ImportErrorCode::ResourceLimit;
        return std::nullopt;
    }

    PendingImage pending;
    pending.pixelFormat = transcoded->pixelFormat;
    pending.width = transcoded->width;
    pending.height = transcoded->height;
    pending.colorSpace = ColorSpaceId::Srgb; // base color is the only semantic handled this slice
    pending.pixelBytes = std::move(transcoded->pixelBytes);

    size_t pendingIndex = state.pendingImages.size();
    state.pendingImages.push_back(std::move(pending));
    state.imageIndexToPendingIndex.emplace(imageIndex, pendingIndex);
    return pendingIndex;
}

// Resolves (with dedup by glTF material index) the pending-material index
// for asset.materials[materialIndex]. Only material.pbrData.baseColorTexture
// is inspected, and only when it carries a KHR_texture_basisu image -- a
// plain PNG/JPEG/WebP base-color texture is deliberately skipped (the
// material keeps its factors, no image dependency), matching the design
// doc's "missing/unsupported optional texture falls back without hiding the
// mesh" policy. Returns nullopt only on a fatal error (state.error is set).
std::optional<size_t> ResolveMaterial(WalkState& state, size_t materialIndex)
{
    auto existing = state.materialIndexToPendingIndex.find(materialIndex);
    if (existing != state.materialIndexToPendingIndex.end()) {
        return existing->second;
    }

    if (materialIndex >= state.asset.materials.size()) {
        state.error = ImportErrorCode::MalformedData;
        return std::nullopt;
    }
    if (state.chunks.size() + state.pendingMaterials.size() + state.pendingImages.size()
        >= state.maxChunkCount) {
        state.error = ImportErrorCode::ResourceLimit;
        return std::nullopt;
    }

    const fastgltf::Material& material = state.asset.materials[materialIndex];

    PendingMaterial pending;
    pending.data.baseColorFactor[0] = material.pbrData.baseColorFactor.x();
    pending.data.baseColorFactor[1] = material.pbrData.baseColorFactor.y();
    pending.data.baseColorFactor[2] = material.pbrData.baseColorFactor.z();
    pending.data.baseColorFactor[3] = material.pbrData.baseColorFactor.w();
    pending.data.metallicFactor = material.pbrData.metallicFactor;
    pending.data.roughnessFactor = material.pbrData.roughnessFactor;
    pending.data.emissiveFactor[0] = material.emissiveFactor.x();
    pending.data.emissiveFactor[1] = material.emissiveFactor.y();
    pending.data.emissiveFactor[2] = material.emissiveFactor.z();
    pending.data.uvOffset[0] = 0.0f;
    pending.data.uvOffset[1] = 0.0f;
    pending.data.uvScale[0] = 1.0f;
    pending.data.uvScale[1] = 1.0f;
    pending.data.uvRotation = 0.0f;
    pending.data.alphaMode = static_cast<uint32_t>(material.alphaMode); // AlphaMode/AlphaModeId share numeric values
    pending.data.alphaCutoff = material.alphaCutoff;
    pending.data.flags = (material.doubleSided ? kMaterialFlagDoubleSided : 0u)
        | (material.unlit ? kMaterialFlagUnlit : 0u);
    pending.data.reserved0 = 0;

    if (material.pbrData.baseColorTexture.has_value()) {
        const fastgltf::TextureInfo& textureInfo = *material.pbrData.baseColorTexture;
        if (textureInfo.transform) {
            pending.data.uvOffset[0] = textureInfo.transform->uvOffset.x();
            pending.data.uvOffset[1] = textureInfo.transform->uvOffset.y();
            pending.data.uvScale[0] = textureInfo.transform->uvScale.x();
            pending.data.uvScale[1] = textureInfo.transform->uvScale.y();
            pending.data.uvRotation = textureInfo.transform->rotation;
        }

        if (textureInfo.textureIndex < state.asset.textures.size()) {
            const fastgltf::Texture& texture = state.asset.textures[textureInfo.textureIndex];
            if (texture.basisuImageIndex.has_value()) {
                pending.pendingBaseColorImageIndex = ResolveBasisuImage(state, *texture.basisuImageIndex);
                if (state.error != ImportErrorCode::None) {
                    return std::nullopt;
                }
            }
            // A plain (non-basisu) image index is deliberately skipped this
            // slice -- material keeps its factors, no image dependency.
        }
    }

    size_t pendingIndex = state.pendingMaterials.size();
    state.pendingMaterials.push_back(std::move(pending));
    state.materialIndexToPendingIndex.emplace(materialIndex, pendingIndex);
    return pendingIndex;
}

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

    if (state.chunks.size() + state.pendingMaterials.size() + state.pendingImages.size()
        >= state.maxChunkCount) {
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

    if (primitive.dracoCompression != nullptr) {
        const fastgltf::DracoCompressedPrimitive& dracoPrimitive = *primitive.dracoCompression;
        auto compressedBytes = ResolveBufferViewBytes(state.asset, dracoPrimitive.bufferView);
        if (!compressedBytes) {
            state.error = ImportErrorCode::MalformedData;
            return false;
        }

        // KHR_draco_mesh_compression requires every attribute present in the
        // primitive's regular attributes list also appear in the
        // compression's own attribute-id map -- a mismatch here (hasNormal/
        // hasUv true above but absent from dracoPrimitive.attributes) is a
        // malformed file, not an optional-feature gap.
        DracoAttributeIds attributeIds;
        if (auto it = dracoPrimitive.findAttribute("POSITION"); it != dracoPrimitive.attributes.end()) {
            attributeIds.position = static_cast<uint32_t>(it->accessorIndex);
        }
        if (hasNormal) {
            auto it = dracoPrimitive.findAttribute("NORMAL");
            if (it == dracoPrimitive.attributes.end()) {
                state.error = ImportErrorCode::MalformedData;
                return false;
            }
            attributeIds.normal = static_cast<uint32_t>(it->accessorIndex);
        }
        if (hasUv) {
            auto it = dracoPrimitive.findAttribute("TEXCOORD_0");
            if (it == dracoPrimitive.attributes.end()) {
                state.error = ImportErrorCode::MalformedData;
                return false;
            }
            attributeIds.uv0 = static_cast<uint32_t>(it->accessorIndex);
        }

        auto decoded = DecodeDracoMesh(*compressedBytes, attributeIds, positionAccessor.count,
                                        indexAccessor.count);
        if (std::holds_alternative<ImportErrorCode>(decoded)) {
            state.error = std::get<ImportErrorCode>(decoded);
            return false;
        }
        DracoDecodedMesh& decodedMesh = std::get<DracoDecodedMesh>(decoded);

        for (size_t idx = 0; idx < chunk.vertices.size(); ++idx) {
            fastgltf::math::fvec4 worldPos = world
                * fastgltf::math::fvec4(decodedMesh.positions[idx * 3 + 0],
                                         decodedMesh.positions[idx * 3 + 1],
                                         decodedMesh.positions[idx * 3 + 2], 1.0f);
            chunk.vertices[idx].px = worldPos.x();
            chunk.vertices[idx].py = worldPos.y();
            chunk.vertices[idx].pz = worldPos.z();

            if (hasUv) {
                chunk.vertices[idx].u = (*decodedMesh.uv0)[idx * 2 + 0];
                chunk.vertices[idx].v = (*decodedMesh.uv0)[idx * 2 + 1];
            } else {
                chunk.vertices[idx].u = 0.0f;
                chunk.vertices[idx].v = 0.0f;
            }
        }
        chunk.indices = std::move(decodedMesh.indices);

        for (uint32_t index : chunk.indices) {
            if (index >= chunk.vertices.size()) {
                state.error = ImportErrorCode::MalformedData;
                return false;
            }
        }

        if (hasNormal) {
            for (size_t idx = 0; idx < chunk.vertices.size(); ++idx) {
                fastgltf::math::fvec3 n((*decodedMesh.normals)[idx * 3 + 0],
                                         (*decodedMesh.normals)[idx * 3 + 1],
                                         (*decodedMesh.normals)[idx * 3 + 2]);
                fastgltf::math::fvec3 worldNormal = fastgltf::math::normalize(normalMatrix * n);
                chunk.vertices[idx].nx = worldNormal.x();
                chunk.vertices[idx].ny = worldNormal.y();
                chunk.vertices[idx].nz = worldNormal.z();
            }
        } else {
            GenerateFlatNormals(chunk);
        }
    } else {
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
    }

    state.totalVertices += chunk.vertices.size();
    state.totalIndices += chunk.indices.size();

    if (primitive.materialIndex.has_value()) {
        chunk.pendingMaterialIndex = ResolveMaterial(state, *primitive.materialIndex);
        if (state.error != ImportErrorCode::None) {
            return false;
        }
    }

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

    // Only these three extensions are enabled -- a file requiring any other
    // one still fails cleanly with MissingExtensions/UnknownRequiredExtension
    // rather than being parsed. KHR_texture_transform is enabled read-only,
    // purely so MaterialPayload's uvOffset/uvScale/uvRotation fields can be
    // populated when present; if that ever proves troublesome it can be
    // dropped independently of Draco/basisu support (uv transform would
    // just stay at identity). Never LoadExternalBuffers/LoadExternalImages:
    // the worker has no path authority. LoadGLBBuffers is deprecated in
    // 0.9.0 (now default behaviour) and deliberately not passed.
    fastgltf::Parser parser(fastgltf::Extensions::KHR_draco_mesh_compression
                             | fastgltf::Extensions::KHR_texture_basisu
                             | fastgltf::Extensions::KHR_texture_transform);
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

    // Fixed chunk order/numbering across the whole section: meshes
    // [1, meshChunkCount], then materials, then images -- backpatched into
    // dependencyIds below once every chunk's id is known.
    const size_t meshChunkCount = state.chunks.size();
    const size_t materialChunkCount = state.pendingMaterials.size();
    const size_t imageChunkCount = state.pendingImages.size();
    const size_t totalChunkCount = meshChunkCount + materialChunkCount + imageChunkCount;

    auto meshChunkId = [&](size_t i) { return static_cast<uint32_t>(i + 1); };
    auto materialChunkId
        = [&](size_t j) { return static_cast<uint32_t>(meshChunkCount + j + 1); };
    auto imageChunkId = [&](size_t k) {
        return static_cast<uint32_t>(meshChunkCount + materialChunkCount + k + 1);
    };

    // Compute layout and total size before writing anything -- mirrors
    // SyntheticSceneGenerator's "compute everything, check once, then write
    // sequentially, header last" structure.
    auto descriptorTableBytes
        = CheckedMultiply(static_cast<uint64_t>(totalChunkCount), kChunkDescriptorSize);
    if (!descriptorTableBytes) {
        return ImportErrorCode::ResourceLimit;
    }
    auto headerAndTable = CheckedAdd(kSectionHeaderSize, *descriptorTableBytes);
    if (!headerAndTable) {
        return ImportErrorCode::ResourceLimit;
    }

    std::vector<uint64_t> payloadOffsets(totalChunkCount);
    std::vector<uint64_t> payloadSizes(totalChunkCount);
    uint64_t offset = *headerAndTable;
    for (size_t i = 0; i < meshChunkCount; ++i) {
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
    for (size_t j = 0; j < materialChunkCount; ++j) {
        size_t combined = meshChunkCount + j;
        uint64_t payloadSize = sizeof(MaterialPayload);
        auto nextOffset = CheckedAdd(offset, payloadSize);
        if (!nextOffset) {
            return ImportErrorCode::ResourceLimit;
        }
        payloadOffsets[combined] = offset;
        payloadSizes[combined] = payloadSize;
        offset = *nextOffset;
    }
    for (size_t k = 0; k < imageChunkCount; ++k) {
        size_t combined = meshChunkCount + materialChunkCount + k;
        auto payloadSize = CheckedAdd(static_cast<uint64_t>(sizeof(ImagePayloadHeader)),
                                       static_cast<uint64_t>(state.pendingImages[k].pixelBytes.size()));
        auto nextOffset = payloadSize ? CheckedAdd(offset, *payloadSize) : std::nullopt;
        if (!payloadSize || !nextOffset) {
            return ImportErrorCode::ResourceLimit;
        }
        payloadOffsets[combined] = offset;
        payloadSizes[combined] = *payloadSize;
        offset = *nextOffset;
    }
    uint64_t sectionLength = offset;

    if (sectionLength > destination.size()) {
        return ImportErrorCode::ResourceLimit;
    }

    for (size_t i = 0; i < meshChunkCount; ++i) {
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
        descriptor.chunkId = meshChunkId(i);
        descriptor.byteSize = payloadSizes[i];
        if (chunk.pendingMaterialIndex.has_value()) {
            descriptor.dependencyIds[0] = materialChunkId(*chunk.pendingMaterialIndex);
            descriptor.dependencyCount = 1;
        } else {
            descriptor.dependencyCount = 0;
        }
        descriptor.chunkChecksum = Fnv1a64(destination.subspan(payloadOffsets[i], payloadSizes[i]));

        std::memcpy(destination.data() + kSectionHeaderSize + i * kChunkDescriptorSize, &descriptor,
                    sizeof(descriptor));
    }

    for (size_t j = 0; j < materialChunkCount; ++j) {
        size_t combined = meshChunkCount + j;
        const PendingMaterial& material = state.pendingMaterials[j];

        std::memcpy(destination.data() + payloadOffsets[combined], &material.data, sizeof(MaterialPayload));

        ChunkDescriptor descriptor{};
        descriptor.sourceRangeOffset = 0;
        descriptor.sourceRangeLength = 0;
        descriptor.normalizedRangeOffset = payloadOffsets[combined];
        descriptor.normalizedRangeLength = payloadSizes[combined];
        descriptor.topology = ChunkTopology::Material;
        descriptor.indexCount = 0;
        descriptor.vertexCount = 0;
        descriptor.vertexLayoutId = 0;
        descriptor.lodLevel = 0;
        descriptor.chunkId = materialChunkId(j);
        descriptor.byteSize = payloadSizes[combined];
        if (material.pendingBaseColorImageIndex.has_value()) {
            descriptor.dependencyIds[0] = imageChunkId(*material.pendingBaseColorImageIndex);
            descriptor.dependencyCount = 1;
        } else {
            descriptor.dependencyCount = 0;
        }
        descriptor.chunkChecksum
            = Fnv1a64(destination.subspan(payloadOffsets[combined], payloadSizes[combined]));

        std::memcpy(destination.data() + kSectionHeaderSize + combined * kChunkDescriptorSize, &descriptor,
                    sizeof(descriptor));
    }

    for (size_t k = 0; k < imageChunkCount; ++k) {
        size_t combined = meshChunkCount + materialChunkCount + k;
        const PendingImage& image = state.pendingImages[k];

        ImagePayloadHeader imageHeader{};
        imageHeader.pixelFormat = static_cast<uint32_t>(image.pixelFormat);
        imageHeader.width = image.width;
        imageHeader.height = image.height;
        imageHeader.mipLevels = 1; // level 0 only this slice
        imageHeader.colorSpace = static_cast<uint32_t>(image.colorSpace);
        imageHeader.reserved0 = 0;
        imageHeader.pixelDataByteSize = image.pixelBytes.size();

        std::memcpy(destination.data() + payloadOffsets[combined], &imageHeader, sizeof(imageHeader));
        std::memcpy(destination.data() + payloadOffsets[combined] + sizeof(imageHeader),
                    image.pixelBytes.data(), image.pixelBytes.size());

        ChunkDescriptor descriptor{};
        descriptor.sourceRangeOffset = 0;
        descriptor.sourceRangeLength = 0;
        descriptor.normalizedRangeOffset = payloadOffsets[combined];
        descriptor.normalizedRangeLength = payloadSizes[combined];
        descriptor.topology = ChunkTopology::Image;
        descriptor.indexCount = 0;
        descriptor.vertexCount = 0;
        descriptor.vertexLayoutId = 0;
        descriptor.lodLevel = 0;
        descriptor.chunkId = imageChunkId(k);
        descriptor.byteSize = payloadSizes[combined];
        descriptor.dependencyCount = 0; // images reference nothing
        descriptor.chunkChecksum
            = Fnv1a64(destination.subspan(payloadOffsets[combined], payloadSizes[combined]));

        std::memcpy(destination.data() + kSectionHeaderSize + combined * kChunkDescriptorSize, &descriptor,
                    sizeof(descriptor));
    }

    SectionHeader header{};
    header.magic = kSectionMagic;
    header.protocolVersion = kCurrentProtocolVersion;
    header.generationId = generationId;
    header.sectionLength = sectionLength;
    header.chunkCount = static_cast<uint32_t>(totalChunkCount);
    header.reserved = 0;
    header.sectionChecksum
        = Fnv1a64(destination.subspan(kSectionHeaderSize, sectionLength - kSectionHeaderSize));

    std::memcpy(destination.data(), &header, sizeof(header));

    GltfImportResult result;
    result.chunkCount = static_cast<uint32_t>(totalChunkCount);
    result.sectionBytesWritten = sectionLength;
    return result;
}

} // namespace import_worker
