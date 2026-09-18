#include <catch2/catch_test_macros.hpp>

#include "SandboxTestSupport.h"
#include "ThreeMfSpikeWorker.h"
#include "import_broker/SharedSection.h"
#include "import_broker/WorkerPool.h"
#include "model_core/ControlProtocol.h"
#include "platform/MappedView.h"
#include "platform/Sha256.h"
#include "platform/Win32Handle.h"

#include <Bindings/Cpp/lib3mf_types.hpp>

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
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

#ifndef PREVIEW3D_3MF_FIXTURES_DIR
#error "PREVIEW3D_3MF_FIXTURES_DIR must be defined"
#endif

namespace {

std::vector<std::byte> ReadFile(const char* name)
{
    const auto path = std::filesystem::path(PREVIEW3D_3MF_FIXTURES_DIR) / name;
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
    const auto encoded = ReadFile(name);
    std::vector<std::byte> decoded;
    uint32_t accumulator = 0;
    unsigned bits = 0;
    for (const std::byte raw : encoded) {
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
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const std::byte byte : bytes) {
        const uint8_t value = std::to_integer<uint8_t>(byte);
        result.push_back(digits[value >> 4]);
        result.push_back(digits[value & 15]);
    }
    return result;
}

class TemporaryFile {
public:
    explicit TemporaryFile(std::span<const std::byte> bytes, uint64_t prefixBytes = 0)
    {
        wchar_t directory[MAX_PATH]{};
        REQUIRE(GetTempPathW(MAX_PATH, directory) != 0);
        wchar_t path[MAX_PATH]{};
        REQUIRE(GetTempFileNameW(directory, L"p3m", 0, path) != 0);
        path_ = path;
        handle_.reset(CreateFileW(path_.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr));
        REQUIRE(handle_);
        if (prefixBytes != 0) {
            DWORD ignored = 0;
            REQUIRE(DeviceIoControl(handle_.get(), FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                                    &ignored, nullptr));
            LARGE_INTEGER prefix{};
            prefix.QuadPart = static_cast<LONGLONG>(prefixBytes);
            REQUIRE(SetFilePointerEx(handle_.get(), prefix, nullptr, FILE_BEGIN));
        }
        size_t offset = 0;
        while (offset < bytes.size()) {
            const DWORD chunk = static_cast<DWORD>((std::min)(bytes.size() - offset,
                                                              size_t{MAXDWORD}));
            DWORD written = 0;
            REQUIRE(WriteFile(handle_.get(), bytes.data() + offset, chunk, &written, nullptr));
            REQUIRE(written == chunk);
            offset += written;
        }
        REQUIRE(FlushFileBuffers(handle_.get()));
        LARGE_INTEGER zero{};
        REQUIRE(SetFilePointerEx(handle_.get(), zero, nullptr, FILE_BEGIN));
    }

