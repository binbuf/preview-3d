#include "framework.h"

#include <draco/compression/encode.h>
#include <draco/mesh/triangle_soup_mesh_builder.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
// Duplicated from gen-test-glbs.cpp rather than shared -- small deliberate
// duplication over cross-tool coupling, matching this repo's established
// precedent (see e.g. StlAdapter/PlyAdapter's separate request structs).
void AppendU32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

void WriteGlb(const wchar_t* path, const std::string& json, const std::vector<std::uint8_t>& binary)
{
    std::vector<std::uint8_t> jsonBytes(json.begin(), json.end());
    while (jsonBytes.size() % 4 != 0) jsonBytes.push_back(' ');
    std::vector<std::uint8_t> bin = binary;
    while (bin.size() % 4 != 0) bin.push_back(0);

    std::vector<std::uint8_t> out;
    AppendU32(out, 0x46546C67);                       // 'glTF'
    AppendU32(out, 2);                                // version
    AppendU32(out, static_cast<std::uint32_t>(12 + 8 + jsonBytes.size() + 8 + bin.size()));
    AppendU32(out, static_cast<std::uint32_t>(jsonBytes.size()));
    AppendU32(out, 0x4E4F534A);                       // 'JSON'
    out.insert(out.end(), jsonBytes.begin(), jsonBytes.end());
    AppendU32(out, static_cast<std::uint32_t>(bin.size()));
    AppendU32(out, 0x004E4942);                       // 'BIN\0'
    out.insert(out.end(), bin.begin(), bin.end());

    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"wb") != 0 || !file) { wprintf(L"cannot write %s\n", path); return; }
    fwrite(out.data(), 1, out.size(), file);
    fclose(file);
    wprintf(L"wrote %s (%zu bytes)\n", path, out.size());
}

