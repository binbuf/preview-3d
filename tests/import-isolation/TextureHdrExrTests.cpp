#define NOMINMAX
#include "EnvironmentAssetDecode.h"
#include "ImageFormatSniff.h"
#include "OpenExrDecode.h"
#include "RadianceHdrDecode.h"
#include "SandboxTestSupport.h"
#include "import_broker/ImportSession.h"
#include "import_broker/SharedSection.h"
#include "model_core/PixelFormats.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

using namespace model_core;
using namespace import_worker;

namespace {

std::filesystem::path FixtureRoot()
{
    return std::filesystem::path(PREVIEW3D_REPO_ROOT) / L"tests" / L"fixtures" / L"textures";
}

std::vector<std::byte> ReadFixture(const std::wstring& relative)
{
    const auto path = FixtureRoot() / relative;
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    REQUIRE(size >= 0);
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(input.good());
    return bytes;
}

import_broker::ImportSessionRequest Request(const std::filesystem::path& path,
                                            import_broker::ImportFormat format, uint64_t generation)
{
    import_broker::ImportSessionRequest request;
    request.workerExePath = sandbox_test_support::WorkerExePath();
    request.sourcePath = path.wstring();
    request.format = format;
    request.generationId = generation;
    request.sectionByteCapacity = import_broker::kImportSectionBytes;
    request.maxChunkCount = 1024;
    request.maxChunkBatchesPerGeneration = 64;
    request.maxChunksPerGeneration = 4096;
    request.maxSidecarRequestsPerGeneration = 64;
    request.maxSidecarFileBytes = 256ull * 1024 * 1024;
    return request;
}

std::byte ByteAt(const std::vector<std::byte>& bytes, size_t index)
{
    REQUIRE(index < bytes.size());
    return bytes[index];
}

} // namespace

TEST_CASE("Radiance HDR color decodes linearly and tonemaps to sRGB RGBA8", "[texture-hdr]")
{
    const auto bytes = ReadFixture(L"hdr_color.hdr");
    CHECK(SniffImageFormat(bytes) == SniffedImageFormat::Hdr);
    CHECK(HasDecodableImageExtension("env/studio.HDR"));
    CHECK(HasDecodableImageExtension("tex/a.hdr:stream"));

    auto decoded = DecodeRadianceHdrImage(bytes, ColorSpaceId::Srgb, {});
    REQUIRE(decoded);
    CHECK(decoded->pixelFormat == PixelFormatId::RGBA8_UNORM);
    CHECK(decoded->width == 8);
    CHECK(decoded->height == 8);
    CHECK(decoded->mipLevels == 4);
    CHECK(decoded->pixelBytes.size() == *ComputeImagePixelBytes(PixelFormatId::RGBA8_UNORM, 8, 8, 4));
    // New-style RLE scanline: (0,0) is 4.0/0.5/0.25 linear. Reinhard gives
    // 0.8/0.3333/0.2, then sRGB encodes to 231/156/124.
    CHECK(ByteAt(decoded->pixelBytes, 0) == std::byte{231});
    CHECK(ByteAt(decoded->pixelBytes, 1) == std::byte{156});
    CHECK(ByteAt(decoded->pixelBytes, 2) == std::byte{124});
    CHECK(ByteAt(decoded->pixelBytes, 3) == std::byte{255});

    // Flat scanline variant (width < 8 uses the legacy flat form).
    const auto flat = ReadFixture(L"hdr_flat.hdr");
    auto linear = DecodeRadianceHdrImage(flat, ColorSpaceId::Linear, {});
    REQUIRE(linear);
    CHECK(linear->width == 4);
    CHECK(linear->height == 4);
    // Data slot: clamp only, no Reinhard curve. 4.0 -> 255, 0.5 -> 128, 0.25 -> 64.
    TextureDecodeOptions dataOptions;
    dataOptions.semantic = TextureSemantic::Data;
    auto data = DecodeRadianceHdrImage(flat, ColorSpaceId::Linear, dataOptions);
    REQUIRE(data);
    CHECK(ByteAt(data->pixelBytes, 0) == std::byte{255});
    CHECK(ByteAt(data->pixelBytes, 1) == std::byte{128});
    CHECK(ByteAt(data->pixelBytes, 2) == std::byte{64});
}

