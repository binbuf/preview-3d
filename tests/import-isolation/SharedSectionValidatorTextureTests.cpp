// Stage 2 (Material/Image wire-format extension) of the Draco decode +
// texture/material wire-format + KTX2/Basis transcode chunk. Unlike
// TriangleList/PointList (which every prior format's adapter already
// exercised through a live worker), Material/Image validation is genuinely
// new SharedSectionValidator surface -- these tests hand-build section byte
// buffers directly, no live sandboxed worker needed, matching how a
// hostile-worker-style test would exercise the validator in isolation.

#include "import_broker/SharedSectionValidator.h"
#include "model_core/Checksum.h"
#include "model_core/MaterialPayload.h"
#include "model_core/PixelFormats.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <limits>
#include <vector>

namespace {

using namespace model_core;

// One chunk's worth of caller-provided descriptor fields plus its raw
// payload bytes; BuildSection fills in normalizedRangeOffset/Length,
// byteSize, and chunkChecksum from the payload -- everything else
// (topology, chunkId, dependencyIds/dependencyCount, vertex/index/layout
// fields) is the caller's to set, so a test can freely construct an invalid
// combination.
struct ChunkSpec {
    ChunkDescriptor descriptor{}; // caller fills everything except the four fields BuildSection computes
    std::vector<std::byte> payload;
};

std::vector<std::byte> BuildSection(std::vector<ChunkSpec> chunks, uint64_t generationId)
{
    uint64_t offset = kSectionHeaderSize + chunks.size() * kChunkDescriptorSize;
    for (auto& c : chunks) {
        c.descriptor.normalizedRangeOffset = offset;
        c.descriptor.normalizedRangeLength = c.payload.size();
        c.descriptor.byteSize = c.payload.size();
        offset += c.payload.size();
    }
    uint64_t sectionLength = offset;

    std::vector<std::byte> section(sectionLength, std::byte{ 0 });
    for (auto& c : chunks) {
        if (!c.payload.empty()) {
            std::memcpy(section.data() + c.descriptor.normalizedRangeOffset, c.payload.data(),
                        c.payload.size());
        }
        c.descriptor.chunkChecksum
            = Fnv1a64(std::span<const std::byte>(section.data() + c.descriptor.normalizedRangeOffset,
                                                  c.payload.size()));
    }
    for (size_t i = 0; i < chunks.size(); ++i) {
        std::memcpy(section.data() + kSectionHeaderSize + i * kChunkDescriptorSize, &chunks[i].descriptor,
                    sizeof(ChunkDescriptor));
    }

    SectionHeader header{};
    header.magic = kSectionMagic;
    header.protocolVersion = kCurrentProtocolVersion;
    header.generationId = generationId;
    header.sectionLength = sectionLength;
    header.chunkCount = static_cast<uint32_t>(chunks.size());
    header.reserved = 0;
    header.sectionChecksum = Fnv1a64(std::span<const std::byte>(
        section.data() + kSectionHeaderSize, sectionLength - kSectionHeaderSize));
    std::memcpy(section.data(), &header, sizeof(header));
    return section;
}

std::vector<std::byte> ToBytes(const MaterialPayload& p)
{
    std::vector<std::byte> bytes(sizeof(p));
    std::memcpy(bytes.data(), &p, sizeof(p));
    return bytes;
}

std::vector<std::byte> ToBytes(const ImagePayloadHeader& h, const std::vector<std::byte>& pixels)
{
    std::vector<std::byte> bytes(sizeof(h) + pixels.size());
    std::memcpy(bytes.data(), &h, sizeof(h));
    if (!pixels.empty()) {
        std::memcpy(bytes.data() + sizeof(h), pixels.data(), pixels.size());
    }
    return bytes;
}

MaterialPayload ValidMaterialPayload()
{
    MaterialPayload p{};
    p.baseColorFactor[0] = p.baseColorFactor[1] = p.baseColorFactor[2] = p.baseColorFactor[3] = 1.0f;
    p.metallicFactor = 1.0f;
    p.roughnessFactor = 1.0f;
    p.uvScale[0] = p.uvScale[1] = 1.0f;
    p.alphaMode = static_cast<uint32_t>(AlphaModeId::Opaque);
    p.alphaCutoff = 0.5f;
    return p;
}

ImagePayloadHeader ValidImageHeader(uint32_t width = 4, uint32_t height = 4)
{
    ImagePayloadHeader h{};
    h.pixelFormat = static_cast<uint32_t>(PixelFormatId::RGBA8_UNORM);
    h.width = width;
    h.height = height;
    h.mipLevels = 1;
    h.colorSpace = static_cast<uint32_t>(ColorSpaceId::Srgb);
    h.pixelDataByteSize = *ComputeImagePixelBytes(PixelFormatId::RGBA8_UNORM, width, height, 1);
    return h;
}

// A valid mesh(1) -> material(2) -> image(3) triple, as a starting point
// each test mutates one field of before rebuilding.
std::vector<ChunkSpec> ValidTriple()
{
    std::vector<ChunkSpec> chunks(3);

    chunks[0].descriptor.topology = ChunkTopology::PointList;
    chunks[0].descriptor.vertexCount = 1;
    chunks[0].descriptor.vertexLayoutId = static_cast<uint32_t>(VertexLayoutId::PositionOnly_F32);
    chunks[0].descriptor.chunkId = 1;
    chunks[0].descriptor.dependencyCount = 1;
    chunks[0].descriptor.dependencyIds[0] = 2;
    chunks[0].payload.resize(sizeof(VertexPositionOnlyF32));

    chunks[1].descriptor.topology = ChunkTopology::Material;
    chunks[1].descriptor.chunkId = 2;
    chunks[1].descriptor.dependencyCount = 1;
    chunks[1].descriptor.dependencyIds[0] = 3;
    chunks[1].payload = ToBytes(ValidMaterialPayload());

    chunks[2].descriptor.topology = ChunkTopology::Image;
    chunks[2].descriptor.chunkId = 3;
    ImagePayloadHeader header = ValidImageHeader();
    std::vector<std::byte> pixels(header.pixelDataByteSize, std::byte{ 0xAB });
    chunks[2].payload = ToBytes(header, pixels);

    return chunks;
}

} // namespace