    ~TemporaryFile()
    {
        handle_.reset();
        if (!path_.empty()) DeleteFileW(path_.c_str());
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    HANDLE get() const { return handle_.get(); }

private:
    std::wstring path_;
    platform::Win32Handle handle_;
};

uint32_t Read32(std::span<const std::byte> bytes, size_t offset)
{
    REQUIRE(offset + 4 <= bytes.size());
    return std::to_integer<uint8_t>(bytes[offset])
        | (uint32_t(std::to_integer<uint8_t>(bytes[offset + 1])) << 8)
        | (uint32_t(std::to_integer<uint8_t>(bytes[offset + 2])) << 16)
        | (uint32_t(std::to_integer<uint8_t>(bytes[offset + 3])) << 24);
}

void Put32(std::span<std::byte> bytes, size_t offset, uint32_t value)
{
    REQUIRE(offset + 4 <= bytes.size());
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes[offset + shift / 8] = std::byte((value >> shift) & 0xffu);
}

std::vector<std::byte> RelocateZip(std::span<const std::byte> source, uint32_t prefix)
{
    std::vector<std::byte> relocated(source.begin(), source.end());
    size_t eocd = std::string::npos;
    for (size_t offset = relocated.size() - 22; ; --offset) {
        if (Read32(relocated, offset) == 0x06054b50u) {
            eocd = offset;
            break;
        }
        REQUIRE(offset != 0);
    }
    const uint32_t centralOffset = Read32(relocated, eocd + 16);
    REQUIRE(uint64_t(centralOffset) + prefix <= UINT32_MAX);
    size_t cursor = centralOffset;
    while (cursor < eocd) {
        REQUIRE(Read32(relocated, cursor) == 0x02014b50u);
        const uint16_t nameLength = uint16_t(std::to_integer<uint8_t>(relocated[cursor + 28]))
            | uint16_t(uint16_t(std::to_integer<uint8_t>(relocated[cursor + 29])) << 8);
        const uint16_t extraLength = uint16_t(std::to_integer<uint8_t>(relocated[cursor + 30]))
            | uint16_t(uint16_t(std::to_integer<uint8_t>(relocated[cursor + 31])) << 8);
        const uint16_t commentLength = uint16_t(std::to_integer<uint8_t>(relocated[cursor + 32]))
            | uint16_t(uint16_t(std::to_integer<uint8_t>(relocated[cursor + 33])) << 8);
        const uint32_t localOffset = Read32(relocated, cursor + 42);
        REQUIRE(uint64_t(localOffset) + prefix <= UINT32_MAX);
        Put32(relocated, cursor + 42, localOffset + prefix);
        cursor += 46u + nameLength + extraLength + commentLength;
    }
    REQUIRE(cursor == eocd);
    Put32(relocated, eocd + 16, centralOffset + prefix);
    return relocated;
}

void Append16(std::vector<std::byte>& output, uint16_t value)
{
    output.push_back(std::byte(value & 0xffu));
    output.push_back(std::byte(value >> 8));
}

void Append32(std::vector<std::byte>& output, uint32_t value)
{
    Append16(output, static_cast<uint16_t>(value & 0xffffu));
    Append16(output, static_cast<uint16_t>(value >> 16));
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

std::vector<std::byte> BuildStoredPackage(
    const std::vector<std::pair<std::string, std::string>>& entries)
{
    struct CentralEntry {
        uint32_t offset;
        uint32_t crc;
        uint32_t size;
        std::string name;
    };
    std::vector<std::byte> output;
    std::vector<CentralEntry> central;
    for (const auto& [name, content] : entries) {
        REQUIRE(name.size() <= UINT16_MAX);
        REQUIRE(content.size() <= UINT32_MAX);
        const auto contentBytes = std::as_bytes(std::span(content));
        const uint32_t crc = Crc32(contentBytes);
        central.push_back({ static_cast<uint32_t>(output.size()), crc,
                            static_cast<uint32_t>(content.size()), name });
        Append32(output, 0x04034b50u); Append16(output, 20); Append16(output, 0);
        Append16(output, 0); Append16(output, 0); Append16(output, 0); Append32(output, crc);
        Append32(output, static_cast<uint32_t>(content.size()));
        Append32(output, static_cast<uint32_t>(content.size()));
        Append16(output, static_cast<uint16_t>(name.size())); Append16(output, 0);
        output.insert(output.end(), reinterpret_cast<const std::byte*>(name.data()),
                      reinterpret_cast<const std::byte*>(name.data() + name.size()));
        output.insert(output.end(), contentBytes.begin(), contentBytes.end());
    }
    const uint32_t centralOffset = static_cast<uint32_t>(output.size());
    for (const auto& entry : central) {
        Append32(output, 0x02014b50u); Append16(output, 20); Append16(output, 20);
        Append16(output, 0); Append16(output, 0); Append16(output, 0); Append16(output, 0);
        Append32(output, entry.crc); Append32(output, entry.size); Append32(output, entry.size);
        Append16(output, static_cast<uint16_t>(entry.name.size())); Append16(output, 0);
        Append16(output, 0); Append16(output, 0); Append16(output, 0); Append32(output, 0);
        Append32(output, entry.offset);
        output.insert(output.end(), reinterpret_cast<const std::byte*>(entry.name.data()),
                      reinterpret_cast<const std::byte*>(entry.name.data() + entry.name.size()));
    }
    const uint32_t centralSize = static_cast<uint32_t>(output.size()) - centralOffset;
    Append32(output, 0x06054b50u); Append16(output, 0); Append16(output, 0);
    Append16(output, static_cast<uint16_t>(central.size()));
    Append16(output, static_cast<uint16_t>(central.size()));
    Append32(output, centralSize); Append32(output, centralOffset); Append16(output, 0);
    return output;
}

std::vector<std::byte> VertexPressurePackage(size_t extraVertexCount)
{
    std::string model = R"(<?xml version="1.0" encoding="UTF-8"?>
<model unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02"><resources><object id="1" type="model"><mesh><vertices>
<vertex x="0" y="0" z="0"/><vertex x="1" y="0" z="0"/><vertex x="1" y="1" z="0"/><vertex x="0" y="1" z="0"/><vertex x="0" y="0" z="1"/><vertex x="1" y="0" z="1"/><vertex x="1" y="1" z="1"/><vertex x="0" y="1" z="1"/>
)";
    model.reserve(extraVertexCount * 32 + 4096);
    for (size_t index = 0; index < extraVertexCount; ++index)
        model += "<vertex x=\"0\" y=\"0\" z=\"0\"/>\n";
    model += R"(</vertices><triangles>
<triangle v1="3" v2="2" v3="1"/><triangle v1="1" v2="0" v3="3"/><triangle v1="4" v2="5" v3="6"/><triangle v1="6" v2="7" v3="4"/><triangle v1="0" v2="1" v3="5"/><triangle v1="5" v2="4" v3="0"/><triangle v1="1" v2="2" v3="6"/><triangle v1="6" v2="5" v3="1"/><triangle v1="2" v2="3" v3="7"/><triangle v1="7" v2="6" v3="2"/><triangle v1="3" v2="0" v3="4"/><triangle v1="4" v2="7" v3="3"/>
</triangles></mesh></object></resources><build><item objectid="1"/></build></model>)";
    const std::string contentTypes = R"(<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>)";
    const std::string relationships = R"(<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>)";
    return BuildStoredPackage({ { "[Content_Types].xml", contentTypes },
                                { "_rels/.rels", relationships },
                                { "3D/3dmodel.model", model } });
}

std::vector<std::byte> DeepComponentPackage(uint32_t depth)
{
    REQUIRE(depth >= 2);
    std::string model = R"(<?xml version="1.0" encoding="UTF-8"?>
<model unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02"><resources>
<object id="1" type="model"><mesh><vertices><vertex x="0" y="0" z="0"/><vertex x="1" y="0" z="0"/><vertex x="0" y="1" z="0"/></vertices><triangles><triangle v1="0" v2="1" v3="2"/></triangles></mesh></object>
)";
    for (uint32_t id = 2; id <= depth; ++id) {
        model += "<object id=\"" + std::to_string(id)
            + "\" type=\"model\"><components><component objectid=\""
            + std::to_string(id - 1) + "\"/></components></object>\n";
    }
    model += "</resources><build><item objectid=\"" + std::to_string(depth)
        + "\"/></build></model>";
    const std::string contentTypes = R"(<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>)";
    const std::string relationships = R"(<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>)";
    return BuildStoredPackage({ { "[Content_Types].xml", contentTypes },
                                { "_rels/.rels", relationships },
                                { "3D/3dmodel.model", model } });
}

