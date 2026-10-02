// T25 glTF/GLB family adapter implementation (see GltfFamilyAdapter.h).

#include "GltfFamilyAdapter.h"

#include "ContainmentStage.h"
#include "Deadline.h"
#include "ProviderLimits.h"
#include "model_core/DracoPreflight.h"
#include "model_core/Ktx2Preflight.h"

#include <fastgltf/core.hpp>
#include <fastgltf/math.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

#include <draco/compression/decode.h>
#include <draco/mesh/mesh.h>

#include <meshoptimizer.h>

#include <ktx.h>
#include <webp/decode.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace preview3d::provider {
namespace {

constexpr std::uint64_t kCheckpointTriangles = 4096;
constexpr std::uint64_t kMaxTransformDepth = 256;
constexpr std::uint64_t kCheckpointImages = 32;

// --- Small double-precision matrix helpers (column-major, col * 4 + row) ----

using Mat4 = std::array<double, 16>;

Mat4 IdentityMatrix() noexcept
{
    return Mat4{1.0, 0.0, 0.0, 0.0,
                0.0, 1.0, 0.0, 0.0,
                0.0, 0.0, 1.0, 0.0,
                0.0, 0.0, 0.0, 1.0};
}

Mat4 Multiply(const Mat4& a, const Mat4& b) noexcept
{
    Mat4 out{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            out[col * 4 + row] = sum;
        }
    }
    return out;
}

void TransformPoint(const Mat4& m, const double p[3], double out[3]) noexcept
{
    for (int row = 0; row < 3; ++row) {
        out[row] = m[0 * 4 + row] * p[0] + m[1 * 4 + row] * p[1]
                 + m[2 * 4 + row] * p[2] + m[3 * 4 + row];
    }
}

// Normal transform = transpose(inverse(upper 3x3)) = cofactor(upper 3x3) / det.
void TransformNormal(const Mat4& m, const float in[3], float out[3]) noexcept
{
    const double m00 = m[0 * 4 + 0], m01 = m[1 * 4 + 0], m02 = m[2 * 4 + 0];
    const double m10 = m[0 * 4 + 1], m11 = m[1 * 4 + 1], m12 = m[2 * 4 + 1];
    const double m20 = m[0 * 4 + 2], m21 = m[1 * 4 + 2], m22 = m[2 * 4 + 2];

    const double c00 = m11 * m22 - m12 * m21;
    const double c01 = m12 * m20 - m10 * m22;
    const double c02 = m10 * m21 - m11 * m20;
    const double c10 = m02 * m21 - m01 * m22;
    const double c11 = m00 * m22 - m02 * m20;
    const double c12 = m01 * m20 - m00 * m21;
    const double c20 = m01 * m12 - m02 * m11;
    const double c21 = m02 * m10 - m00 * m12;
    const double c22 = m00 * m11 - m01 * m10;
    const double det = m00 * c00 + m01 * c01 + m02 * c02;

    const double nx = static_cast<double>(in[0]);
    const double ny = static_cast<double>(in[1]);
    const double nz = static_cast<double>(in[2]);

    double rx = c00 * nx + c01 * ny + c02 * nz;
    double ry = c10 * nx + c11 * ny + c12 * nz;
    double rz = c20 * nx + c21 * ny + c22 * nz;
    if (!(std::abs(det) > 1e-24) || !std::isfinite(rx) || !std::isfinite(ry) || !std::isfinite(rz)) {
        rx = m00 * nx + m01 * ny + m02 * nz;
        ry = m10 * nx + m11 * ny + m12 * nz;
        rz = m20 * nx + m21 * ny + m22 * nz;
    }
    const double lengthSquared = rx * rx + ry * ry + rz * rz;
    if (!std::isfinite(lengthSquared) || lengthSquared <= 1e-24) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    const double inverse = 1.0 / std::sqrt(lengthSquared);
    out[0] = static_cast<float>(rx * inverse);
    out[1] = static_cast<float>(ry * inverse);
    out[2] = static_cast<float>(rz * inverse);
}

Mat4 LocalMatrix(const fastgltf::Node& node) noexcept
{
    Mat4 result = IdentityMatrix();
    if (const auto* matrix = std::get_if<fastgltf::math::fmat4x4>(&node.transform)) {
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                result[col * 4 + row] = static_cast<double>((*matrix)[col][row]);
            }
        }
        return result;
    }
    const fastgltf::TRS& trs = std::get<fastgltf::TRS>(node.transform);
    const fastgltf::math::fmat3x3 rotation = fastgltf::math::asMatrix(trs.rotation);
    for (int col = 0; col < 3; ++col) {
        const double scale = static_cast<double>(trs.scale[col]);
        for (int row = 0; row < 3; ++row) {
            result[col * 4 + row] = static_cast<double>(rotation[col][row]) * scale;
        }
    }
    result[3 * 4 + 0] = static_cast<double>(trs.translation[0]);
    result[3 * 4 + 1] = static_cast<double>(trs.translation[1]);
    result[3 * 4 + 2] = static_cast<double>(trs.translation[2]);
    return result;
}

float Saturate(double value) noexcept
{
    if (!std::isfinite(value)) {
        return 1.0f;
    }
    return static_cast<float>(std::clamp(value, 0.0, 1.0));
}

ErrorCode MapFastGltfError(fastgltf::Error error) noexcept
{
    switch (error) {
        case fastgltf::Error::None:
            return ErrorCode::None;
        case fastgltf::Error::FileBufferAllocationFailed:
            return ErrorCode::OutOfMemory;
        case fastgltf::Error::MissingExternalBuffer:
        case fastgltf::Error::InvalidPath:
            return ErrorCode::UnsafeReference;
        case fastgltf::Error::MissingExtensions:
        case fastgltf::Error::UnknownRequiredExtension:
        case fastgltf::Error::UnsupportedVersion:
            return ErrorCode::UnsupportedRequiredFeature;
        case fastgltf::Error::InvalidURI:
            return ErrorCode::MalformedData;
        case fastgltf::Error::InvalidJson:
        case fastgltf::Error::InvalidGltf:
        case fastgltf::Error::InvalidOrMissingAssetField:
        case fastgltf::Error::InvalidGLB:
        case fastgltf::Error::MissingField:
        case fastgltf::Error::InvalidFileData:
        case fastgltf::Error::FailedWritingFiles:
        default:
            return ErrorCode::MalformedData;
    }
}

// --- fastgltf buffer-view access (meshopt-aware) ----------------------------

