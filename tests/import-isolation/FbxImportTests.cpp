#include "SandboxTestSupport.h"
#include "../../interactive-viewer/src/app/D3D12ImportBridge.h"
#include "import_broker/ImportSession.h"
#include "import_broker/SharedSection.h"
#include "model_core/MaterialPayload.h"
#include "model_core/PixelFormats.h"
#include "model_core/VertexLayouts.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct ScratchFbx {
    std::filesystem::path directory;
    std::filesystem::path path;

    ScratchFbx()
    {
        static std::atomic_uint64_t next{0};
        directory = std::filesystem::temp_directory_path() /
            (L"Preview3D-fbx-" + std::to_wstring(GetCurrentProcessId()) + L"-"
             + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(next.fetch_add(1)));
        REQUIRE(std::filesystem::create_directory(directory));
        path = directory / L"model.fbx";
    }
    ~ScratchFbx()
    {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
    void Write(std::span<const std::byte> bytes)
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        REQUIRE(output.good());
    }
    void Write(std::string_view bytes) { Write(std::as_bytes(std::span(bytes))); }
    void WriteSidecar(const std::filesystem::path& name, std::span<const std::byte> bytes)
    {
        const auto sidecar = directory / name;
        REQUIRE((std::filesystem::create_directories(sidecar.parent_path())
                 || std::filesystem::exists(sidecar.parent_path())));
        std::ofstream output(sidecar, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        REQUIRE(output.good());
    }
};

std::vector<std::byte> DecodeBase64(std::span<const char> raw)
{
    auto digit = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<std::byte> decoded;
    uint32_t accumulator = 0, bits = 0;
    for (unsigned char c : raw) {
        if (c == '=') break;
        const int value = digit(c);
        if (value < 0) continue;
        accumulator = (accumulator << 6) | uint32_t(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(std::byte((accumulator >> bits) & 0xffu));
        }
    }
    return decoded;
}

std::vector<std::byte> ReadFixture(std::string_view name)
{
    const auto path = std::filesystem::path(PREVIEW3D_FBX_FIXTURES_DIR) / name;
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    std::vector<char> raw((std::istreambuf_iterator<char>(input)), {});
    if (!name.ends_with(".base64")) {
        const auto bytes = std::as_bytes(std::span(raw));
        return {bytes.begin(), bytes.end()};
    }
    const auto decoded = DecodeBase64(raw);
    REQUIRE_FALSE(decoded.empty());
    return decoded;
}

std::vector<std::byte> ReadAsset(const std::filesystem::path& name)
{
    std::ifstream input(std::filesystem::path(PREVIEW3D_TEST_ASSETS_DIR) / name, std::ios::binary);
    REQUIRE(input.good());
    std::vector<char> raw((std::istreambuf_iterator<char>(input)), {});
    const auto bytes = std::as_bytes(std::span(raw));
    return {bytes.begin(), bytes.end()};
}

std::string EncodeBase64(std::span<const std::byte> bytes)
{
    static constexpr char digits[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((bytes.size() + 2) / 3) * 4);
    for (size_t offset = 0; offset < bytes.size(); offset += 3) {
        const uint32_t a = std::to_integer<uint8_t>(bytes[offset]);
        const uint32_t b = offset + 1 < bytes.size() ? std::to_integer<uint8_t>(bytes[offset + 1]) : 0;
        const uint32_t c = offset + 2 < bytes.size() ? std::to_integer<uint8_t>(bytes[offset + 2]) : 0;
        const uint32_t value = (a << 16) | (b << 8) | c;
        encoded.push_back(digits[(value >> 18) & 63]);
        encoded.push_back(digits[(value >> 12) & 63]);
        encoded.push_back(offset + 1 < bytes.size() ? digits[(value >> 6) & 63] : '=');
        encoded.push_back(offset + 2 < bytes.size() ? digits[value & 63] : '=');
    }
    return encoded;
}

