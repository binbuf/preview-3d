// Gate 3 slice 2: real binary-STL parsing behind the sandbox. A second
// complete format wired through the same AppContainer-sandboxed pipeline
// GltfImportTests.cpp already proved for glTF -- reuses the same wire
// format/SharedSectionValidator unmodified, and goes straight to the real-
// file (MappedFile/SourceFileAccess/duplicated-handle) input path from the
// start rather than reprising glTF's synthetic-shortcut-then-upgrade
// two-step. ASCII STL is explicitly out of scope (Tier B, a later slice).

#include "SandboxTestSupport.h"
#include "import_broker/SandboxLauncher.h"
#include "import_broker/SharedSection.h"
#include "import_broker/SharedSectionValidator.h"
#include "import_broker/SourceFileAccess.h"
#include "model_core/ControlChannelIo.h"
#include "model_core/ControlProtocol.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"
#include "platform/MappedView.h"
#include "platform/Win32Handle.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#ifndef PREVIEW3D_TEST_ASSETS_DIR
#error "PREVIEW3D_TEST_ASSETS_DIR must be defined by Tests.ImportIsolation.vcxproj"
#endif

namespace {

// One binary-STL facet: a supplied normal plus 3 vertices. 50 bytes on
// disk (12 + 36 + 2 unused attribute-count bytes) -- see StlAdapter.cpp.
struct StlFacet {
    float nx = 0.0f, ny = 0.0f, nz = 0.0f;
    float v0x = 0.0f, v0y = 0.0f, v0z = 0.0f;
    float v1x = 0.0f, v1y = 0.0f, v1z = 0.0f;
    float v2x = 0.0f, v2y = 0.0f, v2z = 0.0f;
};

// A zeroed 80-byte header + a real triangle count + real facet bytes for
// each supplied facet -- the trivial fixed-size binary-STL layout doesn't
// need a gen-test-glbs.cpp-style external generator tool.
std::vector<std::byte> BuildBinaryStl(const std::vector<StlFacet>& facets)
{
    std::vector<std::byte> bytes(80 + 4 + facets.size() * 50, std::byte{ 0 });

    uint32_t count = static_cast<uint32_t>(facets.size());
    std::memcpy(bytes.data() + 80, &count, sizeof(count));

    size_t offset = 84;
    for (const StlFacet& f : facets) {
        float values[12] = { f.nx, f.ny, f.nz, f.v0x, f.v0y, f.v0z, f.v1x, f.v1y,
                              f.v1z, f.v2x, f.v2y, f.v2z };
        std::memcpy(bytes.data() + offset, values, sizeof(values));
        offset += sizeof(values);
        uint16_t attributeByteCount = 0;
        std::memcpy(bytes.data() + offset, &attributeByteCount, sizeof(attributeByteCount));
        offset += sizeof(attributeByteCount);
    }
    return bytes;
}

// A scratch on-disk file holding pre-built bytes -- mirrors
// tests/unit/MappedFileTests.cpp's ScratchFile RAII pattern, duplicated
// here rather than shared across test projects (same precedent
// GltfImportTests.cpp's own local helpers already set).
struct ScratchStlFile {
    std::wstring path;

    explicit ScratchStlFile(const std::vector<std::byte>& bytes)
    {
        wchar_t tempDir[MAX_PATH]{};
        REQUIRE(GetTempPathW(MAX_PATH, tempDir) != 0);
        wchar_t tempPath[MAX_PATH]{};
        REQUIRE(GetTempFileNameW(tempDir, L"stl", 0, tempPath) != 0);
        path = tempPath;

        platform::Win32Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                                FILE_ATTRIBUTE_NORMAL, nullptr));
        REQUIRE(file);
        DWORD written = 0;
        REQUIRE(WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr));
        REQUIRE(written == bytes.size());
    }

    ~ScratchStlFile() { DeleteFileW(path.c_str()); }

    ScratchStlFile(const ScratchStlFile&) = delete;
    ScratchStlFile& operator=(const ScratchStlFile&) = delete;
};

struct StlImportLaunch {
    import_broker::SandboxProcess proc;
    platform::Win32Handle controlInWrite;
    platform::Win32Handle controlOutRead;
};