class GltfBufferAccess {
public:
    GltfBufferAccess(const fastgltf::Asset& asset, AllocationLedger* ledger,
                     std::uint64_t maxDecodedBytes) noexcept
        : asset_(asset), ledger_(ledger), maxDecodedBytes_(maxDecodedBytes)
    {
    }

    GltfBufferAccess(const GltfBufferAccess&) = delete;
    GltfBufferAccess& operator=(const GltfBufferAccess&) = delete;

    // fastgltf's BufferDataAdapter hook.
    std::span<const std::byte> operator()(const fastgltf::Asset& asset,
                                          std::size_t bufferViewIndex) const
    {
        if (&asset != &asset_) {
            return {};
        }
        return Resolve(bufferViewIndex);
    }

    // Direct resolution for Draco geometry and image buffer views.
    std::span<const std::byte> Resolve(std::size_t bufferViewIndex) const
    {
        if (bufferViewIndex >= asset_.bufferViews.size()) {
            return {};
        }
        const fastgltf::BufferView& view = asset_.bufferViews[bufferViewIndex];
        if (view.meshoptCompression != nullptr) {
            usedMeshopt_ = true;
            const auto cached = meshopt_.find(bufferViewIndex);
            if (cached == meshopt_.end()) {
                std::optional<AllocationReservation> reservation;
                std::vector<std::byte> decoded;
                if (!DecodeMeshopt(view, *view.meshoptCompression, reservation, decoded)) {
                    return {};
                }
                const auto inserted = meshopt_.emplace(
                    bufferViewIndex, CachedBuffer{std::move(decoded), std::move(reservation)});
                return std::span<const std::byte>(inserted.first->second.bytes.data(),
                                                  inserted.first->second.bytes.size());
            }
            return std::span<const std::byte>(cached->second.bytes.data(),
                                              cached->second.bytes.size());
        }
        const std::span<const std::byte> buffer = ResolveBuffer(view.bufferIndex);
        if (view.byteOffset > buffer.size() || view.byteLength > buffer.size() - view.byteOffset) {
            return {};
        }
        return buffer.subspan(view.byteOffset, view.byteLength);
    }

    // A failed probe means fastgltf's iterator must not touch this accessor.
    bool AccessorResolvable(const fastgltf::Accessor& accessor) const
    {
        if (accessor.bufferViewIndex.has_value() && Resolve(*accessor.bufferViewIndex).empty()) {
            return false;
        }
        if (accessor.sparse.has_value()) {
            if (Resolve(accessor.sparse->indicesBufferView).empty()
                || Resolve(accessor.sparse->valuesBufferView).empty()) {
                return false;
            }
        }
        return true;
    }

    bool UsedMeshopt() const noexcept { return usedMeshopt_; }

private:
    std::span<const std::byte> ResolveBuffer(std::size_t bufferIndex) const noexcept
    {
        if (bufferIndex >= asset_.buffers.size()) {
            return {};
        }
        const fastgltf::DataSource& data = asset_.buffers[bufferIndex].data;
        if (const auto* array = std::get_if<fastgltf::sources::Array>(&data)) {
            return std::span<const std::byte>(array->bytes.data(), array->bytes.size_bytes());
        }
        if (const auto* vector = std::get_if<fastgltf::sources::Vector>(&data)) {
            return std::span<const std::byte>(vector->bytes.data(), vector->bytes.size());
        }
        if (const auto* view = std::get_if<fastgltf::sources::ByteView>(&data)) {
            return view->bytes;
        }
        return {};
    }

    bool DecodeMeshopt(const fastgltf::BufferView& view,
                       const fastgltf::CompressedBufferView& compressed,
                       std::optional<AllocationReservation>& reservation,
                       std::vector<std::byte>& decoded) const
    {
        const std::span<const std::byte> source = ResolveBuffer(compressed.bufferIndex);
        if (compressed.byteOffset > source.size()
            || compressed.byteLength > source.size() - compressed.byteOffset
            || compressed.byteLength == 0) {
            return false;
        }
        const auto bytes = CheckedMultiply(static_cast<std::uint64_t>(compressed.count),
                                           static_cast<std::uint64_t>(compressed.byteStride));
        if (!bytes.has_value() || *bytes == 0 || *bytes != view.byteLength
            || *bytes > maxDecodedBytes_ || *bytes > static_cast<std::uint64_t>(SIZE_MAX)) {
            return false;
        }
        const std::uint64_t stride = static_cast<std::uint64_t>(compressed.byteStride);
        switch (compressed.mode) {
            case fastgltf::MeshoptCompressionMode::Attributes:
                if (stride > 256 || (stride % 4) != 0) {
                    return false;
                }
                break;
            case fastgltf::MeshoptCompressionMode::Triangles:
                if ((stride != 2 && stride != 4) || (compressed.count % 3) != 0) {
                    return false;
                }
                break;
            case fastgltf::MeshoptCompressionMode::Indices:
                if (stride != 2 && stride != 4) {
                    return false;
                }
                break;
        }
        switch (compressed.filter) {
            case fastgltf::MeshoptCompressionFilter::None:
                break;
            case fastgltf::MeshoptCompressionFilter::Octahedral:
                if (stride != 4 && stride != 8) {
                    return false;
                }
                break;
            case fastgltf::MeshoptCompressionFilter::Quaternion:
                if (stride != 8) {
                    return false;
                }
                break;
            case fastgltf::MeshoptCompressionFilter::Exponential:
                if ((stride % 4) != 0) {
                    return false;
                }
                break;
        }

        if (ledger_ != nullptr) {
            auto scoped = ledger_->ReserveScoped(*bytes);
            if (!scoped.has_value()) {
                return false;
            }
            reservation = std::move(scoped);
        }
        decoded.resize(static_cast<std::size_t>(*bytes));
        const auto* input = reinterpret_cast<const unsigned char*>(source.data() + compressed.byteOffset);
        int result = -1;
        switch (compressed.mode) {
            case fastgltf::MeshoptCompressionMode::Attributes:
                result = meshopt_decodeVertexBuffer(decoded.data(), compressed.count,
                                                    compressed.byteStride, input, compressed.byteLength);
                break;
            case fastgltf::MeshoptCompressionMode::Triangles:
                result = meshopt_decodeIndexBuffer(decoded.data(), compressed.count,
                                                   compressed.byteStride, input, compressed.byteLength);
                break;
            case fastgltf::MeshoptCompressionMode::Indices:
                result = meshopt_decodeIndexSequence(decoded.data(), compressed.count,
                                                     compressed.byteStride, input, compressed.byteLength);
                break;
        }
        if (result != 0) {
            decoded.clear();
            reservation.reset();
            return false;
        }
        switch (compressed.filter) {
            case fastgltf::MeshoptCompressionFilter::None:
                break;
            case fastgltf::MeshoptCompressionFilter::Octahedral:
                meshopt_decodeFilterOct(decoded.data(), compressed.count, compressed.byteStride);
                break;
            case fastgltf::MeshoptCompressionFilter::Quaternion:
                meshopt_decodeFilterQuat(decoded.data(), compressed.count, compressed.byteStride);
                break;
            case fastgltf::MeshoptCompressionFilter::Exponential:
                meshopt_decodeFilterExp(decoded.data(), compressed.count, compressed.byteStride);
                break;
        }
        return true;
    }

