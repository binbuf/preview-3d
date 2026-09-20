#define NOMINMAX

#include "StepSpikeProtocol.h"

#include "import_broker/SandboxLauncher.h"
#include "import_broker/SharedSection.h"
#include "platform/AppContainerSid.h"
#include "platform/MappedView.h"
#include "platform/Win32Handle.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

void Check(bool condition, const char* what)
{
    if (!condition) {
        std::printf("  FAIL: %s\n", what);
        ++gFailures;
    } else {
        std::printf("  ok: %s\n", what);
    }
}

struct HostProfile {
    std::wstring name;
    platform::AppContainerSid sid;
    std::vector<std::byte> sidCopy;
    std::wstring payload;

    HostProfile(const std::wstring& payloadDirectory) : payload(payloadDirectory)
    {
        name = L"Binbuf.Preview3D.StepHost.Spike-" + std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count());
        sid = platform::AppContainerSid::CreateOrOpen(name, L"Preview3D Step Host Spike",
                                                      L"STEP-001 zero-capability step host");
        sidCopy.resize(GetLengthSid(sid.get()));
        CopySid(static_cast<DWORD>(sidCopy.size()), sidCopy.data(), sid.get());
        const bool granted = platform::GrantDirectoryReadExecute(payload, sid.get());
        std::printf("  [profile] grant payload read/execute: %s\n", granted ? "ok" : "failed");
    }

    ~HostProfile()
    {
        platform::RevokeDirectoryAccess(payload, sidCopy.data());
        platform::AppContainerSid::Delete(name);
    }
};

struct Packed {
    platform::Win32Handle section;
    platform::MappedView view;
    step_host::StepSpikeSection* header{};
    std::size_t size{};
};

Packed Pack(std::uint64_t sourceHandle, std::uint64_t cancellationHandle)
{
    const std::size_t size = sizeof(step_host::StepSpikeSection);
    Packed packed;
    packed.section = import_broker::CreateSharedSection(size);
    packed.view = platform::MappedView::Map(packed.section.get(), FILE_MAP_READ | FILE_MAP_WRITE, size);
    packed.size = size;
    std::memset(packed.view.bytes().data(), 0, size);
    packed.header = reinterpret_cast<step_host::StepSpikeSection*>(packed.view.bytes().data());
    *packed.header = step_host::StepSpikeSection{};
    packed.header->sectionByteLength = size;
    packed.header->sourceFileHandleValue = sourceHandle;
    packed.header->cancellationEventHandleValue = cancellationHandle;
    return packed;
}

struct Launch {
    import_broker::SandboxProcess process;
    platform::Win32Handle request;
    platform::Win32Handle response;
};

Launch Start(const HostProfile& profile, const Packed& packed, const std::wstring& executable,
             std::size_t memoryLimit = 0)
{
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE inReadRaw{}, inWriteRaw{}, outReadRaw{}, outWriteRaw{};
    CreatePipe(&inReadRaw, &inWriteRaw, &attributes, 0);
    CreatePipe(&outReadRaw, &outWriteRaw, &attributes, 0);
    platform::Win32Handle inRead(inReadRaw), inWrite(inWriteRaw), outRead(outReadRaw), outWrite(outWriteRaw);
    SetHandleInformation(inWrite.get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead.get(), HANDLE_FLAG_INHERIT, 0);

    std::vector<HANDLE> inherited{inRead.get(), outWrite.get(), packed.section.get(),
                                  reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(
                                      packed.header->sourceFileHandleValue)),
                                  reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(
                                      packed.header->cancellationEventHandleValue))};
    const std::wstring arguments = L"\"" + executable + L"\" --step-001-spike "
        + std::to_wstring(reinterpret_cast<std::uintptr_t>(packed.section.get())) + L" "
        + std::to_wstring(packed.size);
    import_broker::SandboxLimits limits{};
    limits.processMemoryLimitBytes = memoryLimit;
    auto process = import_broker::LaunchSuspendedSandboxed(executable, arguments, inherited,
        outWrite.get(), limits, profile.sid, inRead.get());
    if (!process) {
        std::printf("  [launch] CreateProcess/assign failed, GetLastError=%lu\n", GetLastError());
        return {};
    }
    import_broker::ResumeSandboxProcess(*process);
    inRead.reset();
    outWrite.reset();
    step_host::StepSpikeControl control{};
    DWORD written = 0;
    WriteFile(inWrite.get(), &control, sizeof(control), &written, nullptr);
    return {std::move(*process), std::move(inWrite), std::move(outRead)};
}