struct Fixture {
    const char* name;
    const char* sha256;
    uint32_t minimumMeshes;
    uint32_t minimumBuildItems;
};

constexpr Fixture kFixtures[] = {
    { "core-box.3mf.base64", "E2633A8C2E014D5F8C612F13F64F48D4E722972C0FB0923795FA83DE5AEE24F6", 1, 1 },
    { "nested-components.3mf.base64", "B5FA98899B990C6BCFB615992D8A21739F6B722A05B4070EC08D591BD277B965", 1, 1 },
    { "production-boxes.3mf.base64", "CC479831E02D01F4696719773B2F4BAEB61BD9F2D3AC3B37CDD9F9FD2078DD97", 2, 2 },
    { "materials-texture.3mf.base64", "52F787357062DA8BFBB14C1B5258A11513717B4BEB072B00AA0161AA76A34B36", 1, 1 },
    { "beam-lattice.3mf.base64", "6814EF817D4845B76717BB33F56E06168758F34A5F0B6AF94DC76F5CDCE0E045", 2, 1 },
    { "beam-representation.3mf.base64", "28248E56B8590EA7E2C33CC375CFA9CDDA89EB98B31AE22EE5072E19D058C8BF", 2, 1 },
};

std::optional<import_worker::ThreeMfSpikePayloadHeader> RunSpike(
    import_broker::WorkerPool& pool, size_t worker, HANDLE file, uint64_t sourceLength,
    import_worker::ThreeMfSpikeMode mode, uint64_t generation, bool cancelBeforeStart = false,
    std::optional<Lib3MF::eProgressIdentifier> cancelAtPhase = std::nullopt)
{
    const SIZE_T sectionSize = sizeof(import_worker::ThreeMfSpikePayloadHeader);
    auto section = import_broker::CreateSharedSection(sectionSize);
    if (!section) return std::nullopt;
    auto view = platform::MappedView::Map(section.get(), FILE_MAP_READ | FILE_MAP_WRITE, sectionSize);
    if (!view) return std::nullopt;
    auto* header = reinterpret_cast<import_worker::ThreeMfSpikePayloadHeader*>(view.bytes().data());
    std::memset(header, 0, sizeof(*header));
    header->magic = import_worker::kThreeMfSpikePayloadMagic;
    header->sourceByteLength = sourceLength;
    header->mode = static_cast<uint32_t>(mode);
    if (cancelAtPhase)
        header->reserved = static_cast<uint32_t>(*cancelAtPhase) + 1;

    platform::Win32Handle cancellationEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!cancellationEvent) return std::nullopt;
    if (cancelBeforeStart && !SetEvent(cancellationEvent.get())) return std::nullopt;
    const auto workerFile = pool.DuplicateSectionIntoWorker(worker, file);
    const auto workerEvent = pool.DuplicateSectionIntoWorker(worker, cancellationEvent.get());
    const auto workerSection = pool.DuplicateSectionIntoWorker(worker, section.get());
    if (!workerFile || !workerEvent || !workerSection) return std::nullopt;
    header->sourceFileHandleValue = *workerFile;
    header->cancellationEventHandleValue = *workerEvent;

    model_core::StartGenerationRequest request{};
    request.generationId = generation;
    request.sceneVariant = import_worker::kThreeMfSpikeSceneVariant;
    request.sectionHandleValue = *workerSection;
    request.sectionByteCapacity = sectionSize;
    request.maxChunkCount = 1;
    if (!pool.SendRequest(worker, model_core::ControlOpcode::StartGeneration,
                          &request, sizeof(request))) return std::nullopt;
    model_core::ReceivedControlMessage reply{};
    if (pool.WaitForReply(worker, 30'000, reply) != import_broker::WaitReplyOutcome::Ready
        || reply.header.opcode != uint32_t(model_core::ControlOpcode::ChunksReady)) return std::nullopt;
    import_worker::ThreeMfSpikePayloadHeader result{};
    std::memcpy(&result, header, sizeof(result));
    return result;
}

} // namespace