    struct CachedBuffer {
        std::vector<std::byte> bytes;
        std::optional<AllocationReservation> reservation;
    };

    const fastgltf::Asset& asset_;
    AllocationLedger* ledger_ = nullptr;
    std::uint64_t maxDecodedBytes_ = 0;
    mutable bool usedMeshopt_ = false;
    mutable std::unordered_map<std::size_t, CachedBuffer> meshopt_;
};

// --- Bounded Draco decode ---------------------------------------------------

struct DracoMeshData {
    std::vector<float> positions; // 3 per point
    std::vector<float> normals;   // 3 per point, empty when absent
    std::vector<float> colors;    // 4 per point, empty when absent
    std::vector<std::uint32_t> indices;
};

bool DracoAttributeId(const fastgltf::DracoCompressedPrimitive& draco, const char* name,
                      std::uint32_t& id) noexcept
{
    const auto it = draco.findAttribute(name);
    if (it == draco.attributes.cend()) {
        return false;
    }
    id = static_cast<std::uint32_t>(it->accessorIndex);
    return true;
}

ErrorCode DecodeDraco(std::span<const std::byte> compressed,
                      const fastgltf::DracoCompressedPrimitive& draco,
                      std::size_t expectedVertices, std::size_t expectedIndices,
                      DracoMeshData& out)
{
    std::uint32_t positionId = 0;
    if (!DracoAttributeId(draco, "POSITION", positionId)) {
        return ErrorCode::MalformedData;
    }
    if (expectedVertices == 0 || expectedIndices % 3 != 0
        || expectedIndices / 3 > ProviderLimits::kDracoTrianglesMax) {
        return ErrorCode::DracoPrimitiveLimit;
    }
    const auto declaredVertices = CheckedMultiply(static_cast<std::uint64_t>(expectedVertices), 64);
    const auto declaredIndices = CheckedMultiply(static_cast<std::uint64_t>(expectedIndices), 8);
    const auto declared = declaredVertices && declaredIndices
                              ? CheckedAdd(*declaredVertices, *declaredIndices)
                              : std::nullopt;
    if (!declared.has_value() || *declared > ProviderLimits::kDracoDecodedWorkingSetMaxBytes) {
        return ErrorCode::DracoPrimitiveLimit;
    }
    if (compressed.size() < 5 || std::memcmp(compressed.data(), "DRACO", 5) != 0) {
        return ErrorCode::MalformedData;
    }

    // SEC-02: parse the decoder-declared connectivity counts and reject a stream
    // that disagrees with the glTF accessors or declares an over-budget working
    // set before draco allocates its corner table from those counts.
    model_core::DracoDeclaredCounts declaredCounts;
    const auto declaredStatus = model_core::ValidateDracoCounts(
        compressed, expectedVertices, expectedIndices, ProviderLimits::kDracoTrianglesMax,
        ProviderLimits::kDracoDecodedWorkingSetMaxBytes, &declaredCounts);
    if (declaredStatus == model_core::DracoPreflightStatus::Malformed) {
        return ErrorCode::MalformedData;
    }
    if (declaredStatus == model_core::DracoPreflightStatus::OverLimit) {
        return ErrorCode::DracoPrimitiveLimit;
    }

    draco::DecoderBuffer buffer;
    buffer.Init(reinterpret_cast<const char*>(compressed.data()), compressed.size());
    model_core::DracoDecoderInvocations().fetch_add(1, std::memory_order_relaxed);
    auto statusOrMesh = draco::Decoder().DecodeMeshFromBuffer(&buffer);
    if (!statusOrMesh.ok()) {
        return ErrorCode::MalformedData;
    }
    std::unique_ptr<draco::Mesh> mesh = std::move(statusOrMesh).value();
    if (mesh == nullptr) {
        return ErrorCode::MalformedData;
    }
    const std::size_t pointCount = static_cast<std::size_t>(mesh->num_points());
    const std::size_t faceCount = static_cast<std::size_t>(mesh->num_faces());
    if (faceCount > ProviderLimits::kDracoTrianglesMax) {
        return ErrorCode::ResourceLimit;
    }
    if (pointCount != expectedVertices || faceCount * 3 != expectedIndices) {
        return ErrorCode::MalformedData;
    }

    std::uint32_t normalId = 0;
    std::uint32_t colorId = 0;
    const bool hasNormal = DracoAttributeId(draco, "NORMAL", normalId);
    const bool hasColor = DracoAttributeId(draco, "COLOR_0", colorId);

    const draco::PointAttribute* positionAttr = mesh->GetAttributeByUniqueId(positionId);
    if (positionAttr == nullptr) {
        return ErrorCode::MalformedData;
    }
    const draco::PointAttribute* normalAttr =
        hasNormal ? mesh->GetAttributeByUniqueId(normalId) : nullptr;
    const draco::PointAttribute* colorAttr =
        hasColor ? mesh->GetAttributeByUniqueId(colorId) : nullptr;
    if ((hasNormal && normalAttr == nullptr) || (hasColor && colorAttr == nullptr)) {
        return ErrorCode::MalformedData;
    }

    out.positions.resize(pointCount * 3);
    if (hasNormal) {
        out.normals.resize(pointCount * 3);
    }
    if (hasColor) {
        out.colors.resize(pointCount * 4, 1.0f);
    }
    for (std::size_t p = 0; p < pointCount; ++p) {
        const draco::PointIndex pointIndex(static_cast<std::uint32_t>(p));
        std::array<float, 3> position{};
        if (!positionAttr->GetValue<float, 3>(positionAttr->mapped_index(pointIndex), &position)) {
            return ErrorCode::MalformedData;
        }
        out.positions[p * 3 + 0] = position[0];
        out.positions[p * 3 + 1] = position[1];
        out.positions[p * 3 + 2] = position[2];
        if (hasNormal) {
            std::array<float, 3> normal{};
            if (!normalAttr->GetValue<float, 3>(normalAttr->mapped_index(pointIndex), &normal)) {
                return ErrorCode::MalformedData;
            }
            out.normals[p * 3 + 0] = normal[0];
            out.normals[p * 3 + 1] = normal[1];
            out.normals[p * 3 + 2] = normal[2];
        }
        if (hasColor) {
            std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
            if (colorAttr->num_components() == 3) {
                std::array<float, 3> rgb{};
                if (!colorAttr->GetValue<float, 3>(colorAttr->mapped_index(pointIndex), &rgb)) {
                    return ErrorCode::MalformedData;
                }
                color[0] = rgb[0];
                color[1] = rgb[1];
                color[2] = rgb[2];
            } else if (!colorAttr->GetValue<float, 4>(colorAttr->mapped_index(pointIndex), &color)) {
                return ErrorCode::MalformedData;
            }
            for (int i = 0; i < 4; ++i) {
                out.colors[p * 4 + i] = color[static_cast<std::size_t>(i)];
            }
        }
    }
    out.indices.resize(faceCount * 3);
    for (std::size_t f = 0; f < faceCount; ++f) {
        const draco::Mesh::Face& face = mesh->face(draco::FaceIndex(static_cast<std::uint32_t>(f)));
        for (int corner = 0; corner < 3; ++corner) {
            const std::uint32_t value = face[corner].value();
            if (static_cast<std::size_t>(value) >= pointCount) {
                return ErrorCode::MalformedData;
            }
            out.indices[f * 3 + static_cast<std::size_t>(corner)] = value;
        }
    }
    return ErrorCode::None;
}

