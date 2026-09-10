#include "GltfAdapter.h"

#include "ChunkBatchSink.h"
#include "DracoDecodeAdapter.h"
#include "ImageFormatSniff.h"
#include "SidecarFileClient.h"
#include "TextureTranscodeAdapter.h"
#include "WicImageDecodeAdapter.h"

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
#include <iterator>
#include <optional>
#include <span>
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
    // Indices into WalkState::pendingImages, fixed slot order per
    // WireFormat.h's documented ChunkDescriptor::dependencyIds contract:
    // {baseColor, metallicRoughness, normal, emissive}.
    std::optional<size_t> pendingBaseColorImageIndex;
    std::optional<size_t> pendingMetallicRoughnessImageIndex;
    std::optional<size_t> pendingNormalImageIndex;
    std::optional<size_t> pendingEmissiveImageIndex;
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

// One external (sources::URI) buffer's resolution outcome. The error code
// is memoized alongside the bytes so a failure can be reported with the
// reason the host actually gave -- UnsafeReference when its path policy
// rejected the reference, FileUnavailable when the file was missing, empty
// or oversized -- instead of collapsing every sidecar failure into
// MalformedData.
struct ResolvedExternalBuffer {
    std::optional<std::vector<std::byte>> bytes;
    model_core::ImportErrorCode errorCode = model_core::ImportErrorCode::None;
};

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
    // Lazily populated by ResolveBufferViewBytes on first access to an
    // external (sources::URI) buffer index -- resolves only what's
    // actually needed (e.g. a Draco-compressed primitive's bufferView),
    // memoizing so a buffer referenced by multiple bufferViews is only
    // requested from the sidecar once. Empty/unused entirely for a self-
    // contained .glb. nullptr sidecarClient (the always-self-contained
    // shared-section ParseGltfRequest path) means an external buffer is
    // simply never resolvable.
    std::unordered_map<size_t, ResolvedExternalBuffer> resolvedExternalBuffers;
    SidecarFileClient* sidecarClient = nullptr;
};

// GLB-embedded (sources::Array) buffers, or an external (sources::URI)
// buffer lazily resolved via state.sidecarClient on first access and
// memoized in WalkState::resolvedExternalBuffers (nullptr sidecarClient --
// the always-self-contained shared-section path -- means a URI buffer is
// simply unresolvable). Returns nullopt for any out-of-range index,
// unresolvable data source, or out-of-bounds byteOffset/byteLength.
std::optional<std::span<const std::byte>> ResolveBufferViewBytes(WalkState& state, size_t bufferViewIndex)
{
    if (bufferViewIndex >= state.asset.bufferViews.size()) {
        return std::nullopt;
    }
    const fastgltf::BufferView& view = state.asset.bufferViews[bufferViewIndex];
    if (view.bufferIndex >= state.asset.buffers.size()) {
        return std::nullopt;
    }
    const fastgltf::Buffer& buffer = state.asset.buffers[view.bufferIndex];

    std::span<const std::byte> bufferBytes;
    if (const auto* array = std::get_if<fastgltf::sources::Array>(&buffer.data)) {
        bufferBytes = std::span<const std::byte>(array->bytes.data(), array->bytes.size());
    } else if (const auto* uriSource = std::get_if<fastgltf::sources::URI>(&buffer.data)) {
        auto cached = state.resolvedExternalBuffers.find(view.bufferIndex);
        if (cached == state.resolvedExternalBuffers.end()) {
            ResolvedExternalBuffer resolved;
            if (state.sidecarClient != nullptr) {
                auto result = state.sidecarClient->RequestSidecarBytes(std::string(uriSource->uri.path()));
                resolved.bytes = std::move(result.bytes);
                resolved.errorCode = result.errorCode;
            } else {
                // No sidecar channel at all (the always-self-contained
                // shared-section ParseGltfRequest path): an external
                // reference is unresolvable by construction, not by policy.
                resolved.errorCode = ImportErrorCode::FileUnavailable;
            }
            cached = state.resolvedExternalBuffers.emplace(view.bufferIndex, std::move(resolved)).first;
        }
        if (!cached->second.bytes.has_value()) {
            return std::nullopt;
        }
        bufferBytes
            = std::span<const std::byte>(cached->second.bytes->data(), cached->second.bytes->size());
    } else {
        return std::nullopt;
    }

    auto end = CheckedAdd(static_cast<uint64_t>(view.byteOffset), static_cast<uint64_t>(view.byteLength));
    if (!end || *end > bufferBytes.size()) {
        return std::nullopt;
    }
    return bufferBytes.subspan(view.byteOffset, view.byteLength);
}

