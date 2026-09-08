#include "framework.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
void AppendU32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

void AppendF32(std::vector<std::uint8_t>& out, float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    AppendU32(out, bits);
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
}

int wmain()
{
    const float kTriangle[3][3] = { {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f} };

    // GLB 1: tight layout -> exercises bulk memcpy fast paths
    {
        const char* json =
            "{\"asset\":{\"version\":\"2.0\"},\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
            "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
            "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
            "{\"bufferView\":1,\"componentType\":5125,\"count\":3,\"type\":\"SCALAR\"}],"
            "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
            "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":12}],\"buffers\":[{\"byteLength\":48}]}";
        std::vector<std::uint8_t> bin;
        for (const auto& p : kTriangle) { AppendF32(bin, p[0]); AppendF32(bin, p[1]); AppendF32(bin, p[2]); }
        AppendU32(bin, 0); AppendU32(bin, 1); AppendU32(bin, 2);
        WriteGlb(L"D:\\repos\\binbuf\\3d-preview-windows\\interactive-viewer\\test-assets\\tri_tight.glb", json, bin);
    }

    // GLB 2: interleaved strided positions+normals -> exercises strided path
    {
        const char* json =
            "{\"asset\":{\"version\":\"2.0\"},\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
            "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},\"indices\":2}]}],"
            "\"accessors\":[{\"bufferView\":0,\"byteOffset\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
            "{\"bufferView\":0,\"byteOffset\":12,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
            "{\"bufferView\":1,\"componentType\":5125,\"count\":3,\"type\":\"SCALAR\"}],"
            "\"bufferViews\":[{\"buffer\":0,\"byteStride\":24,\"byteLength\":72},"
            "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":12}],\"buffers\":[{\"byteLength\":84}]}";
        std::vector<std::uint8_t> bin;
        for (const auto& p : kTriangle)
        {
            AppendF32(bin, p[0]); AppendF32(bin, p[1]); AppendF32(bin, p[2]);
            AppendF32(bin, 0.0f); AppendF32(bin, 0.0f); AppendF32(bin, 1.0f);
        }
        AppendU32(bin, 0); AppendU32(bin, 1); AppendU32(bin, 2);
        WriteGlb(L"D:\\repos\\binbuf\\3d-preview-windows\\interactive-viewer\\test-assets\\tri_interleaved.glb", json, bin);
    }

    // GLB 3: same triangle as GLB 1, but on a mesh node that is a child of a
    // root node carrying a non-identity transform (translate [2,0,0],
    // rotate 90 deg about +Z) -> exercises node-transform baking in the
    // Gate 3 fastgltf adapter (import-worker/src/GltfAdapter.cpp).
    {
        const char* json =
            "{\"asset\":{\"version\":\"2.0\"},\"scenes\":[{\"nodes\":[0]}],"
            "\"nodes\":[{\"translation\":[2.0,0.0,0.0],\"rotation\":[0.0,0.0,0.7071068,0.7071068],\"children\":[1]},"
            "{\"mesh\":0}],"
            "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
            "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
            "{\"bufferView\":1,\"componentType\":5125,\"count\":3,\"type\":\"SCALAR\"}],"
            "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
            "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":12}],\"buffers\":[{\"byteLength\":48}]}";
        std::vector<std::uint8_t> bin;
        for (const auto& p : kTriangle) { AppendF32(bin, p[0]); AppendF32(bin, p[1]); AppendF32(bin, p[2]); }
        AppendU32(bin, 0); AppendU32(bin, 1); AppendU32(bin, 2);
        WriteGlb(L"D:\\repos\\binbuf\\3d-preview-windows\\interactive-viewer\\test-assets\\tri_transformed_node.glb", json, bin);
    }

    // GLB 4: same triangle as GLB 1, but declares a required extension the
    // Gate 3 fastgltf adapter's Parser(Extensions::None) does not
    // recognize -> exercises the "unsupported required feature" rejection
    // path.
    {
        const char* json =
            "{\"asset\":{\"version\":\"2.0\"},"
            "\"extensionsRequired\":[\"KHR_materials_unlit\"],\"extensionsUsed\":[\"KHR_materials_unlit\"],"
            "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
            "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
            "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
            "{\"bufferView\":1,\"componentType\":5125,\"count\":3,\"type\":\"SCALAR\"}],"
            "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
            "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":12}],\"buffers\":[{\"byteLength\":48}]}";
        std::vector<std::uint8_t> bin;
        for (const auto& p : kTriangle) { AppendF32(bin, p[0]); AppendF32(bin, p[1]); AppendF32(bin, p[2]); }
        AppendU32(bin, 0); AppendU32(bin, 1); AppendU32(bin, 2);
        WriteGlb(L"D:\\repos\\binbuf\\3d-preview-windows\\interactive-viewer\\test-assets\\unsupported_extension.glb", json, bin);
    }
    return 0;
}
