#include "SandboxTestSupport.h"
#include "import_broker/ImportSession.h"
#include "import_broker/SharedSection.h"
#include "import_broker/SharedSectionValidator.h"
#include "import_broker/UsdFallbackState.h"
#include "model_core/Checksum.h"
#include "model_core/ControlProtocol.h"
#include "model_core/MaterialPayload.h"
#include "model_core/PixelFormats.h"
#include "model_core/WireFormat.h"
#include "model_core/VertexLayouts.h"
#include "platform/Sha256.h"
#include "platform/Win32Handle.h"
#include "UsdZipPreflight.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>

namespace {

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
    const auto path = std::filesystem::path(PREVIEW3D_USD_FIXTURES_DIR) / name;
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    std::vector<char> raw((std::istreambuf_iterator<char>(input)), {});
    if (name.ends_with(".base64")) return DecodeBase64(raw);
    const auto bytes = std::as_bytes(std::span(raw));
    return {bytes.begin(), bytes.end()};
}

std::string Sha256Hex(std::span<const std::byte> bytes)
{
    const auto digest = platform::ComputeSha256(bytes);
    if (!digest) return {};
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest->size() * 2);
    for (const std::byte byte : *digest) {
        const auto value = std::to_integer<uint8_t>(byte);
        result.push_back(digits[value >> 4]);
        result.push_back(digits[value & 0x0f]);
    }
    return result;
}

size_t FindSignature(std::span<const std::byte> bytes, uint32_t signature)
{
    for (size_t offset = 0; offset + sizeof(signature) <= bytes.size(); ++offset) {
        uint32_t candidate = 0;
        std::memcpy(&candidate, bytes.data() + offset, sizeof(candidate));
        if (candidate == signature) return offset;
    }
    return std::string::npos;
}

