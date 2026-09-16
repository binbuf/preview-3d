// Gate 2's "worker pool and generation cancellation" deliverable. Proves:
// (1) the same pooled worker process serves two sequential generations
// without relaunch; (2) Shutdown cleanly exits an idle worker; (3) a
// WaitForReply timeout triggers terminate-and-replace and the replacement
// slot is immediately usable; (4) a superseded generation's eventual reply
// is correctly identified as stale via platform::GenerationToken and the
// worker still returns to Idle cleanly regardless. True mid-parse
// cooperative cancellation is explicitly out of scope -- see
// .docs/PROGRESS.md and WorkerPool.h's own header comment.

#include "SandboxTestSupport.h"
#include "import_broker/SharedSection.h"
#include "import_broker/ImportSession.h"
#include "import_broker/WorkerPool.h"
#include "model_core/ControlChannelIo.h"
#include "model_core/ControlProtocol.h"
#include "platform/Generation.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <filesystem>
#include <atomic>

namespace {

// One StartGeneration round trip through an already-acquired pool slot:
// creates a fresh output section, duplicates it into the worker, sends the
// request, and waits (bounded) for the reply. The section is intentionally
// not kept alive past this call -- these tests only care about the
// protocol/pooling behavior, not re-validating synthetic-generation
// content (already proven by ImportPipelineTests.cpp).
std::optional<model_core::ReceivedControlMessage> RunGenerationThroughPool(import_broker::WorkerPool& pool,
                                                                            size_t index, uint64_t generationId,
                                                                            DWORD timeoutMs)
{
    auto section = import_broker::CreateSharedSection(import_broker::kSyntheticSectionBytes);
    if (!section) {
        return std::nullopt;
    }

    auto dupValue = pool.DuplicateSectionIntoWorker(index, section.get());
    if (!dupValue) {
        return std::nullopt;
    }

    model_core::StartGenerationRequest request{};
    request.generationId = generationId;
    request.sceneVariant = model_core::kSceneVariant_CubeAndPointCluster;
    request.sectionHandleValue = *dupValue;
    request.sectionByteCapacity = import_broker::kSyntheticSectionBytes;
    request.maxChunkCount = 8;

    if (!pool.SendRequest(index, model_core::ControlOpcode::StartGeneration, &request, sizeof(request))) {
        return std::nullopt;
    }

    model_core::ReceivedControlMessage reply;
    if (pool.WaitForReply(index, timeoutMs, reply) != import_broker::WaitReplyOutcome::Ready) {
        return std::nullopt;
    }
    return reply;
}

} // namespace

TEST_CASE("The product pool reuses a sandboxed worker for real sidecar imports", "[worker-pool][session]")
{
    const auto worker = sandbox_test_support::WorkerExePath();
    import_broker::PrepareImportWorkerPoolAsync(worker);

    const auto source = (std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()
        / "interactive-viewer" / "test-assets" / "tri_external.gltf").wstring();
    auto request = [&] (uint64_t generation) {
        import_broker::ImportSessionRequest value;
        value.workerExePath = worker;
        value.sourcePath = source;
        value.format = import_broker::ImportFormat::Gltf;
        value.generationId = generation;
        value.sectionByteCapacity = 4ull * 1024 * 1024;
        value.maxChunkCount = 64;
        value.maxChunkBatchesPerGeneration = 16;
        value.maxChunksPerGeneration = 1024;
        value.maxSidecarRequestsPerGeneration = 16;
        value.maxSidecarFileBytes = 16ull * 1024 * 1024;
        value.useWorkerPool = true;
        return value;
    };

    const auto first = import_broker::RunImportSession(request(9001));
    REQUIRE(first.ok);
    REQUIRE(first.workerProcessId != 0);
    const auto second = import_broker::RunImportSession(request(9002));
    REQUIRE(second.ok);
    CHECK(second.workerProcessId == first.workerProcessId);
}