// --- Embedded image decode (KTX2/Basis + WebP) ------------------------------

bool LooksLikeWebp(std::span<const std::byte> bytes) noexcept
{
    return bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0
        && std::memcmp(bytes.data() + 8, "WEBP", 4) == 0;
}

bool TryDecodeKtx2(std::span<const std::byte> bytes, std::uint64_t remainingPixels,
                   std::uint64_t& pixels) noexcept
{
    if (!model_core::LooksLikeKtx2(bytes)) {
        return false;
    }
    // SEC-02: bound the fixed header, level index and worst-case expansion from
    // the span before ktxTexture2_CreateFromMemory can allocate or Zstd-expand
    // from attacker-controlled fields. Provider and worker share this preflight.
    const std::uint64_t maxDecodedBytes
        = remainingPixels > (std::numeric_limits<std::uint64_t>::max)() / 4
              ? (std::numeric_limits<std::uint64_t>::max)()
              : remainingPixels * 4;
    const model_core::Ktx2Limits limits{ProviderLimits::kStreamMaxBytes, maxDecodedBytes,
                                        remainingPixels, model_core::kMaxTextureDimension};
    const auto preflight = model_core::PreflightKtx2(bytes, limits);
    if (!preflight.has_value()) {
        return false;
    }
    const std::uint64_t width = preflight->header.width;
    const std::uint64_t height = preflight->header.height;
    const auto count = CheckedMultiply(width, height);
    if (!count.has_value() || *count > remainingPixels) {
        return false;
    }
    ktxTexture2* texture = nullptr;
    model_core::Ktx2DecoderInvocations().fetch_add(1, std::memory_order_relaxed);
    if (ktxTexture2_CreateFromMemory(reinterpret_cast<const ktx_uint8_t*>(bytes.data()),
                                     bytes.size(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
                                     &texture) != KTX_SUCCESS
        || texture == nullptr) {
        return false;
    }
    bool ok = texture->baseWidth == width && texture->baseHeight == height;
    if (ok && ktxTexture2_NeedsTranscoding(texture)) {
        if (ktxTexture2_TranscodeBasis(texture, KTX_TTF_RGBA32, 0) != KTX_SUCCESS) {
            ok = false;
        }
    }
    ktxTexture2_Destroy(texture);
    if (ok) {
        pixels = *count;
    }
    return ok;
}

bool TryDecodeWebp(std::span<const std::byte> bytes, std::uint64_t remainingPixels,
                   std::uint64_t& pixels) noexcept
{
    if (!LooksLikeWebp(bytes)) {
        return false;
    }
    int width = 0;
    int height = 0;
    if (!WebPGetInfo(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(),
                     &width, &height)
        || width <= 0 || height <= 0) {
        return false;
    }
    const auto count = CheckedMultiply(static_cast<std::uint64_t>(width),
                                       static_cast<std::uint64_t>(height));
    if (!count.has_value() || *count > remainingPixels) {
        return false;
    }
    std::uint8_t* rgba = WebPDecodeRGBA(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                        bytes.size(), &width, &height);
    if (rgba == nullptr) {
        return false;
    }
    WebPFree(rgba);
    pixels = *count;
    return true;
}

std::span<const std::byte> ImageBytes(const fastgltf::Asset& asset, const GltfBufferAccess& access,
                                      const fastgltf::Image& image) noexcept
{
    const fastgltf::DataSource& data = image.data;
    if (const auto* array = std::get_if<fastgltf::sources::Array>(&data)) {
        return std::span<const std::byte>(array->bytes.data(), array->bytes.size_bytes());
    }
    if (const auto* vector = std::get_if<fastgltf::sources::Vector>(&data)) {
        return std::span<const std::byte>(vector->bytes.data(), vector->bytes.size());
    }
    if (const auto* view = std::get_if<fastgltf::sources::ByteView>(&data)) {
        return view->bytes;
    }
    if (const auto* bufferView = std::get_if<fastgltf::sources::BufferView>(&data)) {
        (void)asset;
        return access.Resolve(bufferView->bufferViewIndex);
    }
    return {};
}

model_core::MaterialPayload ToMaterialPayload(const fastgltf::Material& material) noexcept
{
    model_core::MaterialPayload payload{};
    for (int i = 0; i < 4; ++i) {
        payload.baseColorFactor[i] = material.pbrData.baseColorFactor[static_cast<std::size_t>(i)];
    }
    payload.metallicFactor = material.pbrData.metallicFactor;
    payload.roughnessFactor = material.pbrData.roughnessFactor;
    for (int i = 0; i < 3; ++i) {
        payload.emissiveFactor[i] = material.emissiveFactor[static_cast<std::size_t>(i)];
    }
    payload.uvOffset[0] = 0.0f;
    payload.uvOffset[1] = 0.0f;
    payload.uvScale[0] = 1.0f;
    payload.uvScale[1] = 1.0f;
    payload.uvRotation = 0.0f;
    switch (material.alphaMode) {
        case fastgltf::AlphaMode::Mask:
            payload.alphaMode = static_cast<std::uint32_t>(model_core::AlphaModeId::Mask);
            break;
        case fastgltf::AlphaMode::Blend:
            payload.alphaMode = static_cast<std::uint32_t>(model_core::AlphaModeId::Blend);
            break;
        case fastgltf::AlphaMode::Opaque:
        default:
            payload.alphaMode = static_cast<std::uint32_t>(model_core::AlphaModeId::Opaque);
            break;
    }
    payload.alphaCutoff = material.alphaCutoff;
    payload.flags = 0u;
    if (material.doubleSided) {
        payload.flags |= model_core::kMaterialFlagDoubleSided;
    }
    if (material.unlit) {
        payload.flags |= model_core::kMaterialFlagUnlit;
    }
    payload.transmissionFactor = 0.0f;
    return payload;
}

} // namespace

