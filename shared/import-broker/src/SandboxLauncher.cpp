#include "import_broker/SandboxLauncher.h"

#include "platform/ProcThreadAttributeList.h"

#include <cwchar>
#include <string.h>
#include <string_view>
#include <vector>

namespace import_broker {

namespace {

// Directory containing the child executable. The child is launched with this
// as its working directory so the viewer/broker's current directory (both
// user-controlled) never becomes an implicit loader or relative-path root.
std::wstring PayloadDirectory(const std::wstring& exePath)
{
    const auto slash = exePath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return exePath.substr(0, slash);
}

// Plug-in/loader environment variables every sandboxed child must not inherit.
// This is the union of the two compatibility hosts' explicit scrub lists plus
// the general worker's format libraries; the in-process HardenProcessDiscovery
// repeats it as defense in depth.
bool IsScrubbedVariable(std::wstring_view name)
{
    constexpr std::wstring_view variables[] = {
        L"PREVIEW3D_DISABLED_PLUGIN_PATH", L"PXR_PLUGINPATH_NAME",
        L"PXR_AR_DEFAULT_SEARCH_PATH", L"PYTHONPATH", L"USDIMAGING_ENABLE_PLUGINS",
        L"MATERIALX_SEARCH_PATH", L"RMANTREE", L"RMAN_RIXPLUGINPATH",
        L"CSF_OCCTResourcePath", L"CSF_PluginPath", L"CSF_UnitsLexicon",
        L"CSF_DefaultUnit", L"CSF_UnitsDefinition", L"CSF_IGESDefaults",
        L"CSF_STEPDefaults", L"CSF_XCAFDefaults", L"CSF_DrawPluginPath",
        L"CSF_MDTVTexturesDirectory", L"CSF_ShadersDirectory",
        L"CSF_GraphicShr", L"MMGT_OPT", L"PATH"
    };
    for (const std::wstring_view candidate : variables) {
        if (name.size() == candidate.size()
            && _wcsnicmp(name.data(), candidate.data(), name.size()) == 0) {
            return true;
        }
    }
    return false;
}

// Copies the parent's environment (so SystemRoot/TEMP and the rest of the
// trusted base remain available to OCCT/USD) minus the scrubbed plug-in/loader
// names, and returns a CreateProcessW-ready double-NUL-terminated block.
std::vector<wchar_t> BuildScrubbedEnvironmentBlock()
{
    std::vector<wchar_t> block;
    LPWCH current = GetEnvironmentStringsW();
    if (current == nullptr) {
        block.push_back(L'\0');
        return block;
    }
    for (LPWCH cursor = current; *cursor != L'\0';) {
        const std::wstring entry(cursor);
        cursor += entry.size() + 1;
        const auto equals = entry.find(L'=');
        if (equals != std::wstring::npos && equals > 0
            && IsScrubbedVariable(std::wstring_view(entry).substr(0, equals))) {
            continue;
        }
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    FreeEnvironmentStringsW(current);
    block.push_back(L'\0');
    return block;
}

platform::Win32Handle CreateConfiguredJob(const SandboxLimits& limits)
{
    platform::Win32Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        return {};
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    info.BasicLimitInformation.ActiveProcessLimit = limits.activeProcessLimit;

    if (limits.processMemoryLimitBytes != 0) {
        info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        info.ProcessMemoryLimit = limits.processMemoryLimitBytes;
    }

    if (limits.processCpuTimeLimitMs != 0) {
        // JOBOBJECT_BASIC_LIMIT_INFORMATION::PerProcessUserTimeLimit is in
        // 100 ns units.
        info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_TIME;
        info.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart =
            static_cast<LONGLONG>(limits.processCpuTimeLimitMs) * 10'000;
    }

    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &info,
                                  sizeof(info))) {
        return {};
    }

    return job;
}

} // namespace