std::string ReadAsciiFixture(std::string_view name)
{
    const auto bytes = ReadFixture(name);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void ReplaceOnce(std::string& text, std::string_view needle, std::string_view replacement);

void ReplaceAll(std::string& text, std::string_view needle, std::string_view replacement)
{
    REQUIRE_FALSE(needle.empty());
    size_t offset = 0;
    uint32_t replacements = 0;
    while ((offset = text.find(needle, offset)) != std::string::npos) {
        text.replace(offset, needle.size(), replacement);
        offset += replacement.size();
        ++replacements;
    }
    REQUIRE(replacements != 0);
}

std::string TextureFixture(std::span<const std::byte> embedded, std::string_view relativePath)
{
    std::string ascii = ReadAsciiFixture("embedded-png-ascii.fbx");
    const size_t content = ascii.find("\t\tContent: ,");
    REQUIRE(content != std::string::npos);
    const size_t end = ascii.find("\n\t}", content);
    REQUIRE(end != std::string::npos);
    const std::string replacement = embedded.empty()
        ? std::string{}
        : "\t\tContent: ,\"" + EncodeBase64(embedded) + "\"";
    ascii.replace(content, end - content, replacement);
    ReplaceAll(ascii, "textures\\tiny_clouds.png", relativePath);
    return ascii;
}

std::string MaterialTextureFixture(std::span<const std::byte> embedded)
{
    std::string ascii = TextureFixture(embedded, "material-texture.png");
    ReplaceOnce(ascii, "P: \"DiffuseColor\", \"Color\", \"\", \"A\",1,1,1",
                       "P: \"DiffuseColor\", \"Color\", \"\", \"A\",0.25,0.5,0.75");
    // 3ds Max-style files carry both Opacity and TransparencyFactor; this
    // fixture intentionally authors only the inverted TransparencyFactor so the
    // fallback conversion stays covered.
    ReplaceOnce(ascii, "\t\t\tP: \"Opacity\", \"double\", \"Number\", \"\",1", "");
    ReplaceOnce(ascii, "P: \"TransparencyFactor\", \"Number\", \"\", \"A\",1",
                       "P: \"TransparencyFactor\", \"Number\", \"\", \"A\",0.25");
    ReplaceOnce(ascii, "P: \"Emissive\", \"Vector3D\", \"Vector\", \"\",0,0,0",
                       "P: \"EmissiveColor\", \"Color\", \"\", \"A\",0.1,0.2,0.3\n"
                       "\t\t\tP: \"EmissiveFactor\", \"Number\", \"\", \"A\",2\n"
                       "\t\t\tP: \"3dsMax|main|alphaMode\", \"Integer\", \"\", \"A\",1\n"
                       "\t\t\tP: \"3dsMax|main|alphaCutoff\", \"Float\", \"\", \"A\",0.3");
    ReplaceOnce(ascii, "P: \"UseMaterial\", \"bool\", \"\", \"\",1",
                       "P: \"UseMaterial\", \"bool\", \"\", \"\",1\n"
                       "\t\t\tP: \"Translation\", \"Vector\", \"\", \"A\",0.25,0.5,0\n"
                       "\t\t\tP: \"Rotation\", \"Vector\", \"\", \"A\",0,0,30\n"
                       "\t\t\tP: \"Scaling\", \"Vector\", \"\", \"A\",2,3,1");
    ReplaceOnce(ascii, "C: \"OP\",2483116064336,2483102580784, \"DiffuseColor\"",
                       "C: \"OP\",2483116064336,2483102580784, \"DiffuseColor\"\n"
                       "\tC: \"OP\",2483116064336,2483102580784, \"NormalMap\"\n"
                       "\tC: \"OP\",2483116064336,2483102580784, \"EmissiveColor\"");
    return ascii;
}

std::string MakeInstancedDualQuaternion(double translationX)
{
    std::string ascii = ReadAsciiFixture("dual-quaternion-ascii.fbx");
    ReplaceOnce(ascii, "Definitions:  {\n\tVersion: 100\n\tCount: 19",
                       "Definitions:  {\n\tVersion: 100\n\tCount: 20");
    ReplaceOnce(ascii, "\tObjectType: \"Model\" {\n\t\tCount: 4",
                       "\tObjectType: \"Model\" {\n\t\tCount: 5");
    const std::string duplicate =
        "\tModel: 999000000001, \"Model::pCubeDuplicate\", \"Mesh\" {\n"
        "\t\tVersion: 232\n"
        "\t\tProperties70:  {\n"
        "\t\t\tP: \"RotationActive\", \"bool\", \"\", \"\",1\n"
        "\t\t\tP: \"InheritType\", \"enum\", \"\", \"\",1\n"
        "\t\t\tP: \"ScalingMax\", \"Vector3D\", \"Vector\", \"\",0,0,0\n"
        "\t\t\tP: \"DefaultAttributeIndex\", \"int\", \"Integer\", \"\",0\n"
        "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"AL7\","
        + std::to_string(translationX) + ",0,0\n"
        "\t\t\tP: \"Lcl Rotation\", \"Lcl Rotation\", \"\", \"AL7\",0,0,0\n"
        "\t\t\tP: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"AL7\",1,1,1\n"
        "\t\t\tP: \"currentUVSet\", \"KString\", \"\", \"U\", \"map1\"\n"
        "\t\t}\n\t\tShading: T\n\t\tCulling: \"CullingOff\"\n\t}\n";
    ReplaceOnce(ascii, "\tModel: 2256094537568, \"Model::joint1\", \"LimbNode\" {",
                       duplicate + "\tModel: 2256094537568, \"Model::joint1\", \"LimbNode\" {");
    ReplaceOnce(ascii, "\t;Model::joint1, Model::RootNode\n\tC: \"OO\",2256094537568,0",
        "\t;Model::pCubeDuplicate, Model::RootNode\n"
        "\tC: \"OO\",999000000001,0\n\n"
        "\t;Geometry::, Model::pCubeDuplicate\n"
        "\tC: \"OO\",2259119305632,999000000001\n\n"
        "\t;Material::lambert1, Model::pCubeDuplicate\n"
        "\tC: \"OO\",2256094207216,999000000001\n\n"
        "\t;Model::joint1, Model::RootNode\n\tC: \"OO\",2256094537568,0");
    return ascii;
}

void ReplaceOnce(std::string& text, std::string_view needle, std::string_view replacement)
{
    const size_t offset = text.find(needle);
    REQUIRE(offset != std::string::npos);
    text.replace(offset, needle.size(), replacement);
}

import_broker::ImportSessionRequest Request(const std::filesystem::path& path,
                                             uint64_t generation = 501)
{
    import_broker::ImportSessionRequest request;
    request.workerExePath = sandbox_test_support::WorkerExePath();
    request.sourcePath = path.wstring();
    request.format = import_broker::ImportFormat::Fbx;
    request.generationId = generation;
    request.sectionByteCapacity = import_broker::kImportSectionBytes;
    request.maxChunkCount = 1024;
    request.maxChunkBatchesPerGeneration = 256;
    request.maxChunksPerGeneration = 20'000;
    request.maxSidecarRequestsPerGeneration = 64;
    request.maxSidecarFileBytes = 256ull * 1024 * 1024;
    return request;
}

uint32_t Count(const import_broker::ImportSessionResult& result, model_core::ChunkTopology topology)
{
    return static_cast<uint32_t>(std::count_if(result.chunks.begin(), result.chunks.end(),
        [topology](const import_broker::ValidatedChunk& chunk) {
            return chunk.descriptor.topology == topology;
        }));
}

void HashUint64(uint64_t& hash, uint64_t value)
{
    for (uint32_t shift = 0; shift < 64; shift += 8) {
        hash ^= static_cast<uint8_t>(value >> shift);
        hash *= 1099511628211ull;
    }
}

void HashNumber(uint64_t& hash, double value)
{
    REQUIRE(std::isfinite(value));
    constexpr double tolerance = 1.0e-6;
    const double quantized = std::nearbyint(value / tolerance);
    REQUIRE(quantized >= static_cast<double>((std::numeric_limits<int64_t>::min)()));
    REQUIRE(quantized <= static_cast<double>((std::numeric_limits<int64_t>::max)()));
    HashUint64(hash, static_cast<uint64_t>(static_cast<int64_t>(quantized)));
}

struct WireSnapshot {
    uint64_t hash = 1469598103934665603ull;
    uint64_t triangles = 0;
    uint64_t vertices = 0;
    uint32_t nodes = 0;
    uint32_t instances = 0;
    uint32_t geometryChunks = 0;
    uint32_t warnings = 0;
    model_core::SceneMetadata scene{};
};

WireSnapshot Snapshot(const import_broker::ImportSessionResult& result)
{
    REQUIRE(result.ok);
    REQUIRE_FALSE(result.chunks.empty());
    WireSnapshot snapshot{};
    snapshot.scene = result.chunks.front().scene;
    for (const auto& chunk : result.chunks) {
        const auto& descriptor = chunk.descriptor;
        switch (descriptor.topology) {
        case model_core::ChunkTopology::TriangleList: {
            ++snapshot.geometryChunks;
            snapshot.triangles += descriptor.indexCount / 3;
            snapshot.vertices += descriptor.vertexCount;
            HashUint64(snapshot.hash, descriptor.meshId);
            HashUint64(snapshot.hash, descriptor.vertexCount);
            HashUint64(snapshot.hash, descriptor.indexCount);
            for (uint32_t axis = 0; axis < 3; ++axis) {
                HashNumber(snapshot.hash, descriptor.origin[axis] + descriptor.localMin[axis]);
                HashNumber(snapshot.hash, descriptor.origin[axis] + descriptor.localMax[axis]);
            }
            const auto* vertices = reinterpret_cast<
                const model_core::VertexPositionNormalUv0TangentColorF32*>(chunk.payload.data());
            for (uint32_t index = 0; index < descriptor.vertexCount; ++index) {
                HashNumber(snapshot.hash, descriptor.origin[0] + vertices[index].px);
                HashNumber(snapshot.hash, descriptor.origin[1] + vertices[index].py);
                HashNumber(snapshot.hash, descriptor.origin[2] + vertices[index].pz);
                HashNumber(snapshot.hash, vertices[index].nx);
                HashNumber(snapshot.hash, vertices[index].ny);
                HashNumber(snapshot.hash, vertices[index].nz);
                HashNumber(snapshot.hash, vertices[index].tx);
                HashNumber(snapshot.hash, vertices[index].ty);
                HashNumber(snapshot.hash, vertices[index].tz);
                HashNumber(snapshot.hash, vertices[index].tw);
            }
            break;
        }
        case model_core::ChunkTopology::Node: {
            ++snapshot.nodes;
            model_core::NodePayload node{};
            std::memcpy(&node, chunk.payload.data(), sizeof(node));
            HashUint64(snapshot.hash, node.flags);
            for (double value : node.localTransform) HashNumber(snapshot.hash, value);
            break;
        }
        case model_core::ChunkTopology::MeshInstance: {
            ++snapshot.instances;
            model_core::MeshInstancePayload instance{};
            std::memcpy(&instance, chunk.payload.data(), sizeof(instance));
            HashUint64(snapshot.hash, instance.flags);
            for (double value : instance.worldMin) HashNumber(snapshot.hash, value);
            for (double value : instance.worldMax) HashNumber(snapshot.hash, value);
            break;
        }
        case model_core::ChunkTopology::ImportStatus: {
            model_core::ImportStatusPayload status{};
            std::memcpy(&status, chunk.payload.data(), sizeof(status));
            snapshot.warnings += status.optionalFeatureWarnings;
            break;
        }
        default: break;
        }
    }
    return snapshot;
}

} // namespace

