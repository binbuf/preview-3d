#include "import_broker/ImportSession.h"
#include "import_broker/SharedSection.h"
#include "import_broker/SidecarRequestServicer.h"
#include "import_broker/SourceFileAccess.h"
#include "model_core/TierALimits.h"
#include "SandboxTestSupport.h"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
#include <array>
#include <tlhelp32.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

namespace
{
using namespace model_core;
uint64_t PrivateBytes(HANDLE process)
{
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    return GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))
               ? memory.PrivateUsage
               : 0;
}
uint64_t PeakPrivateBytes(HANDLE process)
{
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    return GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))
               ? memory.PeakPagefileUsage
               : 0;
}
uint64_t MappedAddressBytes(HANDLE process)
{
    uint64_t mapped = 0;
    uintptr_t address = 0;
    MEMORY_BASIC_INFORMATION region{};
    while (VirtualQueryEx(process, reinterpret_cast<const void*>(address), &region, sizeof(region)))
    {
        if (region.State == MEM_COMMIT && region.Type == MEM_MAPPED)
            mapped += region.RegionSize;
        const uintptr_t next = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (next <= address)
            break;
        address = next;
    }
    return mapped;
}
uint64_t WorkerPrivateBytes(uint64_t& mappedBytes)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    uint64_t result = 0;
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
        if (entry.th32ParentProcessID == GetCurrentProcessId() &&
            std::wstring(entry.szExeFile) == L"Preview3DImportWorker.exe")
        {
            HANDLE process =
                OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, entry.th32ProcessID);
            if (process)
            {
                result += (std::max)(PrivateBytes(process), PeakPrivateBytes(process));
                mappedBytes = (std::max)(mappedBytes, MappedAddressBytes(process));
                CloseHandle(process);
            }
        }
    CloseHandle(snapshot);
    return result;
}
struct ScratchFile
{
    std::filesystem::path path;
    explicit ScratchFile(const wchar_t* suffix)
        : path(std::filesystem::temp_directory_path() /
               (L"Preview3D-bounded-" + std::to_wstring(GetCurrentProcessId()) + suffix))
    {
    }
    ~ScratchFile()
    {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};
template <class T> void Write(std::ofstream& output, const T& value)
{
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}
import_broker::ImportSessionRequest Request(const std::filesystem::path& path,
                                            import_broker::ImportFormat format)
{
    import_broker::ImportSessionRequest request;
    request.workerExePath = sandbox_test_support::WorkerExePath();
    request.sourcePath = path.wstring();
    request.format = format;
    request.generationId = 205;
    request.sectionByteCapacity = 16ull * 1024 * 1024;
    request.maxChunkCount = 1024;
    request.maxChunksPerGeneration = kTierACatalogLimit;
    request.maxChunkBatchesPerGeneration = kTierABatchLimit;
    request.maxSidecarRequestsPerGeneration = 64;
    request.maxSidecarFileBytes = kTierAPrimarySourceBytes;
    return request;
}
struct Measurements
{
    uint64_t triangles = 0, points = 0, chunks = 0, batches = 0, workerPeak = 0, hostPeak = 0, maxPayload = 0,
             workerMapped = 0, hostMapped = 0;
    double firstMs = 0, totalMs = 0;
    std::array<double, 3> min{}, max{};
};
Measurements Scan(import_broker::ImportSessionRequest request, uint64_t expectedTriangles,
                  uint64_t expectedPoints, bool cancel = false)
{
    Measurements result;
    bool cancelled = false;
    const auto start = std::chrono::steady_clock::now();
    const uint64_t hostBaseline = PrivateBytes(GetCurrentProcess());
    request.isCancelled = [&] { return cancelled; };
    request.onBatch = [&](std::vector<import_broker::ValidatedChunk>&& chunks) {
        if (!result.batches++)
            result.firstMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        result.workerPeak = (std::max)(result.workerPeak, WorkerPrivateBytes(result.workerMapped));
        result.hostPeak = (std::max)(result.hostPeak, (std::max)(PrivateBytes(GetCurrentProcess()),
                                                                 PeakPrivateBytes(GetCurrentProcess())) -
                                                          hostBaseline);
        result.hostMapped = (std::max)(result.hostMapped, MappedAddressBytes(GetCurrentProcess()));
        uint64_t batchBytes = 0;
        for (const auto& chunk : chunks)
        {
            const auto& d = chunk.descriptor;
            batchBytes += chunk.payload.size();
            if (d.topology != ChunkTopology::TriangleList && d.topology != ChunkTopology::PointList)
                continue;
            CHECK(d.boundsState == BoundsState::Verified);
            CHECK(d.sourceRangeLength > 0);
            CHECK(d.byteSize <= 16ull * 1024 * 1024);
            CHECK(d.indexCount / 3 <= 262144);
            CHECK(d.vertexCount <= 1048576);
            result.maxPayload = (std::max)(result.maxPayload, d.byteSize);
            result.triangles += d.indexCount / 3;
            if (d.topology == ChunkTopology::PointList)
                result.points += d.vertexCount;
            for (unsigned axis = 0; axis < 3; ++axis)
            {
                const double minimum = d.origin[axis] + d.localMin[axis],
                             maximum = d.origin[axis] + d.localMax[axis];
                if (!result.chunks)
                {
                    result.min[axis] = minimum;
                    result.max[axis] = maximum;
                }
                else
                {
                    result.min[axis] = (std::min)(result.min[axis], minimum);
                    result.max[axis] = (std::max)(result.max[axis], maximum);
                }
            }
            ++result.chunks;
        }
        CHECK(batchBytes <= request.sectionByteCapacity);
        if (cancel)
            cancelled = true;
    };
    auto imported = import_broker::RunImportSession(request);
    CAPTURE(imported.stage, imported.errorCode);
    result.totalMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (cancel)
    {
        CHECK_FALSE(imported.ok);
        CHECK(imported.stage == import_broker::ImportStage::Cancelled);
        CHECK(result.batches == 1);
    }
    else
    {
        REQUIRE(imported.ok);
        CHECK(imported.sourceIdentity.sizeBytes == std::filesystem::file_size(request.sourcePath));
        CHECK(imported.sourceIdentity.lastWriteTime > 0);
        CHECK(imported.chunks.empty());
        CHECK(imported.sourceCatalog.size() == result.chunks);
        CHECK(result.triangles == expectedTriangles);
        CHECK(result.points == expectedPoints);
        CHECK(result.chunks > 1);
        CHECK(result.batches > 1);
        CHECK(result.firstMs < result.totalMs);
        for (const auto& range : imported.sourceCatalog)
        {
            CHECK(range.generationId == request.generationId);
            CHECK(range.descriptor.sourceRangeLength > 0);
        }
    }
    CHECK(result.workerPeak < 512ull * 1024 * 1024);
    CHECK(result.hostPeak < 512ull * 1024 * 1024);
    std::cout << "bounded-scan " << std::filesystem::path(request.sourcePath).filename().string() << ": "
              << result.triangles << " triangles " << result.points << " points " << result.chunks
              << " chunks " << result.batches << " batches first=" << result.firstMs
              << "ms total=" << result.totalMs << "ms worker=" << result.workerPeak
              << " host=" << result.hostPeak << " bytes workerMapped=" << result.workerMapped
              << " hostMapped=" << result.hostMapped << " address bytes\n";
    return result;
}
void Stl(const std::filesystem::path& path, uint32_t triangles)
{
    std::ofstream output(path, std::ios::binary);
    std::array<char, 80> header{};
    Write(output, header);
    Write(output, triangles);
    const float facet[]{0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0};
    const uint16_t attributes = 0;
    for (uint32_t i = 0; i < triangles; ++i)
    {
        Write(output, facet);
        Write(output, attributes);
    }
    REQUIRE(output.good());
}
void Ply(const std::filesystem::path& path, uint32_t count, bool points, bool big = false, bool lists = false,
         bool corrupt = false)
{
    std::ofstream output(path, std::ios::binary);
    output << "ply\nformat binary_" << (big ? "big" : "little") << "_endian 1.0\nelement vertex "
           << (points ? count : 6) << "\nproperty float x\nproperty float y\nproperty float z\n";
    if (lists)
        output << "property list uchar uint unrelated\n";
    if (!points)
        output << "element face " << count
               << "\nproperty list uchar uint vertex_indices\nproperty list uchar uint unknown\n";
    output << "end_header\n";
    auto u32 = [&](uint32_t v) {
        if (big)
            v = _byteswap_ulong(v);
        Write(output, v);
    };
    for (uint32_t i = 0; i < (points ? count : 6); ++i)
    {
        float positions[]{float(i % 2), float(i % 3), float(i % 5)};
        for (float value : positions)
        {
            uint32_t bits;
            std::memcpy(&bits, &value, 4);
            u32(bits);
        }
        if (lists)
        {
            Write(output, uint8_t(1));
            u32(i);
        }
    }
    if (!points)
        for (uint32_t i = 0; i < count; ++i)
        {
            Write(output, uint8_t(3));
            u32(5);
            u32(0);
            u32(3);
            Write(output, uint8_t(2));
            u32(i);
            u32(i + 1);
        }
    if (corrupt)
    {
        output.close();
        std::filesystem::resize_file(path, std::filesystem::file_size(path) - 2);
    }
    else
        REQUIRE(output.good());
}
void Glb(const std::filesystem::path& path, uint32_t triangles, bool indexed)
{
    const uint32_t vertices = triangles * 3, positions = vertices * 12, indices = indexed ? vertices * 4 : 0;
    std::string json = "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":" +
                       std::to_string(positions + indices) +
                       "}],\"bufferViews\":[{\"buffer\":0,\"byteLength\":" + std::to_string(positions) + "}";
    if (indexed)
        json += ",{\"buffer\":0,\"byteOffset\":" + std::to_string(positions) +
                ",\"byteLength\":" + std::to_string(indices) + "}";
    json +=
        "],\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":" + std::to_string(vertices) +
        ",\"type\":\"VEC3\"}";
    if (indexed)
        json += ",{\"bufferView\":1,\"componentType\":5125,\"count\":" + std::to_string(vertices) +
                ",\"type\":\"SCALAR\"}";
    json += "],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}";
    if (indexed)
        json += ",\"indices\":1";
    json += "}]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}]}";
    while (json.size() % 4)
        json += ' ';
    std::ofstream output(path, std::ios::binary);
    Write(output, uint32_t(0x46546c67));
    Write(output, uint32_t(2));
    Write(output, uint32_t(28 + json.size() + positions + indices));
    Write(output, uint32_t(json.size()));
    Write(output, uint32_t(0x4e4f534a));
    output.write(json.data(), json.size());
    Write(output, positions + indices);
    Write(output, uint32_t(0x004e4942));
    const float triangle[]{0, 0, 0, 1, 0, 0, 0, 1, 0};
    for (uint32_t i = 0; i < triangles; ++i)
        Write(output, triangle);
    if (indexed)
        for (uint32_t i = 0; i < vertices; ++i)
            Write(output, vertices - 1 - i);
    REQUIRE(output.good());
}
} // namespace
TEST_CASE("Tier A normalization releases split clusters and preserves nonlocal geometry", "[bounded-scan]")
{
    ScratchFile stl(L".stl");
    Stl(stl.path, 400000);
    Scan(Request(stl.path, import_broker::ImportFormat::Stl), 400000, 0);
    for (bool indexed : {false, true})
    {
        ScratchFile glb(L".glb");
        Glb(glb.path, 400000, indexed);
        Scan(Request(glb.path, import_broker::ImportFormat::Gltf), 400000, 0);
    }
    for (bool big : {false, true})
    {
        ScratchFile ply(L".ply");
        Ply(ply.path, 150000, false, big, true);
        auto request = Request(ply.path, import_broker::ImportFormat::Ply);
        request.sectionByteCapacity = 1024 * 1024;
        Scan(request, 150000, 0);
        Ply(ply.path, 1500000, true, big);
        Scan(Request(ply.path, import_broker::ImportFormat::Ply), 0, 1500000);
    }
}
TEST_CASE("Mapped scans cancel after accepted batches and reject a malformed late PLY list", "[bounded-scan]")
{
    ScratchFile stl(L".stl");
    Stl(stl.path, 400000);
    Scan(Request(stl.path, import_broker::ImportFormat::Stl), 0, 0, true);
    ScratchFile glb(L".glb");
    Glb(glb.path, 400000, true);
    Scan(Request(glb.path, import_broker::ImportFormat::Gltf), 0, 0, true);
    ScratchFile ply(L".ply");
    Ply(ply.path, 1500000, true);
    Scan(Request(ply.path, import_broker::ImportFormat::Ply), 0, 0, true);
    Ply(ply.path, 150000, false, false, true, true);
    auto request = Request(ply.path, import_broker::ImportFormat::Ply);
    request.sectionByteCapacity = 1024 * 1024;
    uint32_t batches = 0;
    request.onBatch = [&](auto) { ++batches; };
    auto failed = import_broker::RunImportSession(request);
    CHECK_FALSE(failed.ok);
    CHECK(failed.errorCode == ImportErrorCode::MalformedData);
    CHECK(batches > 0);
}
TEST_CASE("Multi GiB Tier A inputs retain bounded private commit", "[.qualification][large-scan]")
{
    wchar_t directory[32768];
    REQUIRE(GetEnvironmentVariableW(L"PREVIEW3D_TSK205_FIXTURES", directory, 32768) > 0);
    const std::filesystem::path root(directory);
    for (const auto& format :
         {L"glb", L"stl", L"ply-mesh-le", L"ply-mesh-be", L"ply-points-le", L"ply-points-be"})
    {
        const bool points = std::wstring(format).find(L"points") != std::wstring::npos;
        const bool ply = std::wstring(format).find(L"ply") != std::wstring::npos;
        const auto path =
            root / (L"A-large-" + std::wstring(format) + (ply ? L".ply" : L"." + std::wstring(format)));
        REQUIRE(std::filesystem::file_size(path) >= 2ull * 1024 * 1024 * 1024);
        auto request = Request(path, ply                              ? import_broker::ImportFormat::Ply
                                     : std::wstring(format) == L"stl" ? import_broker::ImportFormat::Stl
                                                                      : import_broker::ImportFormat::Gltf);
        request.sectionByteCapacity = import_broker::kImportSectionBytes;
        auto result = Scan(request, points ? 0 : 60000000, points ? 60000000 : 0);
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            CHECK(result.min[axis] == 0);
            CHECK(result.max[axis] == 11);
        }
    }
    auto request = Request(root / L"A-adversarial-layout.gltf", import_broker::ImportFormat::Gltf);
    request.sectionByteCapacity = import_broker::kImportSectionBytes;
    auto sidecar = Scan(request, 35791392, 0);
    for (unsigned axis = 0; axis < 3; ++axis)
    {
        CHECK(sidecar.min[axis] == 0);
        CHECK(sidecar.max[axis] == 11);
    }
}

