#pragma once

// Bounded Draco geometry decode for a KHR_draco_mesh_compression primitive.
// Deliberately decoupled from fastgltf (GltfAdapter.cpp resolves the
// compressed bufferView bytes and draco-internal attribute IDs; this file
// only wraps the real draco::Decoder call) so it's independently testable.
//
// Bound per .docs/design/03-file-formats-and-ingestion.md: "A primitive
// whose decoded working set would exceed 512 MiB or 10 million triangles
// fails with ResourceLimit." Enforced before any decoded attribute data is
// copied out of the draco::Mesh.

#include "model_core/ImportError.h"

#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace import_worker {

// Draco-internal unique attribute IDs for this primitive, from
// fastgltf::DracoCompressedPrimitive::attributes (the JSON "attributes"
// object inside the KHR_draco_mesh_compression extension -- confirmed by
// reading fastgltf's own parser source, not just its header: it stores the
// draco unique attribute id directly into Attribute::accessorIndex, a
// repurposing of that field name specific to this extension).
struct DracoAttributeIds {
    std::optional<uint32_t> position; // required by the caller; absence is the caller's error to reject
    std::optional<uint32_t> normal;
    std::optional<uint32_t> uv0;
};

struct DracoDecodedMesh {
    std::vector<float> positions;                // xyz triplets, size == 3 * pointCount
    std::optional<std::vector<float>> normals;    // xyz triplets, size == 3 * pointCount, if attributeIds.normal was set
    std::optional<std::vector<float>> uv0;        // uv pairs, size == 2 * pointCount, if attributeIds.uv0 was set
    std::vector<uint32_t> indices;                // triangle list, size == 3 * faceCount
};

// compressedBufferViewBytes: the raw bytes of the primitive's
// KHR_draco_mesh_compression bufferView (the compressed Draco bitstream).
// expectedVertexCount/expectedIndexCount: from the primitive's regular
// (uncompressed) accessors, which the extension requires stay correct even
// though their bufferViews are unused -- the decoded point/face*3 counts
// are cross-checked against these and rejected on mismatch, since a hostile
// bitstream's self-described counts need not match the accessor's claim.
std::variant<DracoDecodedMesh, model_core::ImportErrorCode> DecodeDracoMesh(
    std::span<const std::byte> compressedBufferViewBytes, const DracoAttributeIds& attributeIds,
    size_t expectedVertexCount, size_t expectedIndexCount);

} // namespace import_worker
