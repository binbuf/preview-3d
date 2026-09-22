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
#include "model_core/GeometryBounds.h"

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
        if (c.descriptor.topology == ChunkTopology::TriangleList || c.descriptor.topology == ChunkTopology::PointList)
            SetLocalBounds(c.descriptor, c.payload);
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
            = WireChecksum64(std::span<const std::byte>(section.data() + c.descriptor.normalizedRangeOffset,
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
    header.scene.generationId = generationId;
    header.sectionLength = sectionLength;
    header.chunkCount = static_cast<uint32_t>(chunks.size());
    header.reserved = 0;
    header.sectionChecksum = WireChecksum64(std::span<const std::byte>(
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

TEST_CASE("Image refinements reject missing roots, semantic changes and stale resolution", "[shared-section-validator][texture]")
{
    ImagePayloadHeader low=ValidImageHeader(2,2);low.mipLevels=2;low.pixelDataByteSize=20;
    import_broker::KnownImageCatalog priorImages{{1,low}};
    import_broker::KnownChunkCatalog prior{{1,ChunkTopology::Image}};
    auto full=ValidImageHeader(4,4);full.mipLevels=3;full.pixelDataByteSize=84;full.reserved0=1;
    ChunkSpec chunk;chunk.descriptor.topology=ChunkTopology::Image;chunk.descriptor.chunkId=2;
    auto validate=[&](ImagePayloadHeader header,uint64_t bytes=20,uint64_t pixels=5) {
        chunk.payload=ToBytes(header,std::vector<std::byte>(static_cast<size_t>(header.pixelDataByteSize)));
        return import_broker::ValidateAndCopySection(BuildSection({chunk},1),1,8,&prior,false,&priorImages,bytes,pixels);
    };
    CHECK(validate(full).ok);
    auto bad=full;bad.reserved0=3;CHECK_FALSE(validate(bad).ok);
    bad=full;bad.colorSpace=uint32_t(ColorSpaceId::Linear);CHECK_FALSE(validate(bad).ok);
    bad=low;bad.reserved0=1;CHECK_FALSE(validate(bad).ok);
    CHECK_FALSE(validate(full,kMaxAggregateTextureBytes-83).ok);
    CHECK_FALSE(validate(full,20,kMaxAggregateTexturePixels-20).ok);
    bad=full;bad.mipLevels=4;CHECK_FALSE(validate(bad).ok);
    // A refinement cannot resolve through a forward root even if material
    // forward references are enabled by the streaming session.
    chunk.payload=ToBytes(full,std::vector<std::byte>(84));
    CHECK_FALSE(import_broker::ValidateAndCopySection(BuildSection({chunk},1),1,8,nullptr,true).ok);
}

TEST_CASE("Texture warning payload is a closed bounded count", "[shared-section-validator][texture]")
{
    ChunkSpec warning;warning.descriptor.topology=ChunkTopology::TextureWarning;warning.descriptor.chunkId=1;
    for (uint32_t count:{0u,1u,64u,65u,UINT32_MAX}) {
        warning.payload.resize(4);std::memcpy(warning.payload.data(),&count,4);
        CHECK(import_broker::ValidateAndCopySection(BuildSection({warning},1),1,8).ok==(count>0 && count<=64));
    }
}

TEST_CASE("Texture headers survive a bounded rechecksummed mutation corpus", "[texture][fuzz]")
{
    uint32_t seed=0x203A17E5;
    unsigned rejected=0;
    for (unsigned i=0;i<512;++i) {
        auto chunks=ValidTriple();
        seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;
        if (i%32) chunks[2].payload[seed%sizeof(ImagePayloadHeader)]^=std::byte(uint8_t((seed>>16)|1));
        auto result=import_broker::ValidateAndCopySection(BuildSection(chunks,203),203,8);
        if (!result.ok) {++rejected;CHECK(result.chunks.empty());}
        else {REQUIRE(result.chunks.size()==3);CHECK(result.chunks[2].payload.size()==chunks[2].payload.size());}
    }
    CHECK(rejected>400);
}

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
    p.flags = 1u << 12;
    chunks[1].payload = ToBytes(p);
    auto section = BuildSection(chunks, 1);
    auto result = import_broker::ValidateAndCopySection(section, 1, 8);
    CHECK_FALSE(result.ok);
    CHECK(result.errorCode == ImportErrorCode::MalformedData);
}

TEST_CASE("The closed material flag mask accepts the 3MF sampler and blending flags",
          "[shared-section-validator][texture]")
{
    auto chunks = ValidTriple();
    MaterialPayload p = ValidMaterialPayload();
    p.flags = kMaterialFlagsKnownMask;
    chunks[1].payload = ToBytes(p);
    const auto section = BuildSection(chunks, 1);
    const auto result = import_broker::ValidateAndCopySection(section, 1, 8);
    CHECK(result.ok);
}

TEST_CASE("A transmissive Material chunk accepts its in-range factor and rejects the rest",
          "[shared-section-validator][texture]")
{
    auto accepted = ValidTriple();
    MaterialPayload p = ValidMaterialPayload();
    p.flags |= kMaterialFlagTransmissive;
    p.transmissionFactor = 0.5f;
    accepted[1].payload = ToBytes(p);
    const auto acceptedSection = BuildSection(accepted, 1);
    CHECK(import_broker::ValidateAndCopySection(acceptedSection, 1, 8).ok);

    auto outOfRange = p;
    outOfRange.transmissionFactor = 1.5f;
    auto chunks = ValidTriple();
    chunks[1].payload = ToBytes(outOfRange);
    const auto section = BuildSection(chunks, 1);
    const auto result = import_broker::ValidateAndCopySection(section, 1, 8);
    CHECK_FALSE(result.ok);
    CHECK(result.errorCode == ImportErrorCode::MalformedData);
}

TEST_CASE("A Material transmission factor without its flag is rejected",
          "[shared-section-validator][texture]")
{
    auto chunks = ValidTriple();
    MaterialPayload p = ValidMaterialPayload();
    p.transmissionFactor = 0.5f; // flag deliberately not set
    chunks[1].payload = ToBytes(p);
    const auto section = BuildSection(chunks, 1);
    const auto result = import_broker::ValidateAndCopySection(section, 1, 8);
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

TEST_CASE("Hostile dimensions fail before accepting fabricated image expansion", "[shared-section-validator][texture]")
{
    ChunkSpec image;image.descriptor.chunkId=1;image.descriptor.topology=ChunkTopology::Image;
    auto header=ValidImageHeader();header.width=32000;header.height=32000;
    header.pixelDataByteSize=512'000'000;
    image.payload=ToBytes(header,{});
    auto result=import_broker::ValidateAndCopySection(BuildSection({image},1),1,8);
    CHECK_FALSE(result.ok);CHECK(result.errorCode==ImportErrorCode::ResourceLimit);
}
