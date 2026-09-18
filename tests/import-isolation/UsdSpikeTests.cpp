#include <catch2/catch_test_macros.hpp>

#include "SandboxTestSupport.h"
#include "UsdSpikeWorker.h"
#include "UsdZipPreflight.h"
#include "import_broker/SharedSection.h"
#include "import_broker/WorkerPool.h"
#include "model_core/ControlProtocol.h"
#include "platform/MappedView.h"
#include "platform/Sha256.h"
#include "platform/Win32Handle.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef PREVIEW3D_USD_FIXTURES_DIR
#error "PREVIEW3D_USD_FIXTURES_DIR must be defined"
#endif

namespace {

std::vector<std::byte> ReadFile(const char* name)
{
    const auto path = std::filesystem::path(PREVIEW3D_USD_FIXTURES_DIR) / name;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    REQUIRE(input);
    const auto length = input.tellg();
    REQUIRE(length >= 0);
    std::vector<std::byte> bytes(static_cast<size_t>(length));
    input.seekg(0);
    if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()), length);
    REQUIRE((input.good() || input.eof()));
    return bytes;
}

std::vector<std::byte> DecodeBase64(const char* name)
{
    const auto encodedBytes = ReadFile(name);
    std::vector<std::byte> decoded;
    uint32_t accumulator = 0;
    unsigned bits = 0;
    for (const std::byte raw : encodedBytes) {
        const unsigned char ch = std::to_integer<unsigned char>(raw);
        if (std::isspace(ch)) continue;
        if (ch == '=') break;
        uint32_t value = 0;
        if (ch >= 'A' && ch <= 'Z') value = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') value = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') value = ch - '0' + 52;
        else if (ch == '+') value = 62;
        else if (ch == '/') value = 63;
        else FAIL("invalid base64 fixture byte");
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(std::byte((accumulator >> bits) & 0xffu));
        }
    }
    return decoded;
}

std::string Hex(std::span<const std::byte> bytes)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const std::byte byte : bytes) {
        const uint8_t value = std::to_integer<uint8_t>(byte);
        text.push_back(digits[value >> 4]);
        text.push_back(digits[value & 15]);
    }
    return text;
}

void CheckSha256(std::span<const std::byte> bytes, std::string_view expected)
{
    const auto digest = platform::ComputeSha256(bytes);
    REQUIRE(digest);
    CHECK(Hex(*digest) == expected);
}

struct Fixture {
    const char* name;
    std::vector<std::byte> bytes;
    import_worker::UsdSpikeFormat format;
    const char* sha256;
};

std::vector<Fixture> Fixtures()
{
    return {
        { "mesh.usda", ReadFile("mesh.usda"), import_worker::UsdSpikeFormat::Usda,
          "A56B200F47B0112078A3C01A37C66B94C24F7E5C82E853B873E41D40A646BDB3" },
        { "cube.usdc", DecodeBase64("cube.usdc.base64"), import_worker::UsdSpikeFormat::Usdc,
          "D4C3C527DE10837036C602982E9D6733B14AD26AB17C2BCA6D43F2FCFE7C97C3" },
        { "cube.usdz", DecodeBase64("cube.usdz.base64"), import_worker::UsdSpikeFormat::Usdz,
          "DBBFCC999D2ABE55F0F1DD49E158D65739B87F002287C8B669A57DBA5F33E52D" },
    };
}

