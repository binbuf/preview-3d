#include "DracoDecodeAdapter.h"

#include "platform/CheckedMath.h"

#include <draco/compression/decode.h>
#include <draco/mesh/mesh.h>

#include <array>
#include <cstring>

namespace import_worker {

namespace {

using model_core::ImportErrorCode;
using platform::CheckedAdd;
using platform::CheckedMultiply;

// .docs/design/03-file-formats-and-ingestion.md: "A primitive whose decoded
// working set would exceed 512 MiB or 10 million triangles fails with
// ResourceLimit."
constexpr uint64_t kMaxDracoDecodedWorkingSetBytes = 512ull * 1024ull * 1024ull;
constexpr uint64_t kMaxDracoTriangles = 10'000'000;

} // namespace

std::variant<DracoDecodedMesh, ImportErrorCode> DecodeDracoMesh(
    std::span<const std::byte> compressedBufferViewBytes, const DracoAttributeIds& attributeIds,
    size_t expectedVertexCount, size_t expectedIndexCount)
{
    if (!attributeIds.position.has_value()) {
        return ImportErrorCode::MalformedData; // POSITION is required geometry
    }

    // Cheap pre-check, before ever calling into draco: reject anything not
    // even starting with the format's own 5-byte "DRACO" magic. draco's own
    // DecodeHeader performs the identical bounds-checked comparison
    // internally (confirmed by reading point_cloud_decoder.cc directly),
    // so this doesn't change what's accepted -- it only guarantees a
    // non-Draco buffer is rejected without invoking the third-party
    // decoder at all, matching this repo's "never trust a library's own
    // checks alone" discipline one step further upstream.
    constexpr char kDracoMagic[] = "DRACO";
    if (compressedBufferViewBytes.size() < 5
        || std::memcmp(compressedBufferViewBytes.data(), kDracoMagic, 5) != 0) {
        return ImportErrorCode::MalformedData;
    }

    draco::DecoderBuffer buffer;
    buffer.Init(reinterpret_cast<const char*>(compressedBufferViewBytes.data()),
                compressedBufferViewBytes.size());

    draco::Decoder decoder;
    draco::StatusOr<std::unique_ptr<draco::Mesh>> statusOrMesh = decoder.DecodeMeshFromBuffer(&buffer);
    if (!statusOrMesh.ok()) {
        return ImportErrorCode::MalformedData;
    }
    std::unique_ptr<draco::Mesh> mesh = std::move(statusOrMesh).value();
    if (mesh == nullptr) {
        return ImportErrorCode::MalformedData;
    }

    const size_t pointCount = static_cast<size_t>(mesh->num_points());
    const size_t faceCount = static_cast<size_t>(mesh->num_faces());

    // Bound check BEFORE any attribute data is copied out, so an
    // oversized/mismatched hostile bitstream is caught cheaply.
    if (faceCount > kMaxDracoTriangles) {
        return ImportErrorCode::ResourceLimit;
    }
    if (pointCount != expectedVertexCount || faceCount * 3 != expectedIndexCount) {
        return ImportErrorCode::MalformedData;
    }

    const bool hasNormal = attributeIds.normal.has_value();
    const bool hasUv0 = attributeIds.uv0.has_value();
    uint64_t bytesPerPoint = sizeof(float) * 3; // position
    if (hasNormal) {
        bytesPerPoint += sizeof(float) * 3;
    }
    if (hasUv0) {
        bytesPerPoint += sizeof(float) * 2;
    }
    auto pointBytes = CheckedMultiply(static_cast<uint64_t>(pointCount), bytesPerPoint);
    auto indexBytes = CheckedMultiply(static_cast<uint64_t>(faceCount) * 3, static_cast<uint64_t>(sizeof(uint32_t)));
    auto workingSetBytes = (pointBytes && indexBytes) ? CheckedAdd(*pointBytes, *indexBytes) : std::nullopt;
    if (!workingSetBytes || *workingSetBytes > kMaxDracoDecodedWorkingSetBytes) {
        return ImportErrorCode::ResourceLimit;
    }

    const draco::PointAttribute* positionAttr = mesh->GetAttributeByUniqueId(*attributeIds.position);
    if (positionAttr == nullptr) {
        return ImportErrorCode::MalformedData;
    }
    const draco::PointAttribute* normalAttr
        = hasNormal ? mesh->GetAttributeByUniqueId(*attributeIds.normal) : nullptr;
    if (hasNormal && normalAttr == nullptr) {
        return ImportErrorCode::MalformedData;
    }
    const draco::PointAttribute* uvAttr = hasUv0 ? mesh->GetAttributeByUniqueId(*attributeIds.uv0) : nullptr;
    if (hasUv0 && uvAttr == nullptr) {
        return ImportErrorCode::MalformedData;
    }

    DracoDecodedMesh result;
    result.positions.resize(pointCount * 3);
    if (hasNormal) {
        result.normals = std::vector<float>(pointCount * 3);
    }
    if (hasUv0) {
        result.uv0 = std::vector<float>(pointCount * 2);
    }

    for (size_t p = 0; p < pointCount; ++p) {
        const draco::PointIndex pointIndex(static_cast<uint32_t>(p));

        std::array<float, 3> position{};
        if (!positionAttr->GetValue<float, 3>(positionAttr->mapped_index(pointIndex), &position)) {
            return ImportErrorCode::MalformedData;
        }
        result.positions[p * 3 + 0] = position[0];
        result.positions[p * 3 + 1] = position[1];
        result.positions[p * 3 + 2] = position[2];

        if (hasNormal) {
            std::array<float, 3> normal{};
            if (!normalAttr->GetValue<float, 3>(normalAttr->mapped_index(pointIndex), &normal)) {
                return ImportErrorCode::MalformedData;
            }
            (*result.normals)[p * 3 + 0] = normal[0];
            (*result.normals)[p * 3 + 1] = normal[1];
            (*result.normals)[p * 3 + 2] = normal[2];
        }

        if (hasUv0) {
            std::array<float, 2> uv{};
            if (!uvAttr->GetValue<float, 2>(uvAttr->mapped_index(pointIndex), &uv)) {
                return ImportErrorCode::MalformedData;
            }
            (*result.uv0)[p * 2 + 0] = uv[0];
            (*result.uv0)[p * 2 + 1] = uv[1];
        }
    }

    result.indices.resize(faceCount * 3);
    for (size_t f = 0; f < faceCount; ++f) {
        const draco::Mesh::Face& face = mesh->face(draco::FaceIndex(static_cast<uint32_t>(f)));
        for (int corner = 0; corner < 3; ++corner) {
            // draco::PointIndex::ValueType is uint32_t -- no negative case to guard.
            const uint32_t pointValue = face[corner].value();
            if (static_cast<size_t>(pointValue) >= pointCount) {
                return ImportErrorCode::MalformedData;
            }
            result.indices[f * 3 + corner] = pointValue;
        }
    }

    return result;
}

} // namespace import_worker