// Launches the real worker in --parse-stl mode. Duplicates
// GltfImportTests.cpp's LaunchGltfImportWorker shape rather than sharing
// it (file-local to that translation unit) -- same "small deliberate
// duplication over cross-file coupling" precedent used throughout this
// suite.
std::optional<StlImportLaunch> LaunchStlImportWorker(const platform::AppContainerSid& sid,
                                                       HANDLE sourceFileHandle, HANDLE outputSectionHandle)
{
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE inReadRaw = nullptr;
    HANDLE inWriteRaw = nullptr;
    if (!CreatePipe(&inReadRaw, &inWriteRaw, &sa, 0)) {
        return std::nullopt;
    }
    platform::Win32Handle controlInRead(inReadRaw);
    platform::Win32Handle controlInWrite(inWriteRaw);
    SetHandleInformation(controlInWrite.get(), HANDLE_FLAG_INHERIT, 0);

    HANDLE outReadRaw = nullptr;
    HANDLE outWriteRaw = nullptr;
    if (!CreatePipe(&outReadRaw, &outWriteRaw, &sa, 0)) {
        return std::nullopt;
    }
    platform::Win32Handle controlOutRead(outReadRaw);
    platform::Win32Handle controlOutWrite(outWriteRaw);
    SetHandleInformation(controlOutRead.get(), HANDLE_FLAG_INHERIT, 0);

    std::vector<HANDLE> inherited{ controlInRead.get(), controlOutWrite.get(), sourceFileHandle,
                                    outputSectionHandle };
    std::wstring cmdLine = L"\"" + std::wstring(sandbox_test_support::WorkerExePath()) + L"\" --parse-stl";

    import_broker::SandboxLimits limits{};
    auto proc = import_broker::LaunchSuspendedSandboxed(
        sandbox_test_support::WorkerExePath(), cmdLine, inherited, controlOutWrite.get(), limits, sid,
        controlInRead.get());
    if (!proc) {
        return std::nullopt;
    }

    controlInRead.reset();
    controlOutWrite.reset();

    StlImportLaunch launch;
    launch.proc = std::move(*proc);
    launch.controlInWrite = std::move(controlInWrite);
    launch.controlOutRead = std::move(controlOutRead);
    return launch;
}

struct StlImportRun {
    bool ready = false;
    import_broker::ValidationResult validation;
    model_core::GenerationErrorNotice errorNotice{};
};

// End-to-end, real file only: the trusted process opens+canonicalizes a
// real on-disk file, the broker duplicates its raw FILE handle inheritable,
// and the worker builds its own model_core::MappedFile from that handle --
// mirrors GltfImportTests.cpp's RunGltfImportFromRealFile exactly.
StlImportRun RunStlImportFromRealFile(const platform::AppContainerSid& sid, const std::wstring& path,
                                       uint64_t generationId, uint32_t maxChunkCount)
{
    StlImportRun run;

    auto opened = import_broker::OpenAndCanonicalizeSourceFile(path);
    REQUIRE(opened.file);

    auto duplicated = import_broker::DuplicateInheritableHandle(opened.file.get());
    REQUIRE(duplicated.has_value());

    auto outputSection = import_broker::CreateSharedSection(import_broker::kSyntheticSectionBytes);
    REQUIRE(outputSection);

    auto launch = LaunchStlImportWorker(sid, duplicated->get(), outputSection.get());
    REQUIRE(launch.has_value());
    REQUIRE(import_broker::ResumeSandboxProcess(launch->proc));

    model_core::ParseStlFileRequest request{};
    request.generationId = generationId;
    request.sourceFileHandleValue = reinterpret_cast<uint64_t>(duplicated->get());
    request.sectionHandleValue = reinterpret_cast<uint64_t>(outputSection.get());
    request.sectionByteCapacity = import_broker::kSyntheticSectionBytes;
    request.maxChunkCount = maxChunkCount;
    REQUIRE(model_core::WriteControlMessage(launch->controlInWrite.get(),
                                             model_core::ControlOpcode::StartStlImportFromFile, &request,
                                             sizeof(request)));

    auto received = model_core::ReadControlMessage(launch->controlOutRead.get());
    REQUIRE(received.has_value());

    WaitForSingleObject(launch->proc.process.get(), 5000);

    if (received->header.opcode == static_cast<uint32_t>(model_core::ControlOpcode::GenerationError)) {
        REQUIRE(received->payload.size() == sizeof(run.errorNotice));
        std::memcpy(&run.errorNotice, received->payload.data(), sizeof(run.errorNotice));
        return run;
    }

    REQUIRE(received->header.opcode == static_cast<uint32_t>(model_core::ControlOpcode::ChunksReady));
    run.ready = true;

    auto view = platform::MappedView::Map(outputSection.get(), FILE_MAP_READ, import_broker::kSyntheticSectionBytes);
    REQUIRE(view);
    run.validation = import_broker::ValidateAndCopySection(view.bytes(), generationId, maxChunkCount);
    return run;
}