TEST_CASE("ASCII FBX preserves hierarchy and shares static mesh geometry across instances",
          "[fbx][hierarchy][instances]")
{
    ScratchFbx source;
    source.Write(ReadFixture("hierarchy-instances-pivots-ascii.fbx"));
    const auto result = import_broker::RunImportSession(Request(source.path));
    CAPTURE(result.stage, result.errorCode, result.errorPhase, result.chunks.size(), result.batchCount);
    REQUIRE(result.ok);
    REQUIRE(Count(result, model_core::ChunkTopology::TriangleList) > 0);
    REQUIRE(Count(result, model_core::ChunkTopology::Node) > 1);
    REQUIRE(Count(result, model_core::ChunkTopology::MeshInstance) > 1);
    CHECK(Count(result, model_core::ChunkTopology::Material) == 1);

    uint32_t firstGeometry = 0, matchingInstances = 0;
    uint32_t authoredMaterial = 0, materialBoundInstances = 0;
    bool transformedNode = false;
    for (const auto& chunk : result.chunks) {
        CHECK(chunk.scene.format == model_core::SourceFormatId::Fbx);
        CHECK(chunk.scene.upAxis == model_core::UpAxisId::Y);
        CHECK(chunk.scene.metersPerUnit == 1.0);
        if (chunk.descriptor.topology == model_core::ChunkTopology::TriangleList && !firstGeometry)
            firstGeometry = chunk.descriptor.chunkId;
        if (chunk.descriptor.topology == model_core::ChunkTopology::MeshInstance) {
            model_core::MeshInstancePayload instance{};
            std::memcpy(&instance, chunk.payload.data(), sizeof(instance));
            matchingInstances += instance.geometryChunkId == firstGeometry;
            materialBoundInstances += instance.materialChunkId != 0;
            if (authoredMaterial) CHECK(instance.materialChunkId == authoredMaterial);
        }
        if (chunk.descriptor.topology == model_core::ChunkTopology::Material) {
            authoredMaterial = chunk.descriptor.chunkId;
            model_core::MaterialPayload material{};
            std::memcpy(&material, chunk.payload.data(), sizeof(material));
            CHECK(material.baseColorFactor[0] == Catch::Approx(0.4f));
            CHECK(material.baseColorFactor[1] == Catch::Approx(0.4f));
            CHECK(material.baseColorFactor[2] == Catch::Approx(0.4f));
            // This fixture authors both Opacity=1 and TransparencyFactor=1,
            // the 3ds Max pairing for an opaque material: the explicit Opacity
            // wins over the inverted TransparencyFactor convention.
            CHECK(material.baseColorFactor[3] == Catch::Approx(1.0f));
            CHECK(material.alphaMode == uint32_t(model_core::AlphaModeId::Opaque));
            CHECK(material.metallicFactor == Catch::Approx(0.0f));
            CHECK(material.roughnessFactor == Catch::Approx(1.0f));
            CHECK((material.flags & model_core::kMaterialFlagDoubleSided) != 0);
        }
        if (chunk.descriptor.topology == model_core::ChunkTopology::Node) {
            model_core::NodePayload node{};
            std::memcpy(&node, chunk.payload.data(), sizeof(node));
            transformedNode |= node.localTransform[12] != 0.0 || node.localTransform[13] != 0.0
                || node.localTransform[14] != 0.0;
        }
    }
    CHECK(firstGeometry != 0);
    CHECK(matchingInstances >= 2);
    CHECK(authoredMaterial != 0);
    CHECK(materialBoundInstances >= 2);
    CHECK(transformedNode);
}

TEST_CASE("Viewer bridge routes uppercase FBX through the product broker mapping",
          "[fbx][product-integration]")
{
    ScratchFbx source;
    source.path = source.directory / L"model.FBX";
    source.Write(ReadFixture("cube-binary.fbx.base64"));
    d3d12_import_bridge::EnsureImportSandboxPrepared();
    const auto result = d3d12_import_bridge::RunImport(
        d3d12_import_bridge::SourceFormat::Fbx, source.path.wstring(), 5001);
    CAPTURE(result.errorStage, result.errorCode, result.errorPhase);
    REQUIRE(result.ok);
    CHECK(result.scene.format == model_core::SourceFormatId::Fbx);
    CHECK_FALSE(result.meshes.empty());
    CHECK_FALSE(result.nodes.empty());
    CHECK_FALSE(result.instances.empty());
}

