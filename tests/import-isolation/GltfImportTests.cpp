// Gate 3 slice 1: real fastgltf parsing behind the sandbox. Proves a real,
// previously-unvetted third-party parser can run inside
// Preview3DImportWorker.exe and produce output the existing, unmodified
// SharedSectionValidator accepts -- exactly like ImportPipelineTests.cpp
// already proved for the synthetic generator. Does not touch
// interactive-viewer/; scope is core untextured geometry only. See
// .docs/design/04-rendering-and-streaming.md (vertex-layout set) and
// .docs/PROGRESS.md for the fastgltf-API findings this adapter relies on.

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

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#ifndef PREVIEW3D_TEST_ASSETS_DIR
#error "PREVIEW3D_TEST_ASSETS_DIR must be defined by Tests.ImportIsolation.vcxproj"
#endif

namespace {

std::wstring TestAssetPath(const wchar_t* fileName)
{
    return std::wstring(PREVIEW3D_TEST_ASSETS_DIR) + fileName;
}

std::optional<std::vector<std::byte>> ReadFileBytes(const std::wstring& path)
{
    platform::Win32Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        return std::nullopt;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0) {
        return std::nullopt;
    }

    std::vector<std::byte> bytes(static_cast<size_t>(size.QuadPart));
    DWORD bytesRead = 0;
    if (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &bytesRead, nullptr)
        || bytesRead != bytes.size()) {
        return std::nullopt;
    }
    return bytes;
}

// Pushes `bytes` into a fresh pagefile-backed section, reusing the
// already-built CreateSharedSection/MappedView mechanism a second time for
// input rather than building a real file-backed MappedFile abstraction
// (that's Gate 2 workstream B's job) -- honestly scoped to proving a real
// parser runs safely in the sandbox, not the production large-file input
// path.
std::optional<platform::Win32Handle> PushBytesIntoNewSection(const std::vector<std::byte>& bytes)
{
    auto section = import_broker::CreateSharedSection(bytes.size());
    if (!section) {
        return std::nullopt;
    }
    auto view = platform::MappedView::Map(section.get(), FILE_MAP_WRITE, bytes.size());
    if (!view) {
        return std::nullopt;
    }
    std::memcpy(view.bytes().data(), bytes.data(), bytes.size());
    return section;
}

struct GltfImportLaunch {
    import_broker::SandboxProcess proc;
    platform::Win32Handle controlInWrite;
    platform::Win32Handle controlOutRead;
};

// Launches the real worker in --parse-gltf mode. Duplicates
// GenerationLaunchSupport.h's pipe-setup shape rather than modifying it --
// that helper only takes one section handle, and this mode needs two
// (input GLB bytes + output chunk section), so extending it would risk the
// already-passing ImportPipelineTests.cpp/HostileWorkerTests.cpp suites for
// no benefit to them.
std::optional<GltfImportLaunch> LaunchGltfImportWorker(const platform::AppContainerSid& sid,
                                                        HANDLE sourceSectionHandle,
                                                        HANDLE outputSectionHandle)
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

    std::vector<HANDLE> inherited{ controlInRead.get(), controlOutWrite.get(), sourceSectionHandle,
                                    outputSectionHandle };
    std::wstring cmdLine
        = L"\"" + std::wstring(sandbox_test_support::WorkerExePath()) + L"\" --parse-gltf";

    import_broker::SandboxLimits limits{};
    auto proc = import_broker::LaunchSuspendedSandboxed(
        sandbox_test_support::WorkerExePath(), cmdLine, inherited, controlOutWrite.get(), limits,
        sid, controlInRead.get());
    if (!proc) {
        return std::nullopt;
    }

    controlInRead.reset();
    controlOutWrite.reset();

    GltfImportLaunch launch;
    launch.proc = std::move(*proc);
    launch.controlInWrite = std::move(controlInWrite);
    launch.controlOutRead = std::move(controlOutRead);
    return launch;
}

struct GltfImportRun {
    bool ready = false;
    import_broker::ValidationResult validation;
    model_core::GenerationErrorNotice errorNotice{};
};

