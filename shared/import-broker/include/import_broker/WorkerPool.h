#pragma once

// A small pool of pre-launched, reused --pool-mode Preview3DImportWorker.exe
// processes -- Gate 2's "worker pool" deliverable. Each pooled worker keeps
// its own persistent control-channel pipe pair (frozen at launch, per
// SandboxLauncher's PROC_THREAD_ATTRIBUTE_HANDLE_LIST) for its whole pool
// lifetime; a NEW request's shared-section handle is duplicated directly
// into the already-running worker process via DuplicateSectionIntoWorker,
// since inherited-handle-list membership can't be extended after launch.
//
// Cancellation is host-side, per .docs/design/06-application-lifecycle-and-ipc.md's
// contract: a caller that has moved on to a newer generation simply stops
// caring about a stale reply (platform::GenerationToken filtering is the
// caller's job, mirroring D3D12UploadRing::DrainCompletedPublications) and,
// if a worker doesn't reply within a bounded grace period, TerminateAndReplace
// hard-kills it via the already-proven Job Object kill-on-close rather than
// attempting an in-process interrupt -- true mid-parse cooperative
// cancellation is out of scope for this prototype (see .docs/PROGRESS.md).
//
// Index-based, synchronous API -- deliberately not a general async task
// queue; a prototype's job is to prove reuse-across-generations and
// stale-reply/timeout semantics, not to be the final scheduler.

#include "import_broker/SandboxLauncher.h"
#include "model_core/ControlChannelIo.h"
#include "model_core/ControlProtocol.h"
#include "platform/AppContainerSid.h"
#include "platform/Win32Handle.h"

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

namespace import_broker {

enum class WaitReplyOutcome { Ready, TimedOut, Eof };

class WorkerPool {
public:
    WorkerPool() = default;
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    WorkerPool(WorkerPool&&) = default;
    WorkerPool& operator=(WorkerPool&&) = default;
    ~WorkerPool();

    // Launches `size` --pool-mode workers up front, each under `sid`'s
    // AppContainer token with `limits`. All-or-nothing: if any launch
    // fails, every worker already launched this call is torn down and the
    // call fails. Takes ownership of `sid` (move-only) -- a caller's
    // SandboxFixture still cleans up the AppContainer profile by name in
    // its own destructor regardless of whether its `sid` member was moved
    // from.
    bool Initialize(std::wstring exePath, platform::AppContainerSid sid, SandboxLimits limits,
                     size_t size, std::wstring& error);

    size_t Size() const noexcept;
    size_t IdleCount() const noexcept;

    // Returns the index of an idle worker, or nullopt if every worker is
    // currently busy (a prototype's fixed-size pool does not grow on
    // demand).
    std::optional<size_t> AcquireIdle();

    // Marks a worker Idle again. Callers must have already drained its
    // reply (via WaitForReply) or discarded it (TerminateAndReplace)
    // before releasing it.
    void Release(size_t index);

    // Duplicates `sourceSectionHandle` (owned by this process) into the
    // pooled worker at `index`'s process. Returns the handle's numeric
    // value as valid in THAT worker's own handle table -- not usable
    // directly by the caller, only for embedding into a request payload's
    // ...HandleValue field, exactly like the existing single-shot launch
    // path already does with launch-time-inherited handles.
    std::optional<uint64_t> DuplicateSectionIntoWorker(size_t index, HANDLE sourceSectionHandle);

    bool SendRequest(size_t index, model_core::ControlOpcode opcode, const void* payload,
                      uint32_t payloadSize);

    // Bounded wait for a reply on worker `index`'s notice pipe. Never
    // blocks past timeoutMs -- polls via PeekNamedPipe rather than an
    // unbounded ReadControlMessage, so a superseded/hung worker's caller
    // can enforce a grace period without ControlChannelIo itself needing
    // any timeout concept.
    WaitReplyOutcome WaitForReply(size_t index, DWORD timeoutMs,
                                   model_core::ReceivedControlMessage& outMessage);

    // A worker that missed its grace period: drops its Job Object handle
    // (kill-on-close terminates it and everything in its job) and launches
    // a replacement in its place so pool size never shrinks. Returns false
    // (pool left short by one) only if the replacement launch itself
    // fails.
    bool TerminateAndReplace(size_t index, std::wstring& error);

    // Sends Shutdown to every worker and waits (bounded, 2s per worker) for
    // clean exit; any that don't exit in time are left to their Job
    // Object's kill-on-close when this object is destroyed. Idempotent.
    void Shutdown();

    HANDLE ProcessHandle(size_t index) const noexcept;
    DWORD ProcessId(size_t index) const noexcept;

private:
    struct PooledWorker {
        SandboxProcess proc;
        platform::Win32Handle controlInWrite; // host writes requests here
        platform::Win32Handle controlOutRead; // host reads replies here
        bool busy = false;
    };

    std::optional<PooledWorker> LaunchOne(std::wstring& error) const;

    std::wstring exePath_;
    platform::AppContainerSid sid_;
    SandboxLimits limits_{};
    std::vector<PooledWorker> workers_;
    bool shutdownCalled_ = false;
};

} // namespace import_broker
