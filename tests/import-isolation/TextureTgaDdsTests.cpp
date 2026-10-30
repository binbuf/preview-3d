#define NOMINMAX
#include "DdsImageDecode.h"
#include "ImageFormatSniff.h"
#include "TgaImageDecode.h"
#include "SandboxTestSupport.h"
#include "import_broker/ImportSession.h"
#include "import_broker/SharedSection.h"
#include "model_core/PixelFormats.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
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

TEST_CASE("TGA true-color uncompressed and RLE decode with origin and alpha", "[texture-tga]")
{
    TextureDecodeOptions options;
    auto plain = DecodeTgaImage(ReadFixture(L"tga24_uncompressed.tga"), ColorSpaceId::Srgb, options);
    REQUIRE(plain);
    CHECK(plain->pixelFormat == PixelFormatId::RGBA8_UNORM);
    CHECK(plain->width == 4);
    CHECK(plain->height == 4);
    CHECK(plain->mipLevels == 3);
    CHECK(plain->pixelBytes.size() == *ComputeImagePixelBytes(PixelFormatId::RGBA8_UNORM, 4, 4, 3));
    CHECK(ByteAt(plain->pixelBytes, 0) == std::byte{0});
    CHECK(ByteAt(plain->pixelBytes, 1) == std::byte{0});
    CHECK(ByteAt(plain->pixelBytes, 2) == std::byte{32});
    CHECK(ByteAt(plain->pixelBytes, 3) == std::byte{255});
    // Bottom-up storage is flipped: decoded row 3 comes from the authored top.
    CHECK(ByteAt(plain->pixelBytes, (3 * 4) * 4 + 1) == std::byte{255});

    auto alpha = DecodeTgaImage(ReadFixture(L"tga32_uncompressed.tga"), ColorSpaceId::Srgb, options);
    REQUIRE(alpha);
    CHECK(alpha->width == 4);
    CHECK(ByteAt(alpha->pixelBytes, 3) == std::byte{63});

    auto rle = DecodeTgaImage(ReadFixture(L"tga24_rle.tga"), ColorSpaceId::Srgb, options);
    REQUIRE(rle);
    CHECK(rle->width == 8);
    CHECK(rle->height == 8);
    CHECK(rle->mipLevels == 4);
    CHECK(ByteAt(rle->pixelBytes, 2) == std::byte{32});
    CHECK(ByteAt(rle->pixelBytes, 3) == std::byte{255});
    TextureDecodeOptions shrink;
    shrink.maxDimension = 2;
    auto reduced = DecodeTgaImage(ReadFixture(L"tga24_rle.tga"), ColorSpaceId::Linear, shrink);
    REQUIRE(reduced);
    CHECK(reduced->width == 2);
    CHECK(reduced->height == 2);
    CHECK(reduced->mipLevels == 2);
    auto rleAlpha = DecodeTgaImage(ReadFixture(L"tga32_rle.tga"), ColorSpaceId::Linear, options);
    REQUIRE(rleAlpha);
    CHECK(ByteAt(rleAlpha->pixelBytes, 3) == std::byte{31});

    CHECK(SniffImageFormat(ReadFixture(L"tga24_uncompressed.tga")) == SniffedImageFormat::Tga);
    CHECK(HasDecodableImageExtension("Textures/albedo.TGA"));
    CHECK(HasDecodableImageExtension("tex/a.dds:stream"));
    CHECK_FALSE(HasDecodableImageExtension("tex/a.exr"));
}

TEST_CASE("TGA malformed forms are rejected typed and observe budgets and cancellation", "[texture-tga]")
{
    const auto good = ReadFixture(L"tga24_rle.tga");
    TextureDecodeOptions options;

    auto palette = good;
    palette[2] = std::byte{1};
    CHECK_FALSE(DecodeTgaImage(palette, ColorSpaceId::Srgb, options));

    auto indexed = good;
    indexed[2] = std::byte{9};
    CHECK_FALSE(DecodeTgaImage(indexed, ColorSpaceId::Srgb, options));

    auto badDepth = good;
    badDepth[16] = std::byte{16};
    CHECK_FALSE(DecodeTgaImage(badDepth, ColorSpaceId::Srgb, options));

    auto interleaved = good;
    interleaved[17] = std::byte{0x40};
    CHECK_FALSE(DecodeTgaImage(interleaved, ColorSpaceId::Srgb, options));

    auto nonzeroOrigin = good;
    nonzeroOrigin[8] = std::byte{1};
    CHECK_FALSE(DecodeTgaImage(nonzeroOrigin, ColorSpaceId::Srgb, options));

    auto truncated = good;
    truncated.resize(truncated.size() - 1);
    CHECK_FALSE(DecodeTgaImage(truncated, ColorSpaceId::Srgb, options));

    auto overrun = good;
    overrun[18] = std::byte{0xFF}; // RLE packet of 128 pixels into a 64-pixel image
    CHECK_FALSE(DecodeTgaImage(overrun, ColorSpaceId::Srgb, options));

    auto hostileDimension = good;
    hostileDimension[12] = std::byte{0xFF};
    hostileDimension[13] = std::byte{0xFF};
    CHECK_FALSE(DecodeTgaImage(hostileDimension, ColorSpaceId::Srgb, options));
    CHECK(SniffImageFormat(hostileDimension) == SniffedImageFormat::Unknown);

    options.maxDecodedBytes = 16;
    CHECK_FALSE(DecodeTgaImage(good, ColorSpaceId::Srgb, options));
    options = {};
    options.maxEncodedBytes = good.size() - 1;
    CHECK_FALSE(DecodeTgaImage(good, ColorSpaceId::Srgb, options));
    options = {};
    options.isCancelled = [] { return true; };
    CHECK_FALSE(DecodeTgaImage(good, ColorSpaceId::Srgb, options));
}