std::vector<std::byte> LargeUsda()
{
    std::string text = "#usda 1.0\ndef Mesh \"Pressure\" { point3f[] points = [";
    text.reserve(4'100'000);
    for (size_t index = 0; index < 500'000; ++index) text += "(0,0,0),";
    text += "] int[] faceVertexCounts = [] int[] faceVertexIndices = [] }\n";
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

std::optional<import_worker::UsdSpikePayloadHeader> RunSpike(
    import_broker::WorkerPool& pool, size_t workerIndex, std::span<const std::byte> source,
    import_worker::UsdSpikeMode mode, uint64_t generation)
{
    const SIZE_T sectionSize = sizeof(import_worker::UsdSpikePayloadHeader) + source.size();
    auto section = import_broker::CreateSharedSection(sectionSize);
    if (!section) return std::nullopt;
    auto view = platform::MappedView::Map(section.get(), FILE_MAP_READ | FILE_MAP_WRITE, sectionSize);
    if (!view) return std::nullopt;
    auto* header = reinterpret_cast<import_worker::UsdSpikePayloadHeader*>(view.bytes().data());
    std::memset(header, 0, sizeof(*header));
    header->magic = import_worker::kUsdSpikePayloadMagic;
    header->sourceByteLength = source.size();
    header->mode = static_cast<uint32_t>(mode);
    if (!source.empty()) std::memcpy(view.bytes().data() + sizeof(*header), source.data(), source.size());

    platform::Win32Handle cancellationEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!cancellationEvent) return std::nullopt;
    const auto workerEvent = pool.DuplicateSectionIntoWorker(workerIndex, cancellationEvent.get());
    const auto workerSection = pool.DuplicateSectionIntoWorker(workerIndex, section.get());
    if (!workerEvent || !workerSection) return std::nullopt;
    header->cancellationEventHandleValue = *workerEvent;

    model_core::StartGenerationRequest request{};
    request.generationId = generation;
    request.sceneVariant = import_worker::kUsdSpikeSceneVariant;
    request.sectionHandleValue = *workerSection;
    request.sectionByteCapacity = sectionSize;
    request.maxChunkCount = 1;
    if (!pool.SendRequest(workerIndex, model_core::ControlOpcode::StartGeneration,
                          &request, sizeof(request))) return std::nullopt;
    model_core::ReceivedControlMessage reply{};
    if (pool.WaitForReply(workerIndex, 30'000, reply) != import_broker::WaitReplyOutcome::Ready
        || reply.header.opcode != uint32_t(model_core::ControlOpcode::ChunksReady)) return std::nullopt;
    import_worker::UsdSpikePayloadHeader result{};
    std::memcpy(&result, header, sizeof(result));
    return result;
}

void Write16(std::vector<std::byte>& out, uint16_t value)
{
    out.push_back(std::byte(value & 0xffu));
    out.push_back(std::byte(value >> 8));
}

void Write32(std::vector<std::byte>& out, uint32_t value)
{
    Write16(out, uint16_t(value & 0xffffu));
    Write16(out, uint16_t(value >> 16));
}

void Put16(std::vector<std::byte>& out, size_t offset, uint16_t value)
{
    out[offset] = std::byte(value & 0xffu);
    out[offset + 1] = std::byte(value >> 8);
}

size_t FindSignature(std::span<const std::byte> bytes, uint32_t signature)
{
    for (size_t offset = 0; offset + 4 <= bytes.size(); ++offset) {
        const uint32_t value = std::to_integer<uint8_t>(bytes[offset])
            | (uint32_t(std::to_integer<uint8_t>(bytes[offset + 1])) << 8)
            | (uint32_t(std::to_integer<uint8_t>(bytes[offset + 2])) << 16)
            | (uint32_t(std::to_integer<uint8_t>(bytes[offset + 3])) << 24);
        if (value == signature) return offset;
    }
    return std::string::npos;
}

std::vector<std::byte> BuildStoredZip(std::span<const std::string_view> names)
{
    struct Entry { uint32_t localOffset; std::string_view name; };
    std::vector<std::byte> zip;
    std::vector<Entry> entries;
    for (const auto name : names) {
        entries.push_back({ static_cast<uint32_t>(zip.size()), name });
        Write32(zip, 0x04034b50u); Write16(zip, 20); Write16(zip, 0); Write16(zip, 0);
        Write16(zip, 0); Write16(zip, 0); Write32(zip, 0xd202ef8du); Write32(zip, 1); Write32(zip, 1);
        Write16(zip, static_cast<uint16_t>(name.size()));
        const size_t extraLength = (64 - ((zip.size() + 2 + name.size()) & 63u)) & 63u;
        Write16(zip, static_cast<uint16_t>(extraLength));
        for (const char ch : name) zip.push_back(std::byte(static_cast<unsigned char>(ch)));
        zip.insert(zip.end(), extraLength, std::byte{0});
        zip.push_back(std::byte{0});
    }
    const uint32_t centralOffset = static_cast<uint32_t>(zip.size());
    for (const auto& entry : entries) {
        Write32(zip, 0x02014b50u); Write16(zip, 20); Write16(zip, 20); Write16(zip, 0);
        Write16(zip, 0); Write16(zip, 0); Write16(zip, 0); Write32(zip, 0xd202ef8du); Write32(zip, 1);
        Write32(zip, 1); Write16(zip, static_cast<uint16_t>(entry.name.size())); Write16(zip, 0);
        Write16(zip, 0); Write16(zip, 0); Write16(zip, 0); Write32(zip, 0);
        Write32(zip, entry.localOffset);
        for (const char ch : entry.name) zip.push_back(std::byte(static_cast<unsigned char>(ch)));
    }
    const uint32_t centralSize = static_cast<uint32_t>(zip.size()) - centralOffset;
    Write32(zip, 0x06054b50u); Write16(zip, 0); Write16(zip, 0);
    Write16(zip, static_cast<uint16_t>(entries.size()));
    Write16(zip, static_cast<uint16_t>(entries.size()));
    Write32(zip, centralSize); Write32(zip, centralOffset); Write16(zip, 0);
    return zip;
}

} // namespace

TEST_CASE("USD-001 pinned TinyUSDZ parses USDA USDC and USDZ deterministically in the real sandbox",
          "[usd-spike][formats][sandbox][determinism]")
{
    sandbox_test_support::SandboxFixture fixture;
    import_broker::WorkerPool pool;
    std::wstring error;
    REQUIRE(pool.InitializeForTesting(sandbox_test_support::WorkerExePath(), std::move(fixture.sid),
                                      {}, 1, L"--usd-spike-pool", error));
    const auto worker = pool.AcquireIdle();
    REQUIRE(worker);
    uint64_t generation = 0x757364010000ull;
    for (const auto& fixtureData : Fixtures()) {
        CheckSha256(fixtureData.bytes, fixtureData.sha256);
        uint64_t expectedHash = 0;
        for (unsigned run = 0; run < 3; ++run) {
            const auto result = RunSpike(pool, *worker, fixtureData.bytes,
                                         import_worker::UsdSpikeMode::ParseAndConvert,
                                         ++generation);
            CAPTURE(fixtureData.name, run);
            REQUIRE(result);
            REQUIRE(result->succeeded == 1);
            CHECK(result->detectedFormat == static_cast<uint32_t>(fixtureData.format));
            CHECK(result->meshCount >= 1);
            CHECK(result->nodeCount >= 1);
            CHECK(result->normalizedBytes > 0);
            if (run == 0) expectedHash = result->normalizedHash;
            else CHECK(result->normalizedHash == expectedHash);
            if (run == 0) {
                std::cout << "USD-001 fixture=" << fixtureData.name
                          << " parse-us=" << result->parseMicroseconds
                          << " convert-us=" << result->convertMicroseconds
                          << " private-before=" << result->privateBytesBefore
                          << " private-parse=" << result->privateBytesAfterParse
                          << " private-convert=" << result->privateBytesAfterConvert
                          << " peak-working-set=" << result->peakWorkingSetBytes
                          << " normalized-bytes=" << result->normalizedBytes
                          << " hash=0x" << std::hex << result->normalizedHash << std::dec << '\n';
            }
        }
    }
    pool.Release(*worker);
}

TEST_CASE("USD-001 all three encodings load through the custom memory resolver",
          "[usd-spike][resolver][sandbox]")
{
    sandbox_test_support::SandboxFixture fixture;
    import_broker::WorkerPool pool;
    std::wstring error;
    REQUIRE(pool.InitializeForTesting(sandbox_test_support::WorkerExePath(), std::move(fixture.sid),
                                      {}, 1, L"--usd-spike-pool", error));
    const auto worker = pool.AcquireIdle();
    REQUIRE(worker);
    uint64_t generation = 0x757364020000ull;
    for (const auto& fixtureData : Fixtures()) {
        const auto result = RunSpike(pool, *worker, fixtureData.bytes,
                                     import_worker::UsdSpikeMode::ResolverLayer, ++generation);
        CAPTURE(fixtureData.name);
        REQUIRE(result);
        CHECK(result->succeeded == 1);
        CHECK(result->resolverCalls >= 2);
        CHECK(result->detectedFormat == static_cast<uint32_t>(fixtureData.format));
    }
    pool.Release(*worker);
}

TEST_CASE("USD-001 malformed inputs fail without poisoning the pooled worker",
          "[usd-spike][malformed][recovery]")
{
    sandbox_test_support::SandboxFixture fixture;
    import_broker::WorkerPool pool;
    std::wstring error;
    REQUIRE(pool.InitializeForTesting(sandbox_test_support::WorkerExePath(), std::move(fixture.sid),
                                      {}, 1, L"--usd-spike-pool", error));
    const auto worker = pool.AcquireIdle();
    REQUIRE(worker);
    const DWORD processId = pool.ProcessId(*worker);
    uint64_t generation = 0x757364030000ull;
    const auto fixtures = Fixtures();
    for (const auto& fixtureData : fixtures) {
        std::vector<std::byte> malformed;
        if (fixtureData.format == import_worker::UsdSpikeFormat::Usda) {
            const std::string invalid =
                "#usda 1.0\ndef Mesh \"Broken\" { point3f[] points = [";
            malformed.resize(invalid.size());
            std::memcpy(malformed.data(), invalid.data(), invalid.size());
        } else if (fixtureData.format == import_worker::UsdSpikeFormat::Usdc) {
            // Retain the crate signature and enough TOC bytes to enter the
            // USDC reader, then truncate the object graph.
            malformed.assign(fixtureData.bytes.begin(), fixtureData.bytes.begin()
                + (std::min)(fixtureData.bytes.size(), size_t{128}));
        } else {
            // Retain the ZIP signature and local data but remove the EOCD.
            malformed.assign(fixtureData.bytes.begin(), fixtureData.bytes.end() - 8);
        }
        const auto failed = RunSpike(pool, *worker, malformed,
                                     import_worker::UsdSpikeMode::ParseAndConvert, ++generation);
        CAPTURE(fixtureData.name);
        REQUIRE(failed);
        CHECK(failed->succeeded == 0);
    }
    const auto recovered = RunSpike(pool, *worker, fixtures.front().bytes,
                                    import_worker::UsdSpikeMode::ParseAndConvert, ++generation);
    REQUIRE(recovered);
    CHECK(recovered->succeeded == 1);
    CHECK(pool.ProcessId(*worker) == processId);
    pool.Release(*worker);
}

TEST_CASE("USD-001 library memory setting is advisory and a later request succeeds",
          "[usd-spike][memory][recovery]")
{
    sandbox_test_support::SandboxFixture fixture;
    import_broker::WorkerPool pool;
    std::wstring error;
    REQUIRE(pool.InitializeForTesting(sandbox_test_support::WorkerExePath(), std::move(fixture.sid),
                                      {}, 1, L"--usd-spike-pool", error));
    const auto worker = pool.AcquireIdle();
    REQUIRE(worker);
    const DWORD processId = pool.ProcessId(*worker);
    const auto pressure = LargeUsda();
    REQUIRE(pressure.size() > 1024u * 1024u);
    const auto limited = RunSpike(pool, *worker, pressure,
                                  import_worker::UsdSpikeMode::OneMiBAdvisoryLimit,
                                  0x757364035001ull);
    REQUIRE(limited);
    // TinyUSDZ 0.9.1 documents this setting as advisory. USDA parsing succeeds
    // despite a source four times the requested limit, proving it cannot be a
    // product allocation boundary.
    CHECK(limited->succeeded == 1);
    CHECK(pool.ProcessId(*worker) == processId);
    const auto valid = ReadFile("mesh.usda");
    const auto recovered = RunSpike(pool, *worker, valid,
                                    import_worker::UsdSpikeMode::ParseAndConvert,
                                    0x757364035002ull);
    REQUIRE(recovered);
    CHECK(recovered->succeeded == 1);
    pool.Release(*worker);
}

TEST_CASE("USD-001 low Job commit cap bounds parse pressure and the worker remains usable",
          "[usd-spike][memory][job][recovery]")
{
    sandbox_test_support::SandboxFixture fixture;
    import_broker::WorkerPool pool;
    import_broker::SandboxLimits limits{};
    limits.processMemoryLimitBytes = 16u * 1024u * 1024u;
    std::wstring error;
    REQUIRE(pool.InitializeForTesting(sandbox_test_support::WorkerExePath(), std::move(fixture.sid),
                                      limits, 1, L"--usd-spike-pool", error));
    const auto worker = pool.AcquireIdle();
    REQUIRE(worker);
    const DWORD originalProcessId = pool.ProcessId(*worker);
    const auto pressure = LargeUsda();
    const auto exhausted = RunSpike(pool, *worker, pressure,
                                    import_worker::UsdSpikeMode::ParseAndConvert,
                                    0x757364036001ull);
    if (exhausted) {
        std::cout << "USD-001 capped-pressure private-before=" << exhausted->privateBytesBefore
                  << " private-parse=" << exhausted->privateBytesAfterParse
                  << " private-convert=" << exhausted->privateBytesAfterConvert
                  << " peak-working-set=" << exhausted->peakWorkingSetBytes << '\n';
    }
    REQUIRE(exhausted);
    CHECK(exhausted->succeeded == 0);
    CHECK(pool.ProcessId(*worker) == originalProcessId);
    const auto valid = ReadFile("mesh.usda");
    const auto recovered = RunSpike(pool, *worker, valid,
                                    import_worker::UsdSpikeMode::ParseAndConvert,
                                    0x757364036002ull);
    REQUIRE(recovered);
    CHECK(recovered->succeeded == 1);
    CHECK(pool.ProcessId(*worker) == originalProcessId);
    pool.Release(*worker);
}

TEST_CASE("USD-001 noninterruptible TinyUSDZ phase is replaced after the cancellation grace",
          "[usd-spike][cancellation][worker-pool]")
{
    sandbox_test_support::SandboxFixture fixture;
    import_broker::WorkerPool pool;
    std::wstring error;
    REQUIRE(pool.InitializeForTesting(sandbox_test_support::WorkerExePath(), std::move(fixture.sid),
                                      {}, 1, L"--usd-spike-pool", error));
    const auto worker = pool.AcquireIdle();
    REQUIRE(worker);
    const DWORD originalProcessId = pool.ProcessId(*worker);
    const auto source = ReadFile("mesh.usda");
    const SIZE_T sectionSize = sizeof(import_worker::UsdSpikePayloadHeader) + source.size();
    auto section = import_broker::CreateSharedSection(sectionSize);
    REQUIRE(section);
    auto view = platform::MappedView::Map(section.get(), FILE_MAP_READ | FILE_MAP_WRITE, sectionSize);
    REQUIRE(view);
    auto* header = reinterpret_cast<import_worker::UsdSpikePayloadHeader*>(view.bytes().data());
    std::memset(header, 0, sizeof(*header));
    header->magic = import_worker::kUsdSpikePayloadMagic;
    header->sourceByteLength = source.size();
    header->mode = static_cast<uint32_t>(import_worker::UsdSpikeMode::HoldAfterParse);
    std::memcpy(view.bytes().data() + sizeof(*header), source.data(), source.size());
    platform::Win32Handle cancellationEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    REQUIRE(cancellationEvent);
    const auto workerEvent = pool.DuplicateSectionIntoWorker(*worker, cancellationEvent.get());
    const auto workerSection = pool.DuplicateSectionIntoWorker(*worker, section.get());
    REQUIRE(workerEvent); REQUIRE(workerSection);
    header->cancellationEventHandleValue = *workerEvent;
    model_core::StartGenerationRequest request{};
    request.generationId = 0x757364040001ull;
    request.sceneVariant = import_worker::kUsdSpikeSceneVariant;
    request.sectionHandleValue = *workerSection;
    request.sectionByteCapacity = sectionSize;
    request.maxChunkCount = 1;
    REQUIRE(pool.SendRequest(*worker, model_core::ControlOpcode::StartGeneration,
                             &request, sizeof(request)));
    const auto enteredDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (InterlockedCompareExchange(&header->state, 0, 0) != 2
           && std::chrono::steady_clock::now() < enteredDeadline) Sleep(1);
    REQUIRE(InterlockedCompareExchange(&header->state, 0, 0) == 2);
    platform::Win32Handle processCopy;
    HANDLE rawCopy = nullptr;
    REQUIRE(DuplicateHandle(GetCurrentProcess(), pool.ProcessHandle(*worker), GetCurrentProcess(),
                            &rawCopy, SYNCHRONIZE, FALSE, 0));
    processCopy.reset(rawCopy);
    const auto cancelStart = std::chrono::steady_clock::now();
    REQUIRE(SetEvent(cancellationEvent.get()));
    model_core::ReceivedControlMessage reply{};
    CHECK(pool.WaitForReply(*worker, 500, reply) == import_broker::WaitReplyOutcome::TimedOut);
    REQUIRE(pool.TerminateAndReplace(*worker, error));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - cancelStart);
    CHECK(elapsed >= std::chrono::milliseconds(450));
    CHECK(elapsed < std::chrono::milliseconds(2500));
    CHECK(WaitForSingleObject(processCopy.get(), 2000) == WAIT_OBJECT_0);
    CHECK(pool.ProcessId(*worker) != originalProcessId);
    pool.Release(*worker);

    const auto replacement = pool.AcquireIdle();
    REQUIRE(replacement);
    const auto recovered = RunSpike(pool, *replacement, source,
                                    import_worker::UsdSpikeMode::ParseAndConvert,
                                    0x757364040002ull);
    REQUIRE(recovered);
    CHECK(recovered->succeeded == 1);
    pool.Release(*replacement);
    std::cout << "USD-001 cancellation-replacement-ms=" << elapsed.count() << '\n';
}