struct GltfAssetHolder {
    fastgltf::GltfDataBuffer buffer;
    fastgltf::Parser parser;
    fastgltf::Asset asset;
    std::unique_ptr<GltfBufferAccess> access;
};

GltfAdapter::GltfAdapter() noexcept = default;

GltfAdapter::~GltfAdapter() noexcept = default;

ErrorCode GltfAdapter::SourceReadFailure() const noexcept
{
    if (input_.deadline != nullptr && !input_.deadline->Checkpoint()) {
        return ErrorCode::Cancelled;
    }
    return ErrorCode::MalformedData;
}

void GltfAdapter::Reset() noexcept
{
    holder_.reset();
    input_ = AdapterInput{};
    parsed_ = false;
    externalReferenceDetected_ = false;
    usedDraco_ = false;
    decodedImageCount_ = 0;
    decodedImagePixels_ = 0;
    bytes_ = {};
    ownedBytes_.clear();
    ownedBytes_.shrink_to_fit();
    bytesReservation_.reset();
    instances_.clear();
    materials_.clear();
    haveOrigin_ = false;
    origin_[0] = origin_[1] = origin_[2] = 0.0;
}

ErrorCode GltfAdapter::Initialize(const AdapterInput& input) noexcept
{
    return RunContainedStageMember([this, &input]() { return InitializeImpl(input); },
                                   DiagnosticStage::AdapterInitialize);
}

ErrorCode GltfAdapter::InitializeImpl(const AdapterInput& input)
{
    Reset();
    if (input.source == nullptr || input.limits == nullptr || input.deadline == nullptr) {
        return ErrorCode::InternalImporterFailure;
    }
    input_ = input;
    return ErrorCode::None;
}

ErrorCode GltfAdapter::LoadSourceBytes()
{
    bytes_ = input_.source->ContiguousView();
    if (!bytes_.empty()) {
        return ErrorCode::None;
    }
    const std::uint64_t size = input_.source->Size();
    if (size == 0) {
        return ErrorCode::MalformedData;
    }
    if (size > ProviderLimits::kContiguousBackingMaxBytes) {
        return ErrorCode::ResourceLimit;
    }
    if (input_.ledger != nullptr) {
        auto reservation = input_.ledger->ReserveScoped(size);
        if (!reservation.has_value()) {
            return ErrorCode::ResourceLimit;
        }
        bytesReservation_ = std::move(reservation);
    }
    ownedBytes_.resize(static_cast<std::size_t>(size));
    if (!input_.source->ReadAt(0, ownedBytes_)) {
        ownedBytes_.clear();
        return SourceReadFailure();
    }
    bytes_ = std::span<const std::byte>(ownedBytes_.data(), ownedBytes_.size());
    return ErrorCode::None;
}

ErrorCode GltfAdapter::Parse() noexcept
{
    return RunContainedStageMember([this]() { return ParseImpl(); }, DiagnosticStage::Parse);
}