bool WaitForReply(Launch& launch, DWORD timeoutMilliseconds)
{
    const auto deadline = GetTickCount64() + timeoutMilliseconds;
    while (GetTickCount64() < deadline) {
        DWORD available = 0;
        if (PeekNamedPipe(launch.response.get(), nullptr, 0, nullptr, &available, nullptr)
            && available >= sizeof(step_host::StepSpikeControl)) {
            step_host::StepSpikeControl response{};
            DWORD read = 0;
            return ReadFile(launch.response.get(), &response, sizeof(response), &read, nullptr)
                && read == sizeof(response) && response.magic == step_host::kStepSpikeMagic;
        }
        if (WaitForSingleObject(launch.process.process.get(), 0) == WAIT_OBJECT_0) return false;
        Sleep(10);
    }
    return false;
}

std::string RunAuthorityProbe(const HostProfile& profile, const std::wstring& executable,
                              const std::wstring& argument, bool& launched)
{
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE inReadRaw{}, inWriteRaw{}, outReadRaw{}, outWriteRaw{};
    CreatePipe(&inReadRaw, &inWriteRaw, &attributes, 0);
    CreatePipe(&outReadRaw, &outWriteRaw, &attributes, 0);
    platform::Win32Handle inRead(inReadRaw), inWrite(inWriteRaw), outRead(outReadRaw), outWrite(outWriteRaw);
    SetHandleInformation(inWrite.get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead.get(), HANDLE_FLAG_INHERIT, 0);
    std::vector<HANDLE> inherited{inRead.get(), outWrite.get()};
    const std::wstring arguments = L"\"" + executable + L"\" \"" + argument + L"\"";
    import_broker::SandboxLimits limits{};
    auto process = import_broker::LaunchSuspendedSandboxed(executable, arguments, inherited,
        outWrite.get(), limits, profile.sid, inRead.get());
    if (!process) { launched = false; return {}; }
    launched = true;
    import_broker::ResumeSandboxProcess(*process);
    inRead.reset();
    outWrite.reset();
    WaitForSingleObject(process->process.get(), 15'000);
    std::string output;
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(outRead.get(), nullptr, 0, nullptr, &available, nullptr) || !available)
            break;
        char buffer[256]{};
        DWORD read = 0;
        if (!ReadFile(outRead.get(), buffer, sizeof(buffer), &read, nullptr) || !read) break;
        output.append(buffer, read);
    }
    TerminateJobObject(process->job.get(), 0);
    return output;
}

struct SourceFile {
    platform::Win32Handle handle;
    explicit SourceFile(const std::filesystem::path& path)
    {
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
        handle.reset(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, &attributes,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    }
};

platform::Win32Handle MakeInheritableEvent(bool signalled)
{
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    return platform::Win32Handle(
        CreateEventW(&attributes, TRUE, signalled ? TRUE : FALSE, nullptr));
}

std::wstring ExecutableDirectory()
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return std::filesystem::path(path).parent_path().wstring();
}