TEST_CASE("A valid mesh -> material -> image triple validates and round-trips dependency ids",
          "[shared-section-validator][texture]")
{
    auto section = BuildSection(ValidTriple(), /*generationId=*/1);
    auto result = import_broker::ValidateAndCopySection(section, /*expectedGenerationId=*/1,
                                                          /*maxChunkCount=*/8);
    REQUIRE(result.ok);
    REQUIRE(result.chunks.size() == 3);
    CHECK(result.chunks[0].descriptor.dependencyIds[0] == 2);
    CHECK(result.chunks[1].descriptor.dependencyIds[0] == 3);
}

TEST_CASE("A Material chunk declaring a nonzero vertexCount is rejected", "[shared-section-validator][texture]")
{
    auto chunks = ValidTriple();
    chunks[1].descriptor.vertexCount = 1;
    auto section = BuildSection(chunks, 1);
    auto result = import_broker::ValidateAndCopySection(section, 1, 8);
    CHECK_FALSE(result.ok);
    CHECK(result.errorCode == ImportErrorCode::MalformedData);
}

TEST_CASE("A Material chunk whose byteSize does not match sizeof(MaterialPayload) is rejected",
          "[shared-section-validator][texture]")
{
    auto chunks = ValidTriple();
    chunks[1].payload.push_back(std::byte{ 0 }); // one extra trailing byte
    auto section = BuildSection(chunks, 1);
    auto result = import_broker::ValidateAndCopySection(section, 1, 8);
    CHECK_FALSE(result.ok);
    CHECK(result.errorCode == ImportErrorCode::MalformedData);
}