ErrorCode GltfAdapter::ParseImpl()
{
    if (input_.deadline == nullptr || !input_.deadline->Checkpoint()) {
        return ErrorCode::Cancelled;
    }
    parsed_ = false;
    externalReferenceDetected_ = false;
    usedDraco_ = false;
    decodedImageCount_ = 0;
    decodedImagePixels_ = 0;
    haveOrigin_ = false;
    instances_.clear();
    materials_.clear();
    holder_.reset();

    ErrorCode load = LoadSourceBytes();
    if (load != ErrorCode::None) {
        return load;
    }
    if (bytes_.size() > ProviderLimits::kContiguousBackingMaxBytes) {
        return ErrorCode::ResourceLimit;
    }

    auto bufferResult = fastgltf::GltfDataBuffer::FromBytes(bytes_.data(), bytes_.size());
    // fastgltf owns its own copy now; release our backing to cut peak memory.
    ownedBytes_.clear();
    ownedBytes_.shrink_to_fit();
    bytesReservation_.reset();
    bytes_ = {};
    if (!bufferResult) {
        return MapFastGltfError(bufferResult.error());
    }

    holder_ = std::make_unique<GltfAssetHolder>();
    holder_->buffer = std::move(bufferResult.get());
    holder_->parser = fastgltf::Parser(
        fastgltf::Extensions::KHR_draco_mesh_compression
        | fastgltf::Extensions::KHR_texture_basisu
        | fastgltf::Extensions::KHR_texture_transform
        | fastgltf::Extensions::KHR_mesh_quantization
        | fastgltf::Extensions::EXT_meshopt_compression
        | fastgltf::Extensions::EXT_texture_webp
        | fastgltf::Extensions::KHR_materials_unlit);

    auto assetResult = holder_->parser.loadGltf(holder_->buffer, std::filesystem::path{},
                                                fastgltf::Options::None);
    if (!assetResult) {
        holder_.reset();
        return MapFastGltfError(assetResult.error());
    }
    holder_->asset = std::move(assetResult.get());
    const fastgltf::Asset& asset = holder_->asset;
    holder_->access = std::make_unique<GltfBufferAccess>(
        asset, input_.ledger, ProviderLimits::kAccountedScratchMaxBytes);

    if (!input_.deadline->Checkpoint()) {
        return ErrorCode::Cancelled;
    }

    if (asset.nodes.size() > ProviderLimits::kNodesMax
        || asset.meshes.size() > ProviderLimits::kNodesMax
        || asset.materials.size() > ProviderLimits::kMaterialsMax) {
        return ErrorCode::ResourceLimit;
    }

    // Any non-`data:` buffer/image URI is an external sidecar the provider must
    // never open: a `.gltf` that depends on one gets the generic icon.
    const auto externalUri = [](const fastgltf::DataSource& data) noexcept {
        const auto* uri = std::get_if<fastgltf::sources::URI>(&data);
        return uri != nullptr && !uri->uri.isDataUri();
    };
    for (const fastgltf::Buffer& buffer : asset.buffers) {
        if (externalUri(buffer.data)) {
            externalReferenceDetected_ = true;
            return ErrorCode::UnsafeReference;
        }
    }
    for (const fastgltf::Image& image : asset.images) {
        if (externalUri(image.data)) {
            externalReferenceDetected_ = true;
            return ErrorCode::UnsafeReference;
        }
    }

    materials_.reserve(asset.materials.size());
    for (const fastgltf::Material& material : asset.materials) {
        materials_.push_back(ToMaterialPayload(material));
    }

    ErrorCode images = DecodeImages();
    if (images != ErrorCode::None) {
        return images;
    }
    if (!input_.deadline->Checkpoint()) {
        return ErrorCode::Cancelled;
    }

    // Build instance transforms from the default scene (or the implicit roots).
    std::vector<std::size_t> roots;
    if (asset.defaultScene.has_value() && *asset.defaultScene < asset.scenes.size()) {
        const auto& indices = asset.scenes[*asset.defaultScene].nodeIndices;
        roots.assign(indices.begin(), indices.end());
    } else if (!asset.scenes.empty()) {
        const auto& indices = asset.scenes[0].nodeIndices;
        roots.assign(indices.begin(), indices.end());
    } else {
        std::vector<bool> isChild(asset.nodes.size(), false);
        for (const fastgltf::Node& node : asset.nodes) {
            for (const std::size_t child : node.children) {
                if (child < isChild.size()) {
                    isChild[child] = true;
                }
            }
        }
        for (std::size_t i = 0; i < isChild.size(); ++i) {
            if (!isChild[i]) {
                roots.push_back(i);
            }
        }
    }

    struct Work {
        std::size_t node = 0;
        Mat4 parent{};
        std::uint64_t depth = 0;
    };
    std::vector<Work> stack;
    stack.reserve(roots.size());
    for (const std::size_t root : roots) {
        if (root < asset.nodes.size()) {
            stack.push_back(Work{root, IdentityMatrix(), 0});
        }
    }
    std::uint64_t visited = 0;
    while (!stack.empty()) {
        if ((++visited & 0x3FFu) == 0 && !input_.deadline->Checkpoint()) {
            return ErrorCode::Cancelled;
        }
        const Work work = stack.back();
        stack.pop_back();
        if (work.node >= asset.nodes.size()) {
            continue;
        }
        if (work.depth >= kMaxTransformDepth) {
            return ErrorCode::ResourceLimit;
        }
        const fastgltf::Node& node = asset.nodes[work.node];
        const Mat4 world = Multiply(work.parent, LocalMatrix(node));
        if (node.meshIndex.has_value()) {
            if (*node.meshIndex >= asset.meshes.size()) {
                return ErrorCode::MalformedData;
            }
            Instance instance;
            instance.meshIndex = *node.meshIndex;
            std::memcpy(instance.world, world.data(), sizeof(double) * 16);
            instances_.push_back(instance);
            if (instances_.size() > ProviderLimits::kNodesMax) {
                return ErrorCode::ResourceLimit;
            }
        }
        for (const std::size_t child : node.children) {
            stack.push_back(Work{child, world, work.depth + 1});
        }
    }

    // Require at least one triangle-list primitive with positions.
    bool hasTriangles = false;
    for (const Instance& instance : instances_) {
        const fastgltf::Mesh& mesh = asset.meshes[instance.meshIndex];
        for (const fastgltf::Primitive& primitive : mesh.primitives) {
            if (primitive.type == fastgltf::PrimitiveType::Triangles
                && primitive.findAttribute("POSITION") != primitive.attributes.end()) {
                hasTriangles = true;
                break;
            }
        }
        if (hasTriangles) {
            break;
        }
    }
    if (!hasTriangles) {
        return ErrorCode::EmptyGeometry;
    }
parsed_ = true;
    return ErrorCode::None;
}

bool GltfAdapter::UsedMeshopt() const noexcept
{
    return holder_ != nullptr && holder_->access != nullptr && holder_->access->UsedMeshopt();
}

ErrorCode GltfAdapter::DecodeImages()
{
    const fastgltf::Asset& asset = holder_->asset;
    std::uint64_t remaining = ProviderLimits::kDecodedTexturePixelsMax;
    std::uint64_t counter = 0;
    for (const fastgltf::Image& image : asset.images) {
        if (((++counter) & (kCheckpointImages - 1)) == 0 && !input_.deadline->Checkpoint()) {
            return ErrorCode::Cancelled;
        }
        const std::span<const std::byte> bytes = ImageBytes(asset, *holder_->access, image);
        if (bytes.empty()) {
            continue;
        }
        std::uint64_t pixels = 0;
        if (TryDecodeKtx2(bytes, remaining, pixels) || TryDecodeWebp(bytes, remaining, pixels)) {
            decodedImagePixels_ += pixels;
            remaining -= pixels;
            ++decodedImageCount_;
        }
    }
    return ErrorCode::None;
}

