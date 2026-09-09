#include "framework.h"

#include <ktx.h>

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
    AppendU32(out, 0x46546C67);
    AppendU32(out, 2);
    AppendU32(out, static_cast<std::uint32_t>(12 + 8 + jsonBytes.size() + 8 + bin.size()));
    AppendU32(out, static_cast<std::uint32_t>(jsonBytes.size()));
    AppendU32(out, 0x4E4F534A);
    out.insert(out.end(), jsonBytes.begin(), jsonBytes.end());
    AppendU32(out, static_cast<std::uint32_t>(bin.size()));
    AppendU32(out, 0x004E4942);
    out.insert(out.end(), bin.begin(), bin.end());

    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"wb") != 0 || !file) { wprintf(L"cannot write %s\n", path); return; }
    fwrite(out.data(), 1, out.size(), file);
    fclose(file);
    wprintf(L"wrote %s (%zu bytes)\n", path, out.size());
}

void WriteFile(const wchar_t* path, const std::vector<std::uint8_t>& bytes)
{
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"wb") != 0 || !file) { wprintf(L"cannot write %s\n", path); return; }
    fwrite(bytes.data(), 1, bytes.size(), file);
    fclose(file);
    wprintf(L"wrote %s (%zu bytes)\n", path, bytes.size());
}

// Encodes a tiny (8x8, RGBA8, solid-color-per-quadrant) texture as a real
// Basis-Universal-compressed KTX2 container. VK_FORMAT_R8G8B8A8_UNORM's
// numeric value (37, stable per the Vulkan spec) is used directly rather
// than pulling in Vulkan headers -- this port depends only on
// opengl-registry, confirmed by reading ktx.h's own #includes.
std::vector<std::uint8_t> EncodeKtx2Basis(uint32_t width, uint32_t height)
{
    constexpr uint32_t kVkFormatR8G8B8A8Unorm = 37;

    std::vector<std::uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            size_t i = (static_cast<size_t>(y) * width + x) * 4;
            bool leftHalf = x < width / 2;
            bool topHalf = y < height / 2;
            rgba[i + 0] = leftHalf ? 220 : 40;
            rgba[i + 1] = topHalf ? 220 : 40;
            rgba[i + 2] = 128;
            rgba[i + 3] = 255;
        }
    }

    ktxTextureCreateInfo createInfo{};
    createInfo.vkFormat = kVkFormatR8G8B8A8Unorm;
    createInfo.baseWidth = width;
    createInfo.baseHeight = height;
    createInfo.baseDepth = 1;
    createInfo.numDimensions = 2;
    createInfo.numLevels = 1;
    createInfo.numLayers = 1;
    createInfo.numFaces = 1;
    createInfo.isArray = KTX_FALSE;
    createInfo.generateMipmaps = KTX_FALSE;

    ktxTexture2* texture = nullptr;
    if (ktxTexture2_Create(&createInfo, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture) != KTX_SUCCESS
        || texture == nullptr) {
        wprintf(L"ktxTexture2_Create failed\n");
        return {};
    }

    KTX_error_code setResult
        = ktxTexture_SetImageFromMemory(ktxTexture(texture), 0, 0, 0, rgba.data(), rgba.size());
    if (setResult != KTX_SUCCESS) {
        wprintf(L"ktxTexture_SetImageFromMemory failed: %d\n", static_cast<int>(setResult));
        ktxTexture2_Destroy(texture);
        return {};
    }

    // Simple variant (fixed quality, ETC1S base) rather than CompressBasisEx
    // -- this is test-fixture generation, not a production encode path, no
    // need for the full ktxBasisParams tuning surface.
    KTX_error_code compressResult = ktxTexture2_CompressBasis(texture, /*quality=*/128);
    if (compressResult != KTX_SUCCESS) {
        wprintf(L"ktxTexture2_CompressBasis failed: %d\n", static_cast<int>(compressResult));
        ktxTexture2_Destroy(texture);
        return {};
    }

    ktx_uint8_t* outBytes = nullptr;
    ktx_size_t outSize = 0;
    KTX_error_code writeResult = ktxTexture_WriteToMemory(ktxTexture(texture), &outBytes, &outSize);
    ktxTexture2_Destroy(texture);
    if (writeResult != KTX_SUCCESS || outBytes == nullptr) {
        wprintf(L"ktxTexture_WriteToMemory failed: %d\n", static_cast<int>(writeResult));
        return {};
    }

    std::vector<std::uint8_t> result(outBytes, outBytes + outSize);
    free(outBytes); // KTX-Software's documented convention: caller frees with plain free().
    return result;
}

// A GLB with one textured triangle: POSITION+TEXCOORD_0 mesh, a material
// whose baseColorTexture uses KHR_texture_basisu pointing at an embedded
// image/ktx2 bufferView holding ktx2Bytes.
void WriteBasisuGlb(const wchar_t* path, const std::vector<std::uint8_t>& ktx2Bytes)
{
    const float tri[3][3] = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    const float uv[3][2] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f } };

    std::vector<std::uint8_t> bin;
    for (const auto& p : tri) { AppendF32(bin, p[0]); AppendF32(bin, p[1]); AppendF32(bin, p[2]); }
    for (const auto& u : uv) { AppendF32(bin, u[0]); AppendF32(bin, u[1]); }
    AppendU32(bin, 0); AppendU32(bin, 1); AppendU32(bin, 2);
    uint32_t geometryBytes = static_cast<uint32_t>(bin.size());
    while (bin.size() % 4 != 0) bin.push_back(0);
    uint32_t ktx2Offset = static_cast<uint32_t>(bin.size());
    bin.insert(bin.end(), ktx2Bytes.begin(), ktx2Bytes.end());

    std::string json = "{\"asset\":{\"version\":\"2.0\"},"
        "\"extensionsUsed\":[\"KHR_texture_basisu\"],"
        "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1},\"indices\":2,"
        "\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
        "\"textures\":[{\"extensions\":{\"KHR_texture_basisu\":{\"source\":0}}}],"
        "\"images\":[{\"bufferView\":3,\"mimeType\":\"image/ktx2\"}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":2,\"componentType\":5125,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":12},"
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(ktx2Offset) + ",\"byteLength\":"
        + std::to_string(ktx2Bytes.size()) + "}],"
        "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]}";
    (void)geometryBytes;

    WriteGlb(path, json, bin);
}
} // namespace

int wmain()
{
    auto ktx2 = EncodeKtx2Basis(8, 8);
    if (ktx2.empty()) {
        wprintf(L"KTX2 encode failed -- aborting\n");
        return 1;
    }

    WriteFile(L"D:\\repos\\binbuf\\3d-preview-windows\\interactive-viewer\\test-assets\\basisu_sample.ktx2",
              ktx2);
    WriteBasisuGlb(
        L"D:\\repos\\binbuf\\3d-preview-windows\\interactive-viewer\\test-assets\\basisu_textured_triangle.glb",
        ktx2);

    // basisu_corrupt_ktx2.glb: a valid KHR_texture_basisu reference whose
    // "KTX2" bytes are garbage -- exercises TranscodeKtx2BasisImage's
    // soft-fail path (material keeps its factors, no image dependency,
    // never a hard import failure).
    std::vector<std::uint8_t> garbage(64, 0x11);
    WriteBasisuGlb(
        L"D:\\repos\\binbuf\\3d-preview-windows\\interactive-viewer\\test-assets\\basisu_corrupt_ktx2.glb",
        garbage);

    return 0;
}