// Maps a failed ResolveBufferViewBytes back to the most specific code
// available: the host's own answer when it gave one (UnsafeReference for a
// path-policy rejection, FileUnavailable for missing/empty/oversized),
// otherwise MalformedData for a structural fault in the asset itself --
// out-of-range index, unsupported data source, or a byteOffset/byteLength
// that overruns the resolved buffer.
ImportErrorCode ExternalBufferFailureCode(const WalkState& state, size_t bufferViewIndex)
{
    if (bufferViewIndex >= state.asset.bufferViews.size()) {
        return ImportErrorCode::MalformedData;
    }
    auto cached
        = state.resolvedExternalBuffers.find(state.asset.bufferViews[bufferViewIndex].bufferIndex);
    if (cached != state.resolvedExternalBuffers.end()
        && cached->second.errorCode != ImportErrorCode::None) {
        return cached->second.errorCode;
    }
    return ImportErrorCode::MalformedData;
}

// Supplies accessor bytes to fastgltf from this worker's own sidecar cache.
//
// fastgltf's accessor readers take a BufferDataAdapter as a defaulted
// template parameter (fastgltf/tools.hpp, DefaultBufferDataAdapter), and
// IterableAccessor routes EVERY read through it -- the primary bufferView
// plus, for a sparse accessor, sparse->indicesBufferView and
// sparse->valuesBufferView. Supplying our own therefore keeps fastgltf's
// sparse-override, component-type up-conversion and interleaved-stride
// handling exactly as written, while sourcing the bytes from
// ResolveBufferViewBytes. That is the only way a sandboxed worker can read
// an external .bin at all: Options::LoadExternalBuffers would make fastgltf
// perform its own file I/O, which this process has no path authority for
// (it is precisely why SidecarFileClient exists).
//
// Callers MUST have run EnsureAccessorBytesResolvable over the accessor
// first -- see that function for why an unresolvable view cannot be
// signalled from inside here.
class SidecarBufferDataAdapter {
public:
    explicit SidecarBufferDataAdapter(WalkState& state) noexcept
        : state_(&state)
    {
    }

    std::span<const std::byte> operator()(const fastgltf::Asset&, size_t bufferViewIndex) const
    {
        auto bytes = ResolveBufferViewBytes(*state_, bufferViewIndex);
        if (!bytes) {
            // Unreachable after a successful pre-flight; kept so a future
            // caller that forgets it fails closed with a recorded error
            // rather than silently importing a truncated mesh.
            if (state_->error == ImportErrorCode::None) {
                state_->error = ExternalBufferFailureCode(*state_, bufferViewIndex);
            }
            return {};
        }
        return *bytes;
    }

private:
    WalkState* state_;
};

// Pre-flights every bufferView an accessor will read through
// SidecarBufferDataAdapter, resolving each one (memoized, so the adapter's
// later call is free) and bounds-checking the byteOffset the library will
// apply to it.
//
// This has to happen before the iterate call rather than inside the
// adapter. fastgltf's IterableAccessor does
// `adapter(...).subspan(accessor.byteOffset)` unconditionally, and
// fastgltf::span::subspan is not bounds-checked -- it evaluates
// `&data()[offset]` and `size() - offset`, so handing back an empty span
// for an accessor with a nonzero byteOffset would form an out-of-range
// pointer and an underflowed length, then read through it. Failing here
// keeps the "never hand the library an unverified read" discipline the rest
// of this file already follows.
bool EnsureAccessorBytesResolvable(WalkState& state, const fastgltf::Accessor& accessor)
{
    auto viewIsReadable = [&](size_t bufferViewIndex, size_t byteOffset) {
        auto bytes = ResolveBufferViewBytes(state, bufferViewIndex);
        if (!bytes) {
            state.error = ExternalBufferFailureCode(state, bufferViewIndex);
            return false;
        }
        if (byteOffset > bytes->size()) {
            state.error = ImportErrorCode::MalformedData;
            return false;
        }
        return true;
    };

    // A sparse accessor may legally omit bufferView entirely (its base
    // values are then all zeros), but fastgltf's IterableAccessor
    // constructor indexes asset.bufferViews[*accessor.bufferViewIndex]
    // unconditionally, with no has_value() guard -- so handing it such an
    // accessor dereferences an empty optional. Reject rather than crash;
    // this is a rare authoring shape, and the previous code reached it too.
    if (!accessor.bufferViewIndex.has_value()) {
        state.error = ImportErrorCode::MalformedData;
        return false;
    }
    if (!viewIsReadable(*accessor.bufferViewIndex, accessor.byteOffset)) {
        return false;
    }
    if (accessor.sparse.has_value()) {
        if (!viewIsReadable(accessor.sparse->indicesBufferView, accessor.sparse->indicesByteOffset)
            || !viewIsReadable(accessor.sparse->valuesBufferView,
                               accessor.sparse->valuesByteOffset)) {
            return false;
        }
    }
    return true;
}

