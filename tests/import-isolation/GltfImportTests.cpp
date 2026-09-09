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
#include "import_broker/SidecarRequestServicer.h"
#include "import_broker/SourceFileAccess.h"
#include "model_core/ControlChannelIo.h"
#include "model_core/ControlProtocol.h"
#include "model_core/MaterialPayload.h"
#include "model_core/PixelFormats.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"
#include "platform/MappedView.h"
#include "platform/Win32Handle.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <variant>
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
    uint32_t sidecarRequestCount = 0; // mid-generation RequestSidecarFile messages serviced
};

// Mirrors D3D12ImportBridge.cpp's own caps so the test host applies the
// same ceilings the real one does.
constexpr uint32_t kMaxSidecarRequestsPerGeneration = 64;
constexpr uint64_t kMaxSidecarFileBytes = 256ull * 1024ull * 1024ull;

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

    // Services zero or more mid-generation RequestSidecarFile messages
    // before the terminal reply, through the same
    // import_broker::ServiceSidecarRequest the real host calls -- that
    // function exists precisely so tests exercise the production path
    // rather than a lookalike. Degrades to exactly one read for a
    // self-contained .glb, which never sends RequestSidecarFile, so every
    // pre-existing caller is unaffected.
    auto received = model_core::ReadControlMessage(launch->controlOutRead.get());
    while (received
           && received->header.opcode
               == static_cast<uint32_t>(model_core::ControlOpcode::RequestSidecarFile)
           && received->payload.size() == sizeof(model_core::RequestSidecarFileNotice)) {
        if (++run.sidecarRequestCount > kMaxSidecarRequestsPerGeneration) {
            break;
        }

        model_core::RequestSidecarFileNotice sidecarRequest{};
        std::memcpy(&sidecarRequest, received->payload.data(), sizeof(sidecarRequest));
        auto serviced = import_broker::ServiceSidecarRequest(
            launch->proc.process.get(), opened.canonicalPath, sidecarRequest, kMaxSidecarFileBytes);

        bool sentReply = false;
        if (const auto* ready = std::get_if<model_core::SidecarFileReadyNotice>(&serviced)) {
            sentReply = model_core::WriteControlMessage(
                launch->controlInWrite.get(), model_core::ControlOpcode::SidecarFileReady, ready,
                sizeof(*ready));
        } else if (const auto* unavailable
                   = std::get_if<model_core::SidecarFileUnavailableNotice>(&serviced)) {
            sentReply = model_core::WriteControlMessage(
                launch->controlInWrite.get(), model_core::ControlOpcode::SidecarFileUnavailable,
                unavailable, sizeof(*unavailable));
        }
        if (!sentReply) {
            received = std::nullopt;
            break;
        }

        received = model_core::ReadControlMessage(launch->controlOutRead.get());
    }
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

// 3 VEC3 float positions (36 B) followed by 3 SCALAR uint32 indices (12 B):
// byte-for-byte what tri_tight.glb carries in its own BIN chunk, so an
// external-buffer .gltf built on it must produce an identical mesh.
std::vector<std::byte> TriangleBufferBytes()
{
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const uint32_t indices[3] = {0, 1, 2};
    std::vector<std::byte> bytes(48);
    std::memcpy(bytes.data(), positions, sizeof(positions));
    std::memcpy(bytes.data() + 36, indices, sizeof(indices));
    return bytes;
}

// A self-cleaning temp directory holding a plain-JSON .gltf whose geometry
// lives in an external sibling. Lets each case vary exactly one thing --
// the buffer URI, whether the sibling exists, how long it is -- without
// shipping a checked-in fixture per case, the same approach
// SidecarPathResolverTests.cpp already takes.
struct ScratchExternalGltf {
    std::wstring directory;
    std::wstring gltfPath;
    std::vector<std::wstring> writtenFiles;