// Deterministic facets: two disjoint triangles in the XY plane (correct
// flat normal (0,0,1) for both, via right-hand winding v0->v1->v2).
StlFacet MakeTriangle(float offsetX, float nx, float ny, float nz)
{
    StlFacet f;
    f.nx = nx;
    f.ny = ny;
    f.nz = nz;
    f.v0x = offsetX + 0.0f;
    f.v0y = 0.0f;
    f.v0z = 0.0f;
    f.v1x = offsetX + 1.0f;
    f.v1y = 0.0f;
    f.v1z = 0.0f;
    f.v2x = offsetX + 0.0f;
    f.v2y = 1.0f;
    f.v2z = 0.0f;
    return f;
}

} // namespace

TEST_CASE("A valid binary STL with several facets round-trips through the real sandboxed pipeline",
          "[stl-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    std::vector<StlFacet> facets{
        MakeTriangle(0.0f, 0.0f, 0.0f, 1.0f),
        MakeTriangle(2.0f, 0.0f, 0.0f, 1.0f),
        MakeTriangle(4.0f, 0.0f, 0.0f, 1.0f),
    };
    ScratchStlFile file(BuildBinaryStl(facets));

    auto run = RunStlImportFromRealFile(fixture.sid, file.path, /*generationId=*/1, /*maxChunkCount=*/4);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);
    REQUIRE(run.validation.chunks.size() == 1);

    const auto& chunk = run.validation.chunks[0];
    CHECK(chunk.descriptor.topology == model_core::ChunkTopology::TriangleList);
    CHECK(chunk.descriptor.vertexCount == 9);
    CHECK(chunk.descriptor.indexCount == 9);
    CHECK(chunk.descriptor.vertexLayoutId
          == static_cast<uint32_t>(model_core::VertexLayoutId::PositionNormalUv0_F32));

    REQUIRE(chunk.payload.size() >= 9 * sizeof(model_core::VertexPositionNormalUv0F32));
    model_core::VertexPositionNormalUv0F32 vertices[9]{};
    std::memcpy(vertices, chunk.payload.data(), sizeof(vertices));
    for (const auto& v : vertices) {
        CHECK(v.nx == Catch::Approx(0.0f).margin(1e-5));
        CHECK(v.ny == Catch::Approx(0.0f).margin(1e-5));
        CHECK(v.nz == Catch::Approx(1.0f).margin(1e-5));
    }
}

TEST_CASE("A plausible (unit-length) supplied normal is used even when it disagrees with the flat "
          "normal",
          "[stl-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    // Geometrically the flat normal is (0,0,1); the supplied normal here is
    // deliberately the opposite, unit-length direction (0,0,-1) -- if the
    // adapter used the flat-computed normal instead of the supplied one,
    // this would read back as +1, not -1.
    std::vector<StlFacet> facets{ MakeTriangle(0.0f, 0.0f, 0.0f, -1.0f) };
    ScratchStlFile file(BuildBinaryStl(facets));

    auto run = RunStlImportFromRealFile(fixture.sid, file.path, /*generationId=*/2, /*maxChunkCount=*/4);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);
    REQUIRE(run.validation.chunks[0].payload.size() >= 3 * sizeof(model_core::VertexPositionNormalUv0F32));

    model_core::VertexPositionNormalUv0F32 vertices[3]{};
    std::memcpy(vertices, run.validation.chunks[0].payload.data(), sizeof(vertices));
    for (const auto& v : vertices) {
        CHECK(v.nz == Catch::Approx(-1.0f).margin(1e-4));
    }
}

TEST_CASE("A garbage (zero) supplied normal falls back to the generated flat normal", "[stl-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    std::vector<StlFacet> facets{ MakeTriangle(0.0f, 0.0f, 0.0f, 0.0f) }; // zero supplied normal
    ScratchStlFile file(BuildBinaryStl(facets));

    auto run = RunStlImportFromRealFile(fixture.sid, file.path, /*generationId=*/3, /*maxChunkCount=*/4);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);

    model_core::VertexPositionNormalUv0F32 vertices[3]{};
    std::memcpy(vertices, run.validation.chunks[0].payload.data(), sizeof(vertices));
    for (const auto& v : vertices) {
        CHECK(v.nz == Catch::Approx(1.0f).margin(1e-4)); // the geometrically-correct flat normal
    }
}