TEST_CASE("DDS BC1/BC3/BC5/BC7 blocks pass through and mip chains are preserved", "[texture-dds]")
{
    struct Fixture {
        const wchar_t* name;
        PixelFormatId format;
    };
    for (const Fixture& fixture : {
             Fixture{L"dds_bc1.dds", PixelFormatId::BC1_UNORM},
             Fixture{L"dds_bc3.dds", PixelFormatId::BC3_UNORM},
             Fixture{L"dds_bc5.dds", PixelFormatId::BC5_UNORM},
             Fixture{L"dds_bc7.dds", PixelFormatId::BC7_UNORM}}) {
        CAPTURE(fixture.name);
        const auto bytes = ReadFixture(fixture.name);
        auto decoded = DecodeDdsImage(bytes, ColorSpaceId::Srgb, {});
        REQUIRE(decoded);
        CHECK(decoded->pixelFormat == fixture.format);
        CHECK(decoded->width == 8);
        CHECK(decoded->height == 8);
        CHECK(decoded->mipLevels == 1);
        CHECK(decoded->pixelBytes.size()
              == *ComputeImagePixelBytes(fixture.format, 8, 8, 1));
        CHECK(SniffImageFormat(bytes) == SniffedImageFormat::Dds);
    }

    const auto mips = ReadFixture(L"dds_bc1_mips.dds");
    auto decoded = DecodeDdsImage(mips, ColorSpaceId::Srgb, {});
    REQUIRE(decoded);
    CHECK(decoded->mipLevels == 4);
    CHECK(decoded->pixelBytes.size() == *ComputeImagePixelBytes(PixelFormatId::BC1_UNORM, 8, 8, 4));
    TextureDecodeOptions shrink;
    shrink.maxDimension = 2;
    decoded = DecodeDdsImage(mips, ColorSpaceId::Srgb, shrink);
    REQUIRE(decoded);
    CHECK(decoded->width == 2);
    CHECK(decoded->height == 2);
    CHECK(decoded->mipLevels == 2);
}

TEST_CASE("DDS uncompressed RGBA8 converts to the closed RGBA format", "[texture-dds]")
{
    const auto bytes = ReadFixture(L"dds_rgba8.dds");
    auto decoded = DecodeDdsImage(bytes, ColorSpaceId::Linear, {});
    REQUIRE(decoded);
    CHECK(decoded->pixelFormat == PixelFormatId::RGBA8_UNORM);
    CHECK(decoded->width == 4);
    CHECK(decoded->height == 4);
    CHECK(decoded->mipLevels == 3);
    CHECK(ByteAt(decoded->pixelBytes, 0) == std::byte{0});
    CHECK(ByteAt(decoded->pixelBytes, 1) == std::byte{0});
    CHECK(ByteAt(decoded->pixelBytes, 2) == std::byte{0});
    CHECK(ByteAt(decoded->pixelBytes, 3) == std::byte{255});
    const size_t at = (1 * 4 + 1) * 4;
    CHECK(ByteAt(decoded->pixelBytes, at) == std::byte{32});
    CHECK(ByteAt(decoded->pixelBytes, at + 1) == std::byte{32});
    CHECK(ByteAt(decoded->pixelBytes, at + 2) == std::byte{32});
}