    ScratchExternalGltf()
    {
        wchar_t tempDir[MAX_PATH]{};
        REQUIRE(GetTempPathW(MAX_PATH, tempDir) != 0);
        directory = std::wstring(tempDir) + L"p3d_extbuf_" + std::to_wstring(GetCurrentProcessId())
            + L"_" + std::to_wstring(reinterpret_cast<uintptr_t>(this));
        REQUIRE(CreateDirectoryW(directory.c_str(), nullptr));
        gltfPath = directory + L"\\scene.gltf";
    }

    void WriteGltf(const std::string& bufferUri)
    {
        std::string json
            = "{\"asset\":{\"version\":\"2.0\"},\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
              "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
              "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
              "{\"bufferView\":1,\"componentType\":5125,\"count\":3,\"type\":\"SCALAR\"}],"
              "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
              "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":12}],"
              "\"buffers\":[{\"uri\":\""
            + bufferUri + "\",\"byteLength\":48}]}";
        WriteRaw(L"scene.gltf", std::span<const std::byte>(
                                    reinterpret_cast<const std::byte*>(json.data()), json.size()));
    }

    void WriteBin(const std::wstring& name, size_t byteCount)
    {
        auto bytes = TriangleBufferBytes();
        bytes.resize(byteCount);
        WriteRaw(name, bytes);
    }

    void WriteRaw(const std::wstring& name, std::span<const std::byte> bytes)
    {
        std::wstring path = directory + L"\\" + name;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        REQUIRE(out.is_open());
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.close();
        writtenFiles.push_back(path);
    }

    ~ScratchExternalGltf()
    {
        for (const auto& path : writtenFiles) {
            DeleteFileW(path.c_str());
        }
        RemoveDirectoryW(directory.c_str());
    }
};

} // namespace

TEST_CASE("tri_external.gltf sources uncompressed geometry from an external .bin over the sidecar "
          "protocol and produces the same mesh as the embedded tri_tight.glb",
          "[gltf-import][sidecar]")
{
    sandbox_test_support::SandboxFixture fixture;

    auto run = RunGltfImportFromRealFile(fixture.sid, TestAssetPath(L"tri_external.gltf"),
                                          /*generationId=*/40, /*maxChunkCount=*/8);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);
    REQUIRE(run.validation.chunks.size() == 1);

    // The bytes genuinely crossed the sidecar protocol rather than being
    // found embedded -- without this the test would still pass if the
    // adapter silently fell back to some in-file copy.
    CHECK(run.sidecarRequestCount >= 1);

    const auto& chunk = run.validation.chunks[0];
    CHECK(chunk.descriptor.topology == model_core::ChunkTopology::TriangleList);
    CHECK(chunk.descriptor.vertexCount == 3);
    CHECK(chunk.descriptor.indexCount == 3);
    CHECK(chunk.descriptor.vertexLayoutId
          == static_cast<uint32_t>(model_core::VertexLayoutId::PositionNormalUv0_F32));

    REQUIRE(chunk.payload.size() >= 3 * sizeof(model_core::VertexPositionNormalUv0F32));
    model_core::VertexPositionNormalUv0F32 vertices[3]{};
    std::memcpy(vertices, chunk.payload.data(), sizeof(vertices));
    const float expected[3][3] = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    for (size_t i = 0; i < 3; ++i) {
        CHECK(vertices[i].px == Catch::Approx(expected[i][0]).margin(1e-5));
        CHECK(vertices[i].py == Catch::Approx(expected[i][1]).margin(1e-5));
        CHECK(vertices[i].pz == Catch::Approx(expected[i][2]).margin(1e-5));
        CHECK(vertices[i].nz == Catch::Approx(1.0f).margin(1e-5));
    }
}

