// Gate 2 workstream A launch spike: proves the AppContainer + Job Object
// sandbox mechanics (zero-capability token, suspended launch with job
// assignment before resume, restricted handle inheritance, Job Object
// enforcement) against the real Preview3DImportWorker.exe. Does not touch
// the wire format, broker protocol, shared-section validation, or the
// synthetic hostile-worker suite (HostileWorkerTests.cpp, still to come) --
// see .docs/design/10-delivery-plan.md Gate 2 for what is deferred to a
// follow-up plan.

// winsock2.h must be included before the first transitive <windows.h> pull
// (via the project headers below) to avoid the old winsock.h/winsock2.h
// redefinition conflict.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "SandboxTestSupport.h"
#include "import_broker/SandboxLauncher.h"
#include "platform/AppContainerSid.h"
#include "platform/Win32Handle.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {

using sandbox_test_support::SandboxFixture;
using sandbox_test_support::WorkerExePath;

struct LaunchResult {
    import_broker::SandboxProcess proc;
    platform::Win32Handle pipeRead;
};

// Launches the real worker suspended and job-assigned (not resumed).
// includeHandleInList controls whether the pipe write end is placed on
// PROC_THREAD_ATTRIBUTE_HANDLE_LIST -- passing false lets a test prove that
// omitting it is rejected outright.
std::optional<LaunchResult> LaunchWorker(const platform::AppContainerSid& sid,
                                          const std::wstring& args,
                                          const import_broker::SandboxLimits& limits,
                                          bool includeHandleInList = true)
{
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE readRaw = nullptr;
    HANDLE writeRaw = nullptr;
    if (!CreatePipe(&readRaw, &writeRaw, &sa, 0)) {
        return std::nullopt;
    }
    platform::Win32Handle pipeRead(readRaw);
    platform::Win32Handle pipeWrite(writeRaw);

    // The parent's own copy of the read end must never be inherited.
    SetHandleInformation(pipeRead.get(), HANDLE_FLAG_INHERIT, 0);

    std::wstring cmdLine = L"\"" + std::wstring(WorkerExePath()) + L"\" " + args;

    std::vector<HANDLE> inherited;
    if (includeHandleInList) {
        inherited.push_back(pipeWrite.get());
    }

    auto proc = import_broker::LaunchSuspendedSandboxed(WorkerExePath(), cmdLine, inherited,
                                                          pipeWrite.get(), limits, sid);
    if (!proc) {
        return std::nullopt;
    }

    // The parent no longer needs its own copy of the write end -- keeping it
    // open would prevent the read end from ever seeing EOF.
    pipeWrite.reset();

    LaunchResult result;
    result.proc = std::move(*proc);
    result.pipeRead = std::move(pipeRead);
    return result;
}

std::string ReadAllUntilEof(HANDLE pipeRead)
{
    std::string buffer;
    char chunk[256];
    DWORD bytesRead = 0;
    while (ReadFile(pipeRead, chunk, sizeof(chunk), &bytesRead, nullptr) && bytesRead > 0) {
        buffer.append(chunk, bytesRead);
    }
    return buffer;
}

// Polls for up to `timeout` for at least one complete line, without ever
// blocking on EOF -- needed for --hang, which never closes its end of the
// pipe on its own.
std::string ReadLineWithTimeout(HANDLE pipeRead, std::chrono::milliseconds timeout)
{
    std::string buffer;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipeRead, nullptr, 0, nullptr, &available, nullptr)) {
            break;
        }
        if (available > 0) {
            char chunk[256];
            DWORD toRead = (std::min)(available, static_cast<DWORD>(sizeof(chunk)));
            DWORD bytesRead = 0;
            if (!ReadFile(pipeRead, chunk, toRead, &bytesRead, nullptr)) {
                break;
            }
            buffer.append(chunk, bytesRead);
            if (buffer.find('\n') != std::string::npos) {
                return buffer;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    return buffer;
}

struct ProbeResult {
    std::string status;
    DWORD code = 0;
};

std::map<std::string, ProbeResult> ParseProbeResults(const std::string& output)
{
    std::map<std::string, ProbeResult> results;
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string name = line.substr(0, eq);
        std::string rest = line.substr(eq + 1);
        auto codePos = rest.find(" code=");
        if (codePos == std::string::npos) {
            continue;
        }
        ProbeResult result;
        result.status = rest.substr(0, codePos);
        result.code = static_cast<DWORD>(std::strtoul(rest.c_str() + codePos + 6, nullptr, 10));
        results[name] = result;
    }
    return results;
}

} // namespace