TEST_CASE("3MF-001 pinned lib3mf loads representative packages through an inherited handle",
          "[3mf-spike][callback-io][sandbox][determinism]")
{
    sandbox_test_support::SandboxFixture sandbox;
    import_broker::WorkerPool pool;
    std::wstring error;
    REQUIRE(pool.InitializeForTesting(sandbox_test_support::WorkerExePath(),
                                      std::move(sandbox.sid), {}, 1,
                                      L"--3mf-spike-pool", error));
    const auto worker = pool.AcquireIdle();
    REQUIRE(worker);
    uint64_t generation = 0x336d66010000ull;
    for (const auto& fixture : kFixtures) {
        const auto bytes = DecodeBase64(fixture.name);
        const auto digest = platform::ComputeSha256(bytes);
        REQUIRE(digest);
        CHECK(Hex(*digest) == fixture.sha256);
        uint64_t expectedHash = 0;
        for (unsigned run = 0; run < 3; ++run) {
            TemporaryFile file(bytes);
            const auto result = RunSpike(pool, *worker, file.get(), bytes.size(),
                import_worker::ThreeMfSpikeMode::ReadAndInspect, ++generation);
            CAPTURE(fixture.name, run);
            REQUIRE(result);
            CAPTURE(result->libraryError, result->warningCount,
                    result->progressIdentifierMask, result->shortReadCount,
                    result->sourceChanged);
            REQUIRE(result->status == static_cast<uint32_t>(
                import_worker::ThreeMfSpikeStatus::Success));
            CHECK(result->versionMajor == 2);
            CHECK(result->versionMinor == 5);
            CHECK(result->versionMicro == 0);
            CHECK(result->meshCount >= fixture.minimumMeshes);
            CHECK(result->buildItemCount >= fixture.minimumBuildItems);
            CHECK(result->readCallbackCount > 0);
            CHECK(result->seekCallbackCount > 0);
            CHECK(result->sourceBytesRead > 0);
            CHECK(result->sourceBytesRead < bytes.size() * 8);
            CHECK(result->normalizedBytes > 0);
            if (run == 0) expectedHash = result->normalizedHash;
            else CHECK(result->normalizedHash == expectedHash);
            if (run == 0) {
                std::cout << "3MF-001 fixture=" << fixture.name
                          << " load-us=" << result->loadMicroseconds
                          << " extract-us=" << result->extractMicroseconds
                          << " private-before=" << result->privateBytesBefore
                          << " private-load=" << result->privateBytesAfterLoad
                          << " private-extract=" << result->privateBytesAfterExtract
                          << " peak-working-set=" << result->peakWorkingSetBytes
                          << " callbacks=" << result->readCallbackCount << '/'
                          << result->seekCallbackCount << " hash=0x" << std::hex
                          << result->normalizedHash << std::dec << '\n';
            }
        }
    }
    pool.Release(*worker);
}

