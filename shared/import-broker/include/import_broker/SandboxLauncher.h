#pragma once

// Launch mechanics only: AppContainer token + Job Object + restricted
// handle inheritance for Preview3DImportWorker.exe (and, later,
// Preview3DImportHost.exe). The versioned wire format, the broker protocol,
// copy-then-validate chunk acceptance, and the synthetic hostile-worker
// suite are separate, deferred work -- see .docs/design/03-file-formats-and-ingestion.md
// and .docs/design/10-delivery-plan.md (Gate 2).

#include "platform/AppContainerSid.h"
#include "platform/Win32Handle.h"

#include <windows.h>

#include <optional>
#include <span>
#include <string>

namespace import_broker {

struct SandboxLimits {
    SIZE_T processMemoryLimitBytes = 0; // 0 = no Job Object commit limit
    DWORD activeProcessLimit = 1;       // no breakaway: blocks any spawned child
};

struct SandboxProcess {
    platform::Win32Handle process;
    platform::Win32Handle thread;
    platform::Win32Handle job;
};

// Launches exePath suspended under sid's zero-capability AppContainer token,
// assigns the process to a freshly configured Job Object (kill-on-close,
// limits from `limits`), and returns without resuming it. Only the handles
// in inheritedHandles are inherited by the child, via
// PROC_THREAD_ATTRIBUTE_HANDLE_LIST -- stdOutput (and stdInput, when
// non-null) must also appear in inheritedHandles or CreateProcessW fails.
// Job assignment happens inside this call, before the caller can possibly
// resume the thread, so no worker code ever runs with fewer restrictions
// than its final policy (see ADR-014 in .docs/design/11-decisions-and-risks.md).
std::optional<SandboxProcess> LaunchSuspendedSandboxed(const std::wstring& exePath,
                                                        std::wstring commandLine,
                                                        std::span<HANDLE> inheritedHandles,
                                                        HANDLE stdOutput,
                                                        const SandboxLimits& limits,
                                                        const platform::AppContainerSid& sid,
                                                        HANDLE stdInput = nullptr);

// Same launch contract for a SID whose lifetime is owned elsewhere (the
// process-wide worker coordinator). The SID is only consumed synchronously
// while the security capabilities attribute is constructed.
std::optional<SandboxProcess> LaunchSuspendedSandboxedWithSid(const std::wstring& exePath,
                                                               std::wstring commandLine,
                                                               std::span<HANDLE> inheritedHandles,
                                                               HANDLE stdOutput,
                                                               const SandboxLimits& limits,
                                                               PSID sid,
                                                               HANDLE stdInput = nullptr);

// Resumes a process created by LaunchSuspendedSandboxed. Never call this
// before job assignment has happened, which LaunchSuspendedSandboxed already
// guarantees by construction.
bool ResumeSandboxProcess(SandboxProcess& proc);

} // namespace import_broker