TEST_CASE("AppContainer SID derivation produces a valid zero-capability SID", "[sandbox]")
{
    SandboxFixture fixture;
    REQUIRE(fixture.sid.get() != nullptr);
    REQUIRE(IsValidSid(fixture.sid.get()));

    SECURITY_CAPABILITIES caps{};
    caps.AppContainerSid = fixture.sid.get();
    caps.Capabilities = nullptr;
    caps.CapabilityCount = 0;
    CHECK(caps.CapabilityCount == 0);
}

TEST_CASE("LaunchSuspendedSandboxedWithSid fails closed on a null SID", "[sandbox]")
{
    // The security primitive must reject a null SID itself rather than trust
    // every caller to have guarded it; a null AppContainerSid pointer would
    // otherwise fault inside CreateProcessW's attribute construction.
    std::vector<HANDLE> inherited;
    auto proc = import_broker::LaunchSuspendedSandboxedWithSid(
        L"C:\\nonexistent-worker.exe", L"", inherited, nullptr, {}, nullptr, nullptr);
    CHECK_FALSE(proc.has_value());
}

TEST_CASE("Worker is created suspended with job assignment before resume", "[sandbox]")
{
    SandboxFixture fixture;
    import_broker::SandboxLimits limits{};

    auto launch = LaunchWorker(fixture.sid, L"--hang", limits);
    REQUIRE(launch.has_value());

    // SuspendThread on an already-suspended thread returns the PREVIOUS
    // suspend count; 1 confirms exactly one outstanding suspend, i.e. the
    // thread was created suspended and nothing has resumed it yet.
    DWORD previousCount = SuspendThread(launch->proc.thread.get());
    CHECK(previousCount == 1);
    ResumeThread(launch->proc.thread.get()); // undo this probe's own extra suspend

    BOOL inJob = FALSE;
    REQUIRE(IsProcessInJob(launch->proc.process.get(), launch->proc.job.get(), &inJob));
    CHECK(inJob == TRUE);

    REQUIRE(import_broker::ResumeSandboxProcess(launch->proc));
    ReadLineWithTimeout(launch->pipeRead.get(), std::chrono::milliseconds(2000));
    launch->proc.job.reset(); // kill-on-close cleans up the now-hung worker
}

TEST_CASE("Omitting the report handle from the restricted handle list fails process creation",
          "[sandbox]")
{
    SandboxFixture fixture;
    import_broker::SandboxLimits limits{};

    auto launch = LaunchWorker(fixture.sid, L"--hang", limits, /*includeHandleInList=*/false);
    REQUIRE_FALSE(launch.has_value());
}