TEST_CASE("FBX decodes an embedded PNG without a filesystem sidecar", "[fbx][materials][embedded-image]")
{
    // Upstream's deliberately small base64-media fixture includes an absolute
    // filename as metadata. Successful import here proves the worker consumes
    // ufbx's embedded blob instead of following that filename.
    ScratchFbx source;
    source.Write(ReadFixture("embedded-png-ascii.fbx"));
    const auto result = import_broker::RunImportSession(Request(source.path, 5011));
    CAPTURE(result.stage, result.errorCode, result.errorPhase, result.chunks.size());
    REQUIRE(result.ok);

    uint32_t baseColorImage = 0;
    uint32_t decodedImage = 0;
    bool statusWarning = false;
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology == model_core::ChunkTopology::Material) {
            REQUIRE(chunk.descriptor.dependencyCount == 1);
            baseColorImage = chunk.descriptor.dependencyIds[0];
            CHECK(baseColorImage != 0);
            CHECK(chunk.descriptor.dependencyIds[1] == 0);
            CHECK(chunk.descriptor.dependencyIds[2] == 0);
            CHECK(chunk.descriptor.dependencyIds[3] == 0);
        } else if (chunk.descriptor.topology == model_core::ChunkTopology::Image) {
            decodedImage = chunk.descriptor.chunkId;
            REQUIRE(chunk.payload.size() >= sizeof(model_core::ImagePayloadHeader));
            model_core::ImagePayloadHeader header{};
            std::memcpy(&header, chunk.payload.data(), sizeof(header));
            CHECK(header.pixelFormat == uint32_t(model_core::PixelFormatId::RGBA8_UNORM));
            CHECK(header.colorSpace == uint32_t(model_core::ColorSpaceId::Srgb));
            CHECK(header.width == 32);
            CHECK(header.height == 32);
            CHECK(header.mipLevels == model_core::FullImageMipCount(32, 32));
            CHECK(header.pixelDataByteSize == chunk.payload.size() - sizeof(header));
        } else if (chunk.descriptor.topology == model_core::ChunkTopology::ImportStatus) {
            statusWarning = true;
        }
    }
    CHECK(baseColorImage != 0);
    CHECK(decodedImage == baseColorImage);
    CHECK_FALSE(statusWarning);
}

TEST_CASE("FBX decodes embedded JPEG and WebP with normalized color semantics",
          "[fbx][materials][embedded-image]")
{
    struct Case {
        std::filesystem::path asset;
        const char* relativePath;
        uint32_t width;
        uint32_t height;
    } cases[]{
        {L"textures/gray.jpg", "embedded.jpg", 8, 8},
        {L"corpus/sample.webp", "embedded.webp", 1, 1},
    };
    uint64_t generation = 5012;
    for (const auto& test : cases) {
        const auto encoded = ReadAsset(test.asset);
        ScratchFbx source;
        source.Write(TextureFixture(encoded, test.relativePath));
        const auto result = import_broker::RunImportSession(Request(source.path, generation++));
        CAPTURE(test.relativePath, result.stage, result.errorCode, result.errorPhase);
        REQUIRE(result.ok);
        REQUIRE(Count(result, model_core::ChunkTopology::Image) == 1);
        for (const auto& chunk : result.chunks) {
            if (chunk.descriptor.topology != model_core::ChunkTopology::Image) continue;
            model_core::ImagePayloadHeader header{};
            std::memcpy(&header, chunk.payload.data(), sizeof(header));
            CHECK(header.pixelFormat == uint32_t(model_core::PixelFormatId::RGBA8_UNORM));
            CHECK(header.colorSpace == uint32_t(model_core::ColorSpaceId::Srgb));
            CHECK(header.width == test.width);
            CHECK(header.height == test.height);
            CHECK(header.mipLevels == model_core::FullImageMipCount(test.width, test.height));
            REQUIRE(chunk.payload.size() >= sizeof(header) + 4);
            const auto* pixel = chunk.payload.data() + sizeof(header);
            if (test.width == 8) {
                for (uint32_t channel = 0; channel < 3; ++channel)
                    CHECK(std::abs(int(std::to_integer<uint8_t>(pixel[channel])) - 128) <= 2);
                CHECK(pixel[3] == std::byte{255});
            } else {
                CHECK(pixel[3] == std::byte{0});
            }
        }
        CHECK(Count(result, model_core::ChunkTopology::ImportStatus) == 0);
    }
}

TEST_CASE("FBX resolves local image sidecars only through the trusted broker",
          "[fbx][materials][sidecar]")
{
    const auto jpeg = ReadAsset(L"textures/gray.jpg");
    ScratchFbx source;
    source.Write(TextureFixture({}, "sidecar.jpg"));
    source.WriteSidecar(L"sidecar.jpg", jpeg);
    const auto result = import_broker::RunImportSession(Request(source.path, 5014));
    CAPTURE(result.stage, result.errorCode, result.errorPhase);
    REQUIRE(result.ok);
    REQUIRE(Count(result, model_core::ChunkTopology::Image) == 1);
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology != model_core::ChunkTopology::Image) continue;
        model_core::ImagePayloadHeader header{};
        std::memcpy(&header, chunk.payload.data(), sizeof(header));
        CHECK(header.width == 8);
        CHECK(header.height == 8);
        CHECK(header.colorSpace == uint32_t(model_core::ColorSpaceId::Srgb));
    }
    CHECK(Count(result, model_core::ChunkTopology::ImportStatus) == 0);
}

TEST_CASE("FBX sidecar byte caps preserve geometry through optional fallback",
          "[fbx][materials][sidecar][limits]")
{
    const auto jpeg = ReadAsset(L"textures/gray.jpg");
    ScratchFbx source;
    source.Write(TextureFixture({}, "sidecar.jpg"));
    source.WriteSidecar(L"sidecar.jpg", jpeg);
    auto request = Request(source.path, 50145);
    request.maxSidecarFileBytes = jpeg.size() - 1;
    const auto result = import_broker::RunImportSession(request);
    CAPTURE(result.stage, result.errorCode, result.errorPhase);
    REQUIRE(result.ok);
    REQUIRE(Count(result, model_core::ChunkTopology::Image) == 1);
    REQUIRE(Count(result, model_core::ChunkTopology::ImportStatus) == 1);
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology == model_core::ChunkTopology::Image) {
            model_core::ImagePayloadHeader header{};
            std::memcpy(&header, chunk.payload.data(), sizeof(header));
            CHECK(header.width == 2);
            CHECK(header.height == 2);
        } else if (chunk.descriptor.topology == model_core::ChunkTopology::ImportStatus) {
            model_core::ImportStatusPayload status{};
            std::memcpy(&status, chunk.payload.data(), sizeof(status));
            CHECK(status.textureWarnings == 1);
        }
    }
}