TEST_CASE("A .gltf whose external buffer is missing fails as FileUnavailable, not MalformedData",
          "[gltf-import][sidecar]")
{
    sandbox_test_support::SandboxFixture fixture;

    ScratchExternalGltf scratch;
    scratch.WriteGltf("mesh.bin"); // deliberately never written

    auto run = RunGltfImportFromRealFile(fixture.sid, scratch.gltfPath, /*generationId=*/41,
                                          /*maxChunkCount=*/8);
    CHECK_FALSE(run.ready);
    CHECK(run.errorNotice.errorCode
          == static_cast<uint32_t>(model_core::ImportErrorCode::FileUnavailable));
}

TEST_CASE("A .gltf whose external buffer is shorter than its declared bufferViews is rejected as "
          "MalformedData",
          "[gltf-import][sidecar]")
{
    sandbox_test_support::SandboxFixture fixture;

    ScratchExternalGltf scratch;
    scratch.WriteGltf("mesh.bin");
    scratch.WriteBin(L"mesh.bin", 20); // declares 48, delivers 20

    auto run = RunGltfImportFromRealFile(fixture.sid, scratch.gltfPath, /*generationId=*/42,
                                          /*maxChunkCount=*/8);
    CHECK_FALSE(run.ready);
    CHECK(run.errorNotice.errorCode
          == static_cast<uint32_t>(model_core::ImportErrorCode::MalformedData));
}

TEST_CASE("A .gltf whose buffer URI escapes the model's own directory is rejected as "
          "UnsafeReference by the host, and the worker never sees the bytes",
          "[gltf-import][sidecar]")
{
    sandbox_test_support::SandboxFixture fixture;

    ScratchExternalGltf scratch;
    scratch.WriteGltf("../escape.bin");

    auto run = RunGltfImportFromRealFile(fixture.sid, scratch.gltfPath, /*generationId=*/43,
                                          /*maxChunkCount=*/8);
    CHECK_FALSE(run.ready);
    CHECK(run.errorNotice.errorCode
          == static_cast<uint32_t>(model_core::ImportErrorCode::UnsafeReference));
}

TEST_CASE("A sparse accessor over an external buffer applies its overrides -- the custom "
          "BufferDataAdapter does not bypass fastgltf's sparse handling",
          "[gltf-import][sidecar]")
{
    sandbox_test_support::SandboxFixture fixture;

    // Base positions are tri_tight's, then a single sparse override
    // replaces vertex 1 with (5,5,5). fastgltf reads the base, sparse index
    // and sparse value bufferViews all through the adapter, so this is the
    // case that proves swapping the adapter kept sparse support intact.
    ScratchExternalGltf scratch;
    std::string json
        = "{\"asset\":{\"version\":\"2.0\"},\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
          "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
          "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
          "\"sparse\":{\"count\":1,\"indices\":{\"bufferView\":2,\"componentType\":5123},"
          "\"values\":{\"bufferView\":3}}},"
          "{\"bufferView\":1,\"componentType\":5125,\"count\":3,\"type\":\"SCALAR\"}],"
          "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
          "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":12},"
          "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":2},"
          "{\"buffer\":0,\"byteOffset\":52,\"byteLength\":12}],"
          "\"buffers\":[{\"uri\":\"mesh.bin\",\"byteLength\":64}]}";
    scratch.WriteRaw(L"scene.gltf",
                     std::span<const std::byte>(reinterpret_cast<const std::byte*>(json.data()),
                                                 json.size()));

    std::vector<std::byte> bin = TriangleBufferBytes();
    bin.resize(64);
    const uint16_t sparseIndex = 1;
    const float sparseValue[3] = {5.0f, 5.0f, 5.0f};
    std::memcpy(bin.data() + 48, &sparseIndex, sizeof(sparseIndex));
    std::memcpy(bin.data() + 52, sparseValue, sizeof(sparseValue));
    scratch.WriteRaw(L"mesh.bin", bin);

    auto run = RunGltfImportFromRealFile(fixture.sid, scratch.gltfPath, /*generationId=*/44,
                                          /*maxChunkCount=*/8);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);
    REQUIRE(run.validation.chunks.size() == 1);

    const auto& chunk = run.validation.chunks[0];
    REQUIRE(chunk.payload.size() >= 3 * sizeof(model_core::VertexPositionNormalUv0F32));
    model_core::VertexPositionNormalUv0F32 vertices[3]{};
    std::memcpy(vertices, chunk.payload.data(), sizeof(vertices));
    CHECK(vertices[0].px == Catch::Approx(0.0f).margin(1e-5));
    CHECK(vertices[1].px == Catch::Approx(5.0f).margin(1e-5));
    CHECK(vertices[1].py == Catch::Approx(5.0f).margin(1e-5));
    CHECK(vertices[1].pz == Catch::Approx(5.0f).margin(1e-5));
    CHECK(vertices[2].py == Catch::Approx(1.0f).margin(1e-5));
}

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

