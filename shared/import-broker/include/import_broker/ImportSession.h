#pragma once

#include "model_core/FileIdentity.h"

// One complete host-side sandboxed import: open+canonicalize a real on-disk
// source, duplicate read-only input/output/cancellation handles into a fixed
// zero-capability AppContainer worker pool (or an isolated one-shot test
// worker), send the
// format's StartXxxImportFromFile request, service any RequestSidecarFile
// messages, and copy-then-validate the worker's output section.
//
// This lived inline in interactive-viewer/src/app/D3D12ImportBridge.cpp
// until it was lifted here. The move is not cosmetic: the bridge is compiled
// only into Preview3D.exe, so none of this sequence was reachable from
// tests/import-isolation/ -- which is why the product could drift away from
// the sandbox configuration that suite already proves (no Job Object commit
// limit, unbounded reply reads). Everything here is compiled into both
// Preview3D.vcxproj and Tests.ImportIsolation.vcxproj, following this repo's
// established cross-project ClCompile pattern.
//
// What deliberately stays with the caller: mapping a file extension to a
// format, unpacking validated chunks into renderer-facing structs, and every
// user-facing string. This layer returns typed results only.

#include "import_broker/SharedSectionValidator.h"
#include "model_core/ControlProtocol.h"
#include "model_core/ImportError.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace import_broker {

// Which Tier A adapter to run. Owning the format -> {worker CLI flag,
// ControlOpcode, request struct} mapping here keeps that triple together
// next to the protocol it belongs to, instead of split across the app.
enum class ImportFormat : uint32_t {
    Gltf, // .glb and .gltf (the latter may pull sidecars)
    Stl,
    Ply,
    Obj,
    Fbx,
    // Test-only protocol route through USD-007. Product extension discovery
    // and activation deliberately remain disabled until USD-008.
    Usd,
    // Bounded 3MF Core/Materials/Production/Beam Lattice viewer route.
    ThreeMf,
    // Bounded STEP/STP static CAD-preview route. Runs only in the dedicated
    // zero-capability Preview3DStepHost.exe; the general worker never accepts
    // this format. Product extension discovery stays disabled until STEP-006.
    Step,
};

enum class ImportProducer : uint32_t {
    None = 0,
    FastWorker = 1,
    CompatibilityHost = 2,
    // Dedicated OCCT STEP host. A STEP generation may only ever be produced
    // by this identity; the broker rejects any other producer/format pairing.
    StepHost = 3,
};

// How far the session got. model_core::ImportErrorCode is not sufficient on
// its own: most failures below are host-side plumbing faults the worker
// never got a chance to report, and the caller renders a distinct message
// for each. Kept out of ImportErrorCode deliberately -- that enum is the
// worker's typed taxonomy crossing the wire, not a record of the host's
// progress.
enum class ImportStage : uint32_t {
    Completed = 0,
    OpenSource,
    DuplicateSourceHandle,
    CreateOutputSection,
    CreateSandboxProfile,
    CreateControlChannel,
    LaunchWorker,
    ResumeWorker,
    SendRequest,
    SidecarRequestLimit,
    AwaitReply,
    ReplyTimedOut,
    Cancelled,
    UnexpectedReply,
    MapOutputSection,
    ValidateSection,
    WorkerReportedError,
    // Progressive delivery. Each is a distinct worker misbehavior the caller
    // reports differently, and none is reachable on a single-batch import.
    ChunkBatchLimit,      // more batches in one generation than the cap allows
    ChunkBatchOutOfOrder, // a replayed, skipped, or wrong-generation batchIndex
    ChunkCountLimit,      // more chunks across the generation than the cap allows
    ChunkBatchAckFailed,  // the ack could not be written (worker gone mid-batch)
    StepProgressLimit,    // a STEP host sent more bounded progress events than the cap allows
    Upload,
};