// End-to-end: launch, push sourceBytes into a fresh input section, send
// ParseGltfRequest, read the reply, and (if ChunksReady) validate the
// output section via the real, unmodified SharedSectionValidator.
GltfImportRun RunGltfImport(const platform::AppContainerSid& sid,
                             const std::vector<std::byte>& sourceBytes, uint64_t generationId,
                             uint32_t maxChunkCount)
{
    GltfImportRun run;

    auto sourceSection = PushBytesIntoNewSection(sourceBytes);
    REQUIRE(sourceSection.has_value());
    auto outputSection = import_broker::CreateSharedSection(import_broker::kSyntheticSectionBytes);
    REQUIRE(outputSection);

    auto launch = LaunchGltfImportWorker(sid, sourceSection->get(), outputSection.get());
    REQUIRE(launch.has_value());
    REQUIRE(import_broker::ResumeSandboxProcess(launch->proc));

    model_core::ParseGltfRequest request{};
    request.generationId = generationId;
    request.sourceHandleValue = reinterpret_cast<uint64_t>(sourceSection->get());
    request.sourceByteLength = sourceBytes.size();
    request.sectionHandleValue = reinterpret_cast<uint64_t>(outputSection.get());
    request.sectionByteCapacity = import_broker::kSyntheticSectionBytes;
    request.maxChunkCount = maxChunkCount;
    REQUIRE(model_core::WriteControlMessage(launch->controlInWrite.get(),
                                             model_core::ControlOpcode::StartGltfImport, &request,
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

    auto view = platform::MappedView::Map(outputSection.get(), FILE_MAP_READ,
                                           import_broker::kSyntheticSectionBytes);
    REQUIRE(view);
    run.validation = import_broker::ValidateAndCopySection(view.bytes(), generationId, maxChunkCount);
    return run;
}

// The real pipeline, no PushBytesIntoNewSection shortcut and no in-memory
// byte vector standing in for the source at all: the trusted process opens
// and canonicalizes a real on-disk file (OpenAndCanonicalizeSourceFile),
// the broker duplicates its raw FILE handle inheritable
// (DuplicateInheritableHandle), and the worker builds its own
// model_core::MappedFile from that handle
// (StartGltfImportFromFile/ParseGltfFileRequest). Reuses
// LaunchGltfImportWorker unchanged -- it already accepts an opaque source
// HANDLE -- only the control message and the handle's origin differ from
// RunGltfImport above.
GltfImportRun RunGltfImportFromRealFile(const platform::AppContainerSid& sid, const std::wstring& path,
                                         uint64_t generationId, uint32_t maxChunkCount)
{
    GltfImportRun run;

    auto opened = import_broker::OpenAndCanonicalizeSourceFile(path);
    REQUIRE(opened.file);

    auto duplicated = import_broker::DuplicateInheritableHandle(opened.file.get());
    REQUIRE(duplicated.has_value());

    auto outputSection = import_broker::CreateSharedSection(import_broker::kSyntheticSectionBytes);
    REQUIRE(outputSection);

    auto launch = LaunchGltfImportWorker(sid, duplicated->get(), outputSection.get());
    REQUIRE(launch.has_value());
    REQUIRE(import_broker::ResumeSandboxProcess(launch->proc));

    model_core::ParseGltfFileRequest request{};
    request.generationId = generationId;
    request.sourceFileHandleValue = reinterpret_cast<uint64_t>(duplicated->get());
    request.sectionHandleValue = reinterpret_cast<uint64_t>(outputSection.get());
    request.sectionByteCapacity = import_broker::kSyntheticSectionBytes;
    request.maxChunkCount = maxChunkCount;
    REQUIRE(model_core::WriteControlMessage(launch->controlInWrite.get(),
                                             model_core::ControlOpcode::StartGltfImportFromFile, &request,
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

    auto view = platform::MappedView::Map(outputSection.get(), FILE_MAP_READ,
                                           import_broker::kSyntheticSectionBytes);
    REQUIRE(view);
    run.validation = import_broker::ValidateAndCopySection(view.bytes(), generationId, maxChunkCount);
    return run;
}

} // namespace

TEST_CASE("tri_tight.glb round-trips through the real fastgltf adapter", "[gltf-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    auto bytes = ReadFileBytes(TestAssetPath(L"tri_tight.glb"));
    REQUIRE(bytes.has_value());

    auto run = RunGltfImport(fixture.sid, *bytes, /*generationId=*/10, /*maxChunkCount=*/8);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);
    REQUIRE(run.validation.chunks.size() == 1);

    const auto& chunk = run.validation.chunks[0];
    CHECK(chunk.descriptor.topology == model_core::ChunkTopology::TriangleList);
    CHECK(chunk.descriptor.vertexCount == 3);
    CHECK(chunk.descriptor.indexCount == 3);
    CHECK(chunk.descriptor.vertexLayoutId
          == static_cast<uint32_t>(model_core::VertexLayoutId::PositionNormalUv0_F32));

    REQUIRE(chunk.payload.size() >= 3 * sizeof(model_core::VertexPositionNormalUv0F32));
    model_core::VertexPositionNormalUv0F32 vertices[3]{};
    std::memcpy(vertices, chunk.payload.data(), sizeof(vertices));
    for (const auto& v : vertices) {
        CHECK(v.nx == Catch::Approx(0.0f).margin(1e-5));
        CHECK(v.ny == Catch::Approx(0.0f).margin(1e-5));
        CHECK(v.nz == Catch::Approx(1.0f).margin(1e-5));
        CHECK(v.u == 0.0f);
        CHECK(v.v == 0.0f);
    }
}

TEST_CASE("tri_interleaved.glb round-trips through the real fastgltf adapter (explicit NORMAL)",
          "[gltf-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    auto bytes = ReadFileBytes(TestAssetPath(L"tri_interleaved.glb"));
    REQUIRE(bytes.has_value());

    auto run = RunGltfImport(fixture.sid, *bytes, /*generationId=*/11, /*maxChunkCount=*/8);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);
    REQUIRE(run.validation.chunks.size() == 1);

    const auto& chunk = run.validation.chunks[0];
    CHECK(chunk.descriptor.vertexCount == 3);
    CHECK(chunk.descriptor.indexCount == 3);

    model_core::VertexPositionNormalUv0F32 vertices[3]{};
    std::memcpy(vertices, chunk.payload.data(), sizeof(vertices));
    for (const auto& v : vertices) {
        CHECK(v.nz == Catch::Approx(1.0f).margin(1e-5));
    }
}

TEST_CASE("tri_transformed_node.glb bakes the parent node's world transform into vertex positions",
          "[gltf-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    auto bytes = ReadFileBytes(TestAssetPath(L"tri_transformed_node.glb"));
    REQUIRE(bytes.has_value());

    auto run = RunGltfImport(fixture.sid, *bytes, /*generationId=*/12, /*maxChunkCount=*/8);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);
    REQUIRE(run.validation.chunks.size() == 1);

    const auto& chunk = run.validation.chunks[0];
    model_core::VertexPositionNormalUv0F32 vertices[3]{};
    std::memcpy(vertices, chunk.payload.data(), sizeof(vertices));

    // Source triangle (0,0,0),(1,0,0),(0,1,0), rotated 90deg about +Z then
    // translated by [2,0,0]: (x,y,0) -> (-y,x,0) -> (-y+2,x,0).
    CHECK(vertices[0].px == Catch::Approx(2.0f).margin(1e-4));
    CHECK(vertices[0].py == Catch::Approx(0.0f).margin(1e-4));
    CHECK(vertices[1].px == Catch::Approx(2.0f).margin(1e-4));
    CHECK(vertices[1].py == Catch::Approx(1.0f).margin(1e-4));
    CHECK(vertices[2].px == Catch::Approx(1.0f).margin(1e-4));
    CHECK(vertices[2].py == Catch::Approx(0.0f).margin(1e-4));

    for (const auto& v : vertices) {
        CHECK(v.pz == Catch::Approx(0.0f).margin(1e-4));
        // Z-rotation-invariant: the generated flat normal should still be
        // (0,0,1), not leak translation or use the wrong matrix.
        CHECK(v.nx == Catch::Approx(0.0f).margin(1e-4));
        CHECK(v.ny == Catch::Approx(0.0f).margin(1e-4));
        CHECK(v.nz == Catch::Approx(1.0f).margin(1e-4));
    }
}