TEST_CASE("Radiance HDR rejects malformed and over-budget files", "[texture-hdr]")
{
    TextureDecodeOptions options;
    CHECK_FALSE(DecodeRadianceHdrImage(ReadFixture(L"hdr_bad_magic.hdr"), ColorSpaceId::Srgb, options));
    CHECK_FALSE(DecodeRadianceHdrImage(ReadFixture(L"hdr_bad_format.hdr"), ColorSpaceId::Srgb, options));
    CHECK_FALSE(DecodeRadianceHdrImage(ReadFixture(L"hdr_bad_resolution.hdr"), ColorSpaceId::Srgb, options));
    CHECK_FALSE(DecodeRadianceHdrImage(ReadFixture(L"hdr_truncated.hdr"), ColorSpaceId::Srgb, options));
    CHECK(SniffImageFormat(ReadFixture(L"hdr_bad_magic.hdr")) == SniffedImageFormat::Unknown);

    const auto good = ReadFixture(L"hdr_color.hdr");
    options.maxEncodedBytes = good.size() - 1;
    CHECK_FALSE(DecodeRadianceHdrImage(good, ColorSpaceId::Srgb, options));
    options = {};
    options.maxDecodedBytes = 16;
    CHECK_FALSE(DecodeRadianceHdrImage(good, ColorSpaceId::Srgb, options));
    options = {};
    options.maxPixels = 8;
    CHECK_FALSE(DecodeRadianceHdrImage(good, ColorSpaceId::Srgb, options));
    options = {};
    options.isCancelled = [] { return true; };
    CHECK_FALSE(DecodeRadianceHdrImage(good, ColorSpaceId::Srgb, options));

    auto shrink = TextureDecodeOptions{};
    shrink.maxDimension = 2;
    auto reduced = DecodeRadianceHdrImage(good, ColorSpaceId::Linear, shrink);
    REQUIRE(reduced);
    CHECK(reduced->width == 2);
    CHECK(reduced->height == 2);
    CHECK(reduced->mipLevels == 2);
}

TEST_CASE("Radiance HDR environment hook returns bounded linear equirect data", "[texture-hdr]")
{
    EnvironmentAssetOptions options;
    auto asset = LoadEnvironmentAsset(ReadFixture(L"hdr_color.hdr"), options);
    REQUIRE(asset);
    CHECK(asset->Valid());
    CHECK(asset->width == 8);
    CHECK(asset->height == 8);
    CHECK(asset->rgba.size() == 8u * 8u * 4u);
    // No tonemap and no sRGB transfer on the environment path.
    CHECK(asset->rgba[0] > 3.9f);
    CHECK(asset->rgba[0] < 4.1f);
    CHECK(asset->rgba[1] > 0.49f);
    CHECK(asset->rgba[1] < 0.51f);
    CHECK(asset->rgba[2] > 0.24f);
    CHECK(asset->rgba[2] < 0.26f);
    CHECK(asset->rgba[3] == 1.0f);

    options.maxDimension = 2;
    auto reduced = LoadEnvironmentAsset(ReadFixture(L"hdr_color.hdr"), options);
    REQUIRE(reduced);
    CHECK(reduced->width == 2);
    CHECK(reduced->height == 2);

    options = {};
    options.maxPixels = 4;
    CHECK_FALSE(LoadEnvironmentAsset(ReadFixture(L"hdr_color.hdr"), options));
    options = {};
    options.isCancelled = [] { return true; };
    CHECK_FALSE(LoadEnvironmentAsset(ReadFixture(L"hdr_color.hdr"), options));
    options = {};
    CHECK_FALSE(LoadEnvironmentAsset(ReadFixture(L"tga24_uncompressed.tga"), options));
    CHECK_FALSE(LoadEnvironmentAsset({}, options));
}

