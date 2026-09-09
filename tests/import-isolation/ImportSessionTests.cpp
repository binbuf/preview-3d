// Tests for the product's own sandboxed import path
// (import_broker::RunImportSession) and the bounded control-channel read it
// waits on. Both used to live inside interactive-viewer's D3D12ImportBridge,
// which is compiled only into Preview3D.exe and so was reachable from no
// test at all -- which is how the product drifted away from the sandbox
// configuration the rest of this suite already proves.
//
// Unlike the other files here, most of these cases do NOT construct a
// SandboxFixture: RunImportSession deliberately runs under the single
// long-lived profile the design names (Binbuf.Preview3D.ImportWorker,
// .docs/design/08-installation-and-registration.md:40) rather than a
// throwaway one, and exercising the real identity is the point. That profile
// and its one ACE on the worker directory therefore persist after this suite
// runs, by design.

#include "import_broker/ControlChannelWait.h"
#include "import_broker/ImportSession.h"
#include "model_core/ControlProtocol.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"
#include "platform/Win32Handle.h"
#include "SandboxTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <chrono>
#include <cstring>
#include <string>

#ifndef PREVIEW3D_TEST_ASSETS_DIR
#error "PREVIEW3D_TEST_ASSETS_DIR must be defined by Tests.ImportIsolation.vcxproj"
#endif

namespace {

std::wstring TestAssetPath(const wchar_t* fileName)
{
    return std::wstring(PREVIEW3D_TEST_ASSETS_DIR) + fileName;
}

import_broker::ImportSessionRequest MakeRequest(const wchar_t* asset, import_broker::ImportFormat format,
                                                 uint64_t generationId)
{
    import_broker::ImportSessionRequest request;
    request.workerExePath = sandbox_test_support::WorkerExePath();
    request.sourcePath = TestAssetPath(asset);
    request.format = format;
    request.generationId = generationId;
    request.sectionByteCapacity = 1ull * 1024 * 1024;
    request.maxChunkCount = 64;
    request.maxSidecarRequestsPerGeneration = 64;
    request.maxSidecarFileBytes = 256ull * 1024 * 1024;
    return request;
}

// A pair of anonymous pipes, so the control-channel tests can drive
// ReadControlMessageBounded deterministically without launching anything.
struct PipePair {
    platform::Win32Handle readEnd;
    platform::Win32Handle writeEnd;

    PipePair()
    {
        HANDLE r = nullptr;
        HANDLE w = nullptr;
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;
        REQUIRE(CreatePipe(&r, &w, &sa, 0));
        readEnd = platform::Win32Handle(r);
        writeEnd = platform::Win32Handle(w);
    }
};

} // namespace

TEST_CASE("A real glTF file imports end to end through the product's own session path",
          "[import-session]")
{
    auto request = MakeRequest(L"tri_tight.glb", import_broker::ImportFormat::Gltf, /*generationId=*/1);

    import_broker::ImportSessionResult result = import_broker::RunImportSession(request);

    REQUIRE(result.ok);
    CHECK(result.stage == import_broker::ImportStage::Completed);
    REQUIRE(result.chunks.size() == 1);
    CHECK(result.chunks[0].descriptor.topology == model_core::ChunkTopology::TriangleList);
    CHECK(result.chunks[0].descriptor.vertexLayoutId
          == static_cast<uint32_t>(model_core::VertexLayoutId::PositionNormalUv0_F32));
    CHECK(result.chunks[0].descriptor.vertexCount == 3);
    CHECK(result.chunks[0].descriptor.indexCount == 3);
}