std::optional<SandboxProcess> LaunchSuspendedSandboxedWithSid(const std::wstring& exePath,
                                                               std::wstring commandLine,
                                                               std::span<HANDLE> inheritedHandles,
                                                               HANDLE stdOutput,
                                                               const SandboxLimits& limits,
                                                               PSID sid,
                                                               HANDLE stdInput)
{
    // The AppContainer security-capabilities attribute dereferences this SID
    // at CreateProcessW; a null value is a launch-time access violation, so
    // the primitive itself fails closed rather than trusting every caller to
    // have guarded it.
    if (sid == nullptr) {
        return std::nullopt;
    }

    platform::Win32Handle job = CreateConfiguredJob(limits);
    if (!job) {
        return std::nullopt;
    }

    SECURITY_CAPABILITIES caps{};
    caps.AppContainerSid = sid;
    caps.Capabilities = nullptr;
    caps.CapabilityCount = 0;
    caps.Reserved = 0;

    // Apply the process mitigation policy at creation time so no import code
    // ever runs under a less-restricted intermediate state. CFG is forced
    // always-on for the child image, extension points (AppInit_DLLs and the
    // other legacy injection seams) are disabled, and ACG prohibits dynamic
    // code. Microsoft-signed-image enforcement is intentionally absent until
    // the payload is Authenticode-signed (SEC-14); the images and app-local
    // DLLs are unsigned today and that policy would make them unloadable.
    // Every child that is launched through this broker parses untrusted data,
    // and the isolation suite validates this set against each pinned parser.
    DWORD64 childMitigationPolicy =
        PROCESS_CREATION_MITIGATION_POLICY_CONTROL_FLOW_GUARD_ALWAYS_ON
        | PROCESS_CREATION_MITIGATION_POLICY_EXTENSION_POINT_DISABLE_ALWAYS_ON
        | PROCESS_CREATION_MITIGATION_POLICY_PROHIBIT_DYNAMIC_CODE_ALWAYS_ON;

    platform::ProcThreadAttributeList attrList(3);
    if (!attrList.Update(PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &caps, sizeof(caps))) {
        return std::nullopt;
    }
    if (!attrList.Update(PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritedHandles.data(),
                          inheritedHandles.size() * sizeof(HANDLE))) {
        return std::nullopt;
    }
    if (!attrList.Update(PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &childMitigationPolicy,
                          sizeof(childMitigationPolicy))) {
        return std::nullopt;
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = stdOutput;
    si.StartupInfo.hStdInput = stdInput;
    si.StartupInfo.hStdError = nullptr;
    si.lpAttributeList = attrList.get();

    PROCESS_INFORMATION pi{};
    // Every child starts with an explicit environment (parent base minus the
    // plug-in/loader escape hatches) and an explicit working directory (its
    // own payload directory) rather than inheriting the viewer's. The
    // in-process hardening each child performs remains as defense in depth.
    const std::wstring directory = PayloadDirectory(exePath);
    std::vector<wchar_t> environment = BuildScrubbedEnvironmentBlock();
    // Import workers are console-subsystem programs solely to use the inherited
    // control pipes.  They never present console output to the user, so prevent
    // Windows from creating a blank console window for every worker launch.
    BOOL created = CreateProcessW(exePath.c_str(), commandLine.data(), nullptr, nullptr,
                                   /*bInheritHandles=*/TRUE,
                                   CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT
                                       | EXTENDED_STARTUPINFO_PRESENT,
                                   environment.data(), directory.empty() ? nullptr : directory.c_str(),
                                   &si.StartupInfo, &pi);

    if (!created) {
        return std::nullopt;
    }

    SandboxProcess proc;
    proc.process = platform::Win32Handle(pi.hProcess);
    proc.thread = platform::Win32Handle(pi.hThread);

    if (!AssignProcessToJobObject(job.get(), proc.process.get())) {
        TerminateProcess(proc.process.get(), 1);
        return std::nullopt;
    }

    proc.job = std::move(job);
    return proc;
}

std::optional<SandboxProcess> LaunchSuspendedSandboxed(const std::wstring& exePath,
                                                        std::wstring commandLine,
                                                        std::span<HANDLE> inheritedHandles,
                                                        HANDLE stdOutput,
                                                        const SandboxLimits& limits,
                                                        const platform::AppContainerSid& sid,
                                                        HANDLE stdInput)
{
    return LaunchSuspendedSandboxedWithSid(exePath, std::move(commandLine), inheritedHandles,
                                            stdOutput, limits, sid.get(), stdInput);
}

bool ResumeSandboxProcess(SandboxProcess& proc)
{
    if (!proc.thread) {
        return false;
    }
    return ResumeThread(proc.thread.get()) != static_cast<DWORD>(-1);
}

} // namespace import_broker