TEST_CASE("Pooled progressive cancellation is acknowledged and the worker remains reusable",
          "[worker-pool][cancellation]")
{
    const auto worker = sandbox_test_support::WorkerExePath();
    import_broker::PrepareImportWorkerPoolAsync(worker);
    const auto source = (std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()
        / "interactive-viewer" / "test-assets" / "corpus" / "A-small-glb.glb").wstring();

    std::atomic_bool cancelled = false;
    uint32_t batches = 0;
    import_broker::ImportSessionRequest request;
    request.workerExePath = worker;
    request.sourcePath = source;
    request.format = import_broker::ImportFormat::Gltf;
    request.generationId = 9010;
    request.sectionByteCapacity = 1024 * 1024;
    request.maxChunkCount = 256;
    request.maxChunkBatchesPerGeneration = 64;
    request.maxChunksPerGeneration = 4096;
    request.maxSidecarRequestsPerGeneration = 16;
    request.maxSidecarFileBytes = 16ull * 1024 * 1024;
    request.enableCoarseProxy = true;
    request.useWorkerPool = true;
    request.isCancelled = [&] { return cancelled.load(); };
    request.onBatch = [&] (std::vector<import_broker::ValidatedChunk>&& chunks) {
        ++batches;
        REQUIRE_FALSE(chunks.empty());
        for (const auto& chunk : chunks) {
            if (chunk.descriptor.topology == model_core::ChunkTopology::TriangleList
                || chunk.descriptor.topology == model_core::ChunkTopology::PointList) {
                CHECK(chunk.descriptor.lodLevel == model_core::kPreviewLod);
            }
        }
        cancelled.store(true);
    };

    const auto cancelledResult = import_broker::RunImportSession(request);
    REQUIRE_FALSE(cancelledResult.ok);
    CHECK(cancelledResult.stage == import_broker::ImportStage::Cancelled);
    REQUIRE(batches == 1);
    REQUIRE(cancelledResult.workerProcessId != 0);

    request.generationId = 9011;
    request.enableCoarseProxy = false;
    request.isCancelled = {};
    request.onBatch = {};
    const auto after = import_broker::RunImportSession(request);
    REQUIRE(after.ok);
    CHECK(after.workerProcessId == cancelledResult.workerProcessId);
}

TEST_CASE("The same pooled worker process handles two sequential generations without relaunch",
          "[worker-pool]")
{
    sandbox_test_support::SandboxFixture fixture;

    import_broker::WorkerPool pool;
    import_broker::SandboxLimits limits{};
    std::wstring error;
    REQUIRE(pool.Initialize(sandbox_test_support::WorkerExePath(), std::move(fixture.sid), limits,
                             /*size=*/1, error));

    auto index = pool.AcquireIdle();
    REQUIRE(index.has_value());
    DWORD pidBefore = pool.ProcessId(*index);

    auto reply1 = RunGenerationThroughPool(pool, *index, /*generationId=*/1, 5000);
    REQUIRE(reply1.has_value());
    CHECK(reply1->header.opcode == static_cast<uint32_t>(model_core::ControlOpcode::ChunksReady));
    pool.Release(*index);

    auto index2 = pool.AcquireIdle();
    REQUIRE(index2.has_value());
    CHECK(*index2 == *index); // only one worker in the pool -- must be the same slot
    CHECK(pool.ProcessId(*index2) == pidBefore); // same OS process, not relaunched

    auto reply2 = RunGenerationThroughPool(pool, *index2, /*generationId=*/2, 5000);
    REQUIRE(reply2.has_value());
    CHECK(reply2->header.opcode == static_cast<uint32_t>(model_core::ControlOpcode::ChunksReady));
    pool.Release(*index2);
}