TEST_CASE("USD-001 USDZ preflight rejects archive authority and expansion hazards",
          "[usd-spike][usdz][archive]")
{
    using import_worker::UsdzPreflightError;
    const auto valid = DecodeBase64("cube.usdz.base64");
    CHECK(import_worker::PreflightUsdz(valid) == UsdzPreflightError::None);

    auto unsafe = valid;
    const std::array<std::byte, 9> original = { std::byte{'c'}, std::byte{'u'}, std::byte{'b'},
        std::byte{'e'}, std::byte{'.'}, std::byte{'u'}, std::byte{'s'}, std::byte{'d'}, std::byte{'c'} };
    const std::array<std::byte, 9> traversal = { std::byte{'.'}, std::byte{'.'}, std::byte{'/'},
        std::byte{'x'}, std::byte{'.'}, std::byte{'u'}, std::byte{'s'}, std::byte{'d'}, std::byte{'c'} };
    for (auto position = std::search(unsafe.begin(), unsafe.end(), original.begin(), original.end());
         position != unsafe.end();
         position = std::search(position + 1, unsafe.end(), original.begin(), original.end()))
        std::copy(traversal.begin(), traversal.end(), position);
    CHECK(import_worker::PreflightUsdz(unsafe) == UsdzPreflightError::UnsafePath);

    auto encrypted = valid;
    const size_t local = FindSignature(encrypted, 0x04034b50u);
    const size_t central = FindSignature(encrypted, 0x02014b50u);
    REQUIRE(local != std::string::npos); REQUIRE(central != std::string::npos);
    Put16(encrypted, local + 6, 1); Put16(encrypted, central + 8, 1);
    CHECK(import_worker::PreflightUsdz(encrypted) == UsdzPreflightError::Encrypted);

    auto compressed = valid;
    Put16(compressed, local + 8, 8); Put16(compressed, central + 10, 8);
    CHECK(import_worker::PreflightUsdz(compressed) == UsdzPreflightError::UnsupportedCompression);

    auto misaligned = valid;
    Put16(misaligned, local + 28, 0);
    CHECK(import_worker::PreflightUsdz(misaligned) == UsdzPreflightError::Misaligned);

    auto zip64 = valid;
    const size_t eocd = FindSignature(zip64, 0x06054b50u);
    REQUIRE(eocd != std::string::npos);
    Put16(zip64, eocd + 8, 0xffffu); Put16(zip64, eocd + 10, 0xffffu);
    CHECK(import_worker::PreflightUsdz(zip64) == UsdzPreflightError::Zip64);

    const std::array<std::string_view, 2> colliding = { "A.usda", "a.usda" };
    const auto duplicate = BuildStoredZip(colliding);
    CHECK(import_worker::PreflightUsdz(duplicate) == UsdzPreflightError::DuplicatePath);

    import_worker::UsdzPreflightLimits limits{};
    limits.maxEntryBytes = 16;
    CHECK(import_worker::PreflightUsdz(valid, limits) == UsdzPreflightError::EntryTooLarge);
    limits = {};
    limits.maxExpandedBytes = 16;
    CHECK(import_worker::PreflightUsdz(valid, limits) == UsdzPreflightError::AggregateTooLarge);
}