void Put16(std::span<std::byte> bytes, size_t offset, uint16_t value)
{
    REQUIRE(offset + sizeof(value) <= bytes.size());
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

uint32_t Crc32(std::span<const std::byte> bytes)
{
    uint32_t crc = 0xffffffffu;
    for (const std::byte byte : bytes) {
        crc ^= std::to_integer<uint8_t>(byte);
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void Append16(std::vector<std::byte>& bytes, uint16_t value)
{
    const auto encoded = std::as_bytes(std::span(&value, 1));
    bytes.insert(bytes.end(), encoded.begin(), encoded.end());
}

void Append32(std::vector<std::byte>& bytes, uint32_t value)
{
    const auto encoded = std::as_bytes(std::span(&value, 1));
    bytes.insert(bytes.end(), encoded.begin(), encoded.end());
}

std::vector<std::byte> StoredUsdz(
    std::span<const std::pair<std::string, std::vector<std::byte>>> entries)
{
    struct Central { std::string name; uint32_t crc = 0, size = 0, offset = 0; };
    std::vector<std::byte> result;
    std::vector<Central> central;
    for (const auto& [name, payload] : entries) {
        Central record{name, Crc32(payload), uint32_t(payload.size()), uint32_t(result.size())};
        const uint16_t extra = uint16_t((64 - ((result.size() + 30 + name.size()) & 63)) & 63);
        Append32(result, 0x04034b50u); Append16(result, 20); Append16(result, 0);
        Append16(result, 0); Append16(result, 0); Append16(result, 0);
        Append32(result, record.crc); Append32(result, record.size); Append32(result, record.size);
        Append16(result, uint16_t(name.size())); Append16(result, extra);
        const auto nameBytes = std::as_bytes(std::span(name));
        result.insert(result.end(), nameBytes.begin(), nameBytes.end());
        result.resize(result.size() + extra);
        REQUIRE((result.size() & 63) == 0);
        result.insert(result.end(), payload.begin(), payload.end());
        central.push_back(std::move(record));
    }
    const uint32_t centralOffset = uint32_t(result.size());
    for (const Central& record : central) {
        Append32(result, 0x02014b50u); Append16(result, 20); Append16(result, 20);
        Append16(result, 0); Append16(result, 0); Append16(result, 0); Append16(result, 0);
        Append32(result, record.crc); Append32(result, record.size); Append32(result, record.size);
        Append16(result, uint16_t(record.name.size())); Append16(result, 0); Append16(result, 0);
        Append16(result, 0); Append16(result, 0); Append32(result, 0); Append32(result, record.offset);
        const auto nameBytes = std::as_bytes(std::span(record.name));
        result.insert(result.end(), nameBytes.begin(), nameBytes.end());
    }
    const uint32_t centralSize = uint32_t(result.size()) - centralOffset;
    Append32(result, 0x06054b50u); Append16(result, 0); Append16(result, 0);
    Append16(result, uint16_t(central.size())); Append16(result, uint16_t(central.size()));
    Append32(result, centralSize); Append32(result, centralOffset); Append16(result, 0);
    return result;
}

struct ScratchUsd {
    std::filesystem::path directory;
    std::filesystem::path path;

    explicit ScratchUsd(std::wstring_view extension)
    {
        static std::atomic_uint64_t next{0};
        directory = std::filesystem::temp_directory_path()
            / (L"Preview3D-usd003-" + std::to_wstring(GetCurrentProcessId()) + L"-"
               + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(next.fetch_add(1)));
        REQUIRE(std::filesystem::create_directory(directory));
        path = directory / (L"model." + std::wstring(extension));
    }
    ~ScratchUsd()
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
    void WriteSidecar(std::wstring_view name, std::span<const std::byte> bytes)
    {
        std::ofstream output(directory / std::wstring(name), std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        REQUIRE(output.good());
    }
};

import_broker::ImportSessionRequest Request(const std::filesystem::path& path, uint64_t generation)
{
    import_broker::ImportSessionRequest request;
    request.workerExePath = sandbox_test_support::WorkerExePath();
    request.sourcePath = path.wstring();
    request.format = import_broker::ImportFormat::Usd;
    request.generationId = generation;
    request.sectionByteCapacity = 1024 * 1024;
    request.maxChunkCount = 16;
    request.maxSidecarRequestsPerGeneration = 8;
    request.maxSidecarFileBytes = 64 * 1024 * 1024;
    request.maxChunkBatchesPerGeneration = 4;
    return request;
}

uint32_t Count(const import_broker::ImportSessionResult& result,
               model_core::ChunkTopology topology)
{
    return static_cast<uint32_t>(std::count_if(result.chunks.begin(), result.chunks.end(),
        [topology](const auto& chunk) { return chunk.descriptor.topology == topology; }));
}

template<class T>
T Payload(const import_broker::ValidatedChunk& chunk)
{
    REQUIRE(chunk.payload.size() == sizeof(T));
    T value{};
    std::memcpy(&value, chunk.payload.data(), sizeof(value));
    return value;
}

uint64_t SemanticFingerprint(const import_broker::ImportSessionResult& result)
{
    uint64_t hash = 14695981039346656037ULL;
    auto add = [&](std::span<const std::byte> bytes) {
        for (const std::byte byte : bytes) {
            hash ^= std::to_integer<uint8_t>(byte);
            hash *= 1099511628211ULL;
        }
    };
    for (const auto& chunk : result.chunks) {
        add(std::as_bytes(std::span(&chunk.scene, 1)));
        add(std::as_bytes(std::span(&chunk.descriptor, 1)));
        add(chunk.payload);
    }
    return hash;
}

uint64_t CanonicalUsdFingerprint(const import_broker::ImportSessionResult& result)
{
    uint64_t hash = 14695981039346656037ULL;
    auto add = [&](std::span<const std::byte> bytes) {
        for (const std::byte byte : bytes) {
            hash ^= std::to_integer<uint8_t>(byte);
            hash *= 1099511628211ULL;
        }
    };
    auto scene = result.chunks.front().scene;
    scene.generationId = 0;
    scene.format = model_core::SourceFormatId::Unknown;
    add(std::as_bytes(std::span(&scene, 1)));
    for (const auto& chunk : result.chunks) {
        auto descriptor = chunk.descriptor;
        descriptor.normalizedRangeOffset = 0;
        descriptor.sourceRangeOffset = 0;
        descriptor.sourceRangeLength = 0;
        descriptor.chunkChecksum = 0;
        add(std::as_bytes(std::span(&descriptor, 1)));
        add(chunk.payload);
    }
    return hash;
}

std::vector<double> CanonicalGeometryPositions(const import_broker::ImportSessionResult& result)
{
    std::vector<double> positions;
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology != model_core::ChunkTopology::TriangleList) continue;
        const auto stride = model_core::VertexStrideForLayout(
            static_cast<model_core::VertexLayoutId>(chunk.descriptor.vertexLayoutId));
        REQUIRE(stride >= 12);
        REQUIRE(chunk.payload.size() >= uint64_t(chunk.descriptor.vertexCount) * stride);
        for (uint32_t vertex = 0; vertex < chunk.descriptor.vertexCount; ++vertex) {
            float local[3]{};
            std::memcpy(local, chunk.payload.data() + uint64_t(vertex) * stride, sizeof(local));
            for (size_t axis = 0; axis < 3; ++axis)
                positions.push_back(chunk.descriptor.origin[axis] + local[axis]);
        }
    }
    std::sort(positions.begin(), positions.end());
    return positions;
}

std::vector<std::byte> MetadataSection(model_core::UpAxisId axis, double metersPerUnit)
{
    using namespace model_core;
    constexpr uint64_t payloadOffset = kSectionHeaderSize + kChunkDescriptorSize;
    std::vector<std::byte> bytes(payloadOffset + sizeof(ImportStatusPayload));
    ImportStatusPayload status{};
    std::memcpy(bytes.data() + payloadOffset, &status, sizeof(status));
    ChunkDescriptor descriptor{};
    descriptor.topology = ChunkTopology::ImportStatus;
    descriptor.chunkId = 1;
    descriptor.normalizedRangeOffset = payloadOffset;
    descriptor.normalizedRangeLength = descriptor.byteSize = sizeof(status);
    descriptor.chunkChecksum = WireChecksum64(std::span(bytes).subspan(payloadOffset));
    std::memcpy(bytes.data() + kSectionHeaderSize, &descriptor, sizeof(descriptor));
    SectionHeader header{};
    header.magic = kSectionMagic;
    header.protocolVersion = kCurrentProtocolVersion;
    header.generationId = header.scene.generationId = 77;
    header.scene.format = SourceFormatId::Usda;
    header.scene.upAxis = axis;
    header.scene.metersPerUnit = metersPerUnit;
    header.chunkCount = 1;
    header.sectionLength = bytes.size();
    header.sectionChecksum = WireChecksum64(std::span(bytes).subspan(kSectionHeaderSize));
    std::memcpy(bytes.data(), &header, sizeof(header));
    return bytes;
}

} // namespace

TEST_CASE("USD-003 extends protocol-v10 with closed additive identities", "[usd-003][protocol]")
{
    CHECK(model_core::kCurrentProtocolVersion == 10);
    CHECK(uint32_t(model_core::SourceFormatId::Usda) == 9);
    CHECK(uint32_t(model_core::SourceFormatId::Usdc) == 10);
    CHECK(uint32_t(model_core::SourceFormatId::Usdz) == 11);
    CHECK(uint32_t(model_core::UpAxisId::Y) == 1);
    CHECK(uint32_t(model_core::UpAxisId::Z) == 2);
    CHECK(uint32_t(model_core::UpAxisId::X) == 3);
    CHECK(sizeof(model_core::ParseUsdFileRequest) == 48);
    CHECK(uint32_t(model_core::ControlOpcode::StartUsdImportFromFile) == 17);
    for (uint32_t code = uint32_t(model_core::ImportErrorCode::UnsupportedComposition);
         code <= uint32_t(model_core::ImportErrorCode::ArchiveLimit); ++code)
        CHECK(model_core::IsKnownImportErrorCode(code));
    CHECK_FALSE(model_core::IsKnownImportErrorCode(
        uint32_t(model_core::ImportErrorCode::TessellationFailed) + 1));
}

TEST_CASE("USD-003 validator accepts concrete X Y Z units and rejects illegal USD metadata",
          "[usd-003][metadata][axis][units]")
{
    for (const auto axis : {model_core::UpAxisId::X, model_core::UpAxisId::Y,
                            model_core::UpAxisId::Z}) {
        auto bytes = MetadataSection(axis, 0.01);
        CHECK(import_broker::ValidateAndCopySection(bytes, 77, 4).ok);
    }
    for (const auto [axis, units] : {
             std::pair{model_core::UpAxisId::Unknown, 0.01},
             std::pair{model_core::UpAxisId::X, 0.0},
             std::pair{model_core::UpAxisId::Y, -1.0},
             std::pair{model_core::UpAxisId::Z, std::numeric_limits<double>::infinity()},
             std::pair{model_core::UpAxisId::X, std::numeric_limits<double>::quiet_NaN()}}) {
        auto bytes = MetadataSection(axis, units);
        const auto result = import_broker::ValidateAndCopySection(bytes, 77, 4);
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    }
}

TEST_CASE("USD-003 one-shot and pooled routes preserve byte-detected encoding",
          "[usd-003][worker][pool][formats]")
{
    struct Fixture {
        std::string_view name;
        std::wstring_view extension;
        model_core::SourceFormatId format;
        model_core::UpAxisId axis;
        double units;
    };
    const Fixture fixtures[] = {
        {"mesh.usda", L"usd", model_core::SourceFormatId::Usda, model_core::UpAxisId::Z, 0.01},
        {"cube.usdc.base64", L"usd", model_core::SourceFormatId::Usdc, model_core::UpAxisId::Z, 1.0},
        {"mesh.usda", L"usda", model_core::SourceFormatId::Usda, model_core::UpAxisId::Z, 0.01},
        {"cube.usdc.base64", L"usdc", model_core::SourceFormatId::Usdc, model_core::UpAxisId::Z, 1.0},
    };
    uint64_t generation = 100;
    import_broker::PrepareImportWorkerPoolAsync(sandbox_test_support::WorkerExePath());
    for (size_t index = 0; index < std::size(fixtures); ++index) {
        DYNAMIC_SECTION("encoding " << index) {
            ScratchUsd scratch(fixtures[index].extension);
            scratch.Write(ReadFixture(fixtures[index].name));
            auto request = Request(scratch.path, ++generation);
            request.useWorkerPool = index == 0; // prove pooled dispatch and one-shot CLI
            auto result = import_broker::RunImportSession(request);
            CAPTURE(uint32_t(result.stage), uint32_t(result.errorCode), result.batchCount);
            REQUIRE(result.ok);
            REQUIRE_FALSE(result.chunks.empty());
            CHECK(result.chunks.front().scene.format == fixtures[index].format);
            CHECK(result.chunks.front().scene.upAxis == fixtures[index].axis);
            CHECK(result.chunks.front().scene.metersPerUnit == fixtures[index].units);
            CHECK(Count(result, model_core::ChunkTopology::TriangleList) > 0);
            CHECK(Count(result, model_core::ChunkTopology::Node) > 0);
            CHECK(Count(result, model_core::ChunkTopology::MeshInstance) > 0);
        }
    }
}

TEST_CASE("USD-004 normalizes static scene geometry instances and policy metadata",
          "[usd-004][worker][geometry][instances]")
{
    ScratchUsd scratch(L"usda");
    scratch.Write(ReadFixture("static-scene.usda"));
    auto request = Request(scratch.path, 150);
    request.sectionByteCapacity = import_broker::kImportSectionBytes;
    request.maxChunkCount = 1024;
    request.maxChunkBatchesPerGeneration = 64;
    const auto first = import_broker::RunImportSession(request);
    CAPTURE(uint32_t(first.stage), uint32_t(first.errorCode), first.batchCount,
            first.chunks.size());
    REQUIRE(first.ok);
    REQUIRE_FALSE(first.chunks.empty());
    CHECK(first.chunks.front().scene.format == model_core::SourceFormatId::Usda);
    CHECK(first.chunks.front().scene.upAxis == model_core::UpAxisId::X);
    CHECK(first.chunks.front().scene.metersPerUnit == 0.001);
    CHECK(Count(first, model_core::ChunkTopology::TriangleList) == 2);
    CHECK(Count(first, model_core::ChunkTopology::Node) >= 7);
    CHECK(Count(first, model_core::ChunkTopology::MeshInstance) >= 4);
    CHECK(Count(first, model_core::ChunkTopology::ImportStatus) == 1);

    struct ReferenceCounts { uint32_t visible = 0; uint32_t hidden = 0; };
    std::unordered_map<uint32_t, ReferenceCounts> geometryReferences;
    bool sawLargeOrigin = false;
    bool sawPrototypeTransform = false;
    bool sawAttributes = false;
    for (const auto& chunk : first.chunks) {
        if (chunk.descriptor.topology == model_core::ChunkTopology::TriangleList) {
            sawAttributes |= (chunk.descriptor.geometryFlags
                & (model_core::kGeometryHasUv0 | model_core::kGeometryHasColors))
                == (model_core::kGeometryHasUv0 | model_core::kGeometryHasColors);
        } else if (chunk.descriptor.topology == model_core::ChunkTopology::MeshInstance) {
            const auto instance = Payload<model_core::MeshInstancePayload>(chunk);
            if (instance.worldMin[0] > 999'999'999.0) sawLargeOrigin = true;
            if ((instance.flags & model_core::kSceneRecordVisible)
                && instance.worldMin[1] >= 12.0) sawPrototypeTransform = true;
            auto& references = geometryReferences[instance.geometryChunkId];
            if (instance.flags & model_core::kSceneRecordVisible) ++references.visible;
            else ++references.hidden;
        }
    }
    const bool sawSharedPrototype = std::ranges::any_of(geometryReferences, [](const auto& item) {
        return item.second.visible >= 1 && item.second.hidden >= 1
            && item.second.visible + item.second.hidden >= 3;
    });
    CHECK(sawAttributes);
    CHECK(sawLargeOrigin);
    CHECK(sawPrototypeTransform);
    CHECK(sawSharedPrototype);

    const auto second = import_broker::RunImportSession(request);
    REQUIRE(second.ok);
    CHECK(SemanticFingerprint(second) == SemanticFingerprint(first));
}

TEST_CASE("USD-004 honors resetXformStack when publishing the node hierarchy",
          "[usd-004][worker][geometry][transforms]")
{
    ScratchUsd scratch(L"usda");
    const std::string source = R"USD(#usda 1.0
(
    defaultPrim = "Parent"
    metersPerUnit = 1
    upAxis = "Y"
)
def Xform "Parent" {
    double3 xformOp:translate = (100, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]
    def Mesh "ResetMesh" {
        double3 xformOp:translate = (2, 0, 0)
        uniform token[] xformOpOrder = ["!resetXformStack!", "xformOp:translate"]
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
        uniform token subdivisionScheme = "none"
    }
}
)USD";
    scratch.Write(std::as_bytes(std::span(source)));
    auto request = Request(scratch.path, 159);
    request.sectionByteCapacity = import_broker::kImportSectionBytes;
    request.maxChunkCount = 1024;
    request.maxChunkBatchesPerGeneration = 64;
    const auto result = import_broker::RunImportSession(request);
    REQUIRE(result.ok);
    const auto instance = std::find_if(result.chunks.begin(), result.chunks.end(), [](const auto& chunk) {
        return chunk.descriptor.topology == model_core::ChunkTopology::MeshInstance;
    });
    REQUIRE(instance != result.chunks.end());
    const auto occurrence = Payload<model_core::MeshInstancePayload>(*instance);
    CHECK(occurrence.worldMin[0] == 2.0);
    CHECK(occurrence.worldMax[0] == 3.0);
    const auto node = std::find_if(result.chunks.begin(), result.chunks.end(), [&](const auto& chunk) {
        return chunk.descriptor.topology == model_core::ChunkTopology::Node
            && Payload<model_core::NodePayload>(chunk).nodeId == occurrence.nodeId;
    });
    REQUIRE(node != result.chunks.end());
    CHECK(Payload<model_core::NodePayload>(*node).parentNodeId == 0);
}