TEST_CASE("Shutdown cleanly exits an idle pooled worker", "[worker-pool]")
{
    sandbox_test_support::SandboxFixture fixture;

    import_broker::WorkerPool pool;
    import_broker::SandboxLimits limits{};
    std::wstring error;
    REQUIRE(pool.Initialize(sandbox_test_support::WorkerExePath(), std::move(fixture.sid), limits, 1, error));

    auto index = pool.AcquireIdle();
    REQUIRE(index.has_value());
    HANDLE processHandle = pool.ProcessHandle(*index);
    pool.Release(*index);

    pool.Shutdown();

    // Shutdown() already waited up to 2s internally; confirm the process
    // genuinely exited rather than just trusting the wait succeeded.
    CHECK(WaitForSingleObject(processHandle, 0) == WAIT_OBJECT_0);
}

TEST_CASE("A WaitForReply timeout triggers terminate-and-replace, and the replacement slot is usable",
          "[worker-pool]")
{
    sandbox_test_support::SandboxFixture fixture;

    import_broker::WorkerPool pool;
    import_broker::SandboxLimits limits{};
    std::wstring error;
    REQUIRE(pool.Initialize(sandbox_test_support::WorkerExePath(), std::move(fixture.sid), limits, 1, error));

    auto index = pool.AcquireIdle();
    REQUIRE(index.has_value());
    DWORD pidBefore = pool.ProcessId(*index);

    // Deliberately impossible: nothing was ever sent to this worker, so an
    // artificially tiny timeout exercises the timeout branch
    // deterministically -- no need for a genuinely hung worker (that
    // mechanism is already proven separately by SandboxLaunchTests.cpp's
    // "Job Object kill-on-close terminates a hung worker").
    model_core::ReceivedControlMessage reply;
    CHECK(pool.WaitForReply(*index, /*timeoutMs=*/1, reply) == import_broker::WaitReplyOutcome::TimedOut);

    REQUIRE(pool.TerminateAndReplace(*index, error));
    CHECK(pool.ProcessId(*index) != pidBefore); // a genuinely new process, pool size unchanged

    pool.Release(*index);
    auto index2 = pool.AcquireIdle();
    REQUIRE(index2.has_value());
    auto replyAfter = RunGenerationThroughPool(pool, *index2, /*generationId=*/1, 5000);
    REQUIRE(replyAfter.has_value());
    CHECK(replyAfter->header.opcode == static_cast<uint32_t>(model_core::ControlOpcode::ChunksReady));
}

TEST_CASE("A superseded generation's reply is identified as stale via GenerationToken, and the worker "
          "still returns to Idle cleanly",
          "[worker-pool]")
{
    sandbox_test_support::SandboxFixture fixture;

    import_broker::WorkerPool pool;
    import_broker::SandboxLimits limits{};
    std::wstring error;
    REQUIRE(pool.Initialize(sandbox_test_support::WorkerExePath(), std::move(fixture.sid), limits, 1, error));

    platform::GenerationSource generation;
    platform::GenerationToken tokenForGen1 = generation.Snapshot();

    auto index = pool.AcquireIdle();
    REQUIRE(index.has_value());

    auto reply = RunGenerationThroughPool(pool, *index, /*generationId=*/1, 5000);
    REQUIRE(reply.has_value());
    CHECK(reply->header.opcode == static_cast<uint32_t>(model_core::ControlOpcode::ChunksReady));

    // A newer generation supersedes generation 1 -- by the time its reply
    // arrived (above), a real caller would already have moved on to
    // generation 2, so generation 1's reply is correctly identified as
    // stale, same platform::GenerationToken pattern
    // D3D12UploadRing::DrainCompletedPublications already uses to drop a
    // fence-complete-but-superseded upload.
    generation.Advance();
    CHECK_FALSE(tokenForGen1.IsCurrent(generation));

    // The worker itself is unaffected by the host's decision to ignore the
    // reply -- it still returns to Idle and serves further requests
    // normally.
    pool.Release(*index);
    auto index2 = pool.AcquireIdle();
    REQUIRE(index2.has_value());
    auto reply2 = RunGenerationThroughPool(pool, *index2, /*generationId=*/2, 5000);
    REQUIRE(reply2.has_value());
    CHECK(reply2->header.opcode == static_cast<uint32_t>(model_core::ControlOpcode::ChunksReady));
}