// Returns owned encoded bytes for asset.images[imageIndex]'s data source.
// A GLB-embedded bufferView is copied out; an external sources::URI is
// resolved via state.sidecarClient (nullptr for the always-self-contained
// shared-section path, in which case a URI source is simply unavailable).
// A data URI never reaches this function as sources::URI -- fastgltf
// decodes it into sources::Array unconditionally, confirmed by reading
// fastgltf.cpp directly (the LoadExternalImages option gate only applies
// to sources::URI's *local-path* branch, not the isDataUri() branch, since
// decoding a data URI needs no filesystem I/O) -- so sources::URI here
// always means a real external file reference.
std::optional<std::vector<std::byte>> ResolveImageEncodedBytes(WalkState& state, size_t imageIndex)
{
    if (imageIndex >= state.asset.images.size()) {
        return std::nullopt;
    }
    const fastgltf::Image& image = state.asset.images[imageIndex];

    if (const auto* bufferViewSource = std::get_if<fastgltf::sources::BufferView>(&image.data)) {
        auto bytes = ResolveBufferViewBytes(state, bufferViewSource->bufferViewIndex);
        if (!bytes) {
            return std::nullopt;
        }
        return std::vector<std::byte>(bytes->begin(), bytes->end());
    }

    if (const auto* uriSource = std::get_if<fastgltf::sources::URI>(&image.data)) {
        if (state.sidecarClient == nullptr) {
            return std::nullopt;
        }
        auto result = state.sidecarClient->RequestSidecarBytes(std::string(uriSource->uri.path()));
        return result.bytes; // nullopt on any failure -- images are never geometry-required
    }

    return std::nullopt;
}

// Resolves (with dedup by glTF image index) the pending-image index for
// asset.images[imageIndex], regardless of source (GLB-embedded bufferView
// or, when state.sidecarClient is set, an external sidecar file) or
// container (KTX2/Basis via TranscodeKtx2BasisImage, or a plain raster via
// DecodeRasterImageWic) -- the actual container is sniffed from the bytes
// themselves (ImageFormatSniff.h), never trusted from a declared MIME type
// alone, per the texture policy's "verified from bytes... rather than
// extension alone." WebP and anything unrecognized soft-fail (no adapter
// this chunk). Decode/transcode failure is soft -- returns nullopt without
// setting state.error, which the caller treats as "no texture in this
// slot," never a hard import failure (Draco geometry decode is the
// asymmetric opposite: required geometry fails hard). A fatal condition
// (resource-limit overflow) sets state.error and also returns nullopt;
// callers must check state.error to distinguish the two.
std::optional<size_t> ResolveImage(WalkState& state, size_t imageIndex, ColorSpaceId colorSpace)
{
    auto existing = state.imageIndexToPendingIndex.find(imageIndex);
    if (existing != state.imageIndexToPendingIndex.end()) {
        return existing->second;
    }

    auto encodedBytes = ResolveImageEncodedBytes(state, imageIndex);
    if (!encodedBytes) {
        return std::nullopt;
    }

    std::optional<PendingImage> decoded;
    switch (SniffImageFormat(*encodedBytes)) {
    case SniffedImageFormat::Ktx2: {
        if (auto transcoded = TranscodeKtx2BasisImage(*encodedBytes)) {
            PendingImage pending;
            pending.pixelFormat = transcoded->pixelFormat;
            pending.width = transcoded->width;
            pending.height = transcoded->height;
            pending.colorSpace = colorSpace;
            pending.pixelBytes = std::move(transcoded->pixelBytes);
            decoded = std::move(pending);
        }
        break;
    }
    case SniffedImageFormat::Png:
    case SniffedImageFormat::Jpeg:
    case SniffedImageFormat::Bmp:
    case SniffedImageFormat::Tiff: {
        if (auto raster = DecodeRasterImageWic(*encodedBytes, colorSpace)) {
            PendingImage pending;
            pending.pixelFormat = raster->pixelFormat;
            pending.width = raster->width;
            pending.height = raster->height;
            pending.colorSpace = raster->colorSpace;
            pending.pixelBytes = std::move(raster->pixelBytes);
            decoded = std::move(pending);
        }
        break;
    }
    case SniffedImageFormat::WebP: // no libwebp adapter this chunk (ADR-006-assigned, deferred)
    case SniffedImageFormat::Unknown:
    default:
        break;
    }

    if (!decoded) {
        return std::nullopt;
    }

    auto pixelCount = CheckedMultiply(static_cast<uint64_t>(decoded->width), static_cast<uint64_t>(decoded->height));
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

    size_t pendingIndex = state.pendingImages.size();
    state.pendingImages.push_back(std::move(*decoded));
    state.imageIndexToPendingIndex.emplace(imageIndex, pendingIndex);
    return pendingIndex;
}

