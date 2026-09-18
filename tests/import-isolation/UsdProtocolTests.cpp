#include "SandboxTestSupport.h"
#include "import_broker/ImportSession.h"
#include "import_broker/SharedSectionValidator.h"
#include "import_broker/UsdFallbackState.h"
#include "model_core/Checksum.h"
#include "model_core/ControlProtocol.h"
#include "model_core/WireFormat.h"

#include <catch2/catch_test_macros.hpp>

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
        uint32_t(model_core::ImportErrorCode::ArchiveLimit) + 1));
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
    struct Fixture { std::string_view name; std::wstring_view extension; model_core::SourceFormatId format; };
    const Fixture fixtures[] = {
        {"mesh.usda", L"usd", model_core::SourceFormatId::Usda},
        {"cube.usdc.base64", L"usd", model_core::SourceFormatId::Usdc},
        {"mesh.usda", L"usda", model_core::SourceFormatId::Usda},
        {"cube.usdc.base64", L"usdc", model_core::SourceFormatId::Usdc},
        {"cube.usdz.base64", L"usdz", model_core::SourceFormatId::Usdz},
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
            REQUIRE(result.chunks.size() == 1);
            CHECK(result.chunks.front().scene.format == fixtures[index].format);
            CHECK(result.chunks.front().scene.upAxis == model_core::UpAxisId::Y);
            CHECK(result.chunks.front().scene.metersPerUnit == 0.01);
        }
    }
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