bool GltfAdapter::BuildVertex(VertexSample& vertex, const double world[16],
                              const float* positions, const float* normals, const float* colors,
                              std::size_t index) noexcept
{
    Mat4 matrix;
    std::memcpy(matrix.data(), world, sizeof(double) * 16);

    const double local[3] = {
        static_cast<double>(positions[index * 3 + 0]),
        static_cast<double>(positions[index * 3 + 1]),
        static_cast<double>(positions[index * 3 + 2]),
    };
    if (!std::isfinite(local[0]) || !std::isfinite(local[1]) || !std::isfinite(local[2])) {
        return false;
    }
    double worldPosition[3] = {0.0, 0.0, 0.0};
    TransformPoint(matrix, local, worldPosition);
    if (!std::isfinite(worldPosition[0]) || !std::isfinite(worldPosition[1])
        || !std::isfinite(worldPosition[2])) {
        return false;
    }
    if (!haveOrigin_) {
        origin_[0] = worldPosition[0];
        origin_[1] = worldPosition[1];
        origin_[2] = worldPosition[2];
        haveOrigin_ = true;
    }
    vertex.position[0] = static_cast<float>(worldPosition[0] - origin_[0]);
    vertex.position[1] = static_cast<float>(worldPosition[1] - origin_[1]);
    vertex.position[2] = static_cast<float>(worldPosition[2] - origin_[2]);
    if (normals != nullptr) {
        const float in[3] = {normals[index * 3 + 0], normals[index * 3 + 1], normals[index * 3 + 2]};
        TransformNormal(matrix, in, vertex.normal);
    }
    if (colors != nullptr) {
        vertex.color[0] = Saturate(colors[index * 4 + 0]);
        vertex.color[1] = Saturate(colors[index * 4 + 1]);
        vertex.color[2] = Saturate(colors[index * 4 + 2]);
        vertex.color[3] = Saturate(colors[index * 4 + 3]);
    }
    return true;
}

ErrorCode GltfAdapter::EnumerateMaterials(IMaterialSink& sink) noexcept
{
    return RunContainedStageMember([this, &sink]() { return EnumerateMaterialsImpl(sink); },
                                   DiagnosticStage::Materials);
}

ErrorCode GltfAdapter::EnumerateMaterialsImpl(IMaterialSink& sink)
{
    if (!parsed_ || holder_ == nullptr) {
        return ErrorCode::InternalImporterFailure;
    }
    for (std::size_t i = 0; i < materials_.size(); ++i) {
        if (!sink.OnMaterial(static_cast<std::uint32_t>(i) + 1u, materials_[i])) {
            return ErrorCode::None;
        }
    }
    return ErrorCode::None;
}

ErrorCode GltfAdapter::EnumerateGeometry(IGeometrySink& sink) noexcept
{
    return RunContainedStageMember([this, &sink]() { return EnumerateGeometryImpl(sink); },
                                   DiagnosticStage::Geometry);
}