TEST_CASE("Default probe run denies filesystem, network, and process-spawn access", "[sandbox]")
{
    SandboxFixture fixture;

    wchar_t tempDir[MAX_PATH];
    REQUIRE(GetTempPathW(MAX_PATH, tempDir) != 0);
    wchar_t canaryPath[MAX_PATH];
    REQUIRE(GetTempFileNameW(tempDir, L"p3d", 0, canaryPath) != 0);

    // Control: confirm the parent (a normal, non-AppContainer process) can
    // open the canary itself, so the worker's denial is provably
    // AppContainer-specific rather than a bad path.
    {
        HANDLE control = CreateFileW(canaryPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(control != INVALID_HANDLE_VALUE);
        CloseHandle(control);
    }

    WSADATA wsaData{};
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsaData) == 0);
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(listener != INVALID_SOCKET);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    REQUIRE(bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    REQUIRE(listen(listener, 1) == 0);

    int addrLen = sizeof(addr);
    REQUIRE(getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addrLen) == 0);
    unsigned short port = ntohs(addr.sin_port);

    std::wstring args
        = L"--probes \"" + std::wstring(canaryPath) + L"\" " + std::to_wstring(port);

    import_broker::SandboxLimits limits{};
    auto launch = LaunchWorker(fixture.sid, args, limits);
    REQUIRE(launch.has_value());
    REQUIRE(import_broker::ResumeSandboxProcess(launch->proc));

    std::string output = ReadAllUntilEof(launch->pipeRead.get());
    auto results = ParseProbeResults(output);

    REQUIRE(results.count("FS_PROBE") == 1);
    CHECK(results["FS_PROBE"].status == "DENIED");
    REQUIRE(results.count("NET_PROBE") == 1);
    CHECK(results["NET_PROBE"].status == "DENIED");
    REQUIRE(results.count("SPAWN_PROBE") == 1);
    CHECK(results["SPAWN_PROBE"].status == "DENIED");

    closesocket(listener);
    WSACleanup();
    DeleteFileW(canaryPath);
}

TEST_CASE("Job Object commit limit is enforced against the real worker", "[sandbox]")
{
    SandboxFixture fixture;
    import_broker::SandboxLimits limits{};
    limits.processMemoryLimitBytes = 64ULL * 1024 * 1024; // 64 MiB test-only cap

    auto launch = LaunchWorker(fixture.sid, L"--overallocate", limits);
    REQUIRE(launch.has_value());
    REQUIRE(import_broker::ResumeSandboxProcess(launch->proc));

    std::string output = ReadAllUntilEof(launch->pipeRead.get());
    auto results = ParseProbeResults(output);

    REQUIRE(results.count("COMMIT_PROBE") == 1);
    CHECK(results["COMMIT_PROBE"].status == "DENIED");
    CHECK(results["COMMIT_PROBE"].code == ERROR_COMMITMENT_LIMIT);
}

TEST_CASE("Job Object CPU-time limit terminates a worker that burns CPU", "[sandbox]")
{
    SandboxFixture fixture;
    import_broker::SandboxLimits limits{};
    // 200 ms of cumulative user CPU. The --cpu-spin probe never blocks, so
    // without JOB_OBJECT_LIMIT_PROCESS_TIME it would stay alive and the
    // bounded wait below would time out.
    limits.processCpuTimeLimitMs = 200;

    auto launch = LaunchWorker(fixture.sid, L"--cpu-spin", limits);
    REQUIRE(launch.has_value());
    REQUIRE(import_broker::ResumeSandboxProcess(launch->proc));

    DWORD waitResult = WaitForSingleObject(launch->proc.process.get(), 15000);
    CHECK(waitResult == WAIT_OBJECT_0);
}

TEST_CASE("Job Object kill-on-close terminates a hung worker", "[sandbox]")
{
    SandboxFixture fixture;
    import_broker::SandboxLimits limits{};

    auto launch = LaunchWorker(fixture.sid, L"--hang", limits);
    REQUIRE(launch.has_value());
    REQUIRE(import_broker::ResumeSandboxProcess(launch->proc));

    DWORD stillRunning = WaitForSingleObject(launch->proc.process.get(), 500);
    REQUIRE(stillRunning == WAIT_TIMEOUT);

    launch->proc.job.reset(); // the only handle to this job -- triggers kill-on-close

    DWORD afterClose = WaitForSingleObject(launch->proc.process.get(), 5000);
    CHECK(afterClose == WAIT_OBJECT_0);
}