TEST_CASE("draco_triangle.glb (KHR_draco_mesh_compression, position+normal+uv0) decodes to the same "
          "chunk shape as an equivalent uncompressed fixture",
          "[gltf-import][draco]")
{
    sandbox_test_support::SandboxFixture fixture;

    auto bytes = ReadFileBytes(TestAssetPath(L"draco_triangle.glb"));
    REQUIRE(bytes.has_value());

    auto run = RunGltfImport(fixture.sid, *bytes, /*generationId=*/17, /*maxChunkCount=*/8);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);
    REQUIRE(run.validation.chunks.size() == 1);

    const auto& chunk = run.validation.chunks[0];
    CHECK(chunk.descriptor.topology == model_core::ChunkTopology::TriangleList);
    CHECK(chunk.descriptor.vertexCount == 12); // 4 triangles * 3 corners, generator uses no dedup-friendly sharing
    CHECK(chunk.descriptor.indexCount == 12);
    CHECK(chunk.descriptor.vertexLayoutId
          == static_cast<uint32_t>(model_core::VertexLayoutId::PositionNormalUv0_F32));

    REQUIRE(chunk.payload.size() >= 12 * sizeof(model_core::VertexPositionNormalUv0F32));
    model_core::VertexPositionNormalUv0F32 vertices[12]{};
    std::memcpy(vertices, chunk.payload.data(), sizeof(vertices));
    // Draco quantization (26 bits position / 16 bits normal+uv, see
    // gen-test-glbs-draco.cpp) is lossy but should stay well within a loose
    // tolerance for these small, hand-picked coordinates.
    for (const auto& v : vertices) {
        CHECK(v.pz == Catch::Approx(0.0f).margin(1e-3));
        CHECK(v.nx == Catch::Approx(0.0f).margin(1e-2));
        CHECK(v.ny == Catch::Approx(0.0f).margin(1e-2));
        CHECK(v.nz == Catch::Approx(1.0f).margin(1e-2));
    }
}

TEST_CASE("draco_position_only.glb (no NORMAL/TEXCOORD_0) falls back to generated flat normals and "
          "zeroed uv, same as the uncompressed no-normal path",
          "[gltf-import][draco]")
{
    sandbox_test_support::SandboxFixture fixture;

    auto bytes = ReadFileBytes(TestAssetPath(L"draco_position_only.glb"));
    REQUIRE(bytes.has_value());

    auto run = RunGltfImport(fixture.sid, *bytes, /*generationId=*/18, /*maxChunkCount=*/8);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);
    REQUIRE(run.validation.chunks.size() == 1);

    const auto& chunk = run.validation.chunks[0];
    CHECK(chunk.descriptor.vertexCount == 6); // 2 triangles * 3 corners
    CHECK(chunk.descriptor.indexCount == 6);

    model_core::VertexPositionNormalUv0F32 vertices[6]{};
    std::memcpy(vertices, chunk.payload.data(), sizeof(vertices));
    for (const auto& v : vertices) {
        CHECK(v.nz == Catch::Approx(1.0f).margin(1e-3)); // generator's triangles all face +Z
        CHECK(v.u == 0.0f);
        CHECK(v.v == 0.0f);
    }
}