// Resolves the pending-image index for a texture slot's KHR_texture_basisu
// or plain image, or nullopt if the slot itself is absent, out of range, or
// carries no supported image source at all -- soft-fail throughout, never
// sets state.error for these "no texture" outcomes (only ResolveImage's own
// resource-limit path does).
std::optional<size_t> ResolveTextureSlotImage(WalkState& state, const fastgltf::TextureInfo* textureInfo,
                                                ColorSpaceId colorSpace)
{
    if (textureInfo == nullptr || textureInfo->textureIndex >= state.asset.textures.size()) {
        return std::nullopt;
    }
    const fastgltf::Texture& texture = state.asset.textures[textureInfo->textureIndex];
    std::optional<size_t> gltfImageIndex
        = texture.basisuImageIndex.has_value() ? texture.basisuImageIndex : texture.imageIndex;
    if (!gltfImageIndex.has_value()) {
        return std::nullopt;
    }
    return ResolveImage(state, *gltfImageIndex, colorSpace);
}

// Resolves (with dedup by glTF material index) the pending-material index
// for asset.materials[materialIndex]. All four PBR texture slots
// (baseColor/metallicRoughness/normal/emissive) are inspected, each
// resolved through ResolveTextureSlotImage/ResolveImage -- a texture this
// chunk can't decode/transcode (or that's simply absent) leaves that slot
// unpopulated, matching the design doc's "missing/unsupported optional
// texture falls back without hiding the mesh" policy. Returns nullopt only
// on a fatal error (state.error is set).
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
        pending.pendingBaseColorImageIndex = ResolveTextureSlotImage(state, &textureInfo, ColorSpaceId::Srgb);
        if (state.error != ImportErrorCode::None) {
            return std::nullopt;
        }
    }
    if (material.pbrData.metallicRoughnessTexture.has_value()) {
        pending.pendingMetallicRoughnessImageIndex
            = ResolveTextureSlotImage(state, &*material.pbrData.metallicRoughnessTexture, ColorSpaceId::Linear);
        if (state.error != ImportErrorCode::None) {
            return std::nullopt;
        }
    }
    if (material.normalTexture.has_value()) {
        // NormalTextureInfo derives from TextureInfo -- ResolveTextureSlotImage
        // only needs the base; its extra .scale field is dropped (no
        // MaterialPayload field exists for it this chunk, a deliberate
        // scope call, not an oversight).
        pending.pendingNormalImageIndex = ResolveTextureSlotImage(state, &*material.normalTexture, ColorSpaceId::Linear);
        if (state.error != ImportErrorCode::None) {
            return std::nullopt;
        }
    }
    if (material.emissiveTexture.has_value()) {
        pending.pendingEmissiveImageIndex
            = ResolveTextureSlotImage(state, &*material.emissiveTexture, ColorSpaceId::Srgb);
        if (state.error != ImportErrorCode::None) {
            return std::nullopt;
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
        auto compressedBytes = ResolveBufferViewBytes(state, dracoPrimitive.bufferView);
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
        // Every accessor below is read through SidecarBufferDataAdapter, so
        // an external .bin resolves via this worker's sidecar cache exactly
        // like an embedded buffer. Resolvability is pre-flighted for all of
        // them up front -- see EnsureAccessorBytesResolvable for why this
        // cannot be deferred into the adapter -- and a failure is hard, not
        // a soft skip, since this is core geometry.
        if (!EnsureAccessorBytesResolvable(state, positionAccessor)
            || !EnsureAccessorBytesResolvable(state, indexAccessor)
            || (hasUv
                && !EnsureAccessorBytesResolvable(state,
                                                  state.asset.accessors[uvIt->accessorIndex]))
            || (hasNormal
                && !EnsureAccessorBytesResolvable(
                    state, state.asset.accessors[normalIt->accessorIndex]))) {
            return false;
        }

        const SidecarBufferDataAdapter bufferAdapter(state);

        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
            state.asset, positionAccessor,
            [&](fastgltf::math::fvec3 pos, size_t idx) {
                fastgltf::math::fvec4 worldPos
                    = world * fastgltf::math::fvec4(pos.x(), pos.y(), pos.z(), 1.0f);
                chunk.vertices[idx].px = worldPos.x();
                chunk.vertices[idx].py = worldPos.y();
                chunk.vertices[idx].pz = worldPos.z();
            },
            bufferAdapter);

        if (hasUv) {
            const fastgltf::Accessor& uvAccessor = state.asset.accessors[uvIt->accessorIndex];
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(
                state.asset, uvAccessor,
                [&](fastgltf::math::fvec2 uv, size_t idx) {
                    chunk.vertices[idx].u = uv.x();
                    chunk.vertices[idx].v = uv.y();
                },
                bufferAdapter);
        } else {
            for (auto& v : chunk.vertices) {
                v.u = 0.0f;
                v.v = 0.0f;
            }
        }

        chunk.indices.resize(indexAccessor.count);
        fastgltf::iterateAccessorWithIndex<uint32_t>(
            state.asset, indexAccessor,
            [&](uint32_t index, size_t idx) { chunk.indices[idx] = index; }, bufferAdapter);

        for (uint32_t index : chunk.indices) {
            if (index >= chunk.vertices.size()) {
                state.error = ImportErrorCode::MalformedData;
                return false;
            }
        }

        if (hasNormal) {
            const fastgltf::Accessor& normalAccessor = state.asset.accessors[normalIt->accessorIndex];
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
                state.asset, normalAccessor,
                [&](fastgltf::math::fvec3 n, size_t idx) {
                    fastgltf::math::fvec3 worldNormal = fastgltf::math::normalize(normalMatrix * n);
                    chunk.vertices[idx].nx = worldNormal.x();
                    chunk.vertices[idx].ny = worldNormal.y();
                    chunk.vertices[idx].nz = worldNormal.z();
                },
                bufferAdapter);
        } else {
            GenerateFlatNormals(chunk);
        }

        // Defensive: the pre-flight above should make this unreachable, but
        // the adapter records rather than throws, so never fall through to
        // publishing a chunk built from a failed read.
        if (state.error != ImportErrorCode::None) {
            return false;
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

// ---------------------------------------------------------------------------
// Section writing, one or many batches.
// ---------------------------------------------------------------------------

// One chunk ready to be written. What is deliberately NOT here is where in
// the section it lands and what its checksum is: both depend on which batch
// takes it, which is only decided once the window's remaining room is known.
//
// The payload is held as spans into the parse's own buffers rather than
// copied, so planning a 350 MiB model's batches costs a few descriptors and
// not a second copy of its geometry.
struct PlannedChunk {
    ChunkDescriptor descriptor{};
    std::span<const std::byte> pieceA;
    std::span<const std::byte> pieceB; // empty unless the payload is two runs
    uint64_t payloadBytes = 0;
};

template <typename T>
std::span<const std::byte> BytesOf(const T& value)
{
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(&value), sizeof(T));
}