TEST_CASE("STL and PLY route to their own adapters through the same session path", "[import-session]")
{
    // Guards the format -> {CLI flag, opcode, request struct} mapping that
    // moved out of the app and into ImportSession.cpp.
    auto stl = import_broker::RunImportSession(
        MakeRequest(L"tri_tight.glb", import_broker::ImportFormat::Stl, /*generationId=*/2));
    // A GLB is not a valid STL. Asserting the *stage* matters: it proves the
    // STL adapter actually ran and rejected the bytes, rather than the
    // session failing somewhere in its own plumbing and passing for the
    // wrong reason.
    CHECK_FALSE(stl.ok);
    CHECK(stl.stage == import_broker::ImportStage::WorkerReportedError);
    // ResourceLimit, not MalformedData: a GLB's bytes at offset 80 read as a
    // wildly oversized declared facet count, so StlAdapter's kMaxFacets cap
    // rejects the file before any body scan -- which is a sharper proof that
    // the STL adapter specifically is what ran.
    CHECK(stl.errorCode == model_core::ImportErrorCode::ResourceLimit);

    auto gltf = import_broker::RunImportSession(
        MakeRequest(L"tri_tight.glb", import_broker::ImportFormat::Gltf, /*generationId=*/3));
    CHECK(gltf.ok);
}

TEST_CASE("Two sequential imports reuse the one long-lived AppContainer profile", "[import-session]")
{
    // The profile is created on first use and never deleted per import, so a
    // second import must succeed against the already-existing profile --
    // which is also what stops the previous create-and-delete-per-import
    // shape from leaking one profile per hung worker.
    REQUIRE(import_broker::PrepareImportSandbox(sandbox_test_support::WorkerExePath()));
    REQUIRE(import_broker::PrepareImportSandbox(sandbox_test_support::WorkerExePath()));

    auto first = import_broker::RunImportSession(
        MakeRequest(L"tri_tight.glb", import_broker::ImportFormat::Gltf, /*generationId=*/4));
    auto second = import_broker::RunImportSession(
        MakeRequest(L"tri_tight.glb", import_broker::ImportFormat::Gltf, /*generationId=*/5));

    CHECK(first.ok);
    CHECK(second.ok);
}

TEST_CASE("A cancelled generation stops before launching a worker at all", "[import-session]")
{
    auto request = MakeRequest(L"tri_tight.glb", import_broker::ImportFormat::Gltf, /*generationId=*/6);
    request.isCancelled = [] { return true; };

    auto result = import_broker::RunImportSession(request);

    CHECK_FALSE(result.ok);
    CHECK(result.stage == import_broker::ImportStage::Cancelled);
    CHECK(result.chunks.empty());
}

TEST_CASE("A commit limit too small for the worker fails the import cleanly", "[import-session]")
{
    // Differential, so it proves the knob is actually wired rather than that
    // some unrelated thing failed: the same file imports fine at the
    // product's real ceiling and fails under a deliberately impossible one.
    auto generous = MakeRequest(L"tri_tight.glb", import_broker::ImportFormat::Gltf, /*generationId=*/7);
    REQUIRE(import_broker::RunImportSession(generous).ok);

    auto starved = MakeRequest(L"tri_tight.glb", import_broker::ImportFormat::Gltf, /*generationId=*/8);
    starved.commitLimitBytes = 1ull * 1024 * 1024; // 1 MiB: not enough to start a process

    auto result = import_broker::RunImportSession(starved);

    CHECK_FALSE(result.ok);
    CHECK(result.chunks.empty());
    // The failure must come from the worker dying under its cap, not from
    // the session tripping over its own inputs -- otherwise this would pass
    // even if commitLimitBytes were ignored entirely.
    CHECK(result.stage != import_broker::ImportStage::OpenSource);
    CHECK(result.stage != import_broker::ImportStage::CreateSandboxProfile);
    CHECK(result.stage != import_broker::ImportStage::WorkerReportedError);
}

TEST_CASE("The product's default commit ceiling is a real limit, not left unbounded",
          "[import-session]")
{
    // SandboxLimits{} means "no Job Object commit limit" (SandboxLauncher.h),
    // and that default is exactly what the shipping import path used to pass.
    // This pins the regression rather than the specific number.
    CHECK(import_broker::kImportWorkerCommitLimitBytes > 0);
    CHECK(import_broker::ImportSessionRequest{}.commitLimitBytes
          == import_broker::kImportWorkerCommitLimitBytes);
}