TEST_CASE("Truncated GLB bytes are rejected as a clean GenerationError, not a crash",
          "[gltf-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    auto bytes = ReadFileBytes(TestAssetPath(L"tri_tight.glb"));
    REQUIRE(bytes.has_value());
    bytes->resize(bytes->size() / 2);

    auto run = RunGltfImport(fixture.sid, *bytes, /*generationId=*/13, /*maxChunkCount=*/8);
    CHECK_FALSE(run.ready);
    CHECK(run.errorNotice.errorCode == static_cast<uint32_t>(model_core::ImportErrorCode::MalformedData));
}

TEST_CASE("A file requiring an unrecognized extension is rejected as a clean GenerationError",
          "[gltf-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    auto bytes = ReadFileBytes(TestAssetPath(L"unsupported_extension.glb"));
    REQUIRE(bytes.has_value());

    auto run = RunGltfImport(fixture.sid, *bytes, /*generationId=*/14, /*maxChunkCount=*/8);
    CHECK_FALSE(run.ready);
    CHECK(run.errorNotice.errorCode == static_cast<uint32_t>(model_core::ImportErrorCode::MalformedData));
}

TEST_CASE("maxChunkCount of 0 against a 1-primitive fixture is rejected as ResourceLimit",
          "[gltf-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    auto bytes = ReadFileBytes(TestAssetPath(L"tri_tight.glb"));
    REQUIRE(bytes.has_value());

    auto run = RunGltfImport(fixture.sid, *bytes, /*generationId=*/15, /*maxChunkCount=*/0);
    CHECK_FALSE(run.ready);
    CHECK(run.errorNotice.errorCode == static_cast<uint32_t>(model_core::ImportErrorCode::ResourceLimit));
}

TEST_CASE("A real on-disk GLB file reaches the sandboxed worker via a duplicated handle and parses "
          "successfully, with no in-memory shortcut anywhere on the input path",
          "[gltf-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    auto run = RunGltfImportFromRealFile(fixture.sid, TestAssetPath(L"tri_tight.glb"), /*generationId=*/16,
                                          /*maxChunkCount=*/8);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);
    REQUIRE(run.validation.chunks.size() == 1);

    const auto& chunk = run.validation.chunks[0];
    CHECK(chunk.descriptor.topology == model_core::ChunkTopology::TriangleList);
    CHECK(chunk.descriptor.vertexCount == 3);
    CHECK(chunk.descriptor.indexCount == 3);
    CHECK(chunk.descriptor.vertexLayoutId
          == static_cast<uint32_t>(model_core::VertexLayoutId::PositionNormalUv0_F32));
}