TEST_CASE("OpenEXR half/float color decodes and keeps data maps linear", "[texture-exr]")
{
    const auto color = ReadFixture(L"exr_color.exr");
    CHECK(SniffImageFormat(color) == SniffedImageFormat::Exr);
    CHECK(HasDecodableImageExtension("tex/a.EXR"));
    CHECK(HasDecodableImageExtension("tex/a.exr:stream"));

    auto decoded = DecodeOpenExrImage(color, ColorSpaceId::Srgb, {});
    REQUIRE(decoded);
    CHECK(decoded->pixelFormat == PixelFormatId::RGBA8_UNORM);
    CHECK(decoded->width == 4);
    CHECK(decoded->height == 4);
    CHECK(decoded->mipLevels == 3);
    CHECK(ByteAt(decoded->pixelBytes, 0) == std::byte{231});
    CHECK(ByteAt(decoded->pixelBytes, 1) == std::byte{156});
    CHECK(ByteAt(decoded->pixelBytes, 2) == std::byte{124});
    CHECK(ByteAt(decoded->pixelBytes, 3) == std::byte{255});

    // FLOAT channels and alpha, uncompressed scanline.
    auto rgba = DecodeOpenExrImage(ReadFixture(L"exr_float_rgba.exr"), ColorSpaceId::Srgb, {});
    REQUIRE(rgba);
    CHECK(ByteAt(rgba->pixelBytes, 0) == std::byte{213});
    CHECK(ByteAt(rgba->pixelBytes, 1) == std::byte{156});
    CHECK(ByteAt(rgba->pixelBytes, 2) == std::byte{124});
    CHECK(ByteAt(rgba->pixelBytes, 3) == std::byte{128});

    // Normal map: linear, no Reinhard/sRGB, center normal preserved.
    TextureDecodeOptions normalOptions;
    normalOptions.semantic = TextureSemantic::Normal;
    auto normal = DecodeOpenExrImage(ReadFixture(L"exr_normal.exr"), ColorSpaceId::Linear, normalOptions);
    REQUIRE(normal);
    CHECK(ByteAt(normal->pixelBytes, 0) == std::byte{128});
    CHECK(ByteAt(normal->pixelBytes, 1) == std::byte{128});
    CHECK(ByteAt(normal->pixelBytes, 2) == std::byte{255});

    // Single-channel roughness map is replicated to RGB.
    TextureDecodeOptions dataOptions;
    dataOptions.semantic = TextureSemantic::Data;
    auto roughness = DecodeOpenExrImage(ReadFixture(L"exr_roughness.exr"), ColorSpaceId::Linear,
                                        dataOptions);
    REQUIRE(roughness);
    CHECK(ByteAt(roughness->pixelBytes, 0) == std::byte{64});
    CHECK(ByteAt(roughness->pixelBytes, 1) == std::byte{64});
    CHECK(ByteAt(roughness->pixelBytes, 2) == std::byte{64});
}

TEST_CASE("OpenEXR rejects tiled, deep, multipart and unsupported forms", "[texture-exr]")
{
    TextureDecodeOptions options;
    CHECK_FALSE(DecodeOpenExrImage(ReadFixture(L"exr_tiled.exr"), ColorSpaceId::Srgb, options));
    CHECK_FALSE(DecodeOpenExrImage(ReadFixture(L"exr_deep.exr"), ColorSpaceId::Srgb, options));
    CHECK_FALSE(DecodeOpenExrImage(ReadFixture(L"exr_multipart.exr"), ColorSpaceId::Srgb, options));
    CHECK_FALSE(DecodeOpenExrImage(ReadFixture(L"exr_piz.exr"), ColorSpaceId::Srgb, options));
    CHECK_FALSE(DecodeOpenExrImage(ReadFixture(L"exr_uint_channel.exr"), ColorSpaceId::Srgb, options));
    CHECK_FALSE(DecodeOpenExrImage(ReadFixture(L"exr_truncated.exr"), ColorSpaceId::Srgb, options));

    const auto good = ReadFixture(L"exr_color.exr");
    options.maxEncodedBytes = good.size() - 1;
    CHECK_FALSE(DecodeOpenExrImage(good, ColorSpaceId::Srgb, options));
    options = {};
    options.maxDecodedBytes = 16;
    CHECK_FALSE(DecodeOpenExrImage(good, ColorSpaceId::Srgb, options));
    options = {};
    options.maxPixels = 8;
    CHECK_FALSE(DecodeOpenExrImage(good, ColorSpaceId::Srgb, options));
    options = {};
    options.isCancelled = [] { return true; };
    CHECK_FALSE(DecodeOpenExrImage(good, ColorSpaceId::Srgb, options));

    auto shrink = TextureDecodeOptions{};
    shrink.maxDimension = 2;
    auto reduced = DecodeOpenExrImage(good, ColorSpaceId::Linear, shrink);
    REQUIRE(reduced);
    CHECK(reduced->width == 2);
    CHECK(reduced->height == 2);
    CHECK(reduced->mipLevels == 2);
}