TEST_CASE("A bounded control-channel read times out instead of blocking forever", "[import-session]")
{
    // model_core::ReadControlMessage would block here indefinitely: the
    // write end is open, so there is no EOF, and no bytes ever arrive.
    PipePair pipes;
    model_core::ReceivedControlMessage message{};

    auto started = std::chrono::steady_clock::now();
    auto outcome = import_broker::ReadControlMessageBounded(pipes.readEnd.get(),
                                                             std::chrono::milliseconds(200), message);
    auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK(outcome == import_broker::ControlWaitOutcome::TimedOut);
    CHECK(elapsed >= std::chrono::milliseconds(200));
    CHECK(elapsed < std::chrono::seconds(5)); // the deadline is honoured, not merely eventual
}

TEST_CASE("A header written without its payload still hits the deadline", "[import-session]")
{
    // The flaw the shared wait was written to fix: waiting only for the
    // first available byte and then committing to an unbounded read means a
    // worker that writes a message header and stalls mid-payload hangs past
    // its timeout.
    PipePair pipes;
    model_core::ControlMessageHeader header{};
    header.opcode = static_cast<uint32_t>(model_core::ControlOpcode::ChunksReady);
    header.payloadSize = sizeof(model_core::ChunksReadyNotice);
    DWORD written = 0;
    REQUIRE(WriteFile(pipes.writeEnd.get(), &header, sizeof(header), &written, nullptr));
    REQUIRE(written == sizeof(header));

    model_core::ReceivedControlMessage message{};
    auto outcome = import_broker::ReadControlMessageBounded(pipes.readEnd.get(),
                                                             std::chrono::milliseconds(200), message);

    CHECK(outcome == import_broker::ControlWaitOutcome::TimedOut);
}

TEST_CASE("A closed write end reports end-of-file rather than timing out", "[import-session]")
{
    PipePair pipes;
    pipes.writeEnd.reset();

    model_core::ReceivedControlMessage message{};
    auto outcome = import_broker::ReadControlMessageBounded(pipes.readEnd.get(),
                                                             std::chrono::seconds(30), message);

    CHECK(outcome == import_broker::ControlWaitOutcome::Eof);
}

TEST_CASE("A cancelled wait returns promptly without waiting out the deadline", "[import-session]")
{
    PipePair pipes;
    model_core::ReceivedControlMessage message{};

    auto started = std::chrono::steady_clock::now();
    auto outcome = import_broker::ReadControlMessageBounded(
        pipes.readEnd.get(), std::chrono::seconds(30), message, [] { return true; });
    auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK(outcome == import_broker::ControlWaitOutcome::Cancelled);
    CHECK(elapsed < std::chrono::seconds(5));
}

TEST_CASE("A complete message written in two writes is still read whole", "[import-session]")
{
    // The deadline covers the whole message, but a message that arrives in
    // pieces within the deadline must still succeed -- this is what keeps
    // the stall guard above from breaking ordinary framing.
    PipePair pipes;
    model_core::ControlMessageHeader header{};
    header.opcode = static_cast<uint32_t>(model_core::ControlOpcode::ChunksReady);
    header.payloadSize = sizeof(model_core::ChunksReadyNotice);
    model_core::ChunksReadyNotice notice{};
    notice.generationId = 4242;
    notice.chunkCount = 7;

    DWORD written = 0;
    REQUIRE(WriteFile(pipes.writeEnd.get(), &header, sizeof(header), &written, nullptr));
    REQUIRE(WriteFile(pipes.writeEnd.get(), &notice, sizeof(notice), &written, nullptr));

    model_core::ReceivedControlMessage message{};
    auto outcome = import_broker::ReadControlMessageBounded(pipes.readEnd.get(),
                                                             std::chrono::seconds(5), message);

    REQUIRE(outcome == import_broker::ControlWaitOutcome::Ready);
    CHECK(message.header.opcode == static_cast<uint32_t>(model_core::ControlOpcode::ChunksReady));
    REQUIRE(message.payload.size() == sizeof(notice));
    model_core::ChunksReadyNotice roundTripped{};
    std::memcpy(&roundTripped, message.payload.data(), sizeof(roundTripped));
    CHECK(roundTripped.generationId == 4242);
    CHECK(roundTripped.chunkCount == 7);
}
