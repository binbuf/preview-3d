#include "import_broker/ImportSession.h"

#include "import_broker/ControlChannelWait.h"
#include "import_broker/SandboxLauncher.h"
#include "import_broker/SharedSection.h"
#include "import_broker/SidecarRequestServicer.h"
#include "import_broker/SourceFileAccess.h"
#include "model_core/ControlChannelIo.h"
#include "model_core/ControlProtocol.h"
#include "platform/AppContainerSid.h"
#include "platform/MappedView.h"
#include "platform/Win32Handle.h"

#include <windows.h>

#include <chrono>
#include <cstring>
#include <exception>
#include <string>
#include <variant>

namespace import_broker {

namespace {

// The design names this profile and has the installer provision it; see
// .docs/design/08-installation-and-registration.md ("Import-process
// identities"). Until then it is created on first use and reused for the
// rest of the session.
constexpr const wchar_t* kWorkerContainerName = L"Binbuf.Preview3D.ImportWorker";

std::wstring DirectoryOf(const std::wstring& filePath)
{
    auto lastSlash = filePath.find_last_of(L"\\/");
    return (lastSlash == std::wstring::npos) ? std::wstring(L".") : filePath.substr(0, lastSlash);
}

struct WorkerContainer {
    platform::AppContainerSid sid;
    bool ready = false;
};

// One profile per session, never deleted per import. The previous
// create-a-uniquely-named-profile-then-delete-it-per-import shape did real
// registry and filesystem work inside every open's latency budget, and
// leaked a persistent per-user profile on the two paths that never reach the
// delete: a worker that hangs, and the app closing mid-import.
//
// workerExePath is consumed on the first call only; every import in a
// session resolves the same worker directory, so there is nothing for a
// later call to change.
const WorkerContainer& AcquireWorkerContainer(const std::wstring& workerExePath)
{
    static WorkerContainer container = [&workerExePath] {
        WorkerContainer created;
        try {
            created.sid = platform::AppContainerSid::CreateOrOpen(
                kWorkerContainerName, L"Preview3D Import Worker",
                L"Zero-capability sandbox identity for Preview3DImportWorker.exe");
        } catch (const std::exception&) {
            return created; // ready stays false
        }
        if (!created.sid) {
            return created;
        }
        // Without this the worker cannot load its own .exe, so a failure
        // here is a launch failure, not a warning.
        created.ready = platform::GrantDirectoryReadExecute(DirectoryOf(workerExePath), created.sid.get());
        return created;
    }();
    return container;
}

const wchar_t* ParseFlagFor(ImportFormat format)
{
    switch (format) {
    case ImportFormat::Stl:
        return L"--parse-stl";
    case ImportFormat::Ply:
        return L"--parse-ply";
    case ImportFormat::Gltf:
    default:
        return L"--parse-gltf";
    }
}

// Every real-file request struct (ParseGltfFileRequest/ParseStlFileRequest/
// ParsePlyFileRequest) is field-for-field identical -- generationId,
// sourceFileHandleValue, sectionHandleValue, sectionByteCapacity,
// maxChunkCount, reserved0 -- but each is its own named type per this
// codebase's "small deliberate duplication over cross-format coupling"
// precedent, so this helper is a template rather than one shared struct.
template <typename Request>
Request MakeFileRequest(const ImportSessionRequest& session, HANDLE sourceFileHandle, HANDLE sectionHandle)
{
    Request request{};
    request.generationId = session.generationId;
    request.sourceFileHandleValue = reinterpret_cast<uint64_t>(sourceFileHandle);
    request.sectionHandleValue = reinterpret_cast<uint64_t>(sectionHandle);
    request.sectionByteCapacity = session.sectionByteCapacity;
    request.maxChunkCount = session.maxChunkCount;
    return request;
}

bool SendStartRequest(const ImportSessionRequest& session, HANDLE controlInWrite, HANDLE sourceFileHandle,
                       HANDLE sectionHandle)
{
    switch (session.format) {
    case ImportFormat::Gltf: {
        auto request = MakeFileRequest<model_core::ParseGltfFileRequest>(session, sourceFileHandle, sectionHandle);
        return model_core::WriteControlMessage(controlInWrite, model_core::ControlOpcode::StartGltfImportFromFile,
                                                &request, sizeof(request));
    }
    case ImportFormat::Stl: {
        auto request = MakeFileRequest<model_core::ParseStlFileRequest>(session, sourceFileHandle, sectionHandle);
        return model_core::WriteControlMessage(controlInWrite, model_core::ControlOpcode::StartStlImportFromFile,
                                                &request, sizeof(request));
    }
    case ImportFormat::Ply: {
        auto request = MakeFileRequest<model_core::ParsePlyFileRequest>(session, sourceFileHandle, sectionHandle);
        return model_core::WriteControlMessage(controlInWrite, model_core::ControlOpcode::StartPlyImportFromFile,
                                                &request, sizeof(request));
    }
    }
    return false;
}

ImportSessionResult Fail(ImportStage stage, model_core::ImportErrorCode code = model_core::ImportErrorCode::None)
{
    ImportSessionResult result;
    result.ok = false;
    result.stage = stage;
    result.errorCode = code;
    return result;
}

} // namespace

bool PrepareImportSandbox(const std::wstring& workerExePath)
{
    return AcquireWorkerContainer(workerExePath).ready;
}

ImportSessionResult RunImportSession(const ImportSessionRequest& request)
{
    // Cheapest possible cancellation: a generation already superseded before
    // it started launches no worker and touches no file at all.
    if (request.isCancelled && request.isCancelled()) {
        return Fail(ImportStage::Cancelled);
    }

    auto opened = OpenAndCanonicalizeSourceFile(request.sourcePath);
    if (!opened.file) {
        ImportSessionResult result = Fail(ImportStage::OpenSource);
        result.openError = opened.error;
        return result;
    }

    auto duplicatedFile = DuplicateInheritableHandle(opened.file.get());
    if (!duplicatedFile) {
        return Fail(ImportStage::DuplicateSourceHandle);
    }

    platform::Win32Handle outputSection = CreateSharedSection(static_cast<SIZE_T>(request.sectionByteCapacity));
    if (!outputSection) {
        return Fail(ImportStage::CreateOutputSection);
    }

    const WorkerContainer& container = AcquireWorkerContainer(request.workerExePath);
    if (!container.ready) {
        return Fail(ImportStage::CreateSandboxProfile);
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE inReadRaw = nullptr;
    HANDLE inWriteRaw = nullptr;
    HANDLE outReadRaw = nullptr;
    HANDLE outWriteRaw = nullptr;
    if (!CreatePipe(&inReadRaw, &inWriteRaw, &sa, 0) || !CreatePipe(&outReadRaw, &outWriteRaw, &sa, 0)) {
        return Fail(ImportStage::CreateControlChannel);
    }
    platform::Win32Handle controlInRead(inReadRaw);
    platform::Win32Handle controlInWrite(inWriteRaw);
    platform::Win32Handle controlOutRead(outReadRaw);
    platform::Win32Handle controlOutWrite(outWriteRaw);
    SetHandleInformation(controlInWrite.get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(controlOutRead.get(), HANDLE_FLAG_INHERIT, 0);

    std::wstring cmdLine = L"\"" + request.workerExePath + L"\" " + ParseFlagFor(request.format);

    HANDLE inherited[] = { controlInRead.get(), controlOutWrite.get(), duplicatedFile->get(), outputSection.get() };
    SandboxLimits limits{};
    limits.processMemoryLimitBytes = static_cast<SIZE_T>(request.commitLimitBytes);
    auto proc = LaunchSuspendedSandboxed(request.workerExePath, cmdLine, inherited, controlOutWrite.get(), limits,
                                          container.sid, controlInRead.get());
    controlInRead.reset();
    controlOutWrite.reset();
    if (!proc) {
        return Fail(ImportStage::LaunchWorker);
    }
    if (!ResumeSandboxProcess(*proc)) {
        return Fail(ImportStage::ResumeWorker);
    }

    if (!SendStartRequest(request, controlInWrite.get(), duplicatedFile->get(), outputSection.get())) {
        return Fail(ImportStage::SendRequest);
    }

    // Services zero or more mid-generation RequestSidecarFile messages before
    // the terminal ChunksReady/GenerationError reply -- degrades to exactly
    // one iteration for STL/PLY and any self-contained GLB, since neither
    // ever sends RequestSidecarFile.
    const auto replyTimeout = std::chrono::milliseconds(kWorkerReplyTimeoutMs);
    uint32_t sidecarRequestCount = 0;
    model_core::ReceivedControlMessage received{};
    ControlWaitOutcome outcome = ReadControlMessageBounded(controlOutRead.get(), replyTimeout, received, request.isCancelled);

    while (outcome == ControlWaitOutcome::Ready
           && received.header.opcode == static_cast<uint32_t>(model_core::ControlOpcode::RequestSidecarFile)
           && received.payload.size() == sizeof(model_core::RequestSidecarFileNotice)) {
        if (++sidecarRequestCount > request.maxSidecarRequestsPerGeneration) {
            break; // never trust worker self-restraint -- treated as a protocol violation below
        }

        model_core::RequestSidecarFileNotice sidecar{};
        std::memcpy(&sidecar, received.payload.data(), sizeof(sidecar));
        auto serviced
            = ServiceSidecarRequest(proc->process.get(), opened.canonicalPath, sidecar, request.maxSidecarFileBytes);

        bool sentReply = false;
        if (const auto* ready = std::get_if<model_core::SidecarFileReadyNotice>(&serviced)) {
            sentReply = model_core::WriteControlMessage(controlInWrite.get(),
                                                         model_core::ControlOpcode::SidecarFileReady, ready,
                                                         sizeof(*ready));
        } else if (const auto* unavailable = std::get_if<model_core::SidecarFileUnavailableNotice>(&serviced)) {
            sentReply = model_core::WriteControlMessage(controlInWrite.get(),
                                                         model_core::ControlOpcode::SidecarFileUnavailable, unavailable,
                                                         sizeof(*unavailable));
        }
        if (!sentReply) {
            outcome = ControlWaitOutcome::Eof;
            break;
        }

        outcome = ReadControlMessageBounded(controlOutRead.get(), replyTimeout, received, request.isCancelled);
    }

    // No wait-for-exit here: `proc` owns the Job Object handle, and dropping
    // it on return is the kill (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE). The
    // previous WaitForSingleObject(..., 5000) existed to let the worker exit
    // before its per-import AppContainer profile was deleted; there is no
    // per-import profile any more. Validation below never depended on the
    // worker being dead either -- ValidateAndCopySection copies the whole
    // section into private memory before trusting any of it, which is
    // precisely what the hostile-worker suite proves.

    if (sidecarRequestCount > request.maxSidecarRequestsPerGeneration) {
        return Fail(ImportStage::SidecarRequestLimit);
    }

    if (outcome == ControlWaitOutcome::Cancelled) {
        return Fail(ImportStage::Cancelled);
    }
    if (outcome == ControlWaitOutcome::TimedOut) {
        return Fail(ImportStage::ReplyTimedOut);
    }
    if (outcome != ControlWaitOutcome::Ready) {
        return Fail(ImportStage::AwaitReply);
    }

    if (received.header.opcode == static_cast<uint32_t>(model_core::ControlOpcode::GenerationError)) {
        model_core::GenerationErrorNotice notice{};
        if (received.payload.size() == sizeof(notice)) {
            std::memcpy(&notice, received.payload.data(), sizeof(notice));
        }
        return Fail(ImportStage::WorkerReportedError, static_cast<model_core::ImportErrorCode>(notice.errorCode));
    }

    if (received.header.opcode != static_cast<uint32_t>(model_core::ControlOpcode::ChunksReady)) {
        return Fail(ImportStage::UnexpectedReply);
    }

    auto view = platform::MappedView::Map(outputSection.get(), FILE_MAP_READ,
                                           static_cast<SIZE_T>(request.sectionByteCapacity));
    if (!view) {
        return Fail(ImportStage::MapOutputSection);
    }

    ValidationResult validation
        = ValidateAndCopySection(view.bytes(), request.generationId, request.maxChunkCount);
    if (!validation.ok) {
        return Fail(ImportStage::ValidateSection, validation.errorCode);
    }

    ImportSessionResult result;
    result.ok = true;
    result.stage = ImportStage::Completed;
    result.chunks = std::move(validation.chunks);
    return result;
}

} // namespace import_broker