TEST_CASE("FBX missing and corrupt optional textures use a deterministic bounded fallback",
          "[fbx][materials][recovery]")
{
    struct Case { const char* path; bool writeCorrupt; } cases[]{
        {"missing.png", false},
        {"corrupt.png", true},
    };
    uint64_t generation = 5015;
    for (const auto& test : cases) {
        ScratchFbx source;
        source.Write(TextureFixture({}, test.path));
        if (test.writeCorrupt) {
            const std::array corrupt{std::byte{'B'}, std::byte{'A'}, std::byte{'D'}};
            source.WriteSidecar(test.path, corrupt);
        }
        const auto result = import_broker::RunImportSession(Request(source.path, generation++));
        CAPTURE(test.path, result.stage, result.errorCode, result.errorPhase);
        REQUIRE(result.ok);
        REQUIRE(Count(result, model_core::ChunkTopology::Image) == 1);
        REQUIRE(Count(result, model_core::ChunkTopology::ImportStatus) == 1);
        for (const auto& chunk : result.chunks) {
            if (chunk.descriptor.topology == model_core::ChunkTopology::Image) {
                model_core::ImagePayloadHeader header{};
                std::memcpy(&header, chunk.payload.data(), sizeof(header));
                CHECK(header.width == 2);
                CHECK(header.height == 2);
                CHECK(header.mipLevels == 1);
            } else if (chunk.descriptor.topology == model_core::ChunkTopology::ImportStatus) {
                model_core::ImportStatusPayload status{};
                std::memcpy(&status, chunk.payload.data(), sizeof(status));
                CHECK(status.textureWarnings == 1);
            }
        }
    }
}

TEST_CASE("FBX unsafe external texture references fail closed",
          "[fbx][materials][sidecar][security]")
{
    const std::array<std::string_view, 5> attacks{
        "../outside.png",
        "C:/outside.png",
        "//server/share/outside.png",
        "https://example.invalid/outside.png",
        "inside.png:stream",
    };
    uint64_t generation = 5017;
    for (const auto attack : attacks) {
        ScratchFbx source;
        source.Write(TextureFixture({}, attack));
        const auto result = import_broker::RunImportSession(Request(source.path, generation++));
        CAPTURE(attack, result.stage, result.errorCode, result.errorPhase);
        REQUIRE_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::UnsafeReference);
        CHECK(result.errorPhase == model_core::ImportFailurePhase::Sidecars);
    }
}

TEST_CASE("FBX maps factors alpha texture roles emissive and UV transform",
          "[fbx][materials][pbr]")
{
    const auto jpeg = ReadAsset(L"textures/gray.jpg");
    ScratchFbx source;
    source.Write(MaterialTextureFixture(jpeg));
    const auto result = import_broker::RunImportSession(Request(source.path, 5022));
    CAPTURE(result.stage, result.errorCode, result.errorPhase);
    REQUIRE(result.ok);
    CHECK(Count(result, model_core::ChunkTopology::Image) == 3);
    REQUIRE(Count(result, model_core::ChunkTopology::Material) == 1);
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology != model_core::ChunkTopology::Material) continue;
        model_core::MaterialPayload material{};
        std::memcpy(&material, chunk.payload.data(), sizeof(material));
        CHECK(material.baseColorFactor[0] == Catch::Approx(0.2f));
        CHECK(material.baseColorFactor[1] == Catch::Approx(0.4f));
        CHECK(material.baseColorFactor[2] == Catch::Approx(0.6f));
        CHECK(material.baseColorFactor[3] == Catch::Approx(0.75f));
        CHECK(material.emissiveFactor[0] == Catch::Approx(0.2f));
        CHECK(material.emissiveFactor[1] == Catch::Approx(0.4f));
        CHECK(material.emissiveFactor[2] == Catch::Approx(0.6f));
        CHECK(material.alphaMode == uint32_t(model_core::AlphaModeId::Mask));
        CHECK(material.alphaCutoff == Catch::Approx(0.3f));
        CHECK(material.uvOffset[0] == Catch::Approx(0.25f));
        CHECK(material.uvOffset[1] == Catch::Approx(0.5f));
        CHECK(material.uvScale[0] == Catch::Approx(2.0f));
        CHECK(material.uvScale[1] == Catch::Approx(3.0f));
        CHECK(material.uvRotation == Catch::Approx(0.5235988f));
        CHECK(chunk.descriptor.dependencyCount == 3);
        CHECK(chunk.descriptor.dependencyIds[0] != 0);
        CHECK(chunk.descriptor.dependencyIds[1] == 0);
        CHECK(chunk.descriptor.dependencyIds[2] != 0);
        CHECK(chunk.descriptor.dependencyIds[3] != 0);
    }
}

TEST_CASE("FBX bump texture is the normal-map fallback", "[fbx][materials][pbr]")
{
    const auto webp = ReadAsset(L"corpus/sample.webp");
    std::string ascii = MaterialTextureFixture(webp);
    ReplaceOnce(ascii, "\"NormalMap\"", "\"Bump\"");
    ScratchFbx source;
    source.Write(ascii);
    const auto result = import_broker::RunImportSession(Request(source.path, 5023));
    CAPTURE(result.stage, result.errorCode, result.errorPhase);
    REQUIRE(result.ok);
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology != model_core::ChunkTopology::Material) continue;
        CHECK(chunk.descriptor.dependencyIds[2] != 0);
    }
}

TEST_CASE("FBX material and image dependencies remain valid across progressive batches",
          "[fbx][materials][progressive]")
{
    const auto webp = ReadAsset(L"corpus/sample.webp");
    ScratchFbx source;
    source.Write(MaterialTextureFixture(webp));
    auto request = Request(source.path, 5024);
    request.sectionByteCapacity = 4096;
    request.maxChunkCount = 4;
    const auto result = import_broker::RunImportSession(request);
    CAPTURE(result.stage, result.errorCode, result.errorPhase, result.batchCount);
    REQUIRE(result.ok);
    CHECK(result.batchCount > 1);
    CHECK(Count(result, model_core::ChunkTopology::Material) == 1);
    CHECK(Count(result, model_core::ChunkTopology::Image) == 3);
}

TEST_CASE("FBX instances select distinct materials without duplicating shared geometry",
          "[fbx][materials][instances]")
{
    ScratchFbx source;
    source.Write(ReadFixture("instanced-materials-ascii.fbx"));
    const auto result = import_broker::RunImportSession(Request(source.path, 5025));
    CAPTURE(result.stage, result.errorCode, result.errorPhase);
    REQUIRE(result.ok);
    REQUIRE(Count(result, model_core::ChunkTopology::TriangleList) == 1);
    REQUIRE(Count(result, model_core::ChunkTopology::MeshInstance) == 3);
    REQUIRE(Count(result, model_core::ChunkTopology::Material) == 3);

    uint32_t geometryId = 0;
    std::vector<uint32_t> materialIds;
    std::vector<uint32_t> instanceMaterialIds;
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology == model_core::ChunkTopology::TriangleList) {
            geometryId = chunk.descriptor.chunkId;
        } else if (chunk.descriptor.topology == model_core::ChunkTopology::Material) {
            materialIds.push_back(chunk.descriptor.chunkId);
        } else if (chunk.descriptor.topology == model_core::ChunkTopology::MeshInstance) {
            model_core::MeshInstancePayload instance{};
            std::memcpy(&instance, chunk.payload.data(), sizeof(instance));
            CHECK(instance.geometryChunkId == geometryId);
            CHECK(instance.materialChunkId != 0);
            instanceMaterialIds.push_back(instance.materialChunkId);
        }
    }
    std::sort(materialIds.begin(), materialIds.end());
    std::sort(instanceMaterialIds.begin(), instanceMaterialIds.end());
    CHECK(instanceMaterialIds == materialIds);
}

