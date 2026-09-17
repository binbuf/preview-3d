#include "import_broker/WorkerPool.h"

#include "import_broker/ControlChannelWait.h"

#include <chrono>
#include <utility>

namespace import_broker {

WorkerPool::~WorkerPool()
{
    Shutdown();
}

std::optional<WorkerPool::PooledWorker> WorkerPool::LaunchOne(std::wstring& error) const
{
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE inReadRaw = nullptr;
    HANDLE inWriteRaw = nullptr;
    if (!CreatePipe(&inReadRaw, &inWriteRaw, &sa, 0)) {
        error = L"Failed to create the control-in pipe.";
        return std::nullopt;
    }
    platform::Win32Handle controlInRead(inReadRaw);
    platform::Win32Handle controlInWrite(inWriteRaw);
    SetHandleInformation(controlInWrite.get(), HANDLE_FLAG_INHERIT, 0);

    HANDLE outReadRaw = nullptr;
    HANDLE outWriteRaw = nullptr;
    if (!CreatePipe(&outReadRaw, &outWriteRaw, &sa, 0)) {
        error = L"Failed to create the control-out pipe.";
        return std::nullopt;
    }
    platform::Win32Handle controlOutRead(outReadRaw);
    platform::Win32Handle controlOutWrite(outWriteRaw);
    SetHandleInformation(controlOutRead.get(), HANDLE_FLAG_INHERIT, 0);

    std::vector<HANDLE> inherited{ controlInRead.get(), controlOutWrite.get() };
    std::wstring cmdLine = L"\"" + exePath_ + L"\" " + workerArguments_;

    const PSID sid = borrowedSid_ ? borrowedSid_ : sid_.get();
    if (!sid) {
        error = L"The pooled worker AppContainer SID is unavailable.";
        return std::nullopt;
    }
    // SandboxLauncher only observes the SID during CreateProcess. A tiny
    // borrowed wrapper would double-free it, so use the explicit SID launch
    // overload supplied for process-wide pools.
    auto proc = LaunchSuspendedSandboxedWithSid(exePath_, cmdLine, inherited, controlOutWrite.get(), limits_,
                                                 sid, controlInRead.get());
    if (!proc) {
        error = L"Failed to launch a pooled worker process.";
        return std::nullopt;
    }
    if (!ResumeSandboxProcess(*proc)) {
        error = L"Failed to resume a pooled worker process.";
        return std::nullopt;
    }

    // The parent no longer needs its own copies of the ends it handed to
    // the child.
    controlInRead.reset();
    controlOutWrite.reset();

    PooledWorker worker;
    worker.proc = std::move(*proc);
    worker.controlInWrite = std::move(controlInWrite);
    worker.controlOutRead = std::move(controlOutRead);
    worker.busy = false;
    return worker;
}

bool WorkerPool::Initialize(std::wstring exePath, platform::AppContainerSid sid, SandboxLimits limits,
                             size_t size, std::wstring& error)
{
    exePath_ = std::move(exePath);
    workerArguments_ = L"--pool";
    sid_ = std::move(sid);
    borrowedSid_ = nullptr;
    limits_ = limits;

    workers_.reserve(size);
    for (size_t i = 0; i < size; ++i) {
        auto worker = LaunchOne(error);
        if (!worker) {
            workers_.clear(); // drops every already-launched worker's Job Object handle -- kill-on-close
            return false;
        }
        workers_.push_back(std::move(*worker));
    }
    return true;
}

bool WorkerPool::InitializeBorrowed(std::wstring exePath, PSID sid, SandboxLimits limits,
                                    size_t size, std::wstring& error)
{
    exePath_ = std::move(exePath);
    workerArguments_ = L"--pool";
    borrowedSid_ = sid;
    limits_ = limits;
    workers_.reserve(size);
    for (size_t i = 0; i < size; ++i) {
        auto worker = LaunchOne(error);
        if (!worker) {
            workers_.clear();
            return false;
        }
        workers_.push_back(std::move(*worker));
    }
    return true;
}

bool WorkerPool::InitializeForTesting(std::wstring exePath, platform::AppContainerSid sid,
                                      SandboxLimits limits, size_t size,
                                      std::wstring workerArguments, std::wstring& error)
{
    if (workerArguments.empty()) {
        error = L"The test worker arguments are empty.";
        return false;
    }
    exePath_ = std::move(exePath);
    workerArguments_ = std::move(workerArguments);
    sid_ = std::move(sid);
    borrowedSid_ = nullptr;
    limits_ = limits;
    workers_.reserve(size);
    for (size_t index = 0; index < size; ++index) {
        auto worker = LaunchOne(error);
        if (!worker) {
            workers_.clear();
            return false;
        }
        workers_.push_back(std::move(*worker));
    }
    return true;
}

size_t WorkerPool::Size() const noexcept
{
    return workers_.size();
}

size_t WorkerPool::IdleCount() const noexcept
{
    size_t count = 0;
    for (const auto& worker : workers_) {
        if (!worker.busy) {
            ++count;
        }
    }
    return count;
}

std::optional<size_t> WorkerPool::AcquireIdle()
{
    for (size_t i = 0; i < workers_.size(); ++i) {
        if (!workers_[i].busy) {
            workers_[i].busy = true;
            return i;
        }
    }
    return std::nullopt;
}

void WorkerPool::Release(size_t index)
{
    workers_[index].busy = false;
}

std::optional<uint64_t> WorkerPool::DuplicateSectionIntoWorker(size_t index, HANDLE sourceSectionHandle)
{
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), sourceSectionHandle, workers_[index].proc.process.get(),
                          &duplicate, 0, /*bInheritHandle=*/FALSE, DUPLICATE_SAME_ACCESS)) {
        return std::nullopt;
    }
    // `duplicate`'s value is meaningful only in the target worker's handle
    // table (DuplicateHandle's documented cross-process behavior) -- this
    // process never uses it directly, only communicates its numeric value.
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(duplicate));
}