TEST_CASE("USD-004 uses progressive bounded sections and reuses the worker after failure",
          "[usd-004][worker][progressive][recovery]")
{
    import_broker::PrepareImportWorkerPoolAsync(sandbox_test_support::WorkerExePath());
    ScratchUsd valid(L"usd");
    valid.Write(ReadFixture("static-scene.usda"));
    auto request = Request(valid.path, 160);
    request.sectionByteCapacity = 4096;
    request.maxChunkCount = 2;
    request.maxChunkBatchesPerGeneration = 64;
    request.maxChunksPerGeneration = 128;
    request.useWorkerPool = true;
    const auto progressive = import_broker::RunImportSession(request);
    CAPTURE(uint32_t(progressive.stage), uint32_t(progressive.errorCode),
            progressive.batchCount, progressive.chunks.size());
    REQUIRE(progressive.ok);
    CHECK(progressive.batchCount > 1);

    std::atomic_bool cancelled = false;
    auto cancelledRequest = request;
    cancelledRequest.generationId = 161;
    cancelledRequest.isCancelled = [&] { return cancelled.load(); };
    cancelledRequest.onBatch = [&](std::vector<import_broker::ValidatedChunk>&& chunks) {
        REQUIRE_FALSE(chunks.empty());
        cancelled.store(true);
    };
    const auto cancelledResult = import_broker::RunImportSession(cancelledRequest);
    CHECK_FALSE(cancelledResult.ok);
    CHECK(cancelledResult.errorCode == model_core::ImportErrorCode::Cancelled);
    CHECK(cancelledResult.workerProcessId == progressive.workerProcessId);

    ScratchUsd malformed(L"usd");
    const std::string hostile =
        "#usda 1.0\n(def Mesh \"Bad\" { int[] faceVertexCounts = [3] "
        "int[] faceVertexIndices = [0, 1, 99] point3f[] points = [(0,0,0)] })";
    malformed.Write(std::as_bytes(std::span(hostile)));
    auto badRequest = Request(malformed.path, 162);
    badRequest.useWorkerPool = true;
    const auto bad = import_broker::RunImportSession(badRequest);
    CHECK_FALSE(bad.ok);
    CHECK(bad.errorCode == model_core::ImportErrorCode::MalformedData);

    request.generationId = 163;
    const auto recovered = import_broker::RunImportSession(request);
    REQUIRE(recovered.ok);
    CHECK(recovered.workerProcessId == bad.workerProcessId);
}

TEST_CASE("USD-005 keeps composition atomic and imports independently preflighted USDZ",
          "[usd-004][usd-005][worker][fallback][usdz]")
{
    ScratchUsd composed(L"usda");
    const std::string source =
        "#usda 1.0\n( subLayers = [@child.usda@] )\n"
        "def Mesh \"M\" {\n int[] faceVertexCounts=[3]\n int[] faceVertexIndices=[0,1,2]\n "
        "point3f[] points=[(0,0,0),(1,0,0),(0,1,0)]\n uniform token subdivisionScheme=\"none\"\n}\n";
    composed.Write(std::as_bytes(std::span(source)));
    const auto fallback = import_broker::RunImportSession(Request(composed.path, 170));
    CHECK_FALSE(fallback.ok);
    CHECK(fallback.errorCode == model_core::ImportErrorCode::UnsupportedComposition);
    CHECK(fallback.compatibilityFallbackRequired);
    CHECK(fallback.batchCount == 0);
    CHECK(fallback.chunks.empty());

    ScratchUsd movingInstances(L"usda");
    const std::string unsupportedMotion =
        "#usda 1.0\ndef Xform \"Root\" {\n"
        " def Mesh \"Prototype\" { int[] faceVertexCounts=[3] "
        "int[] faceVertexIndices=[0,1,2] point3f[] points=[(0,0,0),(1,0,0),(0,1,0)] }\n"
        " def PointInstancer \"Instances\" { rel prototypes=[</Root/Prototype>] "
        "int[] protoIndices=[0] point3f[] positions=[(0,0,0)] "
        "vector3f[] velocities=[(1,0,0)] }\n}";
    movingInstances.Write(std::as_bytes(std::span(unsupportedMotion)));
    const auto unsupported = import_broker::RunImportSession(
        Request(movingInstances.path, 171));
    CHECK_FALSE(unsupported.ok);
    CHECK(unsupported.errorCode == model_core::ImportErrorCode::UnsupportedRequiredFeature);
    CHECK_FALSE(unsupported.compatibilityFallbackRequired);
    CHECK(unsupported.batchCount == 0);

    ScratchUsd archive(L"usdz");
    archive.Write(ReadFixture("cube.usdz.base64"));
    const auto imported = import_broker::RunImportSession(Request(archive.path, 172));
    CAPTURE(uint32_t(imported.errorPhase), uint32_t(imported.stage), imported.batchCount);
    REQUIRE(imported.ok);
    REQUIRE_FALSE(imported.chunks.empty());
    CHECK(imported.chunks.front().scene.format == model_core::SourceFormatId::Usdz);
    CHECK(Count(imported, model_core::ChunkTopology::TriangleList) > 0);
    CHECK(Count(imported, model_core::ChunkTopology::MeshInstance) > 0);
}

TEST_CASE("USD-005 maps Preview Surface textures and material subsets through the broker",
          "[usd-005][worker][materials][textures][sidecars]")
{
    ScratchUsd scratch(L"usda");
    scratch.Write(ReadFixture("materials.usda"));
    const std::string pngBase64 =
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9WlF4iUAAAAASUVORK5CYII=";
    scratch.WriteSidecar(L"albedo.png", DecodeBase64(pngBase64));
    auto request = Request(scratch.path, 180);
    request.sectionByteCapacity = import_broker::kImportSectionBytes;
    request.maxChunkCount = 1024;
    request.maxChunkBatchesPerGeneration = 64;
    const auto result = import_broker::RunImportSession(request);
    CAPTURE(uint32_t(result.stage), uint32_t(result.errorCode),
            uint32_t(result.errorPhase), result.batchCount);
    REQUIRE(result.ok);
    CHECK(Count(result, model_core::ChunkTopology::Material) == 2);
    CHECK(Count(result, model_core::ChunkTopology::Image) >= 1);
    CHECK(Count(result, model_core::ChunkTopology::TriangleList) == 2);

    std::unordered_map<uint32_t, model_core::MaterialPayload> materials;
    std::unordered_set<uint32_t> instanceMaterials;
    bool sawTexture = false;
    bool sawNeutralTextureFactor = false;
    bool sawNeutralBoundVertexColor = false;
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology == model_core::ChunkTopology::Material) {
            const auto material = Payload<model_core::MaterialPayload>(chunk);
            materials.emplace(chunk.descriptor.chunkId, material);
            sawTexture |= chunk.descriptor.dependencyIds[0] != 0;
            if (chunk.descriptor.dependencyIds[0] != 0)
                sawNeutralTextureFactor = material.baseColorFactor[0] == 1.0f
                    && material.baseColorFactor[1] == 1.0f
                    && material.baseColorFactor[2] == 1.0f;
        } else if (chunk.descriptor.topology == model_core::ChunkTopology::TriangleList) {
            REQUIRE(chunk.payload.size()
                >= sizeof(model_core::VertexPositionNormalUv0TangentColorF32));
            model_core::VertexPositionNormalUv0TangentColorF32 vertex{};
            std::memcpy(&vertex, chunk.payload.data(), sizeof(vertex));
            sawNeutralBoundVertexColor |= vertex.r == 1.0f && vertex.g == 1.0f
                && vertex.b == 1.0f && vertex.a == 1.0f;
        } else if (chunk.descriptor.topology == model_core::ChunkTopology::MeshInstance) {
            instanceMaterials.insert(Payload<model_core::MeshInstancePayload>(chunk).materialChunkId);
        } else if (chunk.descriptor.topology == model_core::ChunkTopology::Image) {
            REQUIRE(chunk.payload.size() >= sizeof(model_core::ImagePayloadHeader));
            model_core::ImagePayloadHeader image{};
            std::memcpy(&image, chunk.payload.data(), sizeof(image));
            CHECK(image.colorSpace == uint32_t(model_core::ColorSpaceId::Srgb));
        }
    }
    CHECK(sawTexture);
    CHECK(sawNeutralTextureFactor);
    CHECK(sawNeutralBoundVertexColor);
    CHECK(instanceMaterials.size() == 2);
    CHECK(std::ranges::all_of(materials, [](const auto& item) {
        return (item.second.flags & model_core::kMaterialFlagDoubleSided) != 0;
    }));
    CHECK(std::ranges::any_of(materials, [](const auto& item) {
        return item.second.alphaMode == uint32_t(model_core::AlphaModeId::Mask)
            && std::abs(item.second.metallicFactor - 0.25f) < 1e-6f
            && std::abs(item.second.roughnessFactor - 0.75f) < 1e-6f
            && std::abs(item.second.uvOffset[0] - 0.25f) < 1e-6f
            && std::abs(item.second.uvOffset[1] - 0.5f) < 1e-6f
            && std::abs(item.second.uvScale[0] - 2.0f) < 1e-6f
            && std::abs(item.second.uvScale[1] - 3.0f) < 1e-6f
            && std::abs(item.second.uvRotation - 0.5235988f) < 1e-6f;
    }));
}