TEST_CASE("OpenEXR environment hook returns bounded linear equirect data", "[texture-exr]")
{
    EnvironmentAssetOptions options;
    auto asset = LoadEnvironmentAsset(ReadFixture(L"exr_color.exr"), options);
    REQUIRE(asset);
    CHECK(asset->Valid());
    CHECK(asset->width == 4);
    CHECK(asset->height == 4);
    CHECK(asset->rgba.size() == 4u * 4u * 4u);
    CHECK(asset->rgba[0] > 3.9f);

    options.maxDecodedBytes = 16;
    CHECK_FALSE(LoadEnvironmentAsset(ReadFixture(L"exr_color.exr"), options));
    options = {};
    options.maxDimension = 2;
    auto reduced = LoadEnvironmentAsset(ReadFixture(L"exr_color.exr"), options);
    REQUIRE(reduced);
    CHECK(reduced->width == 2);
    CHECK(reduced->height == 2);
    options = {};
    options.isCancelled = [] { return true; };
    CHECK_FALSE(LoadEnvironmentAsset(ReadFixture(L"exr_color.exr"), options));
}

TEST_CASE("An OBJ package uses EXR base colour and normal maps without warnings",
          "[texture-exr]")
{
    const auto result = import_broker::RunImportSession(
        Request(FixtureRoot() / L"exr" / L"scene.obj", import_broker::ImportFormat::Obj, 1301));
    CAPTURE(uint32_t(result.stage), uint32_t(result.errorCode));
    REQUIRE(result.ok);
    unsigned images = 0, warnings = 0;
    bool color = false, normal = false;
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology == ChunkTopology::TextureWarning) ++warnings;
        if (chunk.descriptor.topology != ChunkTopology::Image) continue;
        ++images;
        ImagePayloadHeader header{};
        REQUIRE(chunk.payload.size() >= sizeof(header));
        std::memcpy(&header, chunk.payload.data(), sizeof(header));
        CHECK(header.pixelFormat == uint32_t(PixelFormatId::RGBA8_UNORM));
        const auto* pixels = chunk.payload.data() + sizeof(header);
        color |= pixels[0] == std::byte{231} && pixels[1] == std::byte{156};
        normal |= pixels[0] == std::byte{128} && pixels[1] == std::byte{128}
            && pixels[2] == std::byte{255};
    }
    CHECK(images >= 2);
    CHECK(color);
    CHECK(normal);
    CHECK(warnings == 0);
}

TEST_CASE("A glTF package uses EXR sidecar base colour and normal maps",
          "[texture-exr]")
{
    const auto result = import_broker::RunImportSession(
        Request(FixtureRoot() / L"exr-gltf" / L"scene.gltf", import_broker::ImportFormat::Gltf, 1303));
    CAPTURE(uint32_t(result.stage), uint32_t(result.errorCode));
    REQUIRE(result.ok);
    unsigned images = 0, warnings = 0;
    bool color = false, normal = false;
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology == ChunkTopology::TextureWarning) ++warnings;
        if (chunk.descriptor.topology != ChunkTopology::Image) continue;
        ++images;
        ImagePayloadHeader header{};
        REQUIRE(chunk.payload.size() >= sizeof(header));
        std::memcpy(&header, chunk.payload.data(), sizeof(header));
        CHECK(header.pixelFormat == uint32_t(PixelFormatId::RGBA8_UNORM));
        const auto* pixels = chunk.payload.data() + sizeof(header);
        color |= pixels[0] == std::byte{231} && pixels[1] == std::byte{156};
        normal |= pixels[0] == std::byte{128} && pixels[1] == std::byte{128}
            && pixels[2] == std::byte{255};
    }
    CHECK(images >= 2);
    CHECK(color);
    CHECK(normal);
    CHECK(warnings == 0);
}

TEST_CASE("A malformed EXR texture falls back with a warning, not a model failure",
          "[texture-exr]")
{
    const auto result = import_broker::RunImportSession(
        Request(FixtureRoot() / L"bad-exr" / L"scene.obj", import_broker::ImportFormat::Obj, 1302));
    REQUIRE(result.ok);
    bool geometry = false;
    unsigned warnings = 0;
    for (const auto& chunk : result.chunks) {
        geometry |= chunk.descriptor.topology == ChunkTopology::TriangleList;
        if (chunk.descriptor.topology == ChunkTopology::TextureWarning) ++warnings;
    }
    CHECK(geometry);
    CHECK(warnings == 1);
}