TEST_CASE("A degenerate (zero-area) facet is dropped without corrupting the rest", "[stl-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    StlFacet degenerate{}; // all vertices default to (0,0,0) -- zero area
    std::vector<StlFacet> facets{ degenerate, MakeTriangle(0.0f, 0.0f, 0.0f, 1.0f) };
    ScratchStlFile file(BuildBinaryStl(facets));

    auto run = RunStlImportFromRealFile(fixture.sid, file.path, /*generationId=*/4, /*maxChunkCount=*/4);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);
    // Only the valid triangle survives.
    CHECK(run.validation.chunks[0].descriptor.vertexCount == 3);
    CHECK(run.validation.chunks[0].descriptor.indexCount == 3);
}

TEST_CASE("A non-finite facet is dropped without corrupting the rest", "[stl-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    StlFacet nonFinite = MakeTriangle(10.0f, 0.0f, 0.0f, 1.0f);
    nonFinite.v0x = std::numeric_limits<float>::quiet_NaN();
    std::vector<StlFacet> facets{ nonFinite, MakeTriangle(0.0f, 0.0f, 0.0f, 1.0f) };
    ScratchStlFile file(BuildBinaryStl(facets));

    auto run = RunStlImportFromRealFile(fixture.sid, file.path, /*generationId=*/5, /*maxChunkCount=*/4);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);
    CHECK(run.validation.chunks[0].descriptor.vertexCount == 3);
}

TEST_CASE("A file with only degenerate/non-finite facets is rejected as MalformedData (empty geometry)",
          "[stl-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    StlFacet degenerate{};
    std::vector<StlFacet> facets{ degenerate };
    ScratchStlFile file(BuildBinaryStl(facets));

    auto run = RunStlImportFromRealFile(fixture.sid, file.path, /*generationId=*/6, /*maxChunkCount=*/4);
    CHECK_FALSE(run.ready);
    CHECK(run.errorNotice.errorCode == static_cast<uint32_t>(model_core::ImportErrorCode::MalformedData));
}

TEST_CASE("A file truncated relative to its declared triangle count is rejected as MalformedData",
          "[stl-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    std::vector<StlFacet> facets{ MakeTriangle(0.0f, 0.0f, 0.0f, 1.0f), MakeTriangle(2.0f, 0.0f, 0.0f, 1.0f) };
    std::vector<std::byte> bytes = BuildBinaryStl(facets); // declares 2 facets
    bytes.resize(bytes.size() - 25); // truncate mid-way through the second facet
    ScratchStlFile file(bytes);

    auto run = RunStlImportFromRealFile(fixture.sid, file.path, /*generationId=*/7, /*maxChunkCount=*/4);
    CHECK_FALSE(run.ready);
    CHECK(run.errorNotice.errorCode == static_cast<uint32_t>(model_core::ImportErrorCode::MalformedData));
}

TEST_CASE("A declared triangle count exceeding the sanity cap is rejected as ResourceLimit",
          "[stl-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    std::vector<std::byte> bytes(84, std::byte{ 0 });
    uint32_t hugeCount = 2'000'001; // one past StlAdapter.cpp's kMaxFacets
    std::memcpy(bytes.data() + 80, &hugeCount, sizeof(hugeCount));
    ScratchStlFile file(bytes);

    auto run = RunStlImportFromRealFile(fixture.sid, file.path, /*generationId=*/8, /*maxChunkCount=*/4);
    CHECK_FALSE(run.ready);
    CHECK(run.errorNotice.errorCode == static_cast<uint32_t>(model_core::ImportErrorCode::ResourceLimit));
}

TEST_CASE("A header shorter than 84 bytes is rejected as MalformedData", "[stl-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    std::vector<std::byte> bytes(50, std::byte{ 0 });
    ScratchStlFile file(bytes);

    auto run = RunStlImportFromRealFile(fixture.sid, file.path, /*generationId=*/9, /*maxChunkCount=*/4);
    CHECK_FALSE(run.ready);
    CHECK(run.errorNotice.errorCode == static_cast<uint32_t>(model_core::ImportErrorCode::MalformedData));
}
