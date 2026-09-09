// Stage 3 (bounded Draco geometry decode) unit tests -- exercise
// import_worker::DecodeDracoMesh directly (no sandboxed worker/pipe
// plumbing needed; GltfImportTests.cpp's draco_triangle.glb/
// draco_position_only.glb cases separately prove the real end-to-end
// worker path through the same function).

#include "DracoDecodeAdapter.h"

#include <catch2/catch_test_macros.hpp>

#include <draco/compression/encode.h>
#include <draco/mesh/triangle_soup_mesh_builder.h>

#include <cstring>
#include <vector>

namespace {

// Encodes a single non-degenerate triangle with position+normal, fixed
// attribute unique IDs (POSITION=0, NORMAL=1) matching the convention
// import-worker/src/GltfAdapter.cpp relies on
// (fastgltf::DracoCompressedPrimitive::attributes[...].accessorIndex is the
// draco-internal unique attribute id, confirmed by reading fastgltf's own
// parser source this session).
std::vector<std::byte> EncodeOneTriangle()
{
    draco::TriangleSoupMeshBuilder builder;
    builder.Start(1);
    int posId = builder.AddAttribute(draco::GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
    builder.SetAttributeUniqueId(posId, 0);
    int normalId = builder.AddAttribute(draco::GeometryAttribute::NORMAL, 3, draco::DT_FLOAT32);
    builder.SetAttributeUniqueId(normalId, 1);

    float p0[3] = { 0.0f, 0.0f, 0.0f };
    float p1[3] = { 1.0f, 0.0f, 0.0f };
    float p2[3] = { 0.0f, 1.0f, 0.0f };
    builder.SetAttributeValuesForFace(posId, draco::FaceIndex(0), p0, p1, p2);
    float n[3] = { 0.0f, 0.0f, 1.0f };
    builder.SetAttributeValuesForFace(normalId, draco::FaceIndex(0), n, n, n);

    auto mesh = builder.Finalize();
    if (!mesh) {
        return {};
    }

    draco::Encoder encoder;
    encoder.SetAttributeQuantization(draco::GeometryAttribute::POSITION, 26);
    encoder.SetAttributeQuantization(draco::GeometryAttribute::NORMAL, 16);
    draco::EncoderBuffer buffer;
    if (!encoder.EncodeMeshToBuffer(*mesh, &buffer).ok()) {
        return {};
    }
    std::vector<std::byte> bytes(buffer.size());
    std::memcpy(bytes.data(), buffer.data(), buffer.size());
    return bytes;
}

} // namespace

TEST_CASE("DecodeDracoMesh decodes a valid one-triangle bitstream with matching expected counts",
          "[draco]")
{
    auto encoded = EncodeOneTriangle();
    REQUIRE_FALSE(encoded.empty());

    import_worker::DracoAttributeIds ids;
    ids.position = 0;
    ids.normal = 1;

    auto result = import_worker::DecodeDracoMesh(encoded, ids, /*expectedVertexCount=*/3,
                                                  /*expectedIndexCount=*/3);
    REQUIRE(std::holds_alternative<import_worker::DracoDecodedMesh>(result));
    const auto& mesh = std::get<import_worker::DracoDecodedMesh>(result);
    CHECK(mesh.positions.size() == 9);
    CHECK(mesh.indices.size() == 3);
    REQUIRE(mesh.normals.has_value());
    CHECK(mesh.normals->size() == 9);
    CHECK_FALSE(mesh.uv0.has_value());
    for (uint32_t idx : mesh.indices) {
        CHECK(idx < 3);
    }
}

TEST_CASE("DecodeDracoMesh rejects a decoded vertex count that disagrees with the caller's expected count",
          "[draco]")
{
    auto encoded = EncodeOneTriangle();
    REQUIRE_FALSE(encoded.empty());

    import_worker::DracoAttributeIds ids;
    ids.position = 0;
    ids.normal = 1;

    // The real primitive's accessor claims 4 vertices; the bitstream only
    // decodes to 3 -- a hostile-bitstream-vs-accessor mismatch.
    auto result = import_worker::DecodeDracoMesh(encoded, ids, /*expectedVertexCount=*/4,
                                                  /*expectedIndexCount=*/3);
    REQUIRE(std::holds_alternative<model_core::ImportErrorCode>(result));
    CHECK(std::get<model_core::ImportErrorCode>(result) == model_core::ImportErrorCode::MalformedData);
}

TEST_CASE("DecodeDracoMesh rejects a corrupt/garbage bitstream as MalformedData, never a crash",
          "[draco]")
{
    std::vector<std::byte> garbage(64, std::byte{ 0x5A });

    import_worker::DracoAttributeIds ids;
    ids.position = 0;

    auto result = import_worker::DecodeDracoMesh(garbage, ids, 3, 3);
    REQUIRE(std::holds_alternative<model_core::ImportErrorCode>(result));
    CHECK(std::get<model_core::ImportErrorCode>(result) == model_core::ImportErrorCode::MalformedData);
}

TEST_CASE("DecodeDracoMesh rejects when the caller's DracoAttributeIds has no position",
          "[draco]")
{
    auto encoded = EncodeOneTriangle();
    REQUIRE_FALSE(encoded.empty());

    import_worker::DracoAttributeIds ids; // position left empty -- caller error
    auto result = import_worker::DecodeDracoMesh(encoded, ids, 3, 3);
    REQUIRE(std::holds_alternative<model_core::ImportErrorCode>(result));
    CHECK(std::get<model_core::ImportErrorCode>(result) == model_core::ImportErrorCode::MalformedData);
}