// Job Object private-commit ceiling every import worker runs under.
//
// .docs/design/09-quality-performance-and-security.md:76 asks for a value
// "measured and fixed in Gate 2 to comfortably exceed the worst-case sum of
// the Tier A/B scratch, texture, and archive-expansion budgets" in
// 03-file-formats-and-ingestion.md. The A-small/medium/large corpus that
// would measure it does not exist yet, so this is *derived* from the
// documented Tier A budgets, not observed:
//
//     parser/normalizer live scratch        1 GiB   (03-...:171)
//   + one Draco primitive decoded set     512 MiB   (03-...:173)
//   + decoded texture work + overhead      ~2 GiB   (allowance, not a doc figure)
//   ------------------------------------------------
//                                           4 GiB
//
// Deliberately a backstop, not the enforcement path: 03-...:158 is explicit
// that "a Job Object kill is a correctness fallback for a budget-check
// defect, not the intended enforcement path" -- the adapters' own in-process
// budgets are the first and cheaper line of defense. Treat this number as
// provisional and re-derive it from a real peak once the perf corpus exists;
// do not cite it as measured.
constexpr uint64_t kImportWorkerCommitLimitBytes = 4ull * 1024 * 1024 * 1024;

// How long to wait for one control message from the worker before giving up
// on it entirely.
//
// A hang backstop, not a latency gate. The design's own timing gates are
// orders of magnitude tighter (A-large: a complete coarse proxy in 5 s,
// .docs/design/09-quality-performance-and-security.md:69), so nothing that
// is merely slow should ever reach this; what it exists to stop is a wedged
// worker stranding its import thread for the life of the process, which is
// what an unbounded ReadControlMessage did before.
constexpr uint32_t kWorkerReplyTimeoutMs = 120'000;

