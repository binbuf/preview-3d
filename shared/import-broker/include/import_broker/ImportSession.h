#pragma once

// One complete host-side sandboxed import: open+canonicalize a real on-disk
// source, duplicate an inheritable handle for it, create the output shared
// section, launch a one-shot zero-capability AppContainer worker, send the
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
    std::wstring workerExePath;
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
    // Polled while waiting on the worker. Return true to abandon this
    // generation: the wait stops, the worker is killed by its Job Object,
    // and the result comes back with stage == Cancelled. Must be cheap and
    // non-blocking. Empty means "never cancelled".
    //
    // This is host-side abandonment only. The design also wants a
    // cooperative in-parse cancel first (06-application-lifecycle-and-ipc.md
    // :119, "cooperative cancel first; only its Job Object may be terminated
    // after the finite grace period"); that half needs stop-token checkpoints
    // threaded through the adapter loops and is not implemented yet.
    std::function<bool()> isCancelled;
};

struct ImportSessionResult {
    bool ok = false;
    ImportStage stage = ImportStage::Completed;
    // Meaningful for stage == WorkerReportedError (the worker's own typed
    // error) and stage == ValidateSection (the validator's rejection).
    model_core::ImportErrorCode errorCode = model_core::ImportErrorCode::None;
    // Meaningful for stage == OpenSource only: OpenAndCanonicalizeSourceFile's
    // own message, which is more specific than anything this layer could say.
    std::wstring openError;
    std::vector<ValidatedChunk> chunks;
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

// Synchronous, and blocking for as long as the worker takes to reply. Call
// from a background thread. Never throws for an ordinary import failure --
// every expected fault is reported through ImportSessionResult::stage.
ImportSessionResult RunImportSession(const ImportSessionRequest& request);

} // namespace import_broker