TEST_CASE("USD-005 recovers an emissive-only material's authored st0 coordinates",
          "[usd-005][worker][materials][textures][primvars]")
{
    ScratchUsd scratch(L"usda");
    const std::string source = R"USD(#usda 1.0
(
    defaultPrim = "Root"
    metersPerUnit = 1
    upAxis = "Y"
)
def Xform "Root" {
    def Mesh "Triangle" {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
        texCoord2f[] primvars:st0 = [(0.25,0.5), (0.75,0.5), (0.25,1)] (
            interpolation = "faceVarying"
        )
        rel material:binding = </Root/Material>
        uniform token subdivisionScheme = "none"
    }
    def Material "Material" {
        token outputs:surface.connect = </Root/Material/Surface.outputs:surface>
        def Shader "Surface" {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0,0,0)
            color3f inputs:emissiveColor.connect = </Root/Material/Texture.outputs:rgb>
            token outputs:surface
        }
        def Shader "Texture" {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @emissive.png@
            float2 inputs:st.connect = </Root/Material/Primvar.outputs:result>
            color3f outputs:rgb
        }
        def Shader "Primvar" {
            uniform token info:id = "UsdPrimvarReader_float2"
            token inputs:varname = "st0"
            float2 outputs:result
        }
    }
}
)USD";
    scratch.Write(std::as_bytes(std::span(source)));
    const std::string pngBase64 =
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9WlF4iUAAAAASUVORK5CYII=";
    scratch.WriteSidecar(L"emissive.png", DecodeBase64(pngBase64));
    auto request = Request(scratch.path, 182);
    request.sectionByteCapacity = import_broker::kImportSectionBytes;
    request.maxChunkCount = 1024;
    request.maxChunkBatchesPerGeneration = 64;
    const auto result = import_broker::RunImportSession(request);
    CAPTURE(uint32_t(result.stage), uint32_t(result.errorCode), result.batchCount);
    REQUIRE(result.ok);
    const auto geometry = std::find_if(result.chunks.begin(), result.chunks.end(), [](const auto& chunk) {
        return chunk.descriptor.topology == model_core::ChunkTopology::TriangleList;
    });
    REQUIRE(geometry != result.chunks.end());
    CHECK((geometry->descriptor.geometryFlags & model_core::kGeometryHasUv0) != 0);
    REQUIRE(geometry->payload.size()
        >= sizeof(model_core::VertexPositionNormalUv0TangentColorF32));
    model_core::VertexPositionNormalUv0TangentColorF32 vertex{};
    std::memcpy(&vertex, geometry->payload.data(), sizeof(vertex));
    CHECK(vertex.u == 0.25f);
    CHECK(vertex.v == 0.5f);
    const auto material = std::find_if(result.chunks.begin(), result.chunks.end(), [](const auto& chunk) {
        return chunk.descriptor.topology == model_core::ChunkTopology::Material;
    });
    REQUIRE(material != result.chunks.end());
    CHECK(material->descriptor.dependencyIds[0] == 0);
    CHECK(material->descriptor.dependencyIds[3] != 0);
    const auto payload = Payload<model_core::MaterialPayload>(*material);
    CHECK((payload.flags & model_core::kMaterialFlagFlipV) != 0);
    CHECK(payload.emissiveFactor[0] == 1.0f);
    CHECK(payload.emissiveFactor[1] == 1.0f);
    CHECK(payload.emissiveFactor[2] == 1.0f);
}

TEST_CASE("USD-005 resolves contained USDZ textures without extraction",
          "[usd-005][worker][usdz][textures][archive]")
{
    const auto layer = ReadFixture("materials.usda");
    const std::string pngBase64 =
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9WlF4iUAAAAASUVORK5CYII=";
    const auto png = DecodeBase64(pngBase64);
    const std::pair<std::string, std::vector<std::byte>> entries[] = {
        {"root.usda", layer}, {"albedo.png", png},
    };
    ScratchUsd archive(L"usdz");
    archive.Write(StoredUsdz(entries));
    auto request = Request(archive.path, 183);
    request.sectionByteCapacity = import_broker::kImportSectionBytes;
    request.maxChunkCount = 1024;
    request.maxChunkBatchesPerGeneration = 64;
    const auto result = import_broker::RunImportSession(request);
    CAPTURE(uint32_t(result.stage), uint32_t(result.errorCode),
            uint32_t(result.errorPhase), result.batchCount);
    REQUIRE(result.ok);
    REQUIRE_FALSE(result.chunks.empty());
    CHECK(result.chunks.front().scene.format == model_core::SourceFormatId::Usdz);
    CHECK(Count(result, model_core::ChunkTopology::Material) == 2);
    CHECK(Count(result, model_core::ChunkTopology::Image) >= 1);
    CHECK(Count(result, model_core::ChunkTopology::TriangleList) == 2);
}

TEST_CASE("USD-005 archive inspection returns bounded entry views and observes cancellation",
          "[usd-005][usdz][archive][cancellation]")
{
    const auto layer = ReadFixture("materials.usda");
    const std::pair<std::string, std::vector<std::byte>> entries[] = {
        {"root.usda", layer}, {"albedo.png", std::vector<std::byte>(32, std::byte{0x5a})},
    };
    const auto bytes = StoredUsdz(entries);
    import_worker::UsdzArchiveView archive;
    REQUIRE(import_worker::InspectUsdz(bytes, &archive) == import_worker::UsdzPreflightError::None);
    REQUIRE(archive.entries.size() == 2);
    CHECK(archive.rootLayerName == "root.usda");
    for (const auto& entry : archive.entries) {
        CHECK((entry.dataOffset & 63) == 0);
        REQUIRE(entry.dataOffset <= bytes.size());
        CHECK(entry.byteSize <= bytes.size() - entry.dataOffset);
    }

    unsigned polls = 0;
    CHECK(import_worker::InspectUsdz(bytes, &archive, {}, [&] { return ++polls >= 1; })
          == import_worker::UsdzPreflightError::Cancelled);
    CHECK(archive.entries.empty());
}

TEST_CASE("USD-005 uses bounded optional texture fallback and rejects unsafe assets atomically",
          "[usd-005][worker][textures][security][fallback]")
{
    ScratchUsd missing(L"usda");
    auto source = ReadFixture("materials.usda");
    missing.Write(source);
    const auto fallback = import_broker::RunImportSession(Request(missing.path, 181));
    CAPTURE(uint32_t(fallback.stage), uint32_t(fallback.errorCode), fallback.batchCount);
    REQUIRE(fallback.ok);
    CHECK(Count(fallback, model_core::ChunkTopology::Image) >= 1);
    CHECK(Count(fallback, model_core::ChunkTopology::ImportStatus) == 1);

    ScratchUsd corrupt(L"usda");
    corrupt.Write(source);
    const std::string invalidPng = "not a PNG payload";
    corrupt.WriteSidecar(L"albedo.png", std::as_bytes(std::span(invalidPng)));
    const auto corruptFallback = import_broker::RunImportSession(Request(corrupt.path, 184));
    CAPTURE(uint32_t(corruptFallback.stage), uint32_t(corruptFallback.errorCode),
            corruptFallback.batchCount);
    REQUIRE(corruptFallback.ok);
    CHECK(Count(corruptFallback, model_core::ChunkTopology::Image) >= 1);
    CHECK(Count(corruptFallback, model_core::ChunkTopology::ImportStatus) == 1);

    std::string unsafe(reinterpret_cast<const char*>(source.data()), source.size());
    const std::string needle = "albedo.png";
    const size_t offset = unsafe.find(needle);
    REQUIRE(offset != std::string::npos);
    std::string mismatched = unsafe;
    mismatched.replace(offset, needle.size(), "albedo.jpg");
    ScratchUsd typeMismatch(L"usda");
    typeMismatch.Write(std::as_bytes(std::span(mismatched)));
    const std::string pngBase64 =
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9WlF4iUAAAAASUVORK5CYII=";
    typeMismatch.WriteSidecar(L"albedo.jpg", DecodeBase64(pngBase64));
    const auto mismatchedResult = import_broker::RunImportSession(Request(typeMismatch.path, 185));
    CHECK_FALSE(mismatchedResult.ok);
    CHECK(mismatchedResult.errorCode == model_core::ImportErrorCode::UnsafeReference);
    CHECK(mismatchedResult.batchCount == 0);
    CHECK(mismatchedResult.chunks.empty());

    unsafe.replace(offset, needle.size(), "../bad.png");
    ScratchUsd traversal(L"usda");
    traversal.Write(std::as_bytes(std::span(unsafe)));
    const auto rejected = import_broker::RunImportSession(Request(traversal.path, 182));
    CHECK_FALSE(rejected.ok);
    CHECK(rejected.errorCode == model_core::ImportErrorCode::UnsafeReference);
    CHECK_FALSE(rejected.compatibilityFallbackRequired);
    CHECK(rejected.batchCount == 0);
    CHECK(rejected.chunks.empty());
}