template <typename T>
std::span<const std::byte> BytesOf(const std::vector<T>& values)
{
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(values.data()),
                                       values.size() * sizeof(T));
}

// Writes one complete, self-consistent section holding exactly `batch` into
// `destination`, header last, exactly as this adapter always did for the
// whole model. Returns the section length, or nullopt if the batch does not
// fit -- which the caller must have already prevented.
std::optional<uint64_t> WriteBatchSection(std::span<std::byte> destination,
                                           std::span<PlannedChunk> batch, uint64_t generationId)
{
    auto descriptorTableBytes
        = CheckedMultiply(static_cast<uint64_t>(batch.size()), kChunkDescriptorSize);
    if (!descriptorTableBytes) {
        return std::nullopt;
    }
    auto headerAndTable = CheckedAdd(kSectionHeaderSize, *descriptorTableBytes);
    if (!headerAndTable) {
        return std::nullopt;
    }

    uint64_t offset = *headerAndTable;
    for (auto& chunk : batch) {
        auto next = CheckedAdd(offset, chunk.payloadBytes);
        if (!next) {
            return std::nullopt;
        }
        chunk.descriptor.normalizedRangeOffset = offset;
        chunk.descriptor.normalizedRangeLength = chunk.payloadBytes;
        chunk.descriptor.byteSize = chunk.payloadBytes;
        offset = *next;
    }
    uint64_t sectionLength = offset;
    if (sectionLength > destination.size()) {
        return std::nullopt;
    }

    for (size_t i = 0; i < batch.size(); ++i) {
        PlannedChunk& chunk = batch[i];
        std::byte* payload = destination.data() + chunk.descriptor.normalizedRangeOffset;
        if (!chunk.pieceA.empty()) {
            std::memcpy(payload, chunk.pieceA.data(), chunk.pieceA.size());
        }
        if (!chunk.pieceB.empty()) {
            std::memcpy(payload + chunk.pieceA.size(), chunk.pieceB.data(), chunk.pieceB.size());
        }
        chunk.descriptor.chunkChecksum = Fnv1a64(
            destination.subspan(chunk.descriptor.normalizedRangeOffset, chunk.payloadBytes));

        std::memcpy(destination.data() + kSectionHeaderSize + i * kChunkDescriptorSize,
                    &chunk.descriptor, sizeof(ChunkDescriptor));
    }

    SectionHeader header{};
    header.magic = kSectionMagic;
    header.protocolVersion = kCurrentProtocolVersion;
    header.generationId = generationId;
    header.sectionLength = sectionLength;
    header.chunkCount = static_cast<uint32_t>(batch.size());
    header.reserved = 0;
    header.sectionChecksum
        = Fnv1a64(destination.subspan(kSectionHeaderSize, sectionLength - kSectionHeaderSize));

    std::memcpy(destination.data(), &header, sizeof(header));
    return sectionLength;
}