TEST_CASE("Named source limits and diagnostics are preserved before worker launch", "[bounded-scan]")
{
    ScratchFile source(L".stl");
    Stl(source.path, 1);
    std::filesystem::resize_file(source.path, kTierAPrimarySourceBytes + 1);
    auto result = import_broker::RunImportSession(Request(source.path, import_broker::ImportFormat::Stl));
    CHECK_FALSE(result.ok);
    CHECK(result.stage == import_broker::ImportStage::OpenSource);
    CHECK(result.errorCode == ImportErrorCode::PrimarySourceLimit);
    CHECK(result.batchCount == 0);
}

TEST_CASE("glTF metadata budgets precede parser asset reservations", "[bounded-scan]")
{
    ScratchFile source(L".gltf");
    for (const auto& field : {"nodes", "materials", "extras"})
    {
        CAPTURE(field);
        const bool nested = std::string_view(field) == "extras";
        const uint32_t count = nested ? 1000 : std::string_view(field) == "nodes" ? 100001 : 65537;
        {
            std::ofstream output(source.path);
            output << "{\"asset\":{\"version\":\"2.0\"},\"" << field << "\":[";
            for (uint32_t i = 0; i < count; ++i)
            {
                if (i)
                    output << ',';
                if (!nested)
                    output << "{}";
                else
                {
                    output << '[';
                    for (uint32_t j = 0; j < 1000; ++j)
                        output << (j ? ",0" : "0");
                    output << ']';
                }
            }
            output << "]}";
        }
        auto result =
            import_broker::RunImportSession(Request(source.path, import_broker::ImportFormat::Gltf));
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == (nested ? ImportErrorCode::ScratchLimit : ImportErrorCode::ResourceLimit));
        CHECK(result.batchCount == 0);
    }
}