// Encodes a triangleCount-triangle strip of unique, non-degenerate
// triangles as a Draco mesh, with fixed attribute unique IDs (POSITION=0,
// NORMAL=1 if withNormal, TEXCOORD_0=2 if withUv) so the caller can emit a
// matching KHR_draco_mesh_compression "attributes" JSON object without
// having to inspect the encoder's own id assignment. Returns the encoded
// bitstream bytes.
std::vector<std::uint8_t> EncodeDracoTriangleStrip(int triangleCount, bool withNormal, bool withUv)
{
    draco::TriangleSoupMeshBuilder builder;
    builder.Start(triangleCount);

    const int positionAttId = builder.AddAttribute(draco::GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
    builder.SetAttributeUniqueId(positionAttId, 0);
    int normalAttId = -1;
    if (withNormal) {
        normalAttId = builder.AddAttribute(draco::GeometryAttribute::NORMAL, 3, draco::DT_FLOAT32);
        builder.SetAttributeUniqueId(normalAttId, 1);
    }
    int uvAttId = -1;
    if (withUv) {
        uvAttId = builder.AddAttribute(draco::GeometryAttribute::TEX_COORD, 2, draco::DT_FLOAT32);
        builder.SetAttributeUniqueId(uvAttId, 2);
    }

    for (int t = 0; t < triangleCount; ++t) {
        // A simple non-degenerate triangle strip along +X, each triangle
        // offset so no two triangles share identical vertex positions --
        // avoids relying on any particular Draco vertex-dedup behavior.
        float base = static_cast<float>(t);
        float p0[3] = { base, 0.0f, 0.0f };
        float p1[3] = { base + 1.0f, 0.0f, 0.0f };
        float p2[3] = { base, 1.0f, 0.0f };
        builder.SetAttributeValuesForFace(positionAttId, draco::FaceIndex(t), p0, p1, p2);

        if (withNormal) {
            float n[3] = { 0.0f, 0.0f, 1.0f };
            builder.SetAttributeValuesForFace(normalAttId, draco::FaceIndex(t), n, n, n);
        }
        if (withUv) {
            float uv0[2] = { 0.0f, 0.0f };
            float uv1[2] = { 1.0f, 0.0f };
            float uv2[2] = { 0.0f, 1.0f };
            builder.SetAttributeValuesForFace(uvAttId, draco::FaceIndex(t), uv0, uv1, uv2);
        }
    }

    std::unique_ptr<draco::Mesh> mesh = builder.Finalize();
    if (!mesh) { wprintf(L"draco mesh build failed\n"); return {}; }

    draco::Encoder encoder;
    // Highest-fidelity lossless-ish quantization (26 bits well exceeds
    // float32's ~24-bit mantissa headroom for these small test coordinates)
    // -- fixture round-trip tests compare decoded values with a small
    // tolerance, not bit-exact, but keeping quantization loss negligible
    // avoids the tolerance needing to be suspiciously wide.
    encoder.SetAttributeQuantization(draco::GeometryAttribute::POSITION, 26);
    if (withNormal) {
        encoder.SetAttributeQuantization(draco::GeometryAttribute::NORMAL, 16);
    }
    if (withUv) {
        encoder.SetAttributeQuantization(draco::GeometryAttribute::TEX_COORD, 16);
    }

    draco::EncoderBuffer buffer;
    draco::Status status = encoder.EncodeMeshToBuffer(*mesh, &buffer);
    if (!status.ok()) {
        wprintf(L"draco encode failed: %hs\n", status.error_msg());
        return {};
    }
    return std::vector<std::uint8_t>(buffer.data(), buffer.data() + buffer.size());
}

// Builds a GLB whose single mesh primitive carries a
// KHR_draco_mesh_compression extension referencing the given compressed
// bytes, plus ordinary (bufferView-less-in-practice, but structurally
// present) accessors declaring the same counts/types the decompressed
// geometry must match -- exactly what the real KHR_draco_mesh_compression
// spec requires and what import-worker/src/GltfAdapter.cpp's Draco path
// relies on for its cross-check.
void WriteDracoGlb(const wchar_t* path, const std::vector<std::uint8_t>& compressed, int triangleCount,
                    bool withNormal, bool withUv)
{
    const int vertexCount = triangleCount * 3;
    const int indexCount = triangleCount * 3;

    std::string attributesJson = "\"POSITION\":0";
    std::string dracoAttributesJson = "\"POSITION\":0";
    if (withNormal) {
        attributesJson += ",\"NORMAL\":1";
        dracoAttributesJson += ",\"NORMAL\":1";
    }
    if (withUv) {
        attributesJson += ",\"TEXCOORD_0\":2";
        dracoAttributesJson += ",\"TEXCOORD_0\":2";
    }

    // bufferView 0 (index 0 in the JSON below) is the Draco-compressed
    // bytes; the accessors reference bufferView 0 too (unused per-spec when
    // draco compression is present -- only their count/type/componentType
    // fields are actually consulted) so no second, real, uncompressed copy
    // of the geometry needs to exist in this fixture.
    std::string json = "{\"asset\":{\"version\":\"2.0\"},"
        "\"extensionsUsed\":[\"KHR_draco_mesh_compression\"],"
        "\"extensionsRequired\":[\"KHR_draco_mesh_compression\"],"
        "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{" + attributesJson + "},\"indices\":"
        + (withNormal ? (withUv ? "3" : "2") : (withUv ? "2" : "1"))
        + ",\"extensions\":{\"KHR_draco_mesh_compression\":{\"bufferView\":0,\"attributes\":{"
        + dracoAttributesJson + "}}}}]}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":" + std::to_string(vertexCount)
        + ",\"type\":\"VEC3\"}";
    if (withNormal) {
        json += ",{\"bufferView\":0,\"componentType\":5126,\"count\":" + std::to_string(vertexCount)
            + ",\"type\":\"VEC3\"}";
    }
    if (withUv) {
        json += ",{\"bufferView\":0,\"componentType\":5126,\"count\":" + std::to_string(vertexCount)
            + ",\"type\":\"VEC2\"}";
    }
    json += ",{\"bufferView\":0,\"componentType\":5125,\"count\":" + std::to_string(indexCount)
        + ",\"type\":\"SCALAR\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" + std::to_string(compressed.size())
        + "}],\"buffers\":[{\"byteLength\":" + std::to_string(compressed.size()) + "}]}";

    WriteGlb(path, json, compressed);
}
} // namespace

int wmain()
{
    // draco_triangle.glb: a valid, small (4-triangle) Draco-compressed
    // mesh with position+normal+uv0 -- the primary round-trip fixture.
    {
        auto compressed = EncodeDracoTriangleStrip(4, true, true);
        if (!compressed.empty()) {
            WriteDracoGlb(
                L"D:\\repos\\binbuf\\3d-preview-windows\\interactive-viewer\\test-assets\\draco_triangle.glb",
                compressed, 4, true, true);
        }
    }

    // draco_position_only.glb: no NORMAL/TEXCOORD_0 at all -- exercises the
    // GenerateFlatNormals fallback path on Draco-decoded geometry.
    {
        auto compressed = EncodeDracoTriangleStrip(2, false, false);
        if (!compressed.empty()) {
            WriteDracoGlb(
                L"D:\\repos\\binbuf\\3d-preview-windows\\interactive-viewer\\test-assets\\draco_position_only.glb",
                compressed, 2, false, false);
        }
    }

    return 0;
}