TEST_CASE("USD-003 explicit suffix mismatch and malformed bytes are terminal typed failures",
          "[usd-003][worker][negative]")
{
    ScratchUsd mismatch(L"usdc");
    mismatch.Write(ReadFixture("mesh.usda"));
    auto mismatchResult = import_broker::RunImportSession(Request(mismatch.path, 200));
    CHECK_FALSE(mismatchResult.ok);
    CHECK(mismatchResult.stage == import_broker::ImportStage::WorkerReportedError);
    CHECK(mismatchResult.errorCode == model_core::ImportErrorCode::UnsupportedEncoding);
    CHECK_FALSE(mismatchResult.compatibilityFallbackRequired);

    ScratchUsd malformed(L"usd");
    const std::string bad = "not a USD layer";
    malformed.Write(std::as_bytes(std::span(bad)));
    auto malformedResult = import_broker::RunImportSession(Request(malformed.path, 201));
    CHECK_FALSE(malformedResult.ok);
    CHECK(malformedResult.errorCode == model_core::ImportErrorCode::MalformedData);
    CHECK_FALSE(malformedResult.compatibilityFallbackRequired);

    auto spoofed = Request(malformed.path, 202);
    spoofed.workerExePath = sandbox_test_support::HostileWorkerExePath();
    spoofed.workerArgumentsOverride = L"--usd-spoofed-format";
    auto spoofedResult = import_broker::RunImportSession(spoofed);
    CHECK_FALSE(spoofedResult.ok);
    CHECK(spoofedResult.errorCode == model_core::ImportErrorCode::ImportProtocolViolation);
    CHECK_FALSE(spoofedResult.compatibilityFallbackRequired);

    auto encryptedBytes = ReadFixture("cube.usdz.base64");
    const size_t local = FindSignature(encryptedBytes, 0x04034b50u);
    const size_t central = FindSignature(encryptedBytes, 0x02014b50u);
    REQUIRE(local != std::string::npos);
    REQUIRE(central != std::string::npos);
    Put16(encryptedBytes, local + 6, 1);
    Put16(encryptedBytes, central + 8, 1);
    ScratchUsd encrypted(L"usdz");
    encrypted.Write(encryptedBytes);
    const auto archiveResult = import_broker::RunImportSession(Request(encrypted.path, 203));
    CHECK_FALSE(archiveResult.ok);
    CHECK(archiveResult.errorCode == model_core::ImportErrorCode::ArchiveLimit);
    CHECK_FALSE(archiveResult.compatibilityFallbackRequired);
}

TEST_CASE("USD-003 fallback state permits exactly one producer for one generation",
          "[usd-003][fallback][state]")
{
    using namespace import_broker;
    using model_core::ImportErrorCode;
    UsdFallbackState fallback(300);
    CHECK(fallback.CommittedProducer() == UsdProducer::None);
    CHECK(fallback.ObserveError(299, UsdProducer::TinyUsdz,
                                ImportErrorCode::UnsupportedComposition)
          == UsdFallbackObservation::StaleGeneration);
    CHECK(fallback.Phase() == UsdFallbackPhase::AwaitingFastClassification);
    CHECK(fallback.ObserveError(300, UsdProducer::TinyUsdz,
                                ImportErrorCode::UnsupportedComposition)
          == UsdFallbackObservation::StartCompatibility);
    CHECK(fallback.ObserveBatch(300, UsdProducer::OpenUsd)
          == UsdFallbackObservation::Accepted);
    CHECK(fallback.ObserveBatch(300, UsdProducer::TinyUsdz)
          == UsdFallbackObservation::ProtocolViolation);

    UsdFallbackState loop(301);
    REQUIRE(loop.ObserveError(301, UsdProducer::TinyUsdz,
                              ImportErrorCode::UnsupportedComposition)
            == UsdFallbackObservation::StartCompatibility);
    CHECK(loop.ObserveError(301, UsdProducer::OpenUsd,
                            ImportErrorCode::UnsupportedComposition)
          == UsdFallbackObservation::ProtocolViolation);

    UsdFallbackState compatibility(302);
    REQUIRE(compatibility.ObserveError(302, UsdProducer::TinyUsdz,
                                       ImportErrorCode::UnsupportedComposition)
            == UsdFallbackObservation::StartCompatibility);
    REQUIRE(compatibility.ObserveBatch(302, UsdProducer::OpenUsd)
            == UsdFallbackObservation::Accepted);
    REQUIRE(compatibility.ObserveComplete(302, UsdProducer::OpenUsd)
            == UsdFallbackObservation::Accepted);
    CHECK(compatibility.CommittedProducer() == UsdProducer::OpenUsd);

    UsdFallbackState fast(303);
    REQUIRE(fast.ObserveBatch(303, UsdProducer::TinyUsdz)
            == UsdFallbackObservation::Accepted);
    REQUIRE(fast.ObserveComplete(303, UsdProducer::TinyUsdz)
            == UsdFallbackObservation::Accepted);
    CHECK(fast.CommittedProducer() == UsdProducer::TinyUsdz);
    CHECK(fast.ObserveBatch(303, UsdProducer::OpenUsd)
          == UsdFallbackObservation::ProtocolViolation);
}

TEST_CASE("USD-003 broker accepts only pre-publication composition fallback",
          "[usd-003][fallback][hostile]")
{
    ScratchUsd scratch(L"usd");
    const std::string source = "#usda 1.0\n";
    scratch.Write(std::as_bytes(std::span(source)));

    auto exact = Request(scratch.path, 400);
    exact.workerExePath = sandbox_test_support::HostileWorkerExePath();
    exact.workerArgumentsOverride = L"--error-unsupported-composition";
    auto exactResult = import_broker::RunImportSession(exact);
    CHECK_FALSE(exactResult.ok);
    CHECK(exactResult.errorCode == model_core::ImportErrorCode::UnsupportedComposition);
    CHECK(exactResult.compatibilityFallbackRequired);
    CHECK(exactResult.batchCount == 0);

    uint32_t published = 0;
    auto late = Request(scratch.path, 401);
    late.workerExePath = sandbox_test_support::HostileWorkerExePath();
    late.workerArgumentsOverride = L"--usd-fallback-after-batch";
    late.onBatch = [&](auto) { ++published; };
    auto lateResult = import_broker::RunImportSession(late);
    CAPTURE(uint32_t(lateResult.stage), uint32_t(lateResult.errorCode), lateResult.batchCount);
    CHECK_FALSE(lateResult.ok);
    CHECK(lateResult.errorCode == model_core::ImportErrorCode::ImportProtocolViolation);
    CHECK_FALSE(lateResult.compatibilityFallbackRequired);
    CHECK(lateResult.batchCount == 1);
    CHECK(published == 1);
}

