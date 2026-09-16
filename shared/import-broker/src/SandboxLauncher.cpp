#include "import_broker/SandboxLauncher.h"

#include "platform/ProcThreadAttributeList.h"

namespace import_broker {

namespace {

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
    platform::Win32Handle job = CreateConfiguredJob(limits);
    if (!job) {
        return std::nullopt;
    }

    SECURITY_CAPABILITIES caps{};
    caps.AppContainerSid = sid;
    caps.Capabilities = nullptr;
    caps.CapabilityCount = 0;
    caps.Reserved = 0;

    platform::ProcThreadAttributeList attrList(2);
    if (!attrList.Update(PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &caps, sizeof(caps))) {
        return std::nullopt;
    }
    if (!attrList.Update(PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritedHandles.data(),
                          inheritedHandles.size() * sizeof(HANDLE))) {
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
    // Import workers are console-subsystem programs solely to use the inherited
    // control pipes.  They never present console output to the user, so prevent
    // Windows from creating a blank console window for every worker launch.
    BOOL created = CreateProcessW(exePath.c_str(), commandLine.data(), nullptr, nullptr,
                                   /*bInheritHandles=*/TRUE,
                                   CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                   nullptr, &si.StartupInfo, &pi);

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