TEST_CASE("A material with flat PBR factors and no texture emits one Material chunk the mesh "
          "depends on, with dependencyCount == 0 on the material itself",
          "[gltf-import][material]")
{
    sandbox_test_support::SandboxFixture fixture;

    // No Draco/KTX2 needed -- a plain glTF material JSON exercises Stage 4's
    // factor-only path (Stage 5's basisu transcode is separately covered).
    const char* json =
        "{\"asset\":{\"version\":\"2.0\"},\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1,\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.25,0.5,0.75,1.0],"
        "\"metallicFactor\":0.1,\"roughnessFactor\":0.9},\"emissiveFactor\":[0.0,0.2,0.0],"
        "\"alphaMode\":\"MASK\",\"alphaCutoff\":0.3,\"doubleSided\":true}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5125,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":12}],\"buffers\":[{\"byteLength\":48}]}";
    std::vector<std::byte> bin;
    auto appendF32 = [&bin](float f) {
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof(bits));
        for (int i = 0; i < 4; ++i) bin.push_back(static_cast<std::byte>((bits >> (i * 8)) & 0xFF));
    };
    auto appendU32 = [&bin](uint32_t v) {
        for (int i = 0; i < 4; ++i) bin.push_back(static_cast<std::byte>((v >> (i * 8)) & 0xFF));
    };
    const float tri[3][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } };
    for (const auto& p : tri) {
        appendF32(p[0]);
        appendF32(p[1]);
        appendF32(p[2]);
    }
    appendU32(0);
    appendU32(1);
    appendU32(2);

    std::string jsonStr(json);
    while (jsonStr.size() % 4 != 0) jsonStr.push_back(' ');
    while (bin.size() % 4 != 0) bin.push_back(std::byte{ 0 });

    std::vector<std::byte> glb;
    auto push32 = [&glb](uint32_t v) {
        for (int i = 0; i < 4; ++i) glb.push_back(static_cast<std::byte>((v >> (i * 8)) & 0xFF));
    };
    push32(0x46546C67);
    push32(2);
    push32(static_cast<uint32_t>(12 + 8 + jsonStr.size() + 8 + bin.size()));
    push32(static_cast<uint32_t>(jsonStr.size()));
    push32(0x4E4F534A);
    for (char c : jsonStr) glb.push_back(static_cast<std::byte>(c));
    push32(static_cast<uint32_t>(bin.size()));
    push32(0x004E4942);
    glb.insert(glb.end(), bin.begin(), bin.end());

    auto run = RunGltfImport(fixture.sid, glb, /*generationId=*/19, /*maxChunkCount=*/8);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);
    REQUIRE(run.validation.chunks.size() == 2);

    const import_broker::ValidatedChunk* meshChunk = nullptr;
    const import_broker::ValidatedChunk* materialChunk = nullptr;
    for (const auto& c : run.validation.chunks) {
        if (c.descriptor.topology == model_core::ChunkTopology::TriangleList) meshChunk = &c;
        if (c.descriptor.topology == model_core::ChunkTopology::Material) materialChunk = &c;
    }
    REQUIRE(meshChunk != nullptr);
    REQUIRE(materialChunk != nullptr);

    CHECK(meshChunk->descriptor.dependencyCount == 1);
    CHECK(meshChunk->descriptor.dependencyIds[0] == materialChunk->descriptor.chunkId);
    CHECK(materialChunk->descriptor.dependencyCount == 0);

    REQUIRE(materialChunk->payload.size() == sizeof(model_core::MaterialPayload));
    model_core::MaterialPayload payload{};
    std::memcpy(&payload, materialChunk->payload.data(), sizeof(payload));
    CHECK(payload.baseColorFactor[0] == Catch::Approx(0.25f));
    CHECK(payload.baseColorFactor[1] == Catch::Approx(0.5f));
    CHECK(payload.baseColorFactor[2] == Catch::Approx(0.75f));
    CHECK(payload.metallicFactor == Catch::Approx(0.1f));
    CHECK(payload.roughnessFactor == Catch::Approx(0.9f));
    CHECK(payload.emissiveFactor[1] == Catch::Approx(0.2f));
    CHECK(payload.alphaMode == static_cast<uint32_t>(model_core::AlphaModeId::Mask));
    CHECK(payload.alphaCutoff == Catch::Approx(0.3f));
    CHECK((payload.flags & model_core::kMaterialFlagDoubleSided) != 0);
}