TEST_CASE("USD-006 lazily launches an isolated compatibility producer and discards fast state",
          "[usd-006][fallback][openusd][sandbox][recovery]")
{
    CHECK(uint32_t(model_core::ControlOpcode::StartOpenUsdImportFromFile) == 18);
    CHECK(sizeof(model_core::ParseOpenUsdFileRequest) == 48);

    ScratchUsd composed(L"usda");
    const std::string source =
        "#usda 1.0\n( subLayers = [@child.usda@] )\n"
        "def Mesh \"M\" {\n int[] faceVertexCounts=[3]\n int[] faceVertexIndices=[0,1,2]\n "
        "point3f[] points=[(0,0,0),(1,0,0),(0,1,0)]\n uniform token subdivisionScheme=\"none\"\n}\n";
    composed.Write(std::as_bytes(std::span(source)));
    const std::string child = "#usda 1.0\n";
    composed.WriteSidecar(L"child.usda", std::as_bytes(std::span(child)));

    auto request = Request(composed.path, 600);
    request.compatibilityHostExePath = PREVIEW3D_IMPORT_HOST_EXE;
    const auto result = import_broker::RunImportSession(request);
    CAPTURE(uint32_t(result.stage), uint32_t(result.errorCode), result.workerProcessId,
            result.batchCount);
    CHECK(result.ok);
    CHECK(result.errorCode == model_core::ImportErrorCode::None);
    CHECK(result.producer == import_broker::ImportProducer::CompatibilityHost);
    CHECK_FALSE(result.compatibilityFallbackRequired);
    CHECK(result.batchCount == 1);
    CHECK(Count(result, model_core::ChunkTopology::TriangleList) == 1);
    CHECK(Count(result, model_core::ChunkTopology::Node) == 1);
    CHECK(Count(result, model_core::ChunkTopology::MeshInstance) == 1);
    REQUIRE(result.workerProcessId != 0);

    // The strict bounded-idle policy exits at generation completion. If the
    // process object still exists, it must already be signaled.
    HANDLE raw = OpenProcess(SYNCHRONIZE, FALSE, result.workerProcessId);
    if (raw) {
        platform::Win32Handle process(raw);
        CHECK(WaitForSingleObject(process.get(), 2000) == WAIT_OBJECT_0);
    } else {
        CHECK(raw == nullptr); // process object already reaped
    }

    // A terminal non-composition failure never changes producer or asks the
    // host for a more permissive retry.
    ScratchUsd motion(L"usda");
    const std::string unsupported =
        "#usda 1.0\ndef Xform \"Root\" { def Mesh \"P\" { "
        "int[] faceVertexCounts=[3] int[] faceVertexIndices=[0,1,2] "
        "point3f[] points=[(0,0,0),(1,0,0),(0,1,0)] } "
        "def PointInstancer \"I\" { rel prototypes=[</Root/P>] int[] protoIndices=[0] "
        "point3f[] positions=[(0,0,0)] vector3f[] velocities=[(1,0,0)] } }";
    motion.Write(std::as_bytes(std::span(unsupported)));
    auto terminalRequest = Request(motion.path, 601);
    terminalRequest.compatibilityHostExePath = PREVIEW3D_IMPORT_HOST_EXE;
    const auto terminal = import_broker::RunImportSession(terminalRequest);
    CHECK_FALSE(terminal.ok);
    CHECK(terminal.errorCode == model_core::ImportErrorCode::UnsupportedRequiredFeature);
    CHECK(terminal.producer == import_broker::ImportProducer::FastWorker);
    CHECK_FALSE(terminal.compatibilityFallbackRequired);

    // A later fast generation remains usable after compatibility-host exit.
    ScratchUsd valid(L"usda");
    valid.Write(ReadFixture("mesh.usda"));
    const auto recovered = import_broker::RunImportSession(Request(valid.path, 602));
    REQUIRE(recovered.ok);
    CHECK(recovered.producer == import_broker::ImportProducer::FastWorker);
    CHECK(Count(recovered, model_core::ChunkTopology::TriangleList) == 1);
}