TEST_CASE("3MF-001 production and lattice surfaces expose the decisions needed by adapters",
          "[3mf-spike][production][beam-lattice]")
{
    sandbox_test_support::SandboxFixture sandbox;
    import_broker::WorkerPool pool;
    std::wstring error;
    REQUIRE(pool.InitializeForTesting(sandbox_test_support::WorkerExePath(),
                                      std::move(sandbox.sid), {}, 1,
                                      L"--3mf-spike-pool", error));
    const auto worker = pool.AcquireIdle();
    REQUIRE(worker);
    const auto productionBytes = DecodeBase64("production-boxes.3mf.base64");
    TemporaryFile productionFile(productionBytes);
    const auto production = RunSpike(pool, *worker, productionFile.get(), productionBytes.size(),
        import_worker::ThreeMfSpikeMode::ReadAndInspect, 0x336d66020001ull);
    REQUIRE(production);
    CAPTURE(production->libraryError, production->warningCount,
            production->progressIdentifierMask, production->shortReadCount,
            production->sourceChanged);
    REQUIRE(production->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::Success));
    CHECK(production->buildItemCount == 2);
    CHECK(production->objectCount >= 2);
    CHECK((production->progressIdentifierMask
           & (1u << static_cast<uint32_t>(Lib3MF::eProgressIdentifier::READNONROOTMODELS))) != 0);

    const auto latticeBytes = DecodeBase64("beam-lattice.3mf.base64");
    TemporaryFile latticeFile(latticeBytes);
    const auto lattice = RunSpike(pool, *worker, latticeFile.get(), latticeBytes.size(),
        import_worker::ThreeMfSpikeMode::ReadAndInspect, 0x336d66020002ull);
    REQUIRE(lattice);
    REQUIRE(lattice->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::Success));
    CHECK(lattice->beamCount == 12);
    CHECK(lattice->tessellatedTriangleCount > lattice->beamCount * 2);
    CHECK(lattice->tessellatedTriangleCount <= 262'144);
    CHECK(lattice->previewHash != 0);
    CHECK(lattice->instancingEligibleBeamCount == 0);

    const auto representationBytes = DecodeBase64("beam-representation.3mf.base64");
    TemporaryFile representationFile(representationBytes);
    const auto representation = RunSpike(pool, *worker, representationFile.get(),
        representationBytes.size(), import_worker::ThreeMfSpikeMode::ReadAndInspect,
        0x336d66020003ull);
    REQUIRE(representation);
    REQUIRE(representation->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::Success));
    CHECK(representation->beamCount > 0);
    CHECK(representation->representationCount == 1);

    const auto deepBytes = DeepComponentPackage(128);
    TemporaryFile deepFile(deepBytes);
    const auto deep = RunSpike(pool, *worker, deepFile.get(), deepBytes.size(),
        import_worker::ThreeMfSpikeMode::ReadAndInspect, 0x336d66020004ull);
    REQUIRE(deep);
    REQUIRE(deep->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::Success));
    CHECK(deep->objectCount == 128);
    CHECK(deep->componentObjectCount == 127);
    CHECK(deep->meshCount == 1);
    std::cout << "3MF-001 lattice beams=" << lattice->beamCount
              << " fallback-triangles=" << lattice->tessellatedTriangleCount
              << " instance-eligible=" << lattice->instancingEligibleBeamCount
              << " authored-representations=" << representation->representationCount
              << " deep-components-load-us=" << deep->loadMicroseconds
              << " deep-components-private=" << deep->privateBytesAfterExtract << '\n';
    pool.Release(*worker);
}