struct ImportSessionRequest {
    // Keeps pinned source/approved sidecars in the same zero-capability worker.
    // The callback returns at most one region id; zero means no work this tick.
    std::function<uint32_t()> nextDetail;
    std::function<void(const model_core::FileIdentity&)> onInitialComplete;
    std::function<bool(uint64_t)> cpuBudgetAllows; // worker private bytes; trusted product policy
    bool enableCoarseProxy = false;
    // Trusted handle-derived identity, available before preview/coarse handoff.
    std::function<void(const model_core::FileIdentity&)> onSourceOpened;
    std::wstring workerExePath;
    // Empty keeps the USD fast path test-only and returns the exact
    // UnsupportedComposition classification. A non-empty path enables the
    // USD-006 atomic fallback: only that classification can lazily launch the
    // separately sandboxed compatibility host for this same generation.
    std::wstring compatibilityHostExePath;
    // Test-only compatibility-host pool mode. Production leaves this empty
    // and the manager forces --pool.
    std::wstring compatibilityHostArgumentsOverride;
    // Dedicated STEP host executable. Like the USD compatibility host, it is
    // a private AppContainer payload in its own directory; the general worker
    // and this host never share an identity.
    std::wstring stepHostExePath;
    // Test-only STEP-host pool mode. Production leaves this empty and the
    // manager forces --pool.
    std::wstring stepHostArgumentsOverride;
    // STEP-005 test-only seam: forces the STEP host's mesher serial so a test
    // can compare parallel and serial normalized output byte-for-byte.
    bool stepForceSerialForTesting = false;
    std::wstring sourcePath;
    ImportFormat format = ImportFormat::Gltf;
    uint64_t generationId = 0;
    uint64_t sectionByteCapacity = 0;
    uint32_t maxChunkCount = 0;
    // Bounds the worker's own RequestSidecarFile loop -- "never trust worker
    // self-restraint." Exceeding it abandons the worker (Job Object
    // kill-on-close) rather than servicing forever.
    uint32_t maxSidecarRequestsPerGeneration = 0;
    uint64_t maxSidecarFileBytes = 0;
    // Job Object private-commit ceiling for this import's worker. Injectable
    // rather than hardcoded so a test can prove enforcement end-to-end at a
    // small, fast cap -- the same seam DxgiBudgetMonitor's QueryFn already
    // established for budget policy.
    uint64_t commitLimitBytes = kImportWorkerCommitLimitBytes;
    // Zero derives the design limit at launch: min(4 GiB, 35% of visible
    // physical memory). Non-zero is a qualification seam for Job-limit tests.
    uint64_t compatibilityHostCommitLimitBytes = 0;
    // Dedicated STEP host Job commit ceiling. Zero derives the same bounded
    // design limit as the compatibility host; STEP-001 measured peak commit
    // far below it for the accepted corpus and STEP-004 may lower it.
    uint64_t stepHostCommitLimitBytes = 0;
    uint32_t replyTimeoutMs = kWorkerReplyTimeoutMs;
    // Polled while waiting on the worker. Return true to signal the request's
    // duplicated cancellation event. The worker acknowledges cooperatively;
    // a missed 500 ms grace terminates/replaces that pool slot. Must be cheap and
    // non-blocking. Empty means "never cancelled".
    //
    std::function<bool()> isCancelled;
    // Bounds how many times one generation may fill and hand over the output
    // window, the same "never trust worker self-restraint" rule
    // maxSidecarRequestsPerGeneration already applies to sidecars. Without it
    // a worker could emit batches forever and hold its import thread for the
    // life of the process. Exceeding it abandons the worker (Job Object
    // kill-on-close) rather than servicing another batch.
    //
    // Counts the terminal ChunksReady batch too, so 1 means "single-window
    // imports only" -- exactly the behavior before progressive delivery
    // existed.
    uint32_t maxChunkBatchesPerGeneration = 1;
    // Caps chunks across the WHOLE generation, where maxChunkCount caps one
    // section. Two separate limits because they bound two different things:
    // maxChunkCount bounds the descriptor table the host allocates from a
    // single worker-declared count, while this bounds the cross-batch
    // catalog, which would otherwise grow by up to maxChunkCount per batch
    // for as many batches as the batch cap allows.
    //
    // 0 means "derive it": maxChunkCount * maxChunkBatchesPerGeneration,
    // which is the loosest value that constrains nothing a caller did not
    // already allow.
    uint32_t maxChunksPerGeneration = 0;
    // Called once per validated batch, in delivery order, before the import
    // completes. Chunks passed here are already copied into host-owned
    // memory, so the callee may keep them.
    //
    // Optional: when empty, every batch accumulates into
    // ImportSessionResult::chunks instead and the caller sees exactly the
    // single-result shape it saw before, whatever the batch count. When
    // supplied, batches are handed over as they arrive and are NOT also
    // accumulated -- a progressive caller would otherwise hold the whole
    // model in host memory as well as on the GPU, which is the thing
    // progressive delivery exists to avoid.
    //
    // Runs on the import thread inside the worker's reply loop, between a
    // batch's validation and its ack. Keep it short: the worker is blocked
    // waiting for that ack.
    std::function<void(std::vector<ValidatedChunk>&&)> onBatch;
    // STEP-005: receives each bounded StepProgressNotice the dedicated STEP
    // host publishes while a generation is in progress, in order. It is
    // optional; when empty the events are still counted and the last one is
    // recorded on ImportSessionResult so qualification can assert progress
    // without a callback. Never invoked for any other producer.
    std::function<void(const model_core::StepProgressNotice&)> onStepProgress;
    // Bounds how many StepProgress messages one STEP generation may send, the
    // same "never trust worker self-restraint" rule the sidecar and batch caps
    // apply. Exceeding it abandons the host rather than servicing a flood.
    uint32_t maxStepProgressPerGeneration = 8192;
    // Replaces the format's own worker CLI flag when non-empty.
    //
    // A test seam, and the same kind commitLimitBytes already is: the reply
    // loop below is the thing progressive delivery's rules live in, it is
    // only reachable through RunImportSession, and proving it rejects a
    // misbehaving worker means launching a deliberately misbehaving worker
    // *through this function* -- which needs to select that worker's attack
    // mode. Production callers leave it empty and get ParseFlagFor's mapping.
    std::wstring workerArgumentsOverride;
    // Product imports use the prelaunched fixed pool. Tests that select an
    // explicit attack-mode executable argument remain one-shot so each test
    // keeps its isolated command-line contract.
    bool useWorkerPool = false;
    // FBX qualification seam: leaves load limits unchanged but constrains both
    // ufbx evaluation allocators to 1 KiB, proving typed exhaustion/recovery.
    bool fbxTinyEvaluationLimitForTesting = false;
    // FBX qualification seam: constrains the aggregate decoded texture budget
    // so pressure and same-worker recovery are practical to exercise.
    bool fbxTinyTextureLimitForTesting = false;
};