TEST_CASE("FBX aggregate texture pressure is typed and pooled recovery remains usable",
          "[fbx][materials][limits][recovery][pool]")
{
    import_broker::PrepareImportWorkerPoolAsync(sandbox_test_support::WorkerExePath());
    const auto jpeg = ReadAsset(L"textures/gray.jpg");
    ScratchFbx pressured;
    pressured.Write(MaterialTextureFixture(jpeg));
    auto request = Request(pressured.path, 5026);
    request.useWorkerPool = true;
    request.fbxTinyTextureLimitForTesting = true;
    auto result = import_broker::RunImportSession(request);
    CAPTURE(result.stage, result.errorCode, result.errorPhase);
    REQUIRE_FALSE(result.ok);
    CHECK(result.errorCode == model_core::ImportErrorCode::ResourceLimit);
    CHECK(result.errorPhase == model_core::ImportFailurePhase::Sidecars);
    const uint32_t worker = result.workerProcessId;

    ScratchFbx valid;
    valid.Write(ReadFixture("cube-binary.fbx.base64"));
    auto recovery = Request(valid.path, 5027);
    recovery.useWorkerPool = true;
    result = import_broker::RunImportSession(recovery);
    CAPTURE(result.stage, result.errorCode, result.errorPhase);
    REQUIRE(result.ok);
    CHECK(result.workerProcessId == worker);
}

TEST_CASE("FBX ambiguous layered textures preserve visible geometry with bounded warnings",
          "[fbx][materials][layered][recovery]")
{
    ScratchFbx source;
    source.Write(ReadFixture("layered-textures-ascii.fbx"));
    const auto result = import_broker::RunImportSession(Request(source.path, 5028));
    CAPTURE(result.stage, result.errorCode, result.errorPhase);
    REQUIRE(result.ok);
    CHECK(Count(result, model_core::ChunkTopology::TriangleList) > 0);
    CHECK(Count(result, model_core::ChunkTopology::Material) == 1);
    REQUIRE(Count(result, model_core::ChunkTopology::ImportStatus) == 1);
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology != model_core::ChunkTopology::ImportStatus) continue;
        model_core::ImportStatusPayload status{};
        std::memcpy(&status, chunk.payload.data(), sizeof(status));
        CHECK(status.optionalFeatureWarnings > 0);
        CHECK(status.optionalFeatureWarnings <= 64);
        CHECK(status.textureWarnings <= 64);
    }
}

TEST_CASE("Binary FBX triangulates and emits generated normalized attributes",
          "[fbx][binary][triangulation][normals]")
{
    ScratchFbx source;
    source.Write(ReadFixture("cube-binary.fbx.base64"));
    const auto result = import_broker::RunImportSession(Request(source.path, 502));
    CAPTURE(result.stage, result.errorCode, result.chunks.size());
    REQUIRE(result.ok);
    uint64_t triangles = 0;
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology != model_core::ChunkTopology::TriangleList) continue;
        CHECK(chunk.descriptor.vertexLayoutId == uint32_t(
            model_core::VertexLayoutId::PositionNormalUv0TangentColor_F32));
        CHECK((chunk.descriptor.geometryFlags & model_core::kGeometryReusableInstanceSource) != 0);
        triangles += chunk.descriptor.indexCount / 3;
        const auto* vertices = reinterpret_cast<const model_core::VertexPositionNormalUv0TangentColorF32*>(
            chunk.payload.data());
        for (uint32_t index = 0; index < chunk.descriptor.vertexCount; ++index) {
            const float length = std::sqrt(vertices[index].nx * vertices[index].nx
                                           + vertices[index].ny * vertices[index].ny
                                           + vertices[index].nz * vertices[index].nz);
            CHECK(length > 0.99f);
            CHECK(length < 1.01f);
        }
    }
    CHECK(triangles == 12);
}

TEST_CASE("FBX preserves UV and vertex-color attributes", "[fbx][attributes][color][uv]")
{
    std::string ascii = ReadAsciiFixture("hierarchy-instances-pivots-ascii.fbx");
    ReplaceOnce(ascii, "\t\tLayerElementMaterial: 0 {",
        "\t\tLayerElementColor: 0 {\n"
        "\t\t\tVersion: 101\n\t\t\tName: \"displayColor\"\n"
        "\t\t\tMappingInformationType: \"ByPolygonVertex\"\n"
        "\t\t\tReferenceInformationType: \"Direct\"\n"
        "\t\t\tColors: *96 {\n"
        "\t\t\t\ta: 1,0,0,1,0,1,0,1,0,0,1,1,1,1,0,1,"
        "1,0,1,1,0,1,1,1,0.25,0.5,0.75,1,1,0.5,0.25,1,"
        "1,0,0,1,0,1,0,1,0,0,1,1,1,1,0,1,"
        "1,0,1,1,0,1,1,1,0.25,0.5,0.75,1,1,0.5,0.25,1,"
        "1,0,0,1,0,1,0,1,0,0,1,1,1,1,0,1,"
        "1,0,1,1,0,1,1,1,0.25,0.5,0.75,1,1,0.5,0.25,1\n"
        "\t\t\t}\n\t\t}\n"
        "\t\tLayerElementMaterial: 0 {");
    ReplaceOnce(ascii,
        "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementUV\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n",
        "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementUV\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n"
        "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementColor\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n");
    ScratchFbx source;
    source.Write(ascii);
    const auto result = import_broker::RunImportSession(Request(source.path, 509));
    CAPTURE(result.stage, result.errorCode);
    REQUIRE(result.ok);
    bool foundAttributes = false, foundNonWhite = false;
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology != model_core::ChunkTopology::TriangleList) continue;
        foundAttributes |= (chunk.descriptor.geometryFlags & model_core::kGeometryHasUv0)
            && (chunk.descriptor.geometryFlags & model_core::kGeometryHasColors);
        const auto* vertices = reinterpret_cast<const model_core::VertexPositionNormalUv0TangentColorF32*>(
            chunk.payload.data());
        for (uint32_t index = 0; index < chunk.descriptor.vertexCount; ++index)
            foundNonWhite |= vertices[index].r != 1.0f || vertices[index].g != 1.0f
                || vertices[index].b != 1.0f;
    }
    CHECK(foundAttributes);
    CHECK(foundNonWhite);
}