void PrintMetrics(const step_host::StepSpikeSection& header)
{
    std::printf("    status=%u roots=%u asm=%u shapes=%u defs=%u instances=%u depth=%u faces=%u "
                "colors=%u transparent=%u tris=%llu mpu=%.9g unit=%s total_us=%llu private=%llu "
                "msg=%s\n",
        static_cast<unsigned>(header.status), header.rootLabelCount, header.assemblyLabelCount,
        header.simpleShapeLabelCount, header.uniqueDefinitionCount, header.instanceCount,
        header.maxObservedDepth, header.faceCount, header.colorCount, header.transparentColorCount,
        static_cast<unsigned long long>(header.triangleCount), header.metersPerUnit,
        header.authoredLengthUnit, static_cast<unsigned long long>(header.totalMicroseconds),
        static_cast<unsigned long long>(header.privateBytes), header.diagnostic);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3) {
        std::printf("usage: StepSpikeSandboxHarness <payloadDir> <fixtureDir>\n");
        return 1;
    }
    const std::wstring payload = argv[1];
    const std::filesystem::path fixtures = argv[2];
    const std::wstring executable = (std::filesystem::path(payload) / L"Preview3DStepSpike.exe").wstring();

    HostProfile profile(payload);

    std::printf("[1] valid self-contained AP214 assembly through inherited handle only\n");
    {
        SourceFile source(fixtures / L"assembly_ap214.stp");
        Check(source.handle.get() != INVALID_HANDLE_VALUE, "source handle opened read-only");
        auto cancellation = MakeInheritableEvent(false);
        auto packed = Pack(reinterpret_cast<std::uintptr_t>(source.handle.get()),
                           reinterpret_cast<std::uintptr_t>(cancellation.get()));
        auto launch = Start(profile, packed, executable);
        Check(static_cast<bool>(launch.process.process), "sandboxed host launched");
        if (launch.process.process) {
            Check(WaitForReply(launch, 30'000), "host replied within budget");
            PrintMetrics(*packed.header);
            Check(packed.header->status == step_host::StepSpikeStatus::Success, "status success");
            Check(packed.header->instanceCount == 5, "five assembly instances");
            Check(packed.header->uniqueDefinitionCount == 2, "two reused definitions");
            Check(packed.header->triangleCount > 0, "tessellated triangles emitted");
            Check(packed.header->transparentColorCount > 0, "transparency preserved");
            TerminateJobObject(launch.process.job.get(), 0);
        }
    }

    std::printf("[2] malformed non-Part-21 bytes are rejected before OCCT transfer\n");
    {
        const auto bogus = std::filesystem::temp_directory_path() / L"step001-bogus.stp";
        { std::FILE* file = nullptr; _wfopen_s(&file, bogus.c_str(), L"wb");
          if (file) { std::fputs("not a step file at all", file); std::fclose(file); } }
        SourceFile source(bogus);
        auto cancellation = MakeInheritableEvent(false);
        auto packed = Pack(reinterpret_cast<std::uintptr_t>(source.handle.get()),
                           reinterpret_cast<std::uintptr_t>(cancellation.get()));
        auto launch = Start(profile, packed, executable);
        if (launch.process.process) {
            Check(WaitForReply(launch, 30'000), "host replied");
            PrintMetrics(*packed.header);
            Check(packed.header->status == step_host::StepSpikeStatus::PreflightFailure,
                  "preflight rejected malformed bytes");
            TerminateJobObject(launch.process.job.get(), 0);
        }
        std::error_code error; std::filesystem::remove(bogus, error);
    }

    std::printf("[3] pre-signalled cancellation is observed without OCCT transfer\n");
    {
        SourceFile source(fixtures / L"part_ap214.stp");
        auto cancellation = MakeInheritableEvent(true);
        auto packed = Pack(reinterpret_cast<std::uintptr_t>(source.handle.get()),
                           reinterpret_cast<std::uintptr_t>(cancellation.get()));
        auto launch = Start(profile, packed, executable);
        if (launch.process.process) {
            Check(WaitForReply(launch, 30'000), "host replied");
            PrintMetrics(*packed.header);
            Check(packed.header->status == step_host::StepSpikeStatus::Cancelled, "status cancelled");
            TerminateJobObject(launch.process.job.get(), 0);
        }
    }

    std::printf("[4] Job commit ceiling terminates the host; a later valid import recovers\n");
    {
        SourceFile source(fixtures / L"assembly_ap214.stp");
        auto cancellation = MakeInheritableEvent(false);
        auto packed = Pack(reinterpret_cast<std::uintptr_t>(source.handle.get()),
                           reinterpret_cast<std::uintptr_t>(cancellation.get()));
        auto launch = Start(profile, packed, executable, 4ull * 1024 * 1024);
        if (launch.process.process) {
            const bool replied = WaitForReply(launch, 15'000);
            std::printf("    constrained host replied=%d\n", replied ? 1 : 0);
            Check(!replied || packed.header->status != step_host::StepSpikeStatus::Success,
                  "Job ceiling prevented a false success");
            TerminateJobObject(launch.process.job.get(), 0);
            WaitForSingleObject(launch.process.process.get(), 5'000);
        }
        SourceFile recovered(fixtures / L"part_ap203.stp");
        auto cancellation2 = MakeInheritableEvent(false);
        auto packed2 = Pack(reinterpret_cast<std::uintptr_t>(recovered.handle.get()),
                            reinterpret_cast<std::uintptr_t>(cancellation2.get()));
        auto launch2 = Start(profile, packed2, executable);
        if (launch2.process.process) {
            Check(WaitForReply(launch2, 30'000), "replacement host replied");
            PrintMetrics(*packed2.header);
            Check(packed2.header->status == step_host::StepSpikeStatus::Success,
                  "next valid import succeeds");
            TerminateJobObject(launch2.process.job.get(), 0);
        }
    }

    std::printf("[5] authority probe: path/network/child-process denial\n");
    {
        const std::wstring probe = (std::filesystem::path(payload) / L"StepAuthorityProbe.exe").wstring();
        bool launched = false;
        const std::string output = RunAuthorityProbe(
            profile, probe, (fixtures / L"part_ap214.stp").wstring(), launched);
        Check(launched, "authority probe launched under AppContainer");
        std::printf("    %s", output.c_str());
        Check(output.find("file_denied=1") != std::string::npos, "direct filesystem open denied");
        Check(output.find("network_denied=1") != std::string::npos, "network connect denied");
        Check(output.find("process_denied=1") != std::string::npos, "child process denied");
    }

    std::printf("%s (%d failures)\n", gFailures == 0 ? "PASS" : "FAIL", gFailures);
    return gFailures == 0 ? 0 : 2;
}
