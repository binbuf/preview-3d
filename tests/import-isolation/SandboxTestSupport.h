#pragma once

// Shared test fixtures for launching the real Preview3DImportWorker.exe (or
// the synthetic Preview3DHostileWorker.exe) under a throwaway AppContainer
// profile, used by SandboxLaunchTests.cpp (the AppContainer/Job Object
// launch spike), ImportPipelineTests.cpp (the wire-format/broker-protocol
// data path), and HostileWorkerTests.cpp (the adversarial suite).

#include "platform/AppContainerSid.h"

#include <windows.h>

#include <chrono>
#include <string>
#include <vector>

#ifndef PREVIEW3D_IMPORT_WORKER_EXE
#error "PREVIEW3D_IMPORT_WORKER_EXE must be defined by Tests.ImportIsolation.vcxproj"
#endif

#ifndef PREVIEW3D_HOSTILE_WORKER_EXE
#error "PREVIEW3D_HOSTILE_WORKER_EXE must be defined by Tests.ImportIsolation.vcxproj"
#endif

namespace sandbox_test_support {

inline const wchar_t* WorkerExePath()
{
    return PREVIEW3D_IMPORT_WORKER_EXE;
}

inline const wchar_t* HostileWorkerExePath()
{
    return PREVIEW3D_HOSTILE_WORKER_EXE;
}

inline std::wstring WorkerDirectory()
{
    std::wstring exePath(WorkerExePath());
    auto lastSlash = exePath.find_last_of(L"\\/");
    return (lastSlash == std::wstring::npos) ? std::wstring(L".") : exePath.substr(0, lastSlash);
}

inline std::wstring MakeUniqueContainerName()
{
    auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return L"Preview3DSandboxSpike-" + std::to_wstring(GetCurrentProcessId()) + L"-"
        + std::to_wstring(ticks);
}

// Owns a throwaway AppContainer profile for the lifetime of one test case,
// so repeated runs don't accumulate entries under %LOCALAPPDATA%\Packages.
//
// An AppContainer process is access-checked even to load its own .exe, so
// the profile's SID needs read+execute on the worker's build output
// directory or every launch here fails at loader level with
// ERROR_ACCESS_DENIED before any product logic runs. The grant names this
// fixture's own throwaway SID -- not the machine-wide "ALL APPLICATION
// PACKAGES" groups an earlier icacls shell-out used -- so the ACE is
// revoked alongside the profile rather than widening the directory to every
// AppContainer on the machine for good.
struct SandboxFixture {
    std::wstring containerName;
    platform::AppContainerSid sid;
    // Our own copy of the SID bytes. `sid` itself may be moved out by a test
    // that hands ownership to a launcher (WorkerPoolTests.cpp passes
    // std::move(fixture.sid) to WorkerPool::Initialize), which would leave
    // sid.get() null at revoke time and strand the ACE on the shared build
    // output directory -- measured as exactly 4 leaked SIDs across a full
    // suite run before this copy existed.
    std::vector<BYTE> sidBytes;

    SandboxFixture()
        : containerName(MakeUniqueContainerName())
        , sid(platform::AppContainerSid::CreateOrOpen(
              containerName, L"Preview3D Sandbox Spike",
              L"Test-only AppContainer profile for Gate 2 workstream A"))
    {
        if (sid) {
            sidBytes.resize(GetLengthSid(sid.get()));
            CopySid(static_cast<DWORD>(sidBytes.size()), sidBytes.data(), sid.get());
        }
        platform::GrantDirectoryReadExecute(WorkerDirectory(), sid.get());
    }

    ~SandboxFixture()
    {
        if (!sidBytes.empty()) {
            platform::RevokeDirectoryAccess(WorkerDirectory(), sidBytes.data());
        }
        platform::AppContainerSid::Delete(containerName);
    }
};

} // namespace sandbox_test_support