TEST_CASE("A Material chunk with a NaN baseColorFactor component is rejected",
          "[shared-section-validator][texture]")
{
    auto chunks = ValidTriple();
    MaterialPayload p = ValidMaterialPayload();
    p.baseColorFactor[1] = std::numeric_limits<float>::quiet_NaN();
    chunks[1].payload = ToBytes(p);
    auto section = BuildSection(chunks, 1);
    auto result = import_broker::ValidateAndCopySection(section, 1, 8);
    CHECK_FALSE(result.ok);
    CHECK(result.errorCode == ImportErrorCode::MalformedData);
}

TEST_CASE("A Material chunk with alphaMode outside {0,1,2} is rejected", "[shared-section-validator][texture]")
{
    auto chunks = ValidTriple();
    MaterialPayload p = ValidMaterialPayload();
    p.alphaMode = 3;
    chunks[1].payload = ToBytes(p);
    auto section = BuildSection(chunks, 1);
    auto result = import_broker::ValidateAndCopySection(section, 1, 8);
    CHECK_FALSE(result.ok);
    CHECK(result.errorCode == ImportErrorCode::MalformedData);
}

TEST_CASE("A Material chunk with an unrecognized flags bit set is rejected",
          "[shared-section-validator][texture]")
{
    auto chunks = ValidTriple();
    MaterialPayload p = ValidMaterialPayload();
    p.flags = 1u << 5;
    chunks[1].payload = ToBytes(p);
    auto section = BuildSection(chunks, 1);
    auto result = import_broker::ValidateAndCopySection(section, 1, 8);
    CHECK_FALSE(result.ok);
    CHECK(result.errorCode == ImportErrorCode::MalformedData);
}

TEST_CASE("A Material chunk whose dependency resolves to a non-Image chunk is rejected",
          "[shared-section-validator][texture]")
{
    auto chunks = ValidTriple();
    chunks[1].descriptor.dependencyIds[0] = 1; // chunk 1 is the PointList mesh, not the image
    auto section = BuildSection(chunks, 1);
    auto result = import_broker::ValidateAndCopySection(section, 1, 8);
    CHECK_FALSE(result.ok);
    CHECK(result.errorCode == ImportErrorCode::MalformedData);
}

TEST_CASE("An Image chunk with an unrecognized pixelFormat is rejected", "[shared-section-validator][texture]")
{
    auto chunks = ValidTriple();
    ImagePayloadHeader h = ValidImageHeader();
    h.pixelFormat = 99;
    chunks[2].payload = ToBytes(h, std::vector<std::byte>(h.pixelDataByteSize, std::byte{ 0 }));
    auto section = BuildSection(chunks, 1);
    auto result = import_broker::ValidateAndCopySection(section, 1, 8);
    CHECK_FALSE(result.ok);
    CHECK(result.errorCode == ImportErrorCode::MalformedData);
}

TEST_CASE("An Image chunk declaring BC5_UNORM with sRGB color space is rejected (no DXGI _SRGB variant)",
          "[shared-section-validator][texture]")
{
    auto chunks = ValidTriple();
    ImagePayloadHeader h{};
    h.pixelFormat = static_cast<uint32_t>(PixelFormatId::BC5_UNORM);
    h.width = 4;
    h.height = 4;
    h.mipLevels = 1;
    h.colorSpace = static_cast<uint32_t>(ColorSpaceId::Srgb);
    h.pixelDataByteSize = *ComputeImagePixelBytes(PixelFormatId::BC5_UNORM, 4, 4, 1);
    chunks[2].payload = ToBytes(h, std::vector<std::byte>(h.pixelDataByteSize, std::byte{ 0 }));
    auto section = BuildSection(chunks, 1);
    auto result = import_broker::ValidateAndCopySection(section, 1, 8);
    CHECK_FALSE(result.ok);
    CHECK(result.errorCode == ImportErrorCode::MalformedData);
}