TEST_CASE("USD-006 compatibility crash timeout protocol and commit faults stay generation-local",
          "[usd-006][fallback][hostile][limits][recovery]")
{
    ScratchUsd composed(L"usda");
    const std::string source =
        "#usda 1.0\n"
        "(\n"
        "    subLayers = [@child.usda@]\n"
        ")\n"
        "def Mesh \"M\"\n"
        "{\n"
        "    int[] faceVertexCounts = [3]\n"
        "    int[] faceVertexIndices = [0, 1, 2]\n"
        "    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n"
        "    uniform token subdivisionScheme = \"none\"\n"
        "}\n";
    composed.Write(std::as_bytes(std::span(source)));
    const std::string child = "#usda 1.0\n";
    composed.WriteSidecar(L"child.usda", std::as_bytes(std::span(child)));

    uint64_t generation = 610;
    for (const wchar_t* mode : {L"--pool-crash", L"--pool-hang", L"--pool-stale",
                                L"--pool-reverse-fallback"}) {
        auto request = Request(composed.path, generation++);
        request.compatibilityHostExePath = PREVIEW3D_IMPORT_HOST_EXE;
        request.compatibilityHostArgumentsOverride = mode;
        request.replyTimeoutMs = 2000;
        const auto result = import_broker::RunImportSession(request);
        CAPTURE(mode, uint32_t(result.stage), uint32_t(result.errorCode));
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::CompatibilityHostFailure);
        CHECK(result.producer == import_broker::ImportProducer::CompatibilityHost);
        CHECK_FALSE(result.compatibilityFallbackRequired);
        CHECK(result.chunks.empty());
    }

    std::atomic_uint32_t cancellationProbes{0};
    auto cancelledRequest = Request(composed.path, generation++);
    cancelledRequest.compatibilityHostExePath = PREVIEW3D_IMPORT_HOST_EXE;
    cancelledRequest.compatibilityHostArgumentsOverride = L"--pool-hang";
    cancelledRequest.replyTimeoutMs = 5000;
    cancelledRequest.isCancelled = [&] {
        // The fast classifier observes only a handful of probes for this tiny
        // layer. The hung host's bounded wait supplies the later probes.
        return cancellationProbes.fetch_add(1) >= 50;
    };
    const auto cancelled = import_broker::RunImportSession(cancelledRequest);
    CHECK_FALSE(cancelled.ok);
    CHECK(cancelled.errorCode == model_core::ImportErrorCode::Cancelled);
    CHECK(cancelled.producer == import_broker::ImportProducer::CompatibilityHost);
    CHECK(cancelled.chunks.empty());

    // A replacement generation may arrive while the old host still owns the
    // single compatibility lease. Cancelling the old generation tears down
    // that Job; the waiter must lazily launch a fresh host instead of failing
    // because the old pool intentionally exited.
    std::atomic_bool replaceOld{false};
    auto oldRequest = Request(composed.path, generation++);
    oldRequest.compatibilityHostExePath = PREVIEW3D_IMPORT_HOST_EXE;
    oldRequest.compatibilityHostArgumentsOverride = L"--pool-hang";
    oldRequest.replyTimeoutMs = 5000;
    oldRequest.isCancelled = [&] { return replaceOld.load(); };
    auto oldFuture = std::async(std::launch::async, [&] {
        return import_broker::RunImportSession(oldRequest);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto newRequest = Request(composed.path, generation++);
    newRequest.compatibilityHostExePath = PREVIEW3D_IMPORT_HOST_EXE;
    auto newFuture = std::async(std::launch::async, [&] {
        return import_broker::RunImportSession(newRequest);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    replaceOld.store(true);
    const auto oldResult = oldFuture.get();
    const auto newResult = newFuture.get();
    CHECK(oldResult.errorCode == model_core::ImportErrorCode::Cancelled);
    CHECK(oldResult.producer == import_broker::ImportProducer::CompatibilityHost);
    CHECK(newResult.ok);
    CHECK(newResult.errorCode == model_core::ImportErrorCode::None);
    CHECK(newResult.producer == import_broker::ImportProducer::CompatibilityHost);
    CHECK(newResult.workerProcessId != 0);

    auto limited = Request(composed.path, generation++);
    limited.compatibilityHostExePath = PREVIEW3D_IMPORT_HOST_EXE;
    limited.compatibilityHostArgumentsOverride = L"--pool-overallocate";
    limited.compatibilityHostCommitLimitBytes = 32ull * 1024ull * 1024ull;
    limited.replyTimeoutMs = 2000;
    const auto limit = import_broker::RunImportSession(limited);
    CAPTURE(uint32_t(limit.stage), uint32_t(limit.errorCode));
    CHECK_FALSE(limit.ok);
    CHECK(limit.errorCode == model_core::ImportErrorCode::CompatibilityHostLimit);
    CHECK(limit.producer == import_broker::ImportProducer::CompatibilityHost);

    // A clean production-mode host can be launched again after every forced
    // Job teardown and publishes a fresh normalized compatibility result.
    auto recoveredRequest = Request(composed.path, generation);
    recoveredRequest.compatibilityHostExePath = PREVIEW3D_IMPORT_HOST_EXE;
    const auto recovered = import_broker::RunImportSession(recoveredRequest);
    CHECK(recovered.ok);
    CHECK(recovered.errorCode == model_core::ImportErrorCode::None);
    CHECK(recovered.producer == import_broker::ImportProducer::CompatibilityHost);
    CHECK(recovered.workerProcessId != 0);
}

TEST_CASE("USD-007 compatibility fixtures retain their reviewed bytes", "[usd-007][fixtures]")
{
    const std::pair<std::string_view, std::string_view> fixtures[] = {
        {"compat-composition.usda", "978d20fa211df079e1c58ec4b850371248157382ef419adf149328e20c10e7c3"},
        {"compat-payload.usda", "d4f2d1588218de42eec1dbbc1258fa1ce418f57652ca22116f7acd05e41482db"},
        {"compat-point-instancer.usda", "4f1b5ddcb318ec0e629d37c62bf7faff6ae341eaf07b483e34c0b2f403a2a70d"},
        {"compat-ref.usda", "ccd77a3bb27608639b1a178766a00c5905c1c7a89718deb67c7033d85874129a"},
        {"compat-sub.usda", "a11a3798ebabeeb81f7cbada0397bb7294e83ff71fa42173b48a405798884df4"},
    };
    for (const auto& [name, expected] : fixtures) {
        CAPTURE(name);
        CHECK(Sha256Hex(ReadFixture(name)) == expected);
    }
}

TEST_CASE("USD-007 composes the bounded local arc set into normalized OpenUSD batches",
          "[usd-007][openusd][composition][variants][payloads][inherits][specializes]")
{
    ScratchUsd scratch(L"usda");
    scratch.Write(ReadFixture("compat-composition.usda"));
    for (const char* name : {"compat-sub.usda", "compat-ref.usda", "compat-payload.usda"}) {
        const std::string narrowName(name);
        scratch.WriteSidecar(std::wstring(narrowName.begin(), narrowName.end()), ReadFixture(narrowName));
    }
    auto request = Request(scratch.path, 700);
    request.compatibilityHostExePath = PREVIEW3D_IMPORT_HOST_EXE;
    request.sectionByteCapacity = import_broker::kImportSectionBytes;
    request.maxChunkCount = 1024;
    request.maxChunkBatchesPerGeneration = 64;
    const auto result = import_broker::RunImportSession(request);
    CAPTURE(uint32_t(result.stage), uint32_t(result.errorCode), result.batchCount);
    REQUIRE(result.ok);
    CHECK(result.producer == import_broker::ImportProducer::CompatibilityHost);
    CHECK(result.chunks.front().scene.format == model_core::SourceFormatId::Usda);
    CHECK(result.chunks.front().scene.upAxis == model_core::UpAxisId::Z);
    CHECK(std::abs(result.chunks.front().scene.metersPerUnit - 0.01) < 1e-12);
    CHECK(Count(result, model_core::ChunkTopology::TriangleList) == 6);
    CHECK(Count(result, model_core::ChunkTopology::MeshInstance) == 6);
    CHECK(Count(result, model_core::ChunkTopology::Node) >= 12);

    const std::pair<std::string, std::vector<std::byte>> packageEntries[] = {
        {"root.usda", ReadFixture("compat-composition.usda")},
        {"compat-sub.usda", ReadFixture("compat-sub.usda")},
        {"compat-ref.usda", ReadFixture("compat-ref.usda")},
        {"compat-payload.usda", ReadFixture("compat-payload.usda")},
    };
    ScratchUsd package(L"usdz");
    package.Write(StoredUsdz(packageEntries));
    auto packageRequest = Request(package.path, 7001);
    packageRequest.compatibilityHostExePath = PREVIEW3D_IMPORT_HOST_EXE;
    packageRequest.sectionByteCapacity = import_broker::kImportSectionBytes;
    packageRequest.maxChunkCount = 1024;
    packageRequest.maxChunkBatchesPerGeneration = 64;
    const auto packageResult = import_broker::RunImportSession(packageRequest);
    CAPTURE(uint32_t(packageResult.stage), uint32_t(packageResult.errorCode));
    REQUIRE(packageResult.ok);
    CHECK(packageResult.producer == import_broker::ImportProducer::CompatibilityHost);
    CHECK(packageResult.chunks.front().scene.format == model_core::SourceFormatId::Usdz);
    CHECK(Count(packageResult, model_core::ChunkTopology::TriangleList) == 6);
    CHECK(Count(packageResult, model_core::ChunkTopology::MeshInstance) == 6);
}

TEST_CASE("USD-007 expands bounded point instances and omits invisible ids",
          "[usd-007][openusd][point-instancer][instances]")
{
    ScratchUsd scratch(L"usda");
    std::string root = "#usda 1.0\ndef Xform \"Imported\" (prepend references = @scene.usda@) {}\n";
    scratch.Write(std::as_bytes(std::span(root)));
    scratch.WriteSidecar(L"scene.usda", ReadFixture("compat-point-instancer.usda"));
    auto request = Request(scratch.path, 701);
    request.compatibilityHostExePath = PREVIEW3D_IMPORT_HOST_EXE;
    request.sectionByteCapacity = import_broker::kImportSectionBytes;
    request.maxChunkCount = 1024;
    request.maxChunkBatchesPerGeneration = 64;
    const auto result = import_broker::RunImportSession(request);
    CAPTURE(uint32_t(result.stage), uint32_t(result.errorCode), result.batchCount);
    REQUIRE(result.ok);
    CHECK(Count(result, model_core::ChunkTopology::TriangleList) == 1);
    CHECK(Count(result, model_core::ChunkTopology::MeshInstance) == 2);
    std::vector<double> minimumX;
    for (const auto& chunk : result.chunks) if (chunk.descriptor.topology == model_core::ChunkTopology::MeshInstance)
        minimumX.push_back(Payload<model_core::MeshInstancePayload>(chunk).worldMin[0]);
    std::sort(minimumX.begin(), minimumX.end());
    REQUIRE(minimumX.size() == 2);
    CHECK(minimumX[0] == 0.0);
    CHECK(minimumX[1] == 4.0);
}

TEST_CASE("USD-007 preserves common fast-path geometry semantics and Preview Surface facts",
          "[usd-007][openusd][equivalence][materials][textures]")
{
    ScratchUsd direct(L"usda");
    direct.Write(ReadFixture("mesh.usda"));
    const auto fast = import_broker::RunImportSession(Request(direct.path, 702));
    REQUIRE(fast.ok);

    ScratchUsd composed(L"usda");
    const auto rootBytes = ReadFixture("mesh.usda");
    std::string root(reinterpret_cast<const char*>(rootBytes.data()), rootBytes.size());
    const auto metadata = root.find("(\n");
    REQUIRE(metadata != std::string::npos);
    root.insert(metadata + 2, "    subLayers = [@empty.usda@]\n");
    composed.Write(std::as_bytes(std::span(root)));
    const std::string emptyLayer = "#usda 1.0\n";
    composed.WriteSidecar(L"empty.usda", std::as_bytes(std::span(emptyLayer)));
    auto compatibilityRequest = Request(composed.path, 702);
    compatibilityRequest.compatibilityHostExePath = PREVIEW3D_IMPORT_HOST_EXE;
    const auto compatibility = import_broker::RunImportSession(compatibilityRequest);
    CAPTURE(uint32_t(compatibility.stage), uint32_t(compatibility.errorCode));
    REQUIRE(compatibility.ok);
    CHECK(compatibility.chunks.front().scene.upAxis == fast.chunks.front().scene.upAxis);
    CHECK(compatibility.chunks.front().scene.metersPerUnit == fast.chunks.front().scene.metersPerUnit);
    CHECK(Count(compatibility, model_core::ChunkTopology::TriangleList)
          == Count(fast, model_core::ChunkTopology::TriangleList));
    CHECK(CanonicalGeometryPositions(compatibility) == CanonicalGeometryPositions(fast));
    CHECK(CanonicalUsdFingerprint(compatibility) == CanonicalUsdFingerprint(fast));
    CHECK(compatibility.chunks.front().scene.meshCount == fast.chunks.front().scene.meshCount);
    CHECK(compatibility.chunks.front().scene.nodeCount == fast.chunks.front().scene.nodeCount);
    REQUIRE(compatibility.chunks.size() == fast.chunks.size());
    for (std::size_t index = 0; index < fast.chunks.size(); ++index) {
        CAPTURE(index, fast.chunks[index].descriptor.topology,
                compatibility.chunks[index].descriptor.topology);
        CHECK(compatibility.chunks[index].descriptor.chunkId
              == fast.chunks[index].descriptor.chunkId);
        CHECK(compatibility.chunks[index].descriptor.meshId
              == fast.chunks[index].descriptor.meshId);
        CHECK(compatibility.chunks[index].descriptor.topology
              == fast.chunks[index].descriptor.topology);
        CHECK(compatibility.chunks[index].descriptor.vertexCount
              == fast.chunks[index].descriptor.vertexCount);
        CHECK(compatibility.chunks[index].descriptor.indexCount
              == fast.chunks[index].descriptor.indexCount);
        CHECK(compatibility.chunks[index].descriptor.vertexLayoutId
              == fast.chunks[index].descriptor.vertexLayoutId);
        CHECK(compatibility.chunks[index].descriptor.geometryFlags
              == fast.chunks[index].descriptor.geometryFlags);
        CHECK(compatibility.chunks[index].descriptor.dependencyCount
              == fast.chunks[index].descriptor.dependencyCount);
        CHECK(std::equal(std::begin(compatibility.chunks[index].descriptor.dependencyIds),
                         std::end(compatibility.chunks[index].descriptor.dependencyIds),
                         std::begin(fast.chunks[index].descriptor.dependencyIds)));
        CHECK(std::equal(std::begin(compatibility.chunks[index].descriptor.origin),
                         std::end(compatibility.chunks[index].descriptor.origin),
                         std::begin(fast.chunks[index].descriptor.origin)));
        CHECK(std::equal(std::begin(compatibility.chunks[index].descriptor.localMin),
                         std::end(compatibility.chunks[index].descriptor.localMin),
                         std::begin(fast.chunks[index].descriptor.localMin)));
        CHECK(std::equal(std::begin(compatibility.chunks[index].descriptor.localMax),
                         std::end(compatibility.chunks[index].descriptor.localMax),
                         std::begin(fast.chunks[index].descriptor.localMax)));
        CHECK(compatibility.chunks[index].payload == fast.chunks[index].payload);
    }

    ScratchUsd materials(L"usda");
    const std::string materialRoot = "#usda 1.0\ndef Xform \"Imported\" (prepend references = @materials.usda@) {}\n";
    materials.Write(std::as_bytes(std::span(materialRoot)));
    materials.WriteSidecar(L"materials.usda", ReadFixture("materials.usda"));
    const std::string pngBase64 =
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9WlF4iUAAAAASUVORK5CYII=";
    materials.WriteSidecar(L"albedo.png", DecodeBase64(pngBase64));
    auto materialRequest = Request(materials.path, 704);
    materialRequest.compatibilityHostExePath = PREVIEW3D_IMPORT_HOST_EXE;
    materialRequest.sectionByteCapacity = import_broker::kImportSectionBytes;
    materialRequest.maxChunkCount = 1024;
    materialRequest.maxChunkBatchesPerGeneration = 64;
    const auto materialResult = import_broker::RunImportSession(materialRequest);
    CAPTURE(uint32_t(materialResult.stage), uint32_t(materialResult.errorCode));
    REQUIRE(materialResult.ok);
    CHECK(Count(materialResult, model_core::ChunkTopology::Material) == 2);
    CHECK(Count(materialResult, model_core::ChunkTopology::Image) >= 1);
    CHECK(Count(materialResult, model_core::ChunkTopology::TriangleList) == 2);
    CHECK(std::ranges::any_of(materialResult.chunks, [](const auto& chunk) {
        if (chunk.descriptor.topology != model_core::ChunkTopology::Material) return false;
        const auto material = Payload<model_core::MaterialPayload>(chunk);
        return std::abs(material.metallicFactor - 0.25f) < 1e-6f
            && std::abs(material.roughnessFactor - 0.75f) < 1e-6f
            && std::abs(material.uvOffset[0] - 0.25f) < 1e-6f
            && std::abs(material.uvScale[0] - 2.0f) < 1e-6f
            && (material.flags & model_core::kMaterialFlagFlipV) != 0
            && material.baseColorFactor[0] == 1.0f
            && chunk.descriptor.dependencyIds[0] != 0;
    }));

    const std::pair<std::string, std::vector<std::byte>> materialEntries[] = {
        {"root.usda", std::vector<std::byte>(std::as_bytes(std::span(materialRoot)).begin(),
                                               std::as_bytes(std::span(materialRoot)).end())},
        {"materials.usda", ReadFixture("materials.usda")},
        {"albedo.png", DecodeBase64(pngBase64)},
    };
    ScratchUsd materialPackage(L"usdz");
    materialPackage.Write(StoredUsdz(materialEntries));
    auto packageRequest = Request(materialPackage.path, 7041);
    packageRequest.compatibilityHostExePath = PREVIEW3D_IMPORT_HOST_EXE;
    packageRequest.sectionByteCapacity = import_broker::kImportSectionBytes;
    packageRequest.maxChunkCount = 1024;
    packageRequest.maxChunkBatchesPerGeneration = 64;
    const auto packageResult = import_broker::RunImportSession(packageRequest);
    CAPTURE(uint32_t(packageResult.stage), uint32_t(packageResult.errorCode));
    REQUIRE(packageResult.ok);
    CHECK(packageResult.producer == import_broker::ImportProducer::CompatibilityHost);
    CHECK(std::ranges::any_of(packageResult.chunks, [](const auto& chunk) {
        if (chunk.descriptor.topology != model_core::ChunkTopology::Image
            || chunk.payload.size() < sizeof(model_core::ImagePayloadHeader)) return false;
        model_core::ImagePayloadHeader image{};
        std::memcpy(&image, chunk.payload.data(), sizeof(image));
        return image.width == 1 && image.height == 1;
    }));

    ScratchUsd missingTexture(L"usda");
    missingTexture.Write(std::as_bytes(std::span(materialRoot)));
    missingTexture.WriteSidecar(L"materials.usda", ReadFixture("materials.usda"));
    auto missingTextureRequest = Request(missingTexture.path, 705);
    missingTextureRequest.compatibilityHostExePath = PREVIEW3D_IMPORT_HOST_EXE;
    const auto missingTextureResult = import_broker::RunImportSession(missingTextureRequest);
    REQUIRE(missingTextureResult.ok);
    CHECK(Count(missingTextureResult, model_core::ChunkTopology::Image) >= 1);
    CHECK(std::ranges::any_of(missingTextureResult.chunks, [](const auto& chunk) {
        if (chunk.descriptor.topology != model_core::ChunkTopology::ImportStatus) return false;
        return Payload<model_core::ImportStatusPayload>(chunk).textureWarnings > 0;
    }));
}

TEST_CASE("USD-007 composition denial and dependency pressure recover on the next stage",
          "[usd-007][openusd][negative][limits][recovery]")
{
    ScratchUsd missing(L"usda");
    const std::string missingRoot =
        "#usda 1.0\ndef Xform \"Root\" (prepend references = @missing.usda@) {}\n";
    missing.Write(std::as_bytes(std::span(missingRoot)));
    auto missingRequest = Request(missing.path, 706);
    missingRequest.compatibilityHostExePath = PREVIEW3D_IMPORT_HOST_EXE;
    const auto denied = import_broker::RunImportSession(missingRequest);
    CHECK_FALSE(denied.ok);
    CHECK(denied.errorCode == model_core::ImportErrorCode::CompatibilityHostFailure);
    CHECK(denied.producer == import_broker::ImportProducer::CompatibilityHost);

    const auto checkClosedCompositionFailure = [](std::string_view root,
                                                   std::string_view sidecar,
                                                   std::uint64_t generation) {
        ScratchUsd scratch(L"usda");
        scratch.Write(std::as_bytes(std::span(root)));
        scratch.WriteSidecar(L"child.usda", std::as_bytes(std::span(sidecar)));
        auto request = Request(scratch.path, generation);
        request.compatibilityHostExePath = PREVIEW3D_IMPORT_HOST_EXE;
        const auto result = import_broker::RunImportSession(request);
        CAPTURE(generation, uint32_t(result.stage), uint32_t(result.errorCode));
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::CompatibilityHostFailure);
        CHECK(result.producer == import_broker::ImportProducer::CompatibilityHost);
    };
    constexpr std::string_view composedRoot =
        "#usda 1.0\n(\n subLayers = [@child.usda@]\n)\n"
        "def Mesh \"M\" { int[] faceVertexCounts=[3] int[] faceVertexIndices=[0,1,2] "
        "point3f[] points=[(0,0,0),(1,0,0),(0,1,0)] uniform token subdivisionScheme=\"none\" }\n";
    checkClosedCompositionFailure(composedRoot, "not a usd layer", 707);
    checkClosedCompositionFailure(composedRoot,
        "#usda 1.0\ndef Volume \"Required\" {}\n", 708);
    checkClosedCompositionFailure(composedRoot,
        "#usda 1.0\n(\n subLayers = [@child.usda@]\n)\n", 709);

    ScratchUsd traversal(L"usda");
    const std::string traversalRoot =
        "#usda 1.0\n(\n subLayers = [@../escape.usda@]\n)\n"
        "def Mesh \"M\" { int[] faceVertexCounts=[3] int[] faceVertexIndices=[0,1,2] "
        "point3f[] points=[(0,0,0),(1,0,0),(0,1,0)] uniform token subdivisionScheme=\"none\" }\n";
    traversal.Write(std::as_bytes(std::span(traversalRoot)));
    auto traversalRequest = Request(traversal.path, 710);
    traversalRequest.compatibilityHostExePath = PREVIEW3D_IMPORT_HOST_EXE;
    const auto traversalResult = import_broker::RunImportSession(traversalRequest);
    CHECK_FALSE(traversalResult.ok);
    CHECK(traversalResult.errorCode == model_core::ImportErrorCode::CompatibilityHostFailure);
    CHECK(traversalResult.producer == import_broker::ImportProducer::CompatibilityHost);

    ScratchUsd pressure(L"usda");
    const std::string pressureRoot = "#usda 1.0\ndef Xform \"One\" (prepend references = @one.usda@) {}\n";
    const std::string one = "#usda 1.0\n(defaultPrim=\"One\")\ndef Xform \"One\" (prepend references = @two.usda@) {}\n";
    const std::string two = "#usda 1.0\n(defaultPrim=\"Two\")\ndef Xform \"Two\" {}\n";
    pressure.Write(std::as_bytes(std::span(pressureRoot)));
    pressure.WriteSidecar(L"one.usda", std::as_bytes(std::span(one)));
    pressure.WriteSidecar(L"two.usda", std::as_bytes(std::span(two)));
    auto pressureRequest = Request(pressure.path, 711);
    pressureRequest.compatibilityHostExePath = PREVIEW3D_IMPORT_HOST_EXE;
    pressureRequest.maxSidecarRequestsPerGeneration = 1;
    const auto limited = import_broker::RunImportSession(pressureRequest);
    CHECK_FALSE(limited.ok);
    CHECK(limited.errorCode == model_core::ImportErrorCode::CompatibilityHostLimit);

    ScratchUsd recovered(L"usda");
    recovered.Write(ReadFixture("mesh.usda"));
    const auto valid = import_broker::RunImportSession(Request(recovered.path, 712));
    REQUIRE(valid.ok);
    CHECK(valid.producer == import_broker::ImportProducer::FastWorker);
}