TEST_CASE("3MF-001 callback reader uses sparse seeks near the Tier-B source limit",
          "[3mf-spike][callback-io][sparse][limits]")
{
    sandbox_test_support::SandboxFixture sandbox;
    import_broker::WorkerPool pool;
    std::wstring error;
    REQUIRE(pool.InitializeForTesting(sandbox_test_support::WorkerExePath(),
                                      std::move(sandbox.sid), {}, 1,
                                      L"--3mf-spike-pool", error));
    const auto worker = pool.AcquireIdle();
    REQUIRE(worker);
    const auto source = DecodeBase64("core-box.3mf.base64");
    const uint64_t targetLength = 2ull * 1024 * 1024 * 1024 - 64 * 1024;
    REQUIRE(targetLength > source.size());
    const uint32_t prefix = static_cast<uint32_t>(targetLength - source.size());
    const auto relocated = RelocateZip(source, prefix);
    TemporaryFile sparseFile(relocated, prefix);
    const auto result = RunSpike(pool, *worker, sparseFile.get(), targetLength,
        import_worker::ThreeMfSpikeMode::ReadAndInspect, 0x336d66025001ull);
    REQUIRE(result);
    REQUIRE(result->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::Success));
    CHECK(result->sparseSeekCount > 0);
    CHECK(result->sourceBytesRead < 1024 * 1024);
    CHECK(result->sourceBytesRead < targetLength / 1024);
    pool.Release(*worker);
}

TEST_CASE("3MF-001 progress cancellation and callback I/O failures are typed and recoverable",
          "[3mf-spike][cancellation][io][recovery]")
{
    sandbox_test_support::SandboxFixture sandbox;
    import_broker::WorkerPool pool;
    std::wstring error;
    REQUIRE(pool.InitializeForTesting(sandbox_test_support::WorkerExePath(),
                                      std::move(sandbox.sid), {}, 1,
                                      L"--3mf-spike-pool", error));
    const auto worker = pool.AcquireIdle();
    REQUIRE(worker);
    const DWORD processId = pool.ProcessId(*worker);
    const auto bytes = DecodeBase64("materials-texture.3mf.base64");

    TemporaryFile cancelledFile(bytes);
    const auto cancelled = RunSpike(pool, *worker, cancelledFile.get(), bytes.size(),
        import_worker::ThreeMfSpikeMode::CancelFromProgress, 0x336d66030001ull, true);
    REQUIRE(cancelled);
    CHECK(cancelled->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::Cancelled));
    CHECK(cancelled->libraryError == LIB3MF_ERROR_CALCULATIONABORTED);
    CHECK(cancelled->progressCallbackCount > 0);

    struct PhaseCase {
        const char* fixture;
        Lib3MF::eProgressIdentifier phase;
    };
    constexpr PhaseCase phaseCases[] = {
        { "core-box.3mf.base64", Lib3MF::eProgressIdentifier::EXTRACTOPCPACKAGE },
        { "core-box.3mf.base64", Lib3MF::eProgressIdentifier::READROOTMODEL },
        { "core-box.3mf.base64", Lib3MF::eProgressIdentifier::READRESOURCES },
        { "production-boxes.3mf.base64", Lib3MF::eProgressIdentifier::READNONROOTMODELS },
        { "materials-texture.3mf.base64", Lib3MF::eProgressIdentifier::READTEXTURETACHMENTS },
    };
    uint64_t phaseGeneration = 0x336d66031000ull;
    for (const auto& phaseCase : phaseCases) {
        const auto phaseBytes = DecodeBase64(phaseCase.fixture);
        TemporaryFile phaseFile(phaseBytes);
        const auto phaseResult = RunSpike(pool, *worker, phaseFile.get(), phaseBytes.size(),
            import_worker::ThreeMfSpikeMode::CancelFromProgress, ++phaseGeneration, false,
            phaseCase.phase);
        CAPTURE(phaseCase.fixture, static_cast<uint32_t>(phaseCase.phase));
        REQUIRE(phaseResult);
        CHECK(phaseResult->status == static_cast<uint32_t>(
            import_worker::ThreeMfSpikeStatus::Cancelled));
        CHECK(phaseResult->libraryError == LIB3MF_ERROR_CALCULATIONABORTED);
    }

    const auto extractionBytes = VertexPressurePackage(250'000);
    TemporaryFile extractionFile(extractionBytes);
    const auto extractionCancelled = RunSpike(pool, *worker, extractionFile.get(),
        extractionBytes.size(), import_worker::ThreeMfSpikeMode::CancelDuringExtraction,
        0x336d66032000ull);
    REQUIRE(extractionCancelled);
    CHECK(extractionCancelled->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::Cancelled));
    CHECK(extractionCancelled->extractMicroseconds > 0);

    const auto latticeBytes = DecodeBase64("beam-lattice.3mf.base64");
    TemporaryFile latticeCancellationFile(latticeBytes);
    const auto latticeCancelled = RunSpike(pool, *worker, latticeCancellationFile.get(),
        latticeBytes.size(), import_worker::ThreeMfSpikeMode::CancelDuringLattice,
        0x336d66032001ull);
    REQUIRE(latticeCancelled);
    CHECK(latticeCancelled->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::Cancelled));
    CHECK(latticeCancelled->extractMicroseconds > 0);
    std::cout << "3MF-001 extraction-cancel-us="
              << extractionCancelled->extractMicroseconds
              << " lattice-cancel-us=" << latticeCancelled->extractMicroseconds << '\n';

    TemporaryFile shortFile(bytes);
    const auto shortRead = RunSpike(pool, *worker, shortFile.get(), bytes.size(),
        import_worker::ThreeMfSpikeMode::ForceShortRead, 0x336d66030002ull);
    REQUIRE(shortRead);
    CHECK(shortRead->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::IoFailure));
    CHECK(shortRead->shortReadCount > 0);

    TemporaryFile changedFile(bytes);
    const auto sourceChanged = RunSpike(pool, *worker, changedFile.get(), bytes.size(),
        import_worker::ThreeMfSpikeMode::SimulateSourceChange, 0x336d660300025ull);
    REQUIRE(sourceChanged);
    CHECK(sourceChanged->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::IoFailure));
    CHECK(sourceChanged->sourceChanged == 1);
    CHECK(sourceChanged->shortReadCount > 0);

    auto malformedBytes = bytes;
    malformedBytes.resize(malformedBytes.size() / 2);
    TemporaryFile malformedFile(malformedBytes);
    const auto malformed = RunSpike(pool, *worker, malformedFile.get(), malformedBytes.size(),
        import_worker::ThreeMfSpikeMode::ReadAndInspect, 0x336d66030003ull);
    REQUIRE(malformed);
    CHECK(malformed->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::Malformed));

    TemporaryFile validFile(bytes);
    const auto recovered = RunSpike(pool, *worker, validFile.get(), bytes.size(),
        import_worker::ThreeMfSpikeMode::ReadAndInspect, 0x336d66030004ull);
    REQUIRE(recovered);
    CHECK(recovered->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::Success));
    CHECK(pool.ProcessId(*worker) == processId);
    pool.Release(*worker);
}