struct SourceChunkRange
{
    uint64_t generationId = 0;
    model_core::ChunkDescriptor descriptor{};
};

struct ImportSessionResult {
    bool ok = false;
    ImportStage stage = ImportStage::Completed;
    // Closed failure code for every unsuccessful stage, including host faults.
    model_core::ImportErrorCode errorCode = model_core::ImportErrorCode::None;
    model_core::ImportFailurePhase errorPhase = model_core::ImportFailurePhase::Unspecified;
    // Meaningful for stage == OpenSource only: OpenAndCanonicalizeSourceFile's
    // own message, which is more specific than anything this layer could say.
    std::wstring openError;
    // Every batch's chunks concatenated in delivery order. Empty when the
    // caller supplied ImportSessionRequest::onBatch, which takes ownership of
    // each batch as it arrives instead.
    std::vector<ValidatedChunk> chunks;
    // How many batches the generation actually took, terminal one included.
    // 1 for every import that fits in a single window. Meaningful on failure
    // too: it says how far a rejected generation got.
    uint32_t batchCount = 0;
    // Geometry provenance only; no normalized payload or source bytes.
    std::vector<SourceChunkRange> sourceCatalog;
    model_core::FileIdentity sourceIdentity{};
    // Test/qualification evidence only; no handle authority crosses this
    // boundary. Sequential pooled imports can prove reuse by stable PID.
    uint32_t workerProcessId = 0;
    // Closed producer identity for USD atomic-fallback and STEP containment
    // evidence. Non-USD/STEP successful imports use FastWorker. A failed
    // compatibility or STEP attempt also reports its host producer so callers
    // never mistake a discarded fast candidate for the result owner.
    ImportProducer producer = ImportProducer::None;
    // STEP-005 evidence: how many bounded progress events the host sent and
    // the last one received. Zero/empty for every non-STEP producer.
    uint32_t stepProgressCount = 0;
    model_core::StepProgressNotice lastStepProgress{};
    // True only for an exact, first-result UnsupportedComposition from the
    // USD fast worker. USD-006 consumes this without reinterpreting generic
    // parser/resource failures as permission to launch OpenUSD.
    bool compatibilityFallbackRequired = false;
};

// Creates (or opens) the single AppContainer profile every import runs
// under and grants it read+execute on the worker's own directory. Idempotent
// and cheap after the first call: the profile is created once per session
// and then reused, per .docs/design/08-installation-and-registration.md,
// which names it `Binbuf.Preview3D.ImportWorker` and has the installer
// provision it "at or before the first Open in a session".
//
// RunImportSession calls this itself, so it is never required -- call it at
// startup only to keep profile creation off the first open's latency, which
// otherwise lands inside the A-small/A-medium gates in
// .docs/design/09-quality-performance-and-security.md.
bool PrepareImportSandbox(const std::wstring& workerExePath);

// Starts profile provisioning and a two-process pool on a coordinator
// thread. RunImportSession waits only on its own background caller when the
// pool is still starting; the UI never participates in this handshake.
void PrepareImportWorkerPoolAsync(const std::wstring& workerExePath,
                                  uint64_t commitLimitBytes = kImportWorkerCommitLimitBytes);
void ShutdownImportWorkerPool();
// Idempotent close-path hook. The host normally exits immediately after its
// one current generation; this also tears down an in-flight/lazy manager when
// the viewer's loader lanes have stopped.
void ShutdownCompatibilityHost();

// Synchronous, and blocking for as long as the worker takes to reply. Call
// from a background thread. Never throws for an ordinary import failure --
// every expected fault is reported through ImportSessionResult::stage.
ImportSessionResult RunImportSession(const ImportSessionRequest& request);

} // namespace import_broker