TEST_CASE("An Image chunk whose pixelDataByteSize disagrees with its declared dimensions is rejected",
          "[shared-section-validator][texture]")
{
    auto chunks = ValidTriple();
    ImagePayloadHeader h = ValidImageHeader();
    h.pixelDataByteSize += 4; // wrong on purpose
    chunks[2].payload = ToBytes(h, std::vector<std::byte>(h.pixelDataByteSize, std::byte{ 0 }));
    auto section = BuildSection(chunks, 1);
    auto result = import_broker::ValidateAndCopySection(section, 1, 8);
    CHECK_FALSE(result.ok);
    CHECK(result.errorCode == ImportErrorCode::MalformedData);
}

TEST_CASE("An Image chunk declaring a nonzero dependencyCount is rejected (images reference nothing)",
          "[shared-section-validator][texture]")
{
    auto chunks = ValidTriple();
    chunks[2].descriptor.dependencyCount = 1;
    chunks[2].descriptor.dependencyIds[0] = 2;
    auto section = BuildSection(chunks, 1);
    auto result = import_broker::ValidateAndCopySection(section, 1, 8);
    CHECK_FALSE(result.ok);
    CHECK(result.errorCode == ImportErrorCode::MalformedData);
}

TEST_CASE("Two chunks sharing the same chunkId are rejected", "[shared-section-validator][texture]")
{
    auto chunks = ValidTriple();
    chunks[2].descriptor.chunkId = chunks[1].descriptor.chunkId;
    auto section = BuildSection(chunks, 1);
    auto result = import_broker::ValidateAndCopySection(section, 1, 8);
    CHECK_FALSE(result.ok);
    CHECK(result.errorCode == ImportErrorCode::MalformedData);
}

TEST_CASE("A chunk declaring the reserved chunkId 0 is rejected", "[shared-section-validator][texture]")
{
    auto chunks = ValidTriple();
    chunks[0].descriptor.dependencyCount = 0; // drop the now-dangling reference to chunk 2 first
    chunks[0].descriptor.dependencyIds[0] = 0;
    chunks[0].descriptor.chunkId = 0;
    auto section = BuildSection(chunks, 1);
    auto result = import_broker::ValidateAndCopySection(section, 1, 8);
    CHECK_FALSE(result.ok);
    CHECK(result.errorCode == ImportErrorCode::MalformedData);
}

TEST_CASE("Aggregate decoded image pixels across the batch exceeding 1 gigapixel is rejected as "
          "ResourceLimit",
          "[shared-section-validator][texture]")
{
    auto chunks = ValidTriple();
    // A single 32000x32000 image mip-0 exceeds 1e9 pixels (1.024e9). Using
    // BC1_UNORM (0.5 bytes/pixel, block-compressed) instead of RGBA8 keeps
    // this test's own allocation to ~488 MiB rather than ~3.8 GiB while
    // still genuinely crossing the real budget via ComputeImagePixelBytes'
    // real math -- this is the one deliberately large-memory test in this
    // file, matching the actual budget it's proving.
    ImagePayloadHeader h{};
    h.pixelFormat = static_cast<uint32_t>(PixelFormatId::BC1_UNORM);
    h.width = 32000;
    h.height = 32000;
    h.mipLevels = 1;
    h.colorSpace = static_cast<uint32_t>(ColorSpaceId::Srgb);
    auto expected = ComputeImagePixelBytes(PixelFormatId::BC1_UNORM, h.width, h.height, 1);
    REQUIRE(expected.has_value());
    h.pixelDataByteSize = *expected;

    std::vector<std::byte> pixels(*expected, std::byte{ 0 });
    chunks[2].payload = ToBytes(h, pixels);
    auto section = BuildSection(chunks, 1);
    auto result = import_broker::ValidateAndCopySection(section, 1, 8);
    CHECK_FALSE(result.ok);
    CHECK(result.errorCode == ImportErrorCode::ResourceLimit);
}