TEST_CASE("3MF-001 Job commit limit contains model construction pressure and recovers",
          "[3mf-spike][memory][job][recovery]")
{
    sandbox_test_support::SandboxFixture sandbox;
    import_broker::WorkerPool pool;
    import_broker::SandboxLimits limits{};
    limits.processMemoryLimitBytes = 20u * 1024u * 1024u;
    std::wstring error;
    REQUIRE(pool.InitializeForTesting(sandbox_test_support::WorkerExePath(),
                                      std::move(sandbox.sid), limits, 1,
                                      L"--3mf-spike-pool", error));
    const auto worker = pool.AcquireIdle();
    REQUIRE(worker);
    const DWORD originalProcessId = pool.ProcessId(*worker);
    const auto pressureBytes = VertexPressurePackage(1'500'000);
    REQUIRE(pressureBytes.size() > 32u * 1024u * 1024u);
    TemporaryFile pressureFile(pressureBytes);
    const auto exhausted = RunSpike(pool, *worker, pressureFile.get(), pressureBytes.size(),
        import_worker::ThreeMfSpikeMode::ReadAndInspect, 0x336d66035001ull);
    if (exhausted) {
        CAPTURE(exhausted->status, exhausted->libraryError,
                exhausted->privateBytesBefore, exhausted->privateBytesAfterLoad,
                exhausted->peakWorkingSetBytes);
        std::cout << "3MF-001 capped-pressure status=" << exhausted->status
                  << " lib-error=" << exhausted->libraryError
                  << " private-before=" << exhausted->privateBytesBefore
                  << " private-load=" << exhausted->privateBytesAfterLoad
                  << " peak-working-set=" << exhausted->peakWorkingSetBytes << '\n';
        CHECK(exhausted->status != static_cast<uint32_t>(
            import_worker::ThreeMfSpikeStatus::Success));
        CHECK(pool.ProcessId(*worker) == originalProcessId);
    }
    // A library-internal allocation failure is surfaced only as generic error
    // 5 and can retain enough heap state to crowd the deliberately tiny Job.
    // The product boundary therefore retires this worker after pressure.
    REQUIRE(pool.TerminateAndReplace(*worker, error));
    CHECK(pool.ProcessId(*worker) != originalProcessId);

    const auto validBytes = DecodeBase64("core-box.3mf.base64");
    TemporaryFile validFile(validBytes);
    const auto recovered = RunSpike(pool, *worker, validFile.get(), validBytes.size(),
        import_worker::ThreeMfSpikeMode::ReadAndInspect, 0x336d66035002ull);
    REQUIRE(recovered);
    CHECK(recovered->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::Success));
    pool.Release(*worker);
}

