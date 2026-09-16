#include "MeshoptDecodeAdapter.h"

#include "platform/CheckedMath.h"

#include <meshoptimizer.h>

namespace import_worker {

std::variant<std::vector<std::byte>, model_core::ImportErrorCode> DecodeMeshoptBuffer(
    std::span<const std::byte> encoded, uint64_t count, uint64_t stride,
    uint64_t decodedByteLength, MeshoptDecodeMode mode, MeshoptDecodeFilter filter,
    const MeshoptDecodeOptions& options)
{
    using model_core::ImportErrorCode;
    const auto cancelled = [&] { return options.isCancelled && options.isCancelled(); };
    if (cancelled()) return ImportErrorCode::Cancelled;
    if (encoded.empty() || count == 0 || stride == 0)
        return ImportErrorCode::MalformedData;
    const auto bytes = platform::CheckedMultiply(count, stride);
    if (!bytes || *bytes != decodedByteLength)
        return ImportErrorCode::MalformedData;
    if (*bytes > options.maxDecodedBytes || *bytes > SIZE_MAX)
        return ImportErrorCode::ResourceLimit;

    switch (mode) {
    case MeshoptDecodeMode::Attributes:
        if (stride > 256 || (stride % 4) != 0) return ImportErrorCode::MalformedData;
        break;
    case MeshoptDecodeMode::Triangles:
        if ((stride != 2 && stride != 4) || (count % 3) != 0)
            return ImportErrorCode::MalformedData;
        if (filter != MeshoptDecodeFilter::None) return ImportErrorCode::MalformedData;
        break;
    case MeshoptDecodeMode::Indices:
        if (stride != 2 && stride != 4) return ImportErrorCode::MalformedData;
        if (filter != MeshoptDecodeFilter::None) return ImportErrorCode::MalformedData;
        break;
    }
    if ((filter == MeshoptDecodeFilter::Octahedral || filter == MeshoptDecodeFilter::Color)
            && stride != 4 && stride != 8)
        return ImportErrorCode::MalformedData;
    if (filter == MeshoptDecodeFilter::Quaternion && stride != 8)
        return ImportErrorCode::MalformedData;
    if (filter == MeshoptDecodeFilter::Exponential && (stride % 4) != 0)
        return ImportErrorCode::MalformedData;

    std::vector<std::byte> decoded(static_cast<size_t>(*bytes));
    const auto* input = reinterpret_cast<const unsigned char*>(encoded.data());
    int result = -1;
    switch (mode) {
    case MeshoptDecodeMode::Attributes:
        result = meshopt_decodeVertexBuffer(decoded.data(), static_cast<size_t>(count),
                                             static_cast<size_t>(stride), input, encoded.size());
        break;
    case MeshoptDecodeMode::Triangles:
        result = meshopt_decodeIndexBuffer(decoded.data(), static_cast<size_t>(count),
                                            static_cast<size_t>(stride), input, encoded.size());
        break;
    case MeshoptDecodeMode::Indices:
        result = meshopt_decodeIndexSequence(decoded.data(), static_cast<size_t>(count),
                                              static_cast<size_t>(stride), input, encoded.size());
        break;
    }
    if (result != 0) return ImportErrorCode::MalformedData;
    if (cancelled()) return ImportErrorCode::Cancelled;

    switch (filter) {
    case MeshoptDecodeFilter::None: break;
    case MeshoptDecodeFilter::Octahedral:
        meshopt_decodeFilterOct(decoded.data(), static_cast<size_t>(count), static_cast<size_t>(stride));
        break;
    case MeshoptDecodeFilter::Quaternion:
        meshopt_decodeFilterQuat(decoded.data(), static_cast<size_t>(count), static_cast<size_t>(stride));
        break;
    case MeshoptDecodeFilter::Exponential:
        meshopt_decodeFilterExp(decoded.data(), static_cast<size_t>(count), static_cast<size_t>(stride));
        break;
    case MeshoptDecodeFilter::Color:
        meshopt_decodeFilterColor(decoded.data(), static_cast<size_t>(count), static_cast<size_t>(stride));
        break;
    }
    return decoded;
}

} // namespace import_worker