// How many of `remaining` fit in one window, always at least one unless the
// first alone cannot fit. The descriptor table grows with the count, so room
// is re-derived per candidate rather than compared against a fixed budget.
size_t CountChunksThatFit(std::span<const PlannedChunk> remaining, uint64_t windowBytes,
                          uint32_t maxChunkCount)
{
    uint64_t payloadSoFar = 0;
    size_t taken = 0;
    for (const auto& chunk : remaining) {
        if (taken + 1 > maxChunkCount) {
            break;
        }
        auto tableBytes = CheckedMultiply(static_cast<uint64_t>(taken + 1), kChunkDescriptorSize);
        if (!tableBytes) {
            break;
        }
        auto headerAndTable = CheckedAdd(kSectionHeaderSize, *tableBytes);
        if (!headerAndTable) {
            break;
        }
        auto payload = CheckedAdd(payloadSoFar, chunk.payloadBytes);
        if (!payload) {
            break;
        }
        auto total = CheckedAdd(*headerAndTable, *payload);
        if (!total || *total > windowBytes) {
            break;
        }
        payloadSoFar = *payload;
        ++taken;
    }
    return taken;
}

} // namespace

std::variant<GltfImportResult, ImportErrorCode> ImportGltf(std::span<const std::byte> sourceGlbBytes,
                                                             std::span<std::byte> destination,
                                                             uint64_t generationId,
                                                             uint32_t maxChunkCount,
                                                             SidecarFileClient* sidecarClient,
                                                             ChunkBatchSink* batchSink)
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
    // loadGltf (rather than loadGltfBinary) auto-detects GLB vs. plain-JSON
    // .gltf via fastgltf::determineGltfFileType internally -- needed so a
    // real multi-file .gltf (this chunk's whole point) parses at all; a
    // self-contained .glb continues to work identically either way.
    auto assetResult = parser.loadGltf(dataBufferResult.get(), std::filesystem::path{},
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
    state.sidecarClient = sidecarClient;

    fastgltf::math::fmat4x4 identity(1.0f);
    for (size_t nodeIndex : asset.scenes[sceneIndex].nodeIndices) {
        if (!VisitNode(state, nodeIndex, identity, 0)) {
            return state.error;
        }
    }

    if (state.chunks.empty()) {
        return ImportErrorCode::MalformedData; // no supported geometry found
    }

    // Chunk ids are fixed for the whole generation, whatever batch each
    // chunk later lands in: meshes [1, meshChunkCount], then materials, then
    // images. That is what lets a mesh in a later batch keep pointing at a
    // material and texture that already crossed in an earlier one -- the host
    // resolves the reference through its per-generation catalog
    // (import_broker::KnownChunkCatalog) -- so a shared 16 MiB texture is
    // sent once for the model rather than once per batch that uses it.
    const size_t meshChunkCount = state.chunks.size();
    const size_t materialChunkCount = state.pendingMaterials.size();
    const size_t imageChunkCount = state.pendingImages.size();

    auto meshChunkId = [&](size_t i) { return static_cast<uint32_t>(i + 1); };
    auto materialChunkId
        = [&](size_t j) { return static_cast<uint32_t>(meshChunkCount + j + 1); };
    auto imageChunkId = [&](size_t k) {
        return static_cast<uint32_t>(meshChunkCount + materialChunkCount + k + 1);
    };

    // Stable storage the image PlannedChunks span, so an image's header and
    // its pixels stay two runs of existing memory instead of being copied
    // into one joined buffer.
    std::vector<ImagePayloadHeader> imageHeaders(imageChunkCount);

    std::vector<PlannedChunk> meshPlans(meshChunkCount);
    std::vector<PlannedChunk> materialPlans(materialChunkCount);
    std::vector<PlannedChunk> imagePlans(imageChunkCount);

    for (size_t i = 0; i < meshChunkCount; ++i) {
        const PendingChunk& chunk = state.chunks[i];
        auto vertexBytes = CheckedMultiply(static_cast<uint64_t>(chunk.vertices.size()),
                                            sizeof(VertexPositionNormalUv0F32));
        auto indexBytes
            = CheckedMultiply(static_cast<uint64_t>(chunk.indices.size()), sizeof(uint32_t));
        if (!vertexBytes || !indexBytes) {
            return ImportErrorCode::ResourceLimit;
        }
        auto payloadSize = CheckedAdd(*vertexBytes, *indexBytes);
        if (!payloadSize) {
            return ImportErrorCode::ResourceLimit;
        }

        PlannedChunk& plan = meshPlans[i];
        plan.descriptor.sourceRangeOffset = 0;
        plan.descriptor.sourceRangeLength = 0;
        plan.descriptor.topology = ChunkTopology::TriangleList;
        plan.descriptor.indexCount = static_cast<uint32_t>(chunk.indices.size());
        plan.descriptor.vertexCount = static_cast<uint32_t>(chunk.vertices.size());
        plan.descriptor.vertexLayoutId = static_cast<uint32_t>(VertexLayoutId::PositionNormalUv0_F32);
        plan.descriptor.lodLevel = 0;
        plan.descriptor.chunkId = meshChunkId(i);
        if (chunk.pendingMaterialIndex.has_value()) {
            plan.descriptor.dependencyIds[0] = materialChunkId(*chunk.pendingMaterialIndex);
            plan.descriptor.dependencyCount = 1;
        } else {
            plan.descriptor.dependencyCount = 0;
        }
        plan.pieceA = BytesOf(chunk.vertices);
        plan.pieceB = BytesOf(chunk.indices);
        plan.payloadBytes = *payloadSize;
    }

    for (size_t j = 0; j < materialChunkCount; ++j) {
        const PendingMaterial& material = state.pendingMaterials[j];

        PlannedChunk& plan = materialPlans[j];
        plan.descriptor.sourceRangeOffset = 0;
        plan.descriptor.sourceRangeLength = 0;
        plan.descriptor.topology = ChunkTopology::Material;
        plan.descriptor.indexCount = 0;
        plan.descriptor.vertexCount = 0;
        plan.descriptor.vertexLayoutId = 0;
        plan.descriptor.lodLevel = 0;
        plan.descriptor.chunkId = materialChunkId(j);
        // Fixed slot order {baseColor, metallicRoughness, normal, emissive}
        // per WireFormat.h; sparsely populated is valid (e.g. a normal-map-
        // only material leaves slots 0/1/3 at their zero-initialized
        // default) -- SharedSectionValidator checks each slot independently,
        // not as a contiguous prefix.
        plan.descriptor.dependencyCount = 0;
        if (material.pendingBaseColorImageIndex.has_value()) {
            plan.descriptor.dependencyIds[0] = imageChunkId(*material.pendingBaseColorImageIndex);
            ++plan.descriptor.dependencyCount;
        }
        if (material.pendingMetallicRoughnessImageIndex.has_value()) {
            plan.descriptor.dependencyIds[1]
                = imageChunkId(*material.pendingMetallicRoughnessImageIndex);
            ++plan.descriptor.dependencyCount;
        }
        if (material.pendingNormalImageIndex.has_value()) {
            plan.descriptor.dependencyIds[2] = imageChunkId(*material.pendingNormalImageIndex);
            ++plan.descriptor.dependencyCount;
        }
        if (material.pendingEmissiveImageIndex.has_value()) {
            plan.descriptor.dependencyIds[3] = imageChunkId(*material.pendingEmissiveImageIndex);
            ++plan.descriptor.dependencyCount;
        }
        plan.pieceA = BytesOf(material.data);
        plan.payloadBytes = sizeof(MaterialPayload);
    }

    for (size_t k = 0; k < imageChunkCount; ++k) {
        const PendingImage& image = state.pendingImages[k];

        ImagePayloadHeader& imageHeader = imageHeaders[k];
        imageHeader.pixelFormat = static_cast<uint32_t>(image.pixelFormat);
        imageHeader.width = image.width;
        imageHeader.height = image.height;
        imageHeader.mipLevels = 1; // level 0 only this slice
        imageHeader.colorSpace = static_cast<uint32_t>(image.colorSpace);
        imageHeader.reserved0 = 0;
        imageHeader.pixelDataByteSize = image.pixelBytes.size();

        auto payloadSize = CheckedAdd(static_cast<uint64_t>(sizeof(ImagePayloadHeader)),
                                       static_cast<uint64_t>(image.pixelBytes.size()));
        if (!payloadSize) {
            return ImportErrorCode::ResourceLimit;
        }

        PlannedChunk& plan = imagePlans[k];
        plan.descriptor.sourceRangeOffset = 0;
        plan.descriptor.sourceRangeLength = 0;
        plan.descriptor.topology = ChunkTopology::Image;
        plan.descriptor.indexCount = 0;
        plan.descriptor.vertexCount = 0;
        plan.descriptor.vertexLayoutId = 0;
        plan.descriptor.lodLevel = 0;
        plan.descriptor.chunkId = imageChunkId(k);
        plan.descriptor.dependencyCount = 0; // images reference nothing
        plan.pieceA = BytesOf(imageHeader);
        plan.pieceB = std::span<const std::byte>(image.pixelBytes.data(), image.pixelBytes.size());
        plan.payloadBytes = *payloadSize;
    }

    // Emission order, decided by whether this model actually needs more than
    // one window -- never merely by whether the caller offered a sink.
    //
    // One section: the order stays what it has always been (meshes, then
    // materials, then images). Nothing can reference across a boundary that
    // does not exist, so dependency order buys nothing, and keeping it means
    // every model that fit before still produces a byte-identical section --
    // including on the product path, which offers a sink for every import and
    // would otherwise have its output silently reordered by this change.
    //
    // Several sections: a reference may only point at a chunk that has
    // ALREADY crossed, never one still to come, because the host's catalog
    // holds exactly what it has accepted so far. Emitting images, then
    // materials, then meshes puts every dependency ahead of its dependents,
    // which is what makes cross-batch references resolvable at all. It is
    // also the order the design wants for display -- textures before the
    // geometry that uses them.
    // PlannedChunk is a descriptor plus two spans, so concatenating the
    // groups twice costs a memcpy of a few hundred bytes per chunk and never
    // touches the geometry the spans point at.
    auto concatenate = [](const std::vector<PlannedChunk>& a, const std::vector<PlannedChunk>& b,
                           const std::vector<PlannedChunk>& c) {
        std::vector<PlannedChunk> joined;
        joined.reserve(a.size() + b.size() + c.size());
        joined.insert(joined.end(), a.begin(), a.end());
        joined.insert(joined.end(), b.begin(), b.end());
        joined.insert(joined.end(), c.begin(), c.end());
        return joined;
    };

    std::vector<PlannedChunk> plans = concatenate(meshPlans, materialPlans, imagePlans);
    if (CountChunksThatFit(plans, destination.size(), maxChunkCount) != plans.size()) {
        if (batchSink == nullptr) {
            // A caller that cannot take a second batch, and a model that
            // needs one: exactly the outcome this path always gave.
            return ImportErrorCode::ResourceLimit;
        }
        plans = concatenate(imagePlans, materialPlans, meshPlans);
    }

    // Fill the window, hand it over, repeat. The last batch is deliberately
    // NOT published here: it stays in the window and this function's caller
    // sends the terminal ChunksReady for it, which is exactly what the
    // single-batch path has always done.
    std::span<PlannedChunk> remaining(plans);
    uint32_t lastBatchChunkCount = 0;
    uint64_t lastBatchLength = 0;
    for (;;) {
        size_t taken = CountChunksThatFit(remaining, destination.size(), maxChunkCount);
        if (taken == 0) {
            // Not even one chunk fits, so no sequence of batches can ever
            // finish. Same outcome the single-window path always gave for a
            // model too big for its window.
            return ImportErrorCode::ResourceLimit;
        }

        auto sectionLength = WriteBatchSection(destination, remaining.subspan(0, taken), generationId);
        if (!sectionLength) {
            return ImportErrorCode::ResourceLimit;
        }

        remaining = remaining.subspan(taken);
        lastBatchChunkCount = static_cast<uint32_t>(taken);
        lastBatchLength = *sectionLength;

        if (remaining.empty()) {
            break;
        }
        // More to come, so this batch is non-terminal: hand the window over
        // and block until the host is finished copying it. batchSink is
        // non-null here by construction -- a null one already returned
        // ResourceLimit above rather than reaching a second batch.
        if (!batchSink->PublishBatch(lastBatchChunkCount, lastBatchLength)) {
            return ImportErrorCode::ImportProtocolViolation;
        }
    }

    GltfImportResult result;
    result.chunkCount = lastBatchChunkCount;
    result.sectionBytesWritten = lastBatchLength;
    return result;
}

} // namespace import_worker