TEST_CASE("3MF-001 noncooperative post-load work is replaced after the cancellation grace",
          "[3mf-spike][cancellation][worker-pool]")
{
    sandbox_test_support::SandboxFixture sandbox;
    import_broker::WorkerPool pool;
    std::wstring error;
    REQUIRE(pool.InitializeForTesting(sandbox_test_support::WorkerExePath(),
                                      std::move(sandbox.sid), {}, 1,
                                      L"--3mf-spike-pool", error));
    const auto worker = pool.AcquireIdle();
    REQUIRE(worker);
    const DWORD originalProcessId = pool.ProcessId(*worker);
    const auto bytes = DecodeBase64("core-box.3mf.base64");
    TemporaryFile file(bytes);

    const SIZE_T sectionSize = sizeof(import_worker::ThreeMfSpikePayloadHeader);
    auto section = import_broker::CreateSharedSection(sectionSize);
    REQUIRE(section);
    auto view = platform::MappedView::Map(section.get(), FILE_MAP_READ | FILE_MAP_WRITE, sectionSize);
    REQUIRE(view);
    auto* header = reinterpret_cast<import_worker::ThreeMfSpikePayloadHeader*>(view.bytes().data());
    std::memset(header, 0, sizeof(*header));
    header->magic = import_worker::kThreeMfSpikePayloadMagic;
    header->sourceByteLength = bytes.size();
    header->mode = static_cast<uint32_t>(import_worker::ThreeMfSpikeMode::HoldAfterLoad);
    platform::Win32Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    REQUIRE(event);
    const auto workerFile = pool.DuplicateSectionIntoWorker(*worker, file.get());
    const auto workerEvent = pool.DuplicateSectionIntoWorker(*worker, event.get());
    const auto workerSection = pool.DuplicateSectionIntoWorker(*worker, section.get());
    REQUIRE(workerFile); REQUIRE(workerEvent); REQUIRE(workerSection);
    header->sourceFileHandleValue = *workerFile;
    header->cancellationEventHandleValue = *workerEvent;
    model_core::StartGenerationRequest request{};
    request.generationId = 0x336d66040001ull;
    request.sceneVariant = import_worker::kThreeMfSpikeSceneVariant;
    request.sectionHandleValue = *workerSection;
    request.sectionByteCapacity = sectionSize;
    request.maxChunkCount = 1;
    REQUIRE(pool.SendRequest(*worker, model_core::ControlOpcode::StartGeneration,
                             &request, sizeof(request)));
    const auto enteredDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (InterlockedCompareExchange(&header->state, 0, 0) != 2
           && std::chrono::steady_clock::now() < enteredDeadline) Sleep(1);
    REQUIRE(InterlockedCompareExchange(&header->state, 0, 0) == 2);
    const auto cancelStart = std::chrono::steady_clock::now();
    REQUIRE(SetEvent(event.get()));
    model_core::ReceivedControlMessage reply{};
    CHECK(pool.WaitForReply(*worker, 500, reply) == import_broker::WaitReplyOutcome::TimedOut);
    REQUIRE(pool.TerminateAndReplace(*worker, error));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - cancelStart);
    CHECK(elapsed >= std::chrono::milliseconds(450));
    CHECK(elapsed < std::chrono::milliseconds(2500));
    CHECK(pool.ProcessId(*worker) != originalProcessId);
    std::cout << "3MF-001 noncooperative-replacement-ms=" << elapsed.count() << '\n';
    pool.Release(*worker);

    const auto replacement = pool.AcquireIdle();
    REQUIRE(replacement);
    TemporaryFile validFile(bytes);
    const auto recovered = RunSpike(pool, *replacement, validFile.get(), bytes.size(),
        import_worker::ThreeMfSpikeMode::ReadAndInspect, 0x336d66040002ull);
    REQUIRE(recovered);
    CHECK(recovered->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::Success));
    pool.Release(*replacement);
}