TEST_CASE("FBX normalizes authored axes units pivots and mirrored transforms",
          "[fbx][axes][units][transforms]")
{
    struct Fixture { const char* name; bool binary; } fixtures[]{
        {"z-up-binary.fbx.base64", true},
        {"negative-scale-pivots-ascii.fbx", false},
    };
    for (const auto& fixture : fixtures) {
        ScratchFbx source;
        source.Write(ReadFixture(fixture.name));
        const auto result = import_broker::RunImportSession(Request(source.path, 510));
        CAPTURE(fixture.name, result.stage, result.errorCode);
        REQUIRE(result.ok);
        bool nonIdentity = false, mirrored = false;
        for (const auto& chunk : result.chunks) {
            CHECK(chunk.scene.upAxis == model_core::UpAxisId::Y);
            CHECK(chunk.scene.metersPerUnit == 1.0);
            if (chunk.descriptor.topology != model_core::ChunkTopology::Node) continue;
            model_core::NodePayload node{};
            std::memcpy(&node, chunk.payload.data(), sizeof(node));
            nonIdentity |= node.localTransform[0] != 1.0 || node.localTransform[5] != 1.0
                || node.localTransform[10] != 1.0 || node.localTransform[12] != 0.0
                || node.localTransform[13] != 0.0 || node.localTransform[14] != 0.0;
            const double determinant = node.localTransform[0]
                * (node.localTransform[5] * node.localTransform[10]
                   - node.localTransform[6] * node.localTransform[9])
                - node.localTransform[1]
                * (node.localTransform[4] * node.localTransform[10]
                   - node.localTransform[6] * node.localTransform[8])
                + node.localTransform[2]
                * (node.localTransform[4] * node.localTransform[9]
                   - node.localTransform[5] * node.localTransform[8]);
            mirrored |= determinant < 0.0;
        }
        CHECK(nonIdentity);
        if (!fixture.binary) CHECK(mirrored);
    }
}

TEST_CASE("FBX geometry crosses small shared sections in progressive batches", "[fbx][progressive]")
{
    ScratchFbx source;
    source.Write(ReadFixture("hierarchy-instances-pivots-ascii.fbx"));
    auto request = Request(source.path, 503);
    request.sectionByteCapacity = 4096;
    request.maxChunkCount = 8;
    const auto result = import_broker::RunImportSession(request);
    CAPTURE(result.stage, result.errorCode, result.batchCount, result.chunks.size());
    REQUIRE(result.ok);
    CHECK(result.batchCount > 1);
    CHECK(Count(result, model_core::ChunkTopology::MeshInstance) > 1);
}

TEST_CASE("FBX deterministic static pose bakes skin and blend deformation",
          "[fbx][deformation][golden]")
{
    struct Golden {
        const char* fixture;
        uint64_t hash;
        uint64_t triangles;
        uint64_t vertices;
        uint32_t nodes;
        uint32_t instances;
        uint32_t geometryChunks;
        uint32_t animations;
        uint32_t skins;
        uint32_t bones;
        uint32_t warnings;
    };
    const Golden goldens[]{
        {"linear-skin-binary.fbx.base64", 0x398380dd84bbda31ull,
            2, 6, 5, 1, 1, 0, 1, 2, 0},
        {"dual-quaternion-ascii.fbx", 0x8c89d1367ba7bf59ull,
            84, 252, 5, 1, 1, 1, 1, 3, 0},
        {"blended-skin-binary.fbx.base64", 0xa60df3590d6b687aull,
            176, 528, 7, 1, 1, 1, 1, 5, 0},
        {"shape-animation-binary.fbx.base64", 0xf337a4a546728ecdull,
            12, 36, 2, 1, 1, 1, 0, 0, 0},
        {"combined-skin-blend-ascii.fbx", 0x9dad0b2ea1cba400ull,
            176, 528, 15, 5, 5, 1, 1, 4, 1},
    };
    uint64_t generation = 520;
    for (const auto& golden : goldens) {
        ScratchFbx source;
        source.Write(ReadFixture(golden.fixture));
        const auto result = import_broker::RunImportSession(Request(source.path, generation++));
        CAPTURE(golden.fixture, result.stage, result.errorCode);
        const WireSnapshot snapshot = Snapshot(result);
        CAPTURE(snapshot.hash, snapshot.triangles, snapshot.vertices, snapshot.nodes,
                snapshot.instances, snapshot.geometryChunks, snapshot.scene.animationCount,
                snapshot.scene.skinCount, snapshot.scene.boneCount, snapshot.warnings);
        CHECK(snapshot.hash == golden.hash);
        CHECK(snapshot.triangles == golden.triangles);
        CHECK(snapshot.vertices == golden.vertices);
        CHECK(snapshot.nodes == golden.nodes);
        CHECK(snapshot.instances == golden.instances);
        CHECK(snapshot.geometryChunks == golden.geometryChunks);
        CHECK(snapshot.scene.animationCount == golden.animations);
        CHECK(snapshot.scene.skinCount == golden.skins);
        CHECK(snapshot.scene.boneCount == golden.bones);
        CHECK(snapshot.warnings == golden.warnings);
    }
}

TEST_CASE("FBX first-stack start and rest fallback are deterministic numeric poses",
          "[fbx][pose][golden]")
{
    struct Golden { const char* fixture; uint64_t hash; uint32_t animations; };
    const Golden goldens[]{
        {"multiple-stacks-ascii.fbx", 0xcb59db2242f2b906ull, 3},
        {"cube-binary.fbx.base64", 0x7cc5b538fdd7df79ull, 0},
    };
    uint64_t generation = 530;
    for (const auto& golden : goldens) {
        ScratchFbx source;
        source.Write(ReadFixture(golden.fixture));
        const WireSnapshot first = Snapshot(import_broker::RunImportSession(
            Request(source.path, generation++)));
        const WireSnapshot second = Snapshot(import_broker::RunImportSession(
            Request(source.path, generation++)));
        CAPTURE(golden.fixture, first.hash, first.scene.animationCount);
        CHECK(first.hash == golden.hash);
        CHECK(first.scene.animationCount == golden.animations);
        CHECK(second.hash == first.hash);
    }
}

TEST_CASE("FBX evaluated geometry shares only when deformed results are equivalent",
          "[fbx][deformation][instances]")
{
    struct Case { double translationX; uint32_t geometry; } cases[]{
        {0.0, 1},
        {2.0, 2},
    };
    uint64_t generation = 540;
    for (const auto& test : cases) {
        ScratchFbx source;
        source.Write(MakeInstancedDualQuaternion(test.translationX));
        const auto result = import_broker::RunImportSession(Request(source.path, generation++));
        CAPTURE(test.translationX, result.stage, result.errorCode);
        REQUIRE(result.ok);
        CHECK(Count(result, model_core::ChunkTopology::TriangleList) == test.geometry);
        CHECK(Count(result, model_core::ChunkTopology::MeshInstance) == 2);
    }
}