TEST_CASE("DDS rejects truncated, cube, array, volume and unsupported resources", "[texture-dds]")
{
    const auto good = ReadFixture(L"dds_bc7.dds");
    CHECK_FALSE(DecodeDdsImage(ReadFixture(L"dds_truncated.dds"), ColorSpaceId::Srgb, {}));

    auto cube = good;
    uint32_t value = 0x200; // DDSCAPS2_CUBEMAP
    std::memcpy(cube.data() + 4 + 108, &value, sizeof(value));
    CHECK_FALSE(DecodeDdsImage(cube, ColorSpaceId::Srgb, {}));

    auto array = good;
    value = 2;
    std::memcpy(array.data() + 4 + 124 + 12, &value, sizeof(value));
    CHECK_FALSE(DecodeDdsImage(array, ColorSpaceId::Srgb, {}));

    auto volume = good;
    value = 2;
    std::memcpy(volume.data() + 4 + 20, &value, sizeof(value));
    CHECK_FALSE(DecodeDdsImage(volume, ColorSpaceId::Srgb, {}));

    auto unsupported = good;
    value = 10; // DXGI_FORMAT_R16G16B16A16_FLOAT
    std::memcpy(unsupported.data() + 4 + 124, &value, sizeof(value));
    CHECK_FALSE(DecodeDdsImage(unsupported, ColorSpaceId::Srgb, {}));

    auto badHeaderSize = good;
    value = 123;
    std::memcpy(badHeaderSize.data() + 4, &value, sizeof(value));
    CHECK_FALSE(DecodeDdsImage(badHeaderSize, ColorSpaceId::Srgb, {}));

    auto badPixelFormatSize = good;
    value = 31;
    std::memcpy(badPixelFormatSize.data() + 4 + 72, &value, sizeof(value));
    CHECK_FALSE(DecodeDdsImage(badPixelFormatSize, ColorSpaceId::Srgb, {}));

    // A declared mip count larger than the payload is clamped, never trusted.
    auto declaredMips = ReadFixture(L"dds_bc1.dds");
    value = 8;
    std::memcpy(declaredMips.data() + 4 + 24, &value, sizeof(value));
    value = 0x20000; // DDSD_MIPMAPCOUNT
    uint32_t flags = 0;
    std::memcpy(&flags, declaredMips.data() + 4 + 4, sizeof(flags));
    flags |= value;
    std::memcpy(declaredMips.data() + 4 + 4, &flags, sizeof(flags));
    auto clamped = DecodeDdsImage(declaredMips, ColorSpaceId::Srgb, {});
    REQUIRE(clamped);
    CHECK(clamped->mipLevels == 1);

    TextureDecodeOptions options;
    options.maxDecodedBytes = 8;
    CHECK_FALSE(DecodeDdsImage(good, ColorSpaceId::Srgb, options));
    options = {};
    options.maxEncodedBytes = good.size() - 1;
    CHECK_FALSE(DecodeDdsImage(good, ColorSpaceId::Srgb, options));
    options = {};
    options.isCancelled = [] { return true; };
    CHECK_FALSE(DecodeDdsImage(good, ColorSpaceId::Srgb, options));
}

TEST_CASE("OBJ package decodes TGA base color and DDS normal maps through the sandbox",
          "[texture-tga][texture-dds]")
{
    const auto result = import_broker::RunImportSession(
        Request(FixtureRoot() / L"package" / L"scene.obj", import_broker::ImportFormat::Obj, 1201));
    CAPTURE(uint32_t(result.stage), uint32_t(result.errorCode));
    REQUIRE(result.ok);
    unsigned images = 0, warnings = 0;
    bool rgba = false, bc5 = false;
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology == ChunkTopology::TextureWarning) ++warnings;
        if (chunk.descriptor.topology != ChunkTopology::Image) continue;
        ++images;
        ImagePayloadHeader header{};
        REQUIRE(chunk.payload.size() >= sizeof(header));
        std::memcpy(&header, chunk.payload.data(), sizeof(header));
        rgba |= header.pixelFormat == uint32_t(PixelFormatId::RGBA8_UNORM);
        bc5 |= header.pixelFormat == uint32_t(PixelFormatId::BC5_UNORM);
    }
    CHECK(images >= 2);
    CHECK(rgba);
    CHECK(bc5);
    CHECK(warnings == 0);
}

TEST_CASE("Malformed TGA/DDS-named textures fall back with a warning, not a model failure",
          "[texture-tga][texture-dds]")
{
    for (const auto* name : {L"scene.obj", L"scene-dds.obj"}) {
        CAPTURE(name);
        const auto result = import_broker::RunImportSession(
            Request(FixtureRoot() / L"bad" / name, import_broker::ImportFormat::Obj, 1202));
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
}

TEST_CASE("ORM metallic-roughness R channel reaches the viewer as ambient occlusion data",
          "[texture-ao]")
{
    const auto result = import_broker::RunImportSession(
        Request(FixtureRoot() / L"orm" / L"scene.gltf", import_broker::ImportFormat::Gltf, 1203));
    CAPTURE(uint32_t(result.stage), uint32_t(result.errorCode));
    REQUIRE(result.ok);
    bool found = false;
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology != ChunkTopology::Image) continue;
        ImagePayloadHeader header{};
        REQUIRE(chunk.payload.size() >= sizeof(header));
        std::memcpy(&header, chunk.payload.data(), sizeof(header));
        CHECK(header.width == 4);
        CHECK(header.height == 4);
        CHECK(header.pixelFormat == uint32_t(PixelFormatId::RGBA8_UNORM));
        const auto* pixels = chunk.payload.data() + sizeof(header);
        CHECK(pixels[0] == std::byte{32});
        CHECK(pixels[1] == std::byte{200});
        CHECK(pixels[2] == std::byte{20});
        CHECK(pixels[3] == std::byte{255});
        found = true;
    }
    CHECK(found);
}