TEST_CASE("basisu_textured_triangle.glb (KHR_texture_basisu base color) produces a full "
          "mesh -> material -> image chain",
          "[gltf-import][texture]")
{
    sandbox_test_support::SandboxFixture fixture;

    auto bytes = ReadFileBytes(TestAssetPath(L"basisu_textured_triangle.glb"));
    REQUIRE(bytes.has_value());

    auto run = RunGltfImport(fixture.sid, *bytes, /*generationId=*/20, /*maxChunkCount=*/8);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);
    REQUIRE(run.validation.chunks.size() == 3);

    const import_broker::ValidatedChunk* meshChunk = nullptr;
    const import_broker::ValidatedChunk* materialChunk = nullptr;
    const import_broker::ValidatedChunk* imageChunk = nullptr;
    for (const auto& c : run.validation.chunks) {
        if (c.descriptor.topology == model_core::ChunkTopology::TriangleList) meshChunk = &c;
        if (c.descriptor.topology == model_core::ChunkTopology::Material) materialChunk = &c;
        if (c.descriptor.topology == model_core::ChunkTopology::Image) imageChunk = &c;
    }
    REQUIRE(meshChunk != nullptr);
    REQUIRE(materialChunk != nullptr);
    REQUIRE(imageChunk != nullptr);

    CHECK(meshChunk->descriptor.dependencyIds[0] == materialChunk->descriptor.chunkId);
    REQUIRE(materialChunk->descriptor.dependencyCount == 1);
    CHECK(materialChunk->descriptor.dependencyIds[0] == imageChunk->descriptor.chunkId);
    CHECK(imageChunk->descriptor.dependencyCount == 0);

    REQUIRE(imageChunk->payload.size() >= sizeof(model_core::ImagePayloadHeader));
    model_core::ImagePayloadHeader header{};
    std::memcpy(&header, imageChunk->payload.data(), sizeof(header));
    CHECK(header.width == 8);
    CHECK(header.height == 8);
    CHECK(header.mipLevels == 1);
    auto pixelFormat = static_cast<model_core::PixelFormatId>(header.pixelFormat);
    CHECK((pixelFormat == model_core::PixelFormatId::BC7_UNORM
           || pixelFormat == model_core::PixelFormatId::RGBA8_UNORM));
}

TEST_CASE("basisu_corrupt_ktx2.glb (valid KHR_texture_basisu reference, garbage KTX2 bytes) soft-fails "
          "to a material with no image dependency, not a hard import failure",
          "[gltf-import][texture]")
{
    sandbox_test_support::SandboxFixture fixture;

    auto bytes = ReadFileBytes(TestAssetPath(L"basisu_corrupt_ktx2.glb"));
    REQUIRE(bytes.has_value());

    auto run = RunGltfImport(fixture.sid, *bytes, /*generationId=*/21, /*maxChunkCount=*/8);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);
    REQUIRE(run.validation.chunks.size() == 2); // mesh + material only, no image chunk

    const import_broker::ValidatedChunk* materialChunk = nullptr;
    for (const auto& c : run.validation.chunks) {
        if (c.descriptor.topology == model_core::ChunkTopology::Material) materialChunk = &c;
    }
    REQUIRE(materialChunk != nullptr);
    CHECK(materialChunk->descriptor.dependencyCount == 0);
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