TEST_CASE("FBX required unsupported geometry fails while evaluator exhaustion recovers",
          "[fbx][deformation][limits][recovery]")
{
    ScratchFbx nurbs;
    nurbs.Write(ReadFixture("nurbs-only-ascii.fbx"));
    auto result = import_broker::RunImportSession(Request(nurbs.path, 550));
    REQUIRE_FALSE(result.ok);
    CHECK(result.errorCode == model_core::ImportErrorCode::UnsupportedRequiredFeature);
    CHECK(result.errorPhase == model_core::ImportFailurePhase::Geometry);

    import_broker::PrepareImportWorkerPoolAsync(sandbox_test_support::WorkerExePath());
    ScratchFbx deformed;
    deformed.Write(ReadFixture("combined-skin-blend-ascii.fbx"));
    auto exhausted = Request(deformed.path, 551);
    exhausted.useWorkerPool = true;
    exhausted.fbxTinyEvaluationLimitForTesting = true;
    result = import_broker::RunImportSession(exhausted);
    REQUIRE_FALSE(result.ok);
    CHECK(result.errorCode == model_core::ImportErrorCode::ScratchLimit);
    CHECK(result.errorPhase == model_core::ImportFailurePhase::Geometry);
    const uint32_t worker = result.workerProcessId;

    ScratchFbx valid;
    valid.Write(ReadFixture("cube-binary.fbx.base64"));
    auto recovered = Request(valid.path, 552);
    recovered.useWorkerPool = true;
    result = import_broker::RunImportSession(recovered);
    CAPTURE(result.stage, result.errorCode);
    REQUIRE(result.ok);
    CHECK(result.workerProcessId == worker);
}

TEST_CASE("Malformed and cancelled FBX requests do not poison pooled recovery", "[fbx][recovery][pool]")
{
    import_broker::PrepareImportWorkerPoolAsync(sandbox_test_support::WorkerExePath());
    ScratchFbx malformed;
    auto truncated = ReadFixture("cube-binary.fbx.base64");
    truncated.resize(truncated.size() / 2);
    malformed.Write(truncated);
    auto badRequest = Request(malformed.path, 505);
    badRequest.useWorkerPool = true;
    auto result = import_broker::RunImportSession(badRequest);
    REQUIRE_FALSE(result.ok);
    CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    const uint32_t worker = result.workerProcessId;

    ScratchFbx progressive;
    progressive.Write(ReadFixture("hierarchy-instances-pivots-ascii.fbx"));
    std::atomic_bool cancelled = false;
    auto cancelledRequest = Request(progressive.path, 506);
    cancelledRequest.useWorkerPool = true;
    cancelledRequest.sectionByteCapacity = 4096;
    cancelledRequest.maxChunkCount = 8;
    cancelledRequest.isCancelled = [&] { return cancelled.load(); };
    cancelledRequest.onBatch = [&](std::vector<import_broker::ValidatedChunk>&& chunks) {
        REQUIRE_FALSE(chunks.empty());
        cancelled.store(true);
    };
    result = import_broker::RunImportSession(cancelledRequest);
    REQUIRE_FALSE(result.ok);
    CHECK(result.errorCode == model_core::ImportErrorCode::Cancelled);
    CHECK(result.workerProcessId == worker);

    ScratchFbx valid;
    valid.Write(ReadFixture("cube-binary.fbx.base64"));
    auto validRequest = Request(valid.path, 507);
    validRequest.useWorkerPool = true;
    result = import_broker::RunImportSession(validRequest);
    CAPTURE(result.stage, result.errorCode);
    REQUIRE(result.ok);
    CHECK(result.workerProcessId == worker);
}

TEST_CASE("FBX parser is forced and cannot be spoofed by extension", "[fbx][security][format]")
{
    ScratchFbx source;
    source.Write("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    const auto result = import_broker::RunImportSession(Request(source.path, 508));
    REQUIRE_FALSE(result.ok);
    CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
}

TEST_CASE("FBX hierarchy depth is bounded before normalization", "[fbx][limits][depth]")
{
    std::string ascii = ReadAsciiFixture("hierarchy-instances-pivots-ascii.fbx");
    ReplaceOnce(ascii, "Definitions:  {\n\tVersion: 100\n\tCount: 9",
                       "Definitions:  {\n\tVersion: 100\n\tCount: 269");
    ReplaceOnce(ascii, "\tObjectType: \"Model\" {\n\t\tCount: 3",
                       "\tObjectType: \"Model\" {\n\t\tCount: 263");
    std::string models, connections;
    uint64_t parent = 0;
    for (uint64_t index = 0; index < 260; ++index) {
        const uint64_t id = 8'000'000 + index;
        models += "\tModel: " + std::to_string(id) + ", \"Model::deep" + std::to_string(index)
            + "\", \"Null\" {\n\t\tVersion: 232\n\t}\n";
        connections += "\tC: \"OO\"," + std::to_string(id) + "," + std::to_string(parent) + "\n";
        parent = id;
    }
    ReplaceOnce(ascii, "\n}\n\n; Object connections", "\n" + models + "}\n\n; Object connections");
    ReplaceOnce(ascii, "\n}\n;Takes section", "\n" + connections + "}\n;Takes section");
    ScratchFbx source;
    source.Write(ascii);
    const auto result = import_broker::RunImportSession(Request(source.path, 511));
    REQUIRE_FALSE(result.ok);
    CHECK(result.errorCode == model_core::ImportErrorCode::ResourceLimit);
}

TEST_CASE("FBX node counts are bounded before scene-record allocation", "[fbx][limits][count]")
{
    std::string ascii = ReadAsciiFixture("hierarchy-instances-pivots-ascii.fbx");
    ReplaceOnce(ascii, "Definitions:  {\n\tVersion: 100\n\tCount: 9",
                       "Definitions:  {\n\tVersion: 100\n\tCount: 50009");
    ReplaceOnce(ascii, "\tObjectType: \"Model\" {\n\t\tCount: 3",
                       "\tObjectType: \"Model\" {\n\t\tCount: 50003");
    std::string models;
    models.reserve(4 * 1024 * 1024);
    for (uint64_t index = 0; index < 50'000; ++index) {
        const uint64_t id = 9'000'000 + index;
        models += "\tModel: " + std::to_string(id) + ", \"Model::bounded\", \"Null\" {\n"
            "\t\tVersion: 232\n\t}\n";
    }
    ReplaceOnce(ascii, "\n}\n\n; Object connections", "\n" + models + "}\n\n; Object connections");
    ScratchFbx source;
    source.Write(ascii);
    const auto result = import_broker::RunImportSession(Request(source.path, 512));
    REQUIRE_FALSE(result.ok);
    CHECK(result.errorCode == model_core::ImportErrorCode::ResourceLimit);
}