ErrorCode GltfAdapter::EnumerateGeometryImpl(IGeometrySink& sink)
{
    if (!parsed_ || holder_ == nullptr) {
        return ErrorCode::InternalImporterFailure;
    }
    const fastgltf::Asset& asset = holder_->asset;
    GltfBufferAccess& access = *holder_->access;

    std::vector<float> ownedPositions;
    std::vector<float> ownedNormals;
    std::vector<float> ownedColors;
    DracoMeshData dracoData;
    std::uint64_t triangleCounter = 0;
    const auto materialFor = [this](const fastgltf::Primitive& primitive) noexcept {
        if (!primitive.materialIndex.has_value()
            || *primitive.materialIndex >= materials_.size()) {
            return std::uint32_t{0};
        }
        return static_cast<std::uint32_t>(*primitive.materialIndex) + 1u;
    };

    for (const Instance& instance : instances_) {
        if (!input_.deadline->Checkpoint()) {
            return ErrorCode::Cancelled;
        }
        const fastgltf::Mesh& mesh = asset.meshes[instance.meshIndex];
        for (const fastgltf::Primitive& primitive : mesh.primitives) {
            if (primitive.type != fastgltf::PrimitiveType::Triangles) {
                continue;
            }
            const auto positionIt = primitive.findAttribute("POSITION");
            if (positionIt == primitive.attributes.end()
                || positionIt->accessorIndex >= asset.accessors.size()) {
                continue;
            }
            const fastgltf::Accessor& positionAccessor = asset.accessors[positionIt->accessorIndex];
            const std::size_t vertexCount = positionAccessor.count;
            if (vertexCount == 0) {
                continue;
            }
            if (vertexCount > ProviderLimits::kPointsInspectedMax) {
                return ErrorCode::ResourceLimit;
            }

            const float* positionData = nullptr;
            const float* normalData = nullptr;
            const float* colorData = nullptr;
            std::size_t indexCount = vertexCount;
            bool hasIndices = false;

            if (primitive.dracoCompression != nullptr) {
                const std::span<const std::byte> compressed =
                    access.Resolve(primitive.dracoCompression->bufferView);
                const std::size_t expectedIndices =
                    (primitive.indicesAccessor.has_value()
                     && *primitive.indicesAccessor < asset.accessors.size())
                        ? asset.accessors[*primitive.indicesAccessor].count
                        : vertexCount;
                ErrorCode decoded = DecodeDraco(compressed, *primitive.dracoCompression,
                                                vertexCount, expectedIndices, dracoData);
                if (decoded != ErrorCode::None) {
                    return decoded;
                }
                positionData = dracoData.positions.data();
                normalData = dracoData.normals.empty() ? nullptr : dracoData.normals.data();
                colorData = dracoData.colors.empty() ? nullptr : dracoData.colors.data();
                indexCount = dracoData.indices.size();
                usedDraco_ = true;
            } else {
                if (!access.AccessorResolvable(positionAccessor)) {
                    continue;
                }
                std::uint64_t vertexBytes = CheckedMultiply(
                    static_cast<std::uint64_t>(vertexCount), 12).value_or(UINT64_MAX);
                const fastgltf::Accessor* normalAccessor = nullptr;
                const fastgltf::Accessor* colorAccessor = nullptr;
                if (const auto normalIt = primitive.findAttribute("NORMAL");
                    normalIt != primitive.attributes.end()
                    && normalIt->accessorIndex < asset.accessors.size()) {
                    normalAccessor = &asset.accessors[normalIt->accessorIndex];
                    if (normalAccessor->type != fastgltf::AccessorType::Vec3
                        || normalAccessor->count != vertexCount
                        || !access.AccessorResolvable(*normalAccessor)) {
                        normalAccessor = nullptr;
                    } else {
                        vertexBytes += static_cast<std::uint64_t>(vertexCount) * 12;
                    }
                }
                if (const auto colorIt = primitive.findAttribute("COLOR_0");
                    colorIt != primitive.attributes.end()
                    && colorIt->accessorIndex < asset.accessors.size()) {
                    colorAccessor = &asset.accessors[colorIt->accessorIndex];
                    if (colorAccessor->count != vertexCount
                        || !access.AccessorResolvable(*colorAccessor)) {
                        colorAccessor = nullptr;
                    } else {
                        vertexBytes += static_cast<std::uint64_t>(vertexCount) * 16;
                    }
                }
                if (vertexBytes > ProviderLimits::kAccountedScratchMaxBytes / 2) {
                    return ErrorCode::ResourceLimit;
                }
                std::optional<AllocationReservation> reservation;
                if (input_.ledger != nullptr) {
                    auto scoped = input_.ledger->ReserveScoped(vertexBytes);
                    if (!scoped.has_value()) {
                        return ErrorCode::ResourceLimit;
                    }
                    reservation = std::move(scoped);
                }

                ownedPositions.assign(vertexCount * 3, 0.0f);
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
                    asset, positionAccessor,
                    [&](fastgltf::math::fvec3 value, std::size_t index) {
                        ownedPositions[index * 3 + 0] = value.x();
                        ownedPositions[index * 3 + 1] = value.y();
                        ownedPositions[index * 3 + 2] = value.z();
                    },
                    access);
                ownedNormals.clear();
                if (normalAccessor != nullptr) {
                    ownedNormals.assign(vertexCount * 3, 0.0f);
                    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
                        asset, *normalAccessor,
                        [&](fastgltf::math::fvec3 value, std::size_t index) {
                            ownedNormals[index * 3 + 0] = value.x();
                            ownedNormals[index * 3 + 1] = value.y();
                            ownedNormals[index * 3 + 2] = value.z();
                        },
                        access);
                }
                ownedColors.clear();
                if (colorAccessor != nullptr) {
                    ownedColors.assign(vertexCount * 4, 1.0f);
                    if (colorAccessor->type == fastgltf::AccessorType::Vec4) {
                        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(
                            asset, *colorAccessor,
                            [&](fastgltf::math::fvec4 value, std::size_t index) {
                                ownedColors[index * 4 + 0] = value.x();
                                ownedColors[index * 4 + 1] = value.y();
                                ownedColors[index * 4 + 2] = value.z();
                                ownedColors[index * 4 + 3] = value.w();
                            },
                            access);
                    } else if (colorAccessor->type == fastgltf::AccessorType::Vec3) {
                        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
                            asset, *colorAccessor,
                            [&](fastgltf::math::fvec3 value, std::size_t index) {
                                ownedColors[index * 4 + 0] = value.x();
                                ownedColors[index * 4 + 1] = value.y();
                                ownedColors[index * 4 + 2] = value.z();
                                ownedColors[index * 4 + 3] = 1.0f;
                            },
                            access);
                    }
                }
                positionData = ownedPositions.data();
                normalData = ownedNormals.empty() ? nullptr : ownedNormals.data();
                colorData = ownedColors.empty() ? nullptr : ownedColors.data();
                if (primitive.indicesAccessor.has_value()) {
                    if (*primitive.indicesAccessor >= asset.accessors.size()) {
                        continue;
                    }
                    const fastgltf::Accessor& indexAccessor =
                        asset.accessors[*primitive.indicesAccessor];
                    if (indexAccessor.count % 3 != 0
                        || !access.AccessorResolvable(indexAccessor)) {
                        continue;
                    }
                    indexCount = indexAccessor.count;
                    hasIndices = true;
                }
                // Keep the reservation alive for the duration of enumeration.
                if (hasIndices) {
                    const fastgltf::Accessor& indexAccessor =
                        asset.accessors[*primitive.indicesAccessor];
                    const std::size_t triangleCount = indexCount / 3;
                    const std::uint32_t materialIndex = materialFor(primitive);
                    for (std::size_t t = 0; t < triangleCount; ++t) {
                        if (((++triangleCounter) & (kCheckpointTriangles - 1)) == 0
                            && !input_.deadline->Checkpoint()) {
                            return ErrorCode::Cancelled;
                        }
                        TriangleSample sample{};
                        bool finite = true;
                        for (int corner = 0; corner < 3; ++corner) {
                            const std::uint32_t index = fastgltf::getAccessorElement<std::uint32_t>(
                                asset, indexAccessor, t * 3 + static_cast<std::size_t>(corner),
                                access);
                            if (static_cast<std::size_t>(index) >= vertexCount) {
                                finite = false;
                                break;
                            }
                            if (!BuildVertex(sample.vertices[corner], instance.world,
                                             positionData, normalData, colorData, index)) {
                                finite = false;
                                break;
                            }
                        }
                        if (!finite) {
                            continue;
                        }
                        sample.materialIndex = materialIndex;
                        if (!sink.OnTriangle(sample)) {
                            return ErrorCode::None;
                        }
                    }
                } else {
                    const std::size_t triangleCount = vertexCount / 3;
                    const std::uint32_t materialIndex = materialFor(primitive);
                    for (std::size_t t = 0; t < triangleCount; ++t) {
                        if (((++triangleCounter) & (kCheckpointTriangles - 1)) == 0
                            && !input_.deadline->Checkpoint()) {
                            return ErrorCode::Cancelled;
                        }
                        TriangleSample sample{};
                        bool finite = true;
                        for (int corner = 0; corner < 3; ++corner) {
                            const std::size_t index = t * 3 + static_cast<std::size_t>(corner);
                            if (!BuildVertex(sample.vertices[corner], instance.world,
                                             positionData, normalData, colorData, index)) {
                                finite = false;
                                break;
                            }
                        }
                        if (!finite) {
                            continue;
                        }
                        sample.materialIndex = materialIndex;
                        if (!sink.OnTriangle(sample)) {
                            return ErrorCode::None;
                        }
                    }
                }
                continue;
            }

            // Draco path: emit from the decoded attribute arrays.
            const std::uint32_t materialIndex =
                primitive.materialIndex.has_value()
                    ? static_cast<std::uint32_t>(*primitive.materialIndex) + 1u
                    : 0u;
            const std::size_t triangleCount = indexCount / 3;
            for (std::size_t t = 0; t < triangleCount; ++t) {
                if (((++triangleCounter) & (kCheckpointTriangles - 1)) == 0
                    && !input_.deadline->Checkpoint()) {
                    return ErrorCode::Cancelled;
                }
                TriangleSample sample{};
                bool finite = true;
                for (int corner = 0; corner < 3; ++corner) {
                    const std::uint32_t index =
                        dracoData.indices[t * 3 + static_cast<std::size_t>(corner)];
                    if (!BuildVertex(sample.vertices[corner], instance.world,
                                     positionData, normalData, colorData, index)) {
                        finite = false;
                        break;
                    }
                }
                if (!finite) {
                    continue;
                }
                sample.materialIndex = materialIndex;
                if (!sink.OnTriangle(sample)) {
                    return ErrorCode::None;
                }
            }
        }
    }
    return ErrorCode::None;
}

} // namespace preview3d::provider