TEST_CASE("Broker source catalogs reject hostile byte accessor and primitive ranges",
          "[bounded-scan][hostile-worker]")
{
    auto request = Request(std::filesystem::path(PREVIEW3D_TEST_ASSETS_DIR) / L"tri_tight.glb",
                           import_broker::ImportFormat::Gltf);
    request.workerExePath = sandbox_test_support::HostileWorkerExePath();
    for (auto mode : {L"--source-byte-offset", L"--source-byte-length", L"--source-primitive-id",
                      L"--source-accessor-range"})
    {
        CAPTURE(mode);
        request.workerArgumentsOverride = mode;
        auto rejected = import_broker::RunImportSession(request);
        CHECK_FALSE(rejected.ok);
        CHECK(rejected.stage == import_broker::ImportStage::ValidateSection);
        CHECK(rejected.errorCode == ImportErrorCode::MalformedData);
    }
    request.workerArgumentsOverride = L"--source-range-valid";
    auto accepted = import_broker::RunImportSession(request);
    REQUIRE(accepted.ok);
    CHECK(accepted.sourceCatalog.size() == 1);
}
TEST_CASE("Aggregate sidecar admission fails before duplicating an over-budget handle",
          "[bounded-scan][sidecar]")
{
    model_core::RequestSidecarFileNotice notice{};
    notice.generationId = 205;
    const std::string relative = "tri_external.bin";
    notice.relativePathLength = uint32_t(relative.size());
    std::memcpy(notice.relativePathUtf8, relative.data(), relative.size());
    auto primary = import_broker::OpenAndCanonicalizeSourceFile(
        (std::filesystem::path(PREVIEW3D_TEST_ASSETS_DIR) / L"tri_external.gltf").wstring());
    REQUIRE(primary.file);
    auto result = import_broker::ServiceSidecarRequest(GetCurrentProcess(), primary.canonicalPath, notice,
                                                       1024 * 1024, 1);
    REQUIRE(std::holds_alternative<SidecarFileUnavailableNotice>(result));
    CHECK(std::get<SidecarFileUnavailableNotice>(result).errorCode ==
          uint32_t(ImportErrorCode::AggregateSourceLimit));
}