bool WorkerPool::SendRequest(size_t index, model_core::ControlOpcode opcode, const void* payload,
                              uint32_t payloadSize)
{
    return model_core::WriteControlMessage(workers_[index].controlInWrite.get(), opcode, payload,
                                            payloadSize);
}

WaitReplyOutcome WorkerPool::WaitForReply(size_t index, DWORD timeoutMs,
                                           model_core::ReceivedControlMessage& outMessage)
{
    // The poll loop this used to open-code now lives in ControlChannelWait.h,
    // shared with the product's one-shot import path. Behaviour is the same
    // except that the deadline is now honest (steady_clock, not accumulated
    // Sleep intervals) and covers the whole message rather than just its
    // first byte.
    switch (ReadControlMessageBounded(workers_[index].controlOutRead.get(),
                                       std::chrono::milliseconds(timeoutMs), outMessage)) {
    case ControlWaitOutcome::Ready:
        return WaitReplyOutcome::Ready;
    case ControlWaitOutcome::TimedOut:
        return WaitReplyOutcome::TimedOut;
    case ControlWaitOutcome::Eof:
    default:
        return WaitReplyOutcome::Eof;
    }
}

bool WorkerPool::TerminateAndReplace(size_t index, std::wstring& error)
{
    // Dropping the Job Object handle is the kill: JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    // (set by SandboxLauncher::CreateConfiguredJob) terminates the worker
    // and everything in its job the moment this process's last handle to
    // it closes -- the same mechanism SandboxLaunchTests.cpp's "Job Object
    // kill-on-close terminates a hung worker" already proves, reused here
    // rather than re-derived.
    workers_[index].proc.job.reset();
    workers_[index].proc.process.reset();
    workers_[index].proc.thread.reset();
    workers_[index].controlInWrite.reset();
    workers_[index].controlOutRead.reset();

    auto replacement = LaunchOne(error);
    if (!replacement) {
        return false;
    }
    workers_[index] = std::move(*replacement);
    return true;
}

void WorkerPool::Shutdown()
{
    if (shutdownCalled_) {
        return;
    }
    shutdownCalled_ = true;

    for (auto& worker : workers_) {
        if (worker.controlInWrite) {
            model_core::WriteControlMessage(worker.controlInWrite.get(), model_core::ControlOpcode::Shutdown,
                                             nullptr, 0);
        }
    }
    for (auto& worker : workers_) {
        if (worker.proc.process) {
            WaitForSingleObject(worker.proc.process.get(), 2000); // bounded; the Job Object dtor is the hard backstop
        }
    }
}

HANDLE WorkerPool::ProcessHandle(size_t index) const noexcept
{
    return workers_[index].proc.process.get();
}

HANDLE WorkerPool::JobHandle(size_t index) const noexcept
{
    return workers_[index].proc.job.get();
}

HANDLE WorkerPool::ControlInput(size_t index) const noexcept
{
    return workers_[index].controlInWrite.get();
}

HANDLE WorkerPool::ControlOutput(size_t index) const noexcept
{
    return workers_[index].controlOutRead.get();
}

DWORD WorkerPool::ProcessId(size_t index) const noexcept
{
    return GetProcessId(workers_[index].proc.process.get());
}

} // namespace import_broker
