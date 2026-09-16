#include "GltfAdapter.h"
#include "BoundedChunkWriter.h"
#include <functional>

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
#include "model_core/GeometryBounds.h"
#include "platform/CheckedMath.h"

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#pragma warning(push)
#pragma warning(disable: 4100 4244) // warnings in the pinned third-party header only
#include <simdjson.h>
#pragma warning(pop)
#include <cmath>

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

// Tier A source counts, independent of the bounded cluster working set.
constexpr size_t kMaxVertices = size_t(kTierAVertices);
constexpr size_t kMaxIndices = size_t(kTierATriangles * 3);
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
    ChunkDescriptor geometry{};
    std::optional<size_t> pendingMaterialIndex; // index into WalkState::pendingMaterials
};

struct PendingImage {
    PixelFormatId pixelFormat = PixelFormatId::Unknown;
    uint32_t width = 0;
    uint32_t height = 0;
    ColorSpaceId colorSpace = ColorSpaceId::Srgb;
    uint32_t mipLevels = 1;
    std::optional<size_t> refines;
    std::vector<std::byte> pixelBytes;
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
ImportErrorCode MapFastgltfError(fastgltf::Error error)
{
    if (error == fastgltf::Error::MissingExtensions || error == fastgltf::Error::UnknownRequiredExtension)
        return ImportErrorCode::UnsupportedRequiredFeature;
    if (error == fastgltf::Error::UnsupportedVersion) return ImportErrorCode::UnsupportedEncoding;
    if (error == fastgltf::Error::FileBufferAllocationFailed) return ImportErrorCode::OutOfMemory;
    return ImportErrorCode::MalformedData;
}

// Builds T*R*S from a node's TRS fields via fastgltf::math's own
// translate/rotate/scale free functions, each confirmed (by reading
// math.hpp directly) to post-multiply the new transform onto the input --
// starting from identity and applying translate, then rotate, then scale in
// that order yields exactly T*R*S, the standard composition. This avoids
// depending on fquat::asMatrix()'s return type/element-access syntax or any
// guessed composition order.
// fastgltf 0.9 stores node TRS/matrices as floats. Read only the bounded JSON
// metadata again to preserve authored double transforms; the binary payload
// remains mapped and is never copied for this pass.
std::variant<std::vector<fastgltf::math::dmat4x4>, ImportErrorCode> ReadPreciseTransforms(
    std::span<const std::byte> source, size_t nodeCount)
{
    using namespace fastgltf::math;
    if (nodeCount == 0) return std::vector<dmat4x4>{};
    if (source.size() >= 4 && std::memcmp(source.data(), "glTF", 4) == 0) {
        if (source.size() < 20) return ImportErrorCode::MalformedData;
        uint32_t length; std::memcpy(&length,source.data()+12,sizeof(length));
        if (length > source.size()-20) return ImportErrorCode::MalformedData;
        source = source.subspan(20,length);
    }
    if (source.size() > 32ull*1024*1024 || nodeCount > 1'000'000) return ImportErrorCode::ResourceLimit;
    simdjson::dom::parser parser;
    simdjson::dom::element document;
    if (parser.parse(reinterpret_cast<const uint8_t*>(source.data()),source.size()).get(document)) return ImportErrorCode::MalformedData;
    simdjson::dom::array nodes;
    if (document["nodes"].get_array().get(nodes) || nodes.size() != nodeCount) return ImportErrorCode::MalformedData;
    std::vector<dmat4x4> transforms; transforms.reserve(nodeCount);
    for (auto node : nodes) {
        auto read = [&](const char* key, double* values, size_t count) {
            simdjson::dom::element field;
            const auto error = node[key].get(field);
            if (error == simdjson::NO_SUCH_FIELD) return true;
            simdjson::dom::array array;
            if (error || field.get_array().get(array) || array.size() != count) return false;
            size_t i=0;
            for (auto value : array) {
                if (value.get_double().get(values[i]) || !std::isfinite(values[i]) || std::abs(values[i]) > 1e30) return false;
                ++i;
            }
            return true;
        };
        double translation[3]{}, scale[3]{1,1,1}, rotation[4]{0,0,0,1};
        dmat4x4 matrix(1.0);
        simdjson::dom::element authoredMatrix;
        if (node["matrix"].get(authoredMatrix) == simdjson::SUCCESS) {
            double values[16];
            if (!read("matrix",values,16)) return ImportErrorCode::MalformedData;
            for (unsigned col=0; col<4; ++col) for (unsigned row=0; row<4; ++row) matrix[col][row]=values[col*4+row];
            if (matrix[0][3] != 0 || matrix[1][3] != 0 || matrix[2][3] != 0 || matrix[3][3] != 1)
                return ImportErrorCode::MalformedData;
        } else {
            if (!read("translation",translation,3) || !read("scale",scale,3) || !read("rotation",rotation,4)) return ImportErrorCode::MalformedData;
            double length = 0; for (double value : rotation) length += value*value;
            if (!std::isfinite(length) || length <= 0) return ImportErrorCode::MalformedData;
            length = std::sqrt(length); for (double& value : rotation) value /= length;
            matrix = fastgltf::math::translate(matrix,dvec3(translation[0],translation[1],translation[2]));
            matrix = fastgltf::math::rotate(matrix,dquat(rotation[0],rotation[1],rotation[2],rotation[3]));
            matrix = fastgltf::math::scale(matrix,dvec3(scale[0],scale[1],scale[2]));
        }
        transforms.push_back(matrix);
    }
    return transforms;
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
    std::optional<model_core::MappedFile> file;
    std::optional<model_core::MappingLease> mapping;
    model_core::ImportErrorCode errorCode = model_core::ImportErrorCode::None;
};

struct WalkState {
    const fastgltf::Asset& asset;
    std::vector<fastgltf::math::dmat4x4> localTransforms;
    std::vector<uint8_t> visitState;
    std::vector<PendingChunk> chunks;
    std::vector<PendingMaterial> pendingMaterials;
    std::unordered_map<size_t, size_t> materialIndexToPendingIndex; // glTF material index -> pendingMaterials index
    std::vector<PendingImage> pendingImages;
    std::unordered_map<size_t, size_t> imageIndexToPendingIndex; // glTF image index -> pendingImages index
    std::function<bool(PendingChunk&&)> emit;
    uint32_t emittedGeometry = 0;
    uint32_t primitiveOccurrences = 0;
    bool preview=false, previewCounting=false;
    uint32_t previewOccurrences=0;
    uint32_t chunkTriangles = kChunkTriangles;
    uint64_t scratchLimit = TierAScratchLimit();
    uint64_t scratchReserve = 160ull * 1024 * 1024;
    uint64_t sourceBytes = 0;
    size_t totalVertices = 0;
    size_t totalIndices = 0;
    uint64_t totalDecodedImagePixels = 0;
    uint64_t totalEncodedImageBytes = 0;
    uint64_t totalDecodedImageBytes = 0;
    uint32_t textureWarningCount = 0;
    TextureDecodeOptions textureOptions;
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
    }
    else if (const auto* viewSource = std::get_if<fastgltf::sources::ByteView>(&buffer.data))
    {
        bufferBytes = {viewSource->bytes.data(), viewSource->bytes.size()};
    }
    else if (const auto* uriSource = std::get_if<fastgltf::sources::URI>(&buffer.data))
    {
        auto cached = state.resolvedExternalBuffers.find(view.bufferIndex);
        if (cached == state.resolvedExternalBuffers.end()) {
            ResolvedExternalBuffer resolved;
            if (state.sidecarClient != nullptr) {
                auto result = state.sidecarClient->RequestSidecarBytes(std::string(uriSource->uri.path()),
                                                                       kTierAPrimaryBytes, true);
                if (result.mapping)
                {
                    if (result.mapping->Bytes().size() > kTierAAllSourceBytes - state.sourceBytes)
                    {
                        state.error = ImportErrorCode::AggregateSourceLimit;
                        return std::nullopt;
                    }
                    state.sourceBytes += result.mapping->Bytes().size();
                }
                resolved.file = std::move(result.file);
                resolved.mapping = std::move(result.mapping);
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
        if (!cached->second.bytes.has_value() && !cached->second.mapping)
        {
            return std::nullopt;
        }
        bufferBytes = cached->second.mapping ? cached->second.mapping->Bytes()
                                             : std::span<const std::byte>(cached->second.bytes->data(),
                                                                          cached->second.bytes->size());
    }
    else
    {
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

    const auto elementBytes = fastgltf::getElementByteSize(accessor.type, accessor.componentType);
    if (!elementBytes || accessor.count > kTierAVertices)
    {
        state.error = ImportErrorCode::ResourceLimit;
        return false;
    }
    auto extent = [&](size_t view, size_t offset, size_t count, size_t stride, size_t element) {
        if (!viewIsReadable(view, offset))
            return false;
        auto bytes = ResolveBufferViewBytes(state, view);
        auto end =
            count ? CheckedMultiply(uint64_t(count - 1), uint64_t(stride)) : std::optional<uint64_t>(0);
        auto length = end ? CheckedAdd(*end, count ? uint64_t(element) : 0) : std::nullopt;
        if (!length || *length > bytes->size() - offset || stride < element)
        {
            state.error = ImportErrorCode::MalformedData;
            return false;
        }
        return true;
    };
    if (accessor.bufferViewIndex)
    {
        const auto view = *accessor.bufferViewIndex;
        if (view >= state.asset.bufferViews.size())
        {
            state.error = ImportErrorCode::MalformedData;
            return false;
        }
        if (!extent(view, accessor.byteOffset, accessor.count,
                    state.asset.bufferViews[view].byteStride.value_or(elementBytes), elementBytes))
            return false;
    }
    else if (!accessor.sparse)
    {
        state.error = ImportErrorCode::MalformedData;
        return false;
    }
    if (accessor.sparse)
    {
        const auto& sparse = *accessor.sparse;
        if (sparse.indexComponentType != fastgltf::ComponentType::UnsignedByte &&
            sparse.indexComponentType != fastgltf::ComponentType::UnsignedShort &&
            sparse.indexComponentType != fastgltf::ComponentType::UnsignedInt)
        {
            state.error = ImportErrorCode::MalformedData;
            return false;
        }
        const size_t stride =
            fastgltf::getElementByteSize(fastgltf::AccessorType::Scalar, sparse.indexComponentType);
        if (!stride || sparse.count > accessor.count ||
            !extent(sparse.indicesBufferView, sparse.indicesByteOffset, sparse.count, stride, stride) ||
            !extent(sparse.valuesBufferView, sparse.valuesByteOffset, sparse.count, elementBytes,
                    elementBytes))
            return false;
        auto indices =
            ResolveBufferViewBytes(state, sparse.indicesBufferView)->subspan(sparse.indicesByteOffset);
        uint32_t previous = 0;
        for (size_t i = 0; i < sparse.count; ++i)
        {
            if ((i % 4096) == 0 && state.textureOptions.Cancelled())
            {
                state.error = ImportErrorCode::Cancelled;
                return false;
            }
            const uint32_t index = fastgltf::internal::getAccessorElementAt<uint32_t>(
                sparse.indexComponentType, indices.data() + i * stride);
            if (index >= accessor.count || (i && index <= previous))
            {
                state.error = ImportErrorCode::MalformedData;
                return false;
            }
            previous = index;
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
std::optional<std::vector<std::byte>> ResolveImageEncodedBytes(WalkState& state, size_t imageIndex,
                                                               uint64_t maxEncodedBytes)
{
    if (state.totalEncodedImageBytes > state.textureOptions.maxEncodedBytes)
        return std::nullopt;
    const uint64_t remaining =
        (std::min)(maxEncodedBytes, state.textureOptions.maxEncodedBytes - state.totalEncodedImageBytes);
    if (imageIndex >= state.asset.images.size()) {
        return std::nullopt;
    }
    const fastgltf::Image& image = state.asset.images[imageIndex];

    if (const auto* bufferViewSource = std::get_if<fastgltf::sources::BufferView>(&image.data)) {
        auto bytes = ResolveBufferViewBytes(state, bufferViewSource->bufferViewIndex);
        if (!bytes) {
            return std::nullopt;
        }
        if (bytes->size() > remaining)
            return std::nullopt;
        return std::vector<std::byte>(bytes->begin(), bytes->end());
    }

    if (const auto* view = std::get_if<fastgltf::sources::ByteView>(&image.data))
    {
        if (view->bytes.size() > remaining)
            return std::nullopt;
        return std::vector<std::byte>(view->bytes.begin(), view->bytes.end());
    }
    if (const auto* array=std::get_if<fastgltf::sources::Array>(&image.data)) {
        if (array->bytes.size() > remaining)
            return std::nullopt;
        return std::vector<std::byte>(array->bytes.begin(),array->bytes.end());
    }
    if (const auto* uriSource = std::get_if<fastgltf::sources::URI>(&image.data)) {
        if (state.sidecarClient == nullptr) {
            return std::nullopt;
        }
        auto result = state.sidecarClient->RequestSidecarBytes(std::string(uriSource->uri.path()), remaining);
        return std::move(result.bytes); // nullopt on any failure -- images are never geometry-required
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
std::optional<size_t> ResolveImage(WalkState& state, size_t imageIndex, ColorSpaceId colorSpace, TextureSemantic semantic)
{
    if (state.textureOptions.Cancelled()) { state.error=ImportErrorCode::Cancelled;return std::nullopt; }
    if (imageIndex>=state.asset.images.size()) {
        state.textureWarningCount = (std::min)(64u, state.textureWarningCount + 1);
        return std::nullopt;
    }
    const size_t semanticKey = imageIndex*4 + static_cast<size_t>(semantic);
    auto existing = state.imageIndexToPendingIndex.find(semanticKey);
    if (existing != state.imageIndexToPendingIndex.end()) {
        return existing->second;
    }

    const uint64_t reserved = state.scratchReserve + state.totalDecodedImageBytes;
    if (reserved >= state.scratchLimit)
    {
        state.error = ImportErrorCode::ScratchLimit;
        return std::nullopt;
    }
    auto encodedBytes = ResolveImageEncodedBytes(state, imageIndex, (state.scratchLimit - reserved) / 2);
    if (state.textureOptions.Cancelled()) { state.error=ImportErrorCode::Cancelled; return std::nullopt; }
    auto options=state.textureOptions;
    options.semantic=semantic;
    options.maxDecodedBytes =
        (std::min)({options.maxDecodedBytes, kMaxAggregateTextureBytes - state.totalDecodedImageBytes,
                    (state.scratchLimit - reserved) / 2});
    options.maxPixels=kMaxAggregateTexturePixels-state.totalDecodedImagePixels;
    while (options.maxDimension>1) {
        const auto bytes=ComputeImagePixelBytes(PixelFormatId::RGBA8_UNORM,options.maxDimension,options.maxDimension,
                                               FullImageMipCount(options.maxDimension,options.maxDimension));
        if (bytes && *bytes+*bytes/64<=options.maxDecodedBytes) break;
        options.maxDimension/=2;
    }
    if (encodedBytes && encodedBytes->size() > options.maxEncodedBytes-state.totalEncodedImageBytes) encodedBytes.reset();
    if (encodedBytes) state.totalEncodedImageBytes += encodedBytes->size();
    // Declared MIME must agree with the encoded bytes when supplied.
    fastgltf::MimeType mime=fastgltf::MimeType::None;
    std::visit([&](const auto& source) { if constexpr (requires { source.mimeType; }) mime=source.mimeType; },state.asset.images[imageIndex].data);
    if (encodedBytes && mime!=fastgltf::MimeType::None) {
        const auto sniff=SniffImageFormat(*encodedBytes);
        if (!((mime==fastgltf::MimeType::PNG && sniff==SniffedImageFormat::Png)
            || (mime==fastgltf::MimeType::JPEG && sniff==SniffedImageFormat::Jpeg)
            || (mime==fastgltf::MimeType::KTX2 && sniff==SniffedImageFormat::Ktx2))) encodedBytes.reset();
    }
    std::optional<PendingImage> decoded;
    switch (encodedBytes ? SniffImageFormat(*encodedBytes) : SniffedImageFormat::Unknown) {
    case SniffedImageFormat::Ktx2: {
        if (auto transcoded = TranscodeKtx2BasisImage(*encodedBytes,options)) {
            PendingImage pending;
            pending.pixelFormat = transcoded->pixelFormat;
            pending.mipLevels = transcoded->mipLevels;
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
    {
        if (auto raster = DecodeRasterImageWic(*encodedBytes, colorSpace,options)) {
            PendingImage pending;
            pending.pixelFormat = raster->pixelFormat;
            pending.mipLevels = raster->mipLevels;
            pending.width = raster->width;
            pending.height = raster->height;
            pending.colorSpace = raster->colorSpace;
            pending.pixelBytes = std::move(raster->pixelBytes);
            decoded = std::move(pending);
        }
        break;
    }
    case SniffedImageFormat::WebP: // bounded WebP decode is TSK-209
    case SniffedImageFormat::Unknown:
    default:
        break;
    }

    if (options.Cancelled()) { state.error=ImportErrorCode::Cancelled; return std::nullopt; }
    if (!decoded) {
        state.textureWarningCount = (std::min)(64u, state.textureWarningCount + 1);
        PendingImage fallback;
        fallback.pixelFormat=PixelFormatId::RGBA8_UNORM; fallback.colorSpace=colorSpace;
        fallback.width=fallback.height=semantic==TextureSemantic::Color ? 2u : 1u;
        fallback.pixelBytes.resize(size_t(fallback.width)*fallback.height*4,std::byte{255});
        if (semantic==TextureSemantic::Color) {
            for (unsigned i=0;i<4;++i) for (unsigned c=0;c<3;++c)
                fallback.pixelBytes[i*4+c]=std::byte((i==0 || i==3) ? 64 : 192);
        } else if (semantic==TextureSemantic::Normal) {
            fallback.pixelBytes[0]=fallback.pixelBytes[1]=std::byte{128};
        } else if (semantic==TextureSemantic::Emissive) {
            fallback.pixelBytes[0]=fallback.pixelBytes[1]=fallback.pixelBytes[2]=std::byte{0};
        }
        decoded=std::move(fallback);
    }
    if (decoded->pixelBytes.size()>kMaxAggregateTextureBytes-state.totalDecodedImageBytes) {
        // Budget exhausted: retain the material's deterministic neutral factors.
        state.textureWarningCount = (std::min)(64u, state.textureWarningCount + 1);
        return std::nullopt;
    }
    state.totalDecodedImageBytes+=decoded->pixelBytes.size();
    auto pixelBytes = ComputeImagePixelBytes(PixelFormatId::RGBA8_UNORM,decoded->width,decoded->height,decoded->mipLevels);
    auto pixelCount = pixelBytes ? std::optional<uint64_t>(*pixelBytes/4) : std::nullopt;
    auto newTotal = pixelCount ? CheckedAdd(state.totalDecodedImagePixels, *pixelCount) : std::nullopt;
    if (!pixelCount || !newTotal || *newTotal > kMaxAggregateDecodedTexturePixels) {
        state.error = ImportErrorCode::ResourceLimit;
        return std::nullopt;
    }
    state.totalDecodedImagePixels = *newTotal;

    if (state.chunks.size() + state.pendingMaterials.size() + state.pendingImages.size() >=
        (state.emit ? kTierACatalogLimit : state.maxChunkCount))
    {
        state.error = ImportErrorCode::ResourceLimit;
        return std::nullopt;
    }

    size_t pendingIndex = state.pendingImages.size();
    state.pendingImages.push_back(std::move(*decoded));
    state.imageIndexToPendingIndex.emplace(semanticKey, pendingIndex);
    return pendingIndex;
}

// Resolves the pending-image index for a texture slot's KHR_texture_basisu
// or plain image, or nullopt if the slot itself is absent, out of range, or
// carries no supported image source at all -- soft-fail throughout, never
// sets state.error for these "no texture" outcomes (only ResolveImage's own
// resource-limit path does).
std::optional<size_t> ResolveTextureSlotImage(WalkState& state, const fastgltf::TextureInfo* textureInfo,
                                                ColorSpaceId colorSpace, TextureSemantic semantic)
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
    return ResolveImage(state, *gltfImageIndex, colorSpace, semantic);
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
    if (state.chunks.size() + state.pendingMaterials.size() + state.pendingImages.size() >=
        (state.emit ? kTierACatalogLimit : state.maxChunkCount))
    {
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
        pending.pendingBaseColorImageIndex = ResolveTextureSlotImage(state, &textureInfo, ColorSpaceId::Srgb,TextureSemantic::Color);
        if (state.error != ImportErrorCode::None) {
            return std::nullopt;
        }
    }
    if (material.pbrData.metallicRoughnessTexture.has_value()) {
        pending.pendingMetallicRoughnessImageIndex
            = ResolveTextureSlotImage(state, &*material.pbrData.metallicRoughnessTexture, ColorSpaceId::Linear,TextureSemantic::Data);
        if (state.error != ImportErrorCode::None) {
            return std::nullopt;
        }
    }
    if (material.normalTexture.has_value()) {
        // NormalTextureInfo derives from TextureInfo -- ResolveTextureSlotImage
        // only needs the base; its extra .scale field is dropped (no
        // MaterialPayload field exists for it this chunk, a deliberate
        // scope call, not an oversight).
        pending.pendingNormalImageIndex = ResolveTextureSlotImage(state, &*material.normalTexture, ColorSpaceId::Linear,TextureSemantic::Normal);
        if (state.error != ImportErrorCode::None) {
            return std::nullopt;
        }
    }
    if (material.emissiveTexture.has_value()) {
        pending.pendingEmissiveImageIndex
            = ResolveTextureSlotImage(state, &*material.emissiveTexture, ColorSpaceId::Srgb,TextureSemantic::Emissive);
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
                      const fastgltf::math::dmat4x4& world, const fastgltf::math::fmat3x3& normalMatrix,
                      uint32_t meshId, uint32_t nodeId, uint32_t primitiveId)
{
    if (++state.primitiveOccurrences > kTierAObjectLimit)
    {
        state.error = ImportErrorCode::ResourceLimit;
        return false;
    }
    if (state.previewCounting) return true;
    if (state.preview) {
        bool selected=false;
        for (uint64_t stratum=0;stratum<8;++stratum)
            selected |= state.primitiveOccurrences-1 == stratum*state.previewOccurrences/8;
        if (!selected) return true;
    }
    if (primitive.type != fastgltf::PrimitiveType::Triangles) {
        return true; // skip, not fatal -- mirrors Model.cpp's leniency for non-triangle primitives
    }

    auto positionIt = primitive.findAttribute("POSITION");
    if (positionIt == primitive.attributes.end()) {
        return true; // skip, not fatal
    }

    if (state.chunks.size() + state.pendingMaterials.size() + state.pendingImages.size() >=
        (state.emit ? kTierACatalogLimit : state.maxChunkCount))
    {
        state.error = ImportErrorCode::ResourceLimit;
        return false;
    }

    for (const auto& attribute : primitive.attributes)
        if (attribute.accessorIndex >= state.asset.accessors.size())
        {
            state.error = ImportErrorCode::MalformedData;
            return false;
        }
    if (primitive.indicesAccessor && *primitive.indicesAccessor >= state.asset.accessors.size())
    {
        state.error = ImportErrorCode::MalformedData;
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

    fastgltf::Accessor implicitIndices;
    implicitIndices.type = fastgltf::AccessorType::Scalar;
    implicitIndices.componentType = fastgltf::ComponentType::UnsignedInt;
    implicitIndices.count = positionAccessor.count;
    const fastgltf::Accessor& indexAccessor =
        primitive.indicesAccessor ? state.asset.accessors[*primitive.indicesAccessor] : implicitIndices;
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

    if (state.emit && !primitive.dracoCompression)
    {
        if (!EnsureAccessorBytesResolvable(state, positionAccessor) ||
            (primitive.indicesAccessor && !EnsureAccessorBytesResolvable(state, indexAccessor)) ||
            (hasNormal &&
             !EnsureAccessorBytesResolvable(state, state.asset.accessors[normalIt->accessorIndex])) ||
            (hasUv && !EnsureAccessorBytesResolvable(state, state.asset.accessors[uvIt->accessorIndex])))
            return false;
        const auto material = !state.preview && primitive.materialIndex ? ResolveMaterial(state, *primitive.materialIndex)
                                                      : std::optional<size_t>{};
        if (state.error != ImportErrorCode::None)
            return false;
        SidecarBufferDataAdapter adapter(state);
        auto linear = world;
        linear[3] = fastgltf::math::dvec4(0, 0, 0, 1);
        const auto previewOffsets=PreviewOffsets(indexAccessor.count/3);
        size_t previewStep=0;
        for (size_t first = 0; first < indexAccessor.count;)
        {
            if (state.textureOptions.Cancelled())
            {
                state.error = ImportErrorCode::Cancelled;
                return false;
            }
            PendingChunk part;
            part.pendingMaterialIndex = material;
            part.geometry.meshId = meshId;
            part.geometry.nodeId = nodeId;
            part.geometry.vertexLayoutId = uint32_t(VertexLayoutId::PositionNormalUv0_F32);
            part.geometry.geometryFlags = hasUv ? kGeometryHasUv0 : 0;
            if (primitive.findAttribute("COLOR_0") != primitive.attributes.end())
                part.geometry.geometryFlags |= kGeometryHasColors;
            if (primitive.findAttribute("TEXCOORD_1") != primitive.attributes.end())
                part.geometry.geometryFlags |= kGeometryHasUv1;
            for (unsigned axis = 0; axis < 3; ++axis)
                part.geometry.origin[axis] = world[3][axis];
            const size_t end = (std::min)(indexAccessor.count, first + size_t(state.chunkTriangles) * 3);
            std::unordered_map<uint32_t, uint32_t> remap;
            remap.reserve(end - first);
            part.vertices.reserve(end - first);
            part.indices.reserve(end - first);
            for (size_t i = first; i < end; ++i)
            {
                if (i % 4096 == 0 && state.textureOptions.Cancelled())
                {
                    state.error = ImportErrorCode::Cancelled;
                    return false;
                }
                const uint32_t sourceIndex =
                    primitive.indicesAccessor
                        ? fastgltf::getAccessorElement<uint32_t>(state.asset, indexAccessor, i, adapter)
                        : uint32_t(i);
                if (sourceIndex >= positionAccessor.count)
                {
                    state.error = ImportErrorCode::MalformedData;
                    return false;
                }
                auto [entry, inserted] = remap.emplace(sourceIndex, uint32_t(part.vertices.size()));
                part.indices.push_back(entry->second);
                if (!inserted)
                    continue;
                const auto pos = fastgltf::getAccessorElement<fastgltf::math::fvec3>(
                    state.asset, positionAccessor, sourceIndex, adapter);
                const auto transformed = linear * fastgltf::math::dvec4(pos.x(), pos.y(), pos.z(), 1);
                VertexPositionNormalUv0F32 v{};
                v.px = float(transformed.x());
                v.py = float(transformed.y());
                v.pz = float(transformed.z());
                if (hasNormal)
                {
                    const auto n = fastgltf::getAccessorElement<fastgltf::math::fvec3>(
                        state.asset, state.asset.accessors[normalIt->accessorIndex], sourceIndex, adapter);
                    const auto wn = fastgltf::math::normalize(normalMatrix * n);
                    v.nx = wn.x();
                    v.ny = wn.y();
                    v.nz = wn.z();
                }
                if (hasUv)
                {
                    const auto uv = fastgltf::getAccessorElement<fastgltf::math::fvec2>(
                        state.asset, state.asset.accessors[uvIt->accessorIndex], sourceIndex, adapter);
                    v.u = uv.x();
                    v.v = uv.y();
                }
                part.vertices.push_back(v);
            }
            if (!hasNormal)
                GenerateFlatNormals(part);
            part.geometry.vertexCount = uint32_t(part.vertices.size());
            if (!RebasePositions(part.geometry, std::as_writable_bytes(std::span(part.vertices))))
            {
                state.error = ImportErrorCode::MalformedData;
                return false;
            }
            // Accessor element interval: enough to re-read indices and resolve
            // nonlocal/sparse attributes using the retained glTF metadata.
            part.geometry.sourceRangeOffset = (uint64_t(primitiveId) << 32) | first;
            part.geometry.sourceRangeLength = end - first;
            if (!state.emit(std::move(part)))
                return false;
            first = state.preview ? (++previewStep<previewOffsets.size() ? size_t(previewOffsets[previewStep]*3) : indexAccessor.count) : end;
        }
        state.totalVertices += positionAccessor.count;
        state.totalIndices += indexAccessor.count;
        return true;
    }
    if (!state.emit && (positionAccessor.count + state.totalVertices) * sizeof(VertexPositionNormalUv0F32) +
                               (indexAccessor.count + state.totalIndices) * sizeof(uint32_t) >
                           128ull * 1024 * 1024)
    {
        state.error = ImportErrorCode::ResourceLimit;
        return false;
    }
    if (primitive.dracoCompression)
    {
        const uint64_t declared = positionAccessor.count * 64ull + indexAccessor.count * 8ull;
        if (indexAccessor.count / 3 > 10000000 || declared > 512ull * 1024 * 1024)
        {
            state.error = ImportErrorCode::DracoPrimitiveLimit;
            return false;
        }
        if (state.scratchReserve + state.totalDecodedImageBytes + declared * 2 > state.scratchLimit)
        {
            state.error = ImportErrorCode::ScratchLimit;
            return false;
        }
    }
    PendingChunk chunk;
    chunk.vertices.resize(positionAccessor.count);
    chunk.geometry.vertexCount = static_cast<uint32_t>(positionAccessor.count);
    chunk.geometry.vertexLayoutId = uint32_t(VertexLayoutId::PositionNormalUv0_F32);
    chunk.geometry.meshId = meshId; chunk.geometry.nodeId = nodeId;
    chunk.geometry.geometryFlags = hasUv ? kGeometryHasUv0 : 0;
    if (primitive.findAttribute("TEXCOORD_1") != primitive.attributes.end()) chunk.geometry.geometryFlags |= kGeometryHasUv1;
    if (primitive.findAttribute("COLOR_0") != primitive.attributes.end()) chunk.geometry.geometryFlags |= kGeometryHasColors;
    for (unsigned axis = 0; axis < 3; ++axis) chunk.geometry.origin[axis] = world[3][axis];
    // Separate translation before narrowing. Multiplying tiny residuals into
    // a huge world position first loses precision even in double arithmetic.
    auto linearWorld = world; linearWorld[3] = fastgltf::math::dvec4(0,0,0,1);


    if (primitive.dracoCompression != nullptr) {
        const uint64_t declared = positionAccessor.count * 64ull + indexAccessor.count * 8ull;
        if (state.scratchReserve + state.totalDecodedImageBytes + declared * 2 > state.scratchLimit)
        {
            state.error = ImportErrorCode::ScratchLimit;
            return false;
        }
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
            fastgltf::math::dvec4 worldPos = linearWorld
                * fastgltf::math::dvec4(decodedMesh.positions[idx * 3 + 0],
                                         decodedMesh.positions[idx * 3 + 1],
                                         decodedMesh.positions[idx * 3 + 2], 1.0f);
            chunk.vertices[idx].px = static_cast<float>(worldPos.x());
            chunk.vertices[idx].py = static_cast<float>(worldPos.y());
            chunk.vertices[idx].pz = static_cast<float>(worldPos.z());

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
        if (!EnsureAccessorBytesResolvable(state, positionAccessor) ||
            (primitive.indicesAccessor && !EnsureAccessorBytesResolvable(state, indexAccessor)) ||
            (hasUv && !EnsureAccessorBytesResolvable(state, state.asset.accessors[uvIt->accessorIndex])) ||
            (hasNormal &&
             !EnsureAccessorBytesResolvable(state, state.asset.accessors[normalIt->accessorIndex])))
        {
            return false;
        }

        auto diagnosticBasePresent=[&](const fastgltf::Accessor& accessor) {
            if (!accessor.bufferViewIndex) {state.error=ImportErrorCode::UnsupportedEncoding;return false;}
            return true;
        };
        if (!diagnosticBasePresent(positionAccessor)
            || (primitive.indicesAccessor && !diagnosticBasePresent(indexAccessor))
            || (hasNormal && !diagnosticBasePresent(state.asset.accessors[normalIt->accessorIndex]))
            || (hasUv && !diagnosticBasePresent(state.asset.accessors[uvIt->accessorIndex]))) return false;
        const SidecarBufferDataAdapter bufferAdapter(state);

        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
            state.asset, positionAccessor,
            [&](fastgltf::math::fvec3 pos, size_t idx) {
                fastgltf::math::dvec4 worldPos
                    = linearWorld * fastgltf::math::dvec4(pos.x(), pos.y(), pos.z(), 1.0f);
                chunk.vertices[idx].px = static_cast<float>(worldPos.x());
                chunk.vertices[idx].py = static_cast<float>(worldPos.y());
                chunk.vertices[idx].pz = static_cast<float>(worldPos.z());
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
        if (primitive.indicesAccessor)
            fastgltf::iterateAccessorWithIndex<uint32_t>(
                state.asset, indexAccessor, [&](uint32_t index, size_t idx) { chunk.indices[idx] = index; },
                bufferAdapter);
        else
            for (size_t i = 0; i < chunk.indices.size(); ++i)
                chunk.indices[i] = uint32_t(i);

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

    if (!RebasePositions(chunk.geometry, std::span<std::byte>(
            reinterpret_cast<std::byte*>(chunk.vertices.data()), chunk.vertices.size() * sizeof(chunk.vertices[0])))) {
        state.error = ImportErrorCode::MalformedData; return false;
    }
    state.totalVertices += chunk.vertices.size();
    state.totalIndices += chunk.indices.size();

    if (!state.preview && primitive.materialIndex.has_value()) {
        chunk.pendingMaterialIndex = ResolveMaterial(state, *primitive.materialIndex);
        if (state.error != ImportErrorCode::None) {
            return false;
        }
    }

    if (state.emit)
    {
        // Draco is an independently bounded decode unit, then its normalized
        // result is split and emitted using cluster-local index remapping.
        const auto previewOffsets=PreviewOffsets(chunk.indices.size()/3);
        size_t previewStep=0;
        for (size_t first = 0; first < chunk.indices.size();)
        {
            if (state.textureOptions.Cancelled())
            {
                state.error = ImportErrorCode::Cancelled;
                return false;
            }
            const size_t end = (std::min)(chunk.indices.size(), first + size_t(state.chunkTriangles) * 3);
            PendingChunk part;
            part.geometry = chunk.geometry;
            part.pendingMaterialIndex = chunk.pendingMaterialIndex;
            std::unordered_map<uint32_t, uint32_t> remap;
            remap.reserve(end - first);
            for (size_t i = first; i < end; ++i)
            {
                auto [entry, inserted] = remap.emplace(chunk.indices[i], uint32_t(part.vertices.size()));
                if (inserted)
                    part.vertices.push_back(chunk.vertices[chunk.indices[i]]);
                part.indices.push_back(entry->second);
            }
            part.geometry.vertexCount = uint32_t(part.vertices.size());
            if (!RebasePositions(part.geometry, std::as_writable_bytes(std::span(part.vertices))))
            {
                state.error = ImportErrorCode::MalformedData;
                return false;
            }
            part.geometry.sourceRangeOffset = (uint64_t(primitiveId) << 32) | first;
            part.geometry.sourceRangeLength = end - first;
            if (!state.emit(std::move(part)))
                return false;
            first = state.preview ? (++previewStep<previewOffsets.size() ? size_t(previewOffsets[previewStep]*3) : chunk.indices.size()) : end;
        }
        return true;
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
bool VisitNode(WalkState& state, size_t nodeIndex, const fastgltf::math::dmat4x4& parentWorld,
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
    auto world = parentWorld * state.localTransforms[nodeIndex];
    fastgltf::math::fmat3x3 linear(1.0f);
    for (unsigned col=0; col<3; ++col) for (unsigned row=0; row<3; ++row) linear[col][row] = float(world[col][row]);

    if (node.meshIndex.has_value()) {
        if (*node.meshIndex >= state.asset.meshes.size()) {
            state.error = ImportErrorCode::MalformedData;
            return false;
        }
        const fastgltf::Mesh& mesh = state.asset.meshes[*node.meshIndex];
        fastgltf::math::fmat3x3 normalMatrix
            = fastgltf::math::transpose(fastgltf::math::inverse(linear));

        for (size_t primitiveId = 0; primitiveId < mesh.primitives.size(); ++primitiveId)
        {
            if (!ConvertPrimitive(state, mesh.primitives[primitiveId], world, normalMatrix,
                                  uint32_t(*node.meshIndex + 1), uint32_t(nodeIndex + 1),
                                  uint32_t(primitiveId)))
            {
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
                                           std::span<PlannedChunk> batch, uint64_t generationId, const SceneMetadata& scene)
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
    header.scene = scene;
    header.scene.generationId = generationId;
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
                                                             ChunkBatchSink* batchSink, const TextureDecodeOptions& textureOptions)
{
    if (sourceGlbBytes.size() < 12)
        return ImportErrorCode::MalformedData;
    // Bound metadata before either JSON parser allocates its document storage.
    size_t metadataBytes = sourceGlbBytes.size();
    if (sourceGlbBytes.size() >= 4 && std::memcmp(sourceGlbBytes.data(),"glTF",4) == 0) {
        if (sourceGlbBytes.size() < 20) return ImportErrorCode::MalformedData;
        uint32_t jsonBytes; std::memcpy(&jsonBytes,sourceGlbBytes.data()+12,sizeof(jsonBytes));
        if (jsonBytes > sourceGlbBytes.size()-20) return ImportErrorCode::MalformedData;
        metadataBytes = jsonBytes;
    }
    if (metadataBytes > 32ull*1024*1024) return ImportErrorCode::ResourceLimit;
    if (sourceGlbBytes.size() > kTierAPrimaryBytes)
        return ImportErrorCode::PrimarySourceLimit;
    if (maxChunkCount == 0)
        return ImportErrorCode::ResourceLimit;
    if (TierAScratchLimit() < 128ull * 1024 * 1024)
        return ImportErrorCode::ScratchLimit;
    if (160ull * 1024 * 1024 + metadataBytes * 8ull > TierAScratchLimit())
        return ImportErrorCode::ScratchLimit;
    std::span<const std::byte> bin;
    if (sourceGlbBytes.size() >= 4 && std::memcmp(sourceGlbBytes.data(), "glTF", 4) == 0)
    {
        uint32_t version, total, jsonType;
        std::memcpy(&version, sourceGlbBytes.data() + 4, 4);
        std::memcpy(&total, sourceGlbBytes.data() + 8, 4);
        std::memcpy(&jsonType, sourceGlbBytes.data() + 16, 4);
        if (version != 2 || total != sourceGlbBytes.size() || jsonType != 0x4e4f534a || metadataBytes % 4)
            return ImportErrorCode::MalformedData;
        const size_t offset = 20 + metadataBytes;
        if (offset < sourceGlbBytes.size())
        {
            if (sourceGlbBytes.size() - offset < 8)
                return ImportErrorCode::MalformedData;
            uint32_t bytes, type;
            std::memcpy(&bytes, sourceGlbBytes.data() + offset, 4);
            std::memcpy(&type, sourceGlbBytes.data() + offset + 4, 4);
            if (type != 0x004e4942 || bytes != sourceGlbBytes.size() - offset - 8 || bytes % 4)
                return ImportErrorCode::MalformedData;
            bin = sourceGlbBytes.subspan(offset + 8, bytes);
        }
    }
    // fastgltf reserves its asset arrays while parsing, before adapter checks.
    // Preflight only the bounded JSON DOM, then release it before constructing
    // that asset. Include conservative storage for every nested array entry.
    uint64_t parserReserve = 160ull * 1024 * 1024 + metadataBytes * 8ull;
    {
        const auto json = sourceGlbBytes.subspan(
            bin.empty() && std::memcmp(sourceGlbBytes.data(), "glTF", 4) != 0 ? 0 : 20, metadataBytes);
        simdjson::dom::parser preflight;
        simdjson::dom::element root;
        if (preflight.parse(reinterpret_cast<const char*>(json.data()), json.size()).get(root))
            return ImportErrorCode::MalformedData;
        simdjson::dom::object object;
        if (root.get_object().get(object))
            return ImportErrorCode::MalformedData;
        for (const auto& field : object)
        {
            simdjson::dom::array array;
            if (field.value.get_array().get(array))
                continue;
            uint64_t limit = kTierAObjectLimit;
            if (field.key == "accessors" || field.key == "bufferViews")
                limit = 300000;
            else if (field.key == "materials")
                limit = kTierAMaterialLimit;
            else if (field.key == "images" || field.key == "textures" || field.key == "samplers")
                limit = uint64_t(kTierAMaterialLimit) * 4;
            else if (field.key != "nodes" && field.key != "meshes" && field.key != "buffers" &&
                     field.key != "scenes" && field.key != "skins" && field.key != "animations")
                continue;
            if (array.size() > limit)
                return ImportErrorCode::ResourceLimit;
        }
        auto countStorage = [&](auto&& self, simdjson::dom::element value,
                                unsigned depth) -> ImportErrorCode {
            if (depth > 256)
                return ImportErrorCode::UnsupportedEncoding;
            simdjson::dom::array array;
            if (!value.get_array().get(array))
            {
                if (array.size() > 300000)
                    return ImportErrorCode::ResourceLimit;
                constexpr size_t entryBytes =
                    (std::max)({size_t(1024), sizeof(fastgltf::Material), sizeof(fastgltf::Node),
                                sizeof(fastgltf::Accessor)});
                const uint64_t bytes = uint64_t(array.size()) * entryBytes;
                if (bytes > TierAScratchLimit() - parserReserve)
                    return ImportErrorCode::ScratchLimit;
                parserReserve += bytes;
                for (auto item : array)
                    if (auto error = self(self, item, depth + 1); error != ImportErrorCode::None)
                        return error;
            }
            else
            {
                simdjson::dom::object nested;
                if (!value.get_object().get(nested))
                    for (const auto& field : nested)
                        if (auto error = self(self, field.value, depth + 1); error != ImportErrorCode::None)
                            return error;
            }
            return ImportErrorCode::None;
        };
        if (auto error = countStorage(countStorage, root, 0); error != ImportErrorCode::None)
            return error;
        simdjson::dom::array meshes;
        uint64_t primitives = 0;
        if (!object["meshes"].get_array().get(meshes))
            for (auto mesh : meshes)
            {
                simdjson::dom::array array;
                if (!mesh["primitives"].get_array().get(array))
                {
                    primitives += array.size();
                    if (primitives > kTierAObjectLimit)
                        return ImportErrorCode::ResourceLimit;
                }
            }
    }
    // fastgltf receives bounded padded JSON and aliases the already mapped BIN.
    // Its buffer allocation callback returns the BIN address; read(void*) detects
    // that exact alias and performs no write to the read-only file mapping.
    class MappedGetter final : public fastgltf::GltfDataGetter
    {
      public:
        std::span<const std::byte> source, bin;
        size_t offset = 0;
        std::vector<std::byte> metadata;
        std::vector<std::vector<std::byte>> decoded;
        void read(void* ptr, size_t count) override
        {
            if (count > source.size() - offset)
                throw std::out_of_range("source range");
            if (ptr != source.data() + offset)
                std::memcpy(ptr, source.data() + offset, count);
            offset += count;
        }
        fastgltf::span<std::byte> read(size_t count, size_t padding) override
        {
            if (count > 32ull * 1024 * 1024 || padding > 4096 || count > source.size() - offset)
                throw std::out_of_range("metadata limit");
            metadata.resize(count + padding);
            read(metadata.data(), count);
            return fastgltf::span<std::byte>(metadata.data(), count + padding);
        }
        void reset() override
        {
            offset = 0;
        }
        size_t bytesRead() override
        {
            return offset;
        }
        size_t totalSize() override
        {
            return source.size();
        }
    } getter;
    getter.source = sourceGlbBytes;
    getter.bin = bin;
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
    parser.setUserPointer(&getter);
    parser.setBufferAllocationCallback(+[](uint64_t bytes, void* user) -> fastgltf::BufferInfo {
        auto& data = *static_cast<MappedGetter*>(user);
        if (!data.bin.empty() && data.source.data() + data.offset == data.bin.data() &&
            bytes <= data.bin.size())
            return {const_cast<std::byte*>(data.bin.data()), 0};
        if (bytes > 32ull * 1024 * 1024)
            throw std::out_of_range("data URI limit");
        data.decoded.emplace_back(size_t(bytes));
        return {data.decoded.back().data(), uint32_t(data.decoded.size())};
    });
    auto assetResult = parser.loadGltf(getter, std::filesystem::path{}, fastgltf::Options::None);
    if (!assetResult) {
        return MapFastgltfError(assetResult.error());
    }
    auto& asset = assetResult.get();
    if (!bin.empty())
    {
        if (asset.buffers.empty() || asset.buffers[0].byteLength > bin.size())
            return ImportErrorCode::MalformedData;
        fastgltf::sources::ByteView view;
        view.bytes = fastgltf::span<const std::byte>(bin.data(), bin.size());
        asset.buffers[0].data = view;
    }
    auto bindCustom = [&](fastgltf::DataSource& source) {
        if (auto custom = std::get_if<fastgltf::sources::CustomBuffer>(&source))
        {
            if (!custom->id || custom->id > getter.decoded.size())
                return false;
            auto& bytes = getter.decoded[size_t(custom->id) - 1];
            fastgltf::sources::ByteView view;
            view.bytes = fastgltf::span<const std::byte>(bytes.data(), bytes.size());
            view.mimeType = custom->mimeType;
            source = view;
        }
        return true;
    };
    for (auto& buffer : asset.buffers)
        if (!bindCustom(buffer.data))
            return ImportErrorCode::MalformedData;
    for (auto& image : asset.images)
        if (!bindCustom(image.data))
            return ImportErrorCode::MalformedData;
    if (asset.nodes.size() > 100000 || asset.materials.size() > 65536 || asset.accessors.size() > 300000 ||
        asset.bufferViews.size() > 300000)
        return ImportErrorCode::ResourceLimit;
    if (asset.meshes.size() > kTierAObjectLimit || asset.animations.size() > kTierAObjectLimit || asset.skins.size() > kTierAObjectLimit)
        return ImportErrorCode::ResourceLimit;

    if (asset.scenes.empty()) {
        return ImportErrorCode::EmptyGeometry;
    }
    size_t sceneIndex = asset.defaultScene.value_or(0);
    if (sceneIndex >= asset.scenes.size()) {
        return ImportErrorCode::MalformedData;
    }

    WalkState state{ asset };
    state.textureOptions = textureOptions;
    state.sourceBytes = sourceGlbBytes.size();
    state.scratchReserve = parserReserve;
    if (state.scratchReserve > state.scratchLimit)
        return ImportErrorCode::ScratchLimit;
    auto preciseTransforms = ReadPreciseTransforms(sourceGlbBytes,asset.nodes.size());
    if (auto error = std::get_if<ImportErrorCode>(&preciseTransforms)) return *error;
    state.localTransforms = std::move(std::get<std::vector<fastgltf::math::dmat4x4>>(preciseTransforms));
    state.visitState.assign(asset.nodes.size(), 0);
    state.maxChunkCount = maxChunkCount;
    state.chunkTriangles = uint32_t(std::min<uint64_t>(
        kChunkTriangles, destination.size() > kSectionHeaderSize + kChunkDescriptorSize
                             ? (destination.size() - kSectionHeaderSize - kChunkDescriptorSize) / 108
                             : 0));
    if (!state.chunkTriangles)
        return ImportErrorCode::ResourceLimit;
    state.sidecarClient = sidecarClient;
    state.preview=batchSink && batchSink->Preview();
    if (state.preview) state.chunkTriangles=1;

    SceneMetadata streamingScene{};
    streamingScene.format = bin.empty() ? SourceFormatId::Gltf : SourceFormatId::Glb;
    streamingScene.upAxis = UpAxisId::Y;
    streamingScene.metersPerUnit = 1;
    streamingScene.meshCount = uint32_t(asset.meshes.size());
    streamingScene.nodeCount = uint32_t(asset.nodes.size());
    streamingScene.animationCount = uint32_t(asset.animations.size());
    streamingScene.skinCount = uint32_t(asset.skins.size());
    for (const auto& skin : asset.skins)
    {
        if (skin.joints.size() > 100000 - streamingScene.boneCount)
            return ImportErrorCode::ResourceLimit;
        streamingScene.boneCount += uint32_t(skin.joints.size());
    }
    BoundedChunkWriter streamingWriter(destination, generationId, maxChunkCount, streamingScene, batchSink);
    size_t emittedImages = 0, emittedMaterials = 0;
    constexpr uint32_t materialBase = 0x40000000u, imageBase = 0x80000000u;
    auto emitImage = [&](size_t i, bool low) {
        auto& image = state.pendingImages[i];
        uint32_t w = image.width, h = image.height, first = 0;
        size_t offset = 0;
        if (low)
            while ((w > 64 || h > 64) && first + 1 < image.mipLevels)
            {
                offset += size_t(*ComputeImagePixelBytes(image.pixelFormat, w, h, 1));
                w = (std::max)(1u, w / 2);
                h = (std::max)(1u, h / 2);
                ++first;
            }
        ImagePayloadHeader header{};
        header.pixelFormat = uint32_t(image.pixelFormat);
        header.width = w;
        header.height = h;
        header.mipLevels = image.mipLevels - first;
        header.colorSpace = uint32_t(image.colorSpace);
        header.pixelDataByteSize = image.pixelBytes.size() - offset;
        header.reserved0 = low ? 0 : (image.refines ? imageBase + uint32_t(i) + 1 : 0);
        ChunkDescriptor d{};
        d.topology = ChunkTopology::Image;
        d.chunkId = imageBase + uint32_t(i) + 1 + (header.reserved0 ? 0x10000000u : 0);
        if (!streamingWriter.Add(d, ChunkBytes(header),
                                 std::span<const std::byte>(image.pixelBytes).subspan(offset)))
        {
            state.error = streamingWriter.Error();
            return false;
        }
        if (first)
            image.refines = i;
        return true;
    };
    auto emitDependencies = [&] {
        while (emittedImages < state.pendingImages.size())
        {
            if (!emitImage(emittedImages, true))
                return false;
            ++emittedImages;
        }
        while (emittedMaterials < state.pendingMaterials.size())
        {
            const auto& material = state.pendingMaterials[emittedMaterials];
            ChunkDescriptor d{};
            d.topology = ChunkTopology::Material;
            d.chunkId = materialBase + uint32_t(emittedMaterials) + 1;
            const std::optional<size_t> images[]{
                material.pendingBaseColorImageIndex, material.pendingMetallicRoughnessImageIndex,
                material.pendingNormalImageIndex, material.pendingEmissiveImageIndex};
            for (unsigned i = 0; i < 4; ++i)
                if (images[i])
                {
                    d.dependencyIds[i] = imageBase + uint32_t(*images[i]) + 1;
                    ++d.dependencyCount;
                }
            if (!streamingWriter.Add(d, ChunkBytes(material.data)))
            {
                state.error = streamingWriter.Error();
                return false;
            }
            ++emittedMaterials;
        }
        return true;
    };
    if (batchSink)
        state.emit = [&](PendingChunk&& chunk) {
            if (!emitDependencies())
                return false;
            auto d = chunk.geometry;
            d.topology = ChunkTopology::TriangleList;
            d.chunkId = ++state.emittedGeometry;
            d.vertexCount = uint32_t(chunk.vertices.size());
            d.indexCount = uint32_t(chunk.indices.size());
            if (chunk.pendingMaterialIndex)
            {
                d.dependencyIds[0] = materialBase + uint32_t(*chunk.pendingMaterialIndex) + 1;
                d.dependencyCount = 1;
            }
            if (!streamingWriter.Add(d, ChunkBytes(chunk.vertices), ChunkBytes(chunk.indices)))
            {
                state.error = streamingWriter.Error();
                return false;
            }
            return true;
        };
    fastgltf::math::dmat4x4 identity(1.0);
    if (state.preview) {
        state.previewCounting=true;
        for (size_t nodeIndex:asset.scenes[sceneIndex].nodeIndices)
            if (!VisitNode(state,nodeIndex,identity,0)) return state.error;
        state.previewOccurrences=state.primitiveOccurrences;
        state.primitiveOccurrences=0; state.previewCounting=false;
        std::fill(state.visitState.begin(),state.visitState.end(),uint8_t(0));
    }
    for (size_t nodeIndex : asset.scenes[sceneIndex].nodeIndices) {
        if (!VisitNode(state, nodeIndex, identity, 0)) {
            return state.error;
        }
    }

    if (batchSink)
    {
        for (const auto& [index, resolved] : state.resolvedExternalBuffers)
        {
            (void)index;
            if (resolved.file && !resolved.file->IsUnchanged())
                return ImportErrorCode::FileChanged;
        }
        if (!state.emittedGeometry)
            return ImportErrorCode::EmptyGeometry;
        const bool haveRefinements = std::any_of(state.pendingImages.begin(), state.pendingImages.end(),
                                                 [](const auto& image) { return image.refines.has_value(); });
        if (haveRefinements && !streamingWriter.PublishPending())
            return streamingWriter.Error();
        for (size_t i = 0; i < state.pendingImages.size(); ++i)
            if (state.pendingImages[i].refines)
            {
                if (textureOptions.Cancelled())
                    return ImportErrorCode::Cancelled;
                if (!emitImage(i, false))
                    return state.error;
            }
        ImportStatusPayload status{};
        for (const auto& extension : asset.extensionsUsed)
            if (extension != "KHR_draco_mesh_compression" && extension != "KHR_texture_basisu" &&
                extension != "KHR_texture_transform")
                status.optionalFeatureWarnings = (std::min)(64u, status.optionalFeatureWarnings + 1);
        if (status.optionalFeatureWarnings)
        {
            ChunkDescriptor d{};
            d.topology = ChunkTopology::ImportStatus;
            d.chunkId = 0xf0000000u;
            if (!streamingWriter.Add(d, ChunkBytes(status)))
                return streamingWriter.Error();
        }
        if (state.textureWarningCount)
        {
            ChunkDescriptor d{};
            d.topology = ChunkTopology::TextureWarning;
            d.chunkId = 0xf0000001u;
            if (!streamingWriter.Add(d, ChunkBytes(state.textureWarningCount)))
                return streamingWriter.Error();
        }
        if (!streamingWriter.Complete()) return streamingWriter.Error();
        return GltfImportResult{streamingWriter.Count(), streamingWriter.Length()};
    }
    if (state.chunks.empty()) {
        return ImportErrorCode::EmptyGeometry; // no supported geometry found
    }

    // Initial low-resolution images retain their logical identity; full chains
    // are distinct immutable chunks refining that identity, emitted after geometry.
    const size_t initialImageCount=state.pendingImages.size();
    if (batchSink) for (size_t k=0;k<initialImageCount;++k) {
        auto& image=state.pendingImages[k];
        uint32_t first=0,w=image.width,h=image.height;
        size_t offset=0;
        while ((w>64 || h>64) && first+1<image.mipLevels) {
            offset+=static_cast<size_t>(*ComputeImagePixelBytes(image.pixelFormat,w,h,1));
            w = (std::max)(1u, w / 2);
            h = (std::max)(1u, h / 2);
            ++first;
        }
        if (!first) continue;
        PendingImage full=std::move(image);
        PendingImage low;
        low.pixelFormat=full.pixelFormat; low.colorSpace=full.colorSpace;
        low.width=w; low.height=h; low.mipLevels=full.mipLevels-first;
        low.pixelBytes.assign(full.pixelBytes.begin()+offset,full.pixelBytes.end());
        if (low.pixelBytes.size()>kMaxAggregateTextureBytes-state.totalDecodedImageBytes
            || state.pendingImages.size()+state.chunks.size()+state.pendingMaterials.size()+1>=maxChunkCount)
            return ImportErrorCode::ResourceLimit;
        state.totalDecodedImageBytes+=low.pixelBytes.size();
        const auto lowPixels=*ComputeImagePixelBytes(PixelFormatId::RGBA8_UNORM,w,h,low.mipLevels)/4;
        if (lowPixels>kMaxAggregateTexturePixels-state.totalDecodedImagePixels) return ImportErrorCode::ResourceLimit;
        state.totalDecodedImagePixels+=lowPixels;
        image=std::move(low); full.refines=k;
        state.pendingImages.push_back(std::move(full));
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

    SceneMetadata scene{};
    scene.generationId = generationId;
    scene.format = sourceGlbBytes.size() >= 4 && std::memcmp(sourceGlbBytes.data(), "glTF", 4) == 0
        ? SourceFormatId::Glb : SourceFormatId::Gltf;
    scene.upAxis = UpAxisId::Y; scene.metersPerUnit = 1.0;
    scene.meshCount = static_cast<uint32_t>(state.asset.meshes.size());
    scene.nodeCount = static_cast<uint32_t>(state.asset.nodes.size());
    scene.animationCount = static_cast<uint32_t>(state.asset.animations.size());
    scene.skinCount = static_cast<uint32_t>(state.asset.skins.size());
    for (const auto& skin : state.asset.skins) {
        if (skin.joints.size() > 1'000'000 - scene.boneCount) return ImportErrorCode::ResourceLimit;
        scene.boneCount += static_cast<uint32_t>(skin.joints.size());
    }
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
        plan.descriptor = chunk.geometry;
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
        imageHeader.mipLevels = image.mipLevels;
        imageHeader.colorSpace = static_cast<uint32_t>(image.colorSpace);
        imageHeader.reserved0 = image.refines ? imageChunkId(*image.refines) : 0;
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
        plan.descriptor.dependencyCount = 0; // refinement roots live in the image header
        plan.pieceA = BytesOf(imageHeader);
        plan.pieceB = std::span<const std::byte>(image.pixelBytes.data(), image.pixelBytes.size());
        plan.payloadBytes = *payloadSize;
    }

    // Single-section imports without refinements retain mesh/material/image
    // ordering. Progressive imports put initial images and materials before
    // their geometry, so earlier batches establish the dependency catalog.
    // Refinements follow an enforced boundary after all initial plans, even
    // when the full import would fit in one section. Concatenation copies
    // descriptors and spans without copying their payloads.
    auto concatenate = [](const std::vector<PlannedChunk>& a, const std::vector<PlannedChunk>& b,
                           const std::vector<PlannedChunk>& c) {
        std::vector<PlannedChunk> joined;
        joined.reserve(a.size() + b.size() + c.size());
        joined.insert(joined.end(), a.begin(), a.end());
        joined.insert(joined.end(), b.begin(), b.end());
        joined.insert(joined.end(), c.begin(), c.end());
        return joined;
    };

    std::vector<PlannedChunk> refinements(imagePlans.begin()+initialImageCount,imagePlans.end());
    imagePlans.resize(initialImageCount);
    std::vector<PlannedChunk> plans = concatenate(meshPlans, materialPlans, imagePlans);
    if (!refinements.empty()) plans=concatenate(imagePlans,materialPlans,meshPlans);
    if (CountChunksThatFit(plans, destination.size(), maxChunkCount) != plans.size()) {
        if (batchSink == nullptr) {
            // A caller that cannot take a second batch, and a model that
            // needs one: exactly the outcome this path always gave.
            return ImportErrorCode::ResourceLimit;
        }
        plans = concatenate(imagePlans, materialPlans, meshPlans);
    }

    size_t initialPlanCount=plans.size();
    plans.insert(plans.end(),refinements.begin(),refinements.end());
    model_core::ImportStatusPayload status{};
    for (const auto& extension : asset.extensionsUsed) {
        if (extension != "KHR_draco_mesh_compression" && extension != "KHR_texture_basisu" && extension != "KHR_texture_transform")
            status.optionalFeatureWarnings = (std::min)(64u, status.optionalFeatureWarnings + 1);
    }
    if (!refinements.empty()) status.flags |= model_core::kStatusRefining;
    if (status.flags || status.optionalFeatureWarnings) {
        if (plans.size()>=maxChunkCount) return ImportErrorCode::ResourceLimit;
        PlannedChunk statusChunk;
        statusChunk.descriptor.topology=ChunkTopology::ImportStatus;
        statusChunk.descriptor.chunkId=static_cast<uint32_t>(meshChunkCount+materialChunkCount+imageChunkCount+2);
        statusChunk.pieceA=BytesOf(status); statusChunk.payloadBytes=sizeof(status);
        plans.insert(plans.begin(),statusChunk);
        ++initialPlanCount;
    }
    uint32_t warningCount=state.textureWarningCount;
    if (warningCount) {
        if (plans.size()>=maxChunkCount) return ImportErrorCode::ResourceLimit;
        PlannedChunk warning;
        warning.descriptor.topology=ChunkTopology::TextureWarning;
        warning.descriptor.chunkId=static_cast<uint32_t>(meshChunkCount+materialChunkCount+imageChunkCount+1);
        warning.pieceA=BytesOf(warningCount); warning.payloadBytes=sizeof(warningCount);
        plans.push_back(warning);
    }
    size_t emitted=0;
    // Fill the window, hand it over, repeat. The last batch is deliberately
    // NOT published here: it stays in the window and this function's caller
    // sends the terminal ChunksReady for it, which is exactly what the
    // single-batch path has always done.
    std::span<PlannedChunk> remaining(plans);
    uint32_t lastBatchChunkCount = 0;
    uint64_t lastBatchLength = 0;
    for (;;) {
        if (textureOptions.Cancelled()) return ImportErrorCode::Cancelled;
        size_t taken = CountChunksThatFit(remaining, destination.size(), maxChunkCount);
        if (!refinements.empty() && emitted < initialPlanCount)
            taken = (std::min)(taken, initialPlanCount - emitted);
        emitted+=taken;
        if (taken == 0) {
            // Not even one chunk fits, so no sequence of batches can ever
            // finish. Same outcome the single-window path always gave for a
            // model too big for its window.
            return ImportErrorCode::ResourceLimit;
        }

        auto sectionLength = WriteBatchSection(destination, remaining.subspan(0, taken), generationId, scene);
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
