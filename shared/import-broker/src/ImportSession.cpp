#include "model_core/TierALimits.h"
#include "import_broker/ImportSession.h"
#include "model_core/VertexLayouts.h"

#include "import_broker/ControlChannelWait.h"
#include "import_broker/SandboxLauncher.h"
#include "import_broker/SharedSection.h"
#include "import_broker/SidecarRequestServicer.h"
#include "import_broker/SourceFileAccess.h"
#include "import_broker/UsdFallbackState.h"
#include "import_broker/WorkerPool.h"
#include "model_core/ControlChannelIo.h"
#include "model_core/ControlProtocol.h"
#include "platform/AppContainerSid.h"
#include "platform/MappedView.h"
#include "platform/Win32Handle.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <cstring>
#include <exception>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace import_broker {

namespace {

// The design names this profile and has the installer provision it; see
// .docs/design/08-installation-and-registration.md ("Import-process
// identities"). Until then it is created on first use and reused for the
// rest of the session.
constexpr const wchar_t* kWorkerContainerName = L"Binbuf.Preview3D.ImportWorker";
constexpr const wchar_t* kCompatibilityContainerName = L"Binbuf.Preview3D.ImportHost";
constexpr const wchar_t* kStepContainerName = L"Binbuf.Preview3D.StepHost";

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
    case ImportFormat::Obj:
        return L"--parse-obj";
    case ImportFormat::Fbx:
        return L"--parse-fbx";
    case ImportFormat::Usd:
        return L"--parse-usd";
    case ImportFormat::ThreeMf:
        return L"--parse-3mf";
    case ImportFormat::Step:
        // Unreachable: STEP always runs in the pooled dedicated host, never
        // the one-shot fast-worker path. Kept explicit so the closed format
        // set is auditable next to its opcode/request mapping.
        return L"--parse-step";
    case ImportFormat::Gltf:
    default:
        return L"--parse-gltf";
    }
}

const WorkerContainer& AcquireCompatibilityContainer(const std::wstring& hostExePath)
{
    static WorkerContainer container = [&hostExePath] {
        WorkerContainer created;
        try {
            created.sid = platform::AppContainerSid::CreateOrOpen(
                kCompatibilityContainerName, L"Preview3D Import Host",
                L"Zero-capability sandbox identity for Preview3DImportHost.exe");
        } catch (const std::exception&) {
            return created;
        }
        if (!created.sid) return created;
        // Deliberately grant only the private OpenUsdHost directory. The
        // general worker profile is never used for this payload (or vice
        // versa), so neither identity gains execute access to the other.
        created.ready = platform::GrantDirectoryReadExecute(DirectoryOf(hostExePath), created.sid.get());
        return created;
    }();
    return container;
}

uint64_t CompatibilityCommitLimitBytes()
{
    return DedicatedHostCommitLimitBytes();
}

// Dedicated STEP host identity. Like the USD compatibility host it is granted
// read+execute on its own private payload directory only, so the general
// worker and either compatibility payload never share an identity or an ACL.
const WorkerContainer& AcquireStepContainer(const std::wstring& hostExePath)
{
    static WorkerContainer container = [&hostExePath] {
        WorkerContainer created;
        try {
            created.sid = platform::AppContainerSid::CreateOrOpen(
                kStepContainerName, L"Preview3D STEP Host",
                L"Zero-capability sandbox identity for Preview3DStepHost.exe");
        } catch (const std::exception&) {
            return created;
        }
        if (!created.sid) return created;
        created.ready = platform::GrantDirectoryReadExecute(DirectoryOf(hostExePath), created.sid.get());
        return created;
    }();
    return container;
}

std::optional<uint32_t> UsdExpectedEncodingFlags(const std::wstring& path)
{
    const auto slash = path.find_last_of(L"\\/");
    const auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
        return std::nullopt;
    std::wstring extension = path.substr(dot + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t value) { return wchar_t(std::towlower(value)); });
    if (extension == L"usd") return 0;
    if (extension == L"usda") return model_core::kImportRequestUsdExpectedUsda;
    if (extension == L"usdc") return model_core::kImportRequestUsdExpectedUsdc;
    if (extension == L"usdz") return model_core::kImportRequestUsdExpectedUsdz;
    return std::nullopt;
}

// Every real-file request struct (ParseGltfFileRequest/ParseStlFileRequest/
// ParsePlyFileRequest) is field-for-field identical -- generationId,
// sourceFileHandleValue, sectionHandleValue, sectionByteCapacity,
// maxChunkCount, reserved0 -- but each is its own named type per this
// codebase's "small deliberate duplication over cross-format coupling"
// precedent, so this helper is a template rather than one shared struct.
template <typename Request>
Request MakeFileRequest(const ImportSessionRequest& session, uint64_t sourceFileHandle,
                        uint64_t sectionHandle, uint64_t cancellationEventHandle)
{
    Request request{};
    request.generationId = session.generationId;
    request.sourceFileHandleValue = sourceFileHandle;
    request.sectionHandleValue = sectionHandle;
    request.sectionByteCapacity = session.sectionByteCapacity;
    request.maxChunkCount = session.maxChunkCount;
    request.requestFlags = (session.enableCoarseProxy ? model_core::kImportRequestCoarseProxy : 0)
        | (session.nextDetail ? model_core::kImportRequestDetailService : 0)
        | (session.workerArgumentsOverride == L"--parse-gltf-delayed-batches"
            ? model_core::kImportRequestDelayedBatchesForTesting : 0)
        | (session.fbxTinyEvaluationLimitForTesting
            ? model_core::kImportRequestFbxTinyEvaluationLimitForTesting : 0)
        | (session.fbxTinyTextureLimitForTesting
            ? model_core::kImportRequestFbxTinyTextureLimitForTesting : 0)
        | (session.stepForceSerialForTesting
            ? model_core::kImportRequestStepForceSerialForTesting : 0);
    request.cancellationEventHandleValue = cancellationEventHandle;
    return request;
}

bool SendStartRequest(const ImportSessionRequest& session, ImportProducer producer,
                      HANDLE controlInWrite, uint64_t sourceFileHandle,
                      uint64_t sectionHandle, uint64_t cancellationEventHandle)
{
    switch (session.format) {
    case ImportFormat::Gltf: {
        auto request = MakeFileRequest<model_core::ParseGltfFileRequest>(session, sourceFileHandle, sectionHandle,
                                                                         cancellationEventHandle);
        return model_core::WriteControlMessage(controlInWrite, model_core::ControlOpcode::StartGltfImportFromFile,
                                                &request, sizeof(request));
    }
    case ImportFormat::Stl: {
        auto request = MakeFileRequest<model_core::ParseStlFileRequest>(session, sourceFileHandle, sectionHandle,
                                                                        cancellationEventHandle);
        return model_core::WriteControlMessage(controlInWrite, model_core::ControlOpcode::StartStlImportFromFile,
                                                &request, sizeof(request));
    }
    case ImportFormat::Ply: {
        auto request = MakeFileRequest<model_core::ParsePlyFileRequest>(session, sourceFileHandle, sectionHandle,
                                                                        cancellationEventHandle);
        return model_core::WriteControlMessage(controlInWrite, model_core::ControlOpcode::StartPlyImportFromFile,
                                                &request, sizeof(request));
    }
    case ImportFormat::Obj: {
        auto request = MakeFileRequest<model_core::ParseObjFileRequest>(session, sourceFileHandle, sectionHandle,
                                                                        cancellationEventHandle);
        return model_core::WriteControlMessage(controlInWrite, model_core::ControlOpcode::StartObjImportFromFile,
                                                &request, sizeof(request));
    }
    case ImportFormat::Fbx: {
        auto request = MakeFileRequest<model_core::ParseFbxFileRequest>(session, sourceFileHandle, sectionHandle,
                                                                        cancellationEventHandle);
        return model_core::WriteControlMessage(controlInWrite, model_core::ControlOpcode::StartFbxImportFromFile,
                                                &request, sizeof(request));
    }
    case ImportFormat::Usd: {
        auto expected = UsdExpectedEncodingFlags(session.sourcePath);
        if (!expected) return false;
        if (producer == ImportProducer::CompatibilityHost) {
            auto request = MakeFileRequest<model_core::ParseOpenUsdFileRequest>(
                session, sourceFileHandle, sectionHandle, cancellationEventHandle);
            request.requestFlags |= *expected;
            return model_core::WriteControlMessage(
                controlInWrite, model_core::ControlOpcode::StartOpenUsdImportFromFile,
                &request, sizeof(request));
        }
        auto request = MakeFileRequest<model_core::ParseUsdFileRequest>(
            session, sourceFileHandle, sectionHandle, cancellationEventHandle);
        request.requestFlags |= *expected;
        return model_core::WriteControlMessage(
            controlInWrite, model_core::ControlOpcode::StartUsdImportFromFile,
            &request, sizeof(request));
    }
    case ImportFormat::ThreeMf: {
        auto request = MakeFileRequest<model_core::ParseThreeMfFileRequest>(
            session, sourceFileHandle, sectionHandle, cancellationEventHandle);
        return model_core::WriteControlMessage(
            controlInWrite, model_core::ControlOpcode::StartThreeMfImportFromFile,
            &request, sizeof(request));
    }
    case ImportFormat::Step: {
        // Only the dedicated STEP host may receive this opcode; the caller
        // (RunImportSessionForProducer) has already enforced producer/format
        // pairing. No expected-encoding bits exist: the host admits ISO
        // 10303-21 by bytes, never by extension.
        auto request = MakeFileRequest<model_core::ParseStepFileRequest>(
            session, sourceFileHandle, sectionHandle, cancellationEventHandle);
        return model_core::WriteControlMessage(
            controlInWrite, model_core::ControlOpcode::StartStepImportFromFile,
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
    if (code == model_core::ImportErrorCode::None) {
        using E = model_core::ImportErrorCode;
        switch (stage) {
        case ImportStage::Cancelled: code = E::Cancelled; break;
        case ImportStage::OpenSource: code = E::FileUnavailable; break;
        case ImportStage::ReplyTimedOut: code = E::WorkerTimedOut; break;
        case ImportStage::SendRequest: case ImportStage::AwaitReply: case ImportStage::ChunkBatchAckFailed: code = E::WorkerCrashed; break;
        case ImportStage::SidecarRequestLimit: case ImportStage::ChunkBatchLimit: case ImportStage::ChunkCountLimit: code = E::ResourceLimit; break;
        case ImportStage::StepProgressLimit: code = E::ResourceLimit; break;
        case ImportStage::UnexpectedReply: case ImportStage::ChunkBatchOutOfOrder: code = E::ImportProtocolViolation; break;
        case ImportStage::CreateOutputSection: case ImportStage::MapOutputSection: code = E::OutOfMemory; break;
        default: code = E::InternalImporterFailure; break;
        }
    }
    result.errorCode = code;
    return result;
}

// Cross-batch acceptance state for one generation.
//
// ValidateAndCopySection is complete *within* one section. These are the
// rules that only exist once a generation writes the window more than once,
// and that no single section can be checked against on its own:
//
//  - batchIndex must be exactly the next one expected. A replayed index would
//    otherwise let a worker re-present a batch the host already accepted, and
//    a skipped one would hide a batch it was supposed to send.
//  - the batch count is capped, so a worker cannot emit batches forever.
//  - the chunk count across the whole generation is capped, so the catalog
//    below cannot grow without bound.
//
// The catalog is what lets a later batch depend on an earlier one's chunk --
// a mesh pointing at a material whose textures already crossed -- instead of
// every batch re-sending everything it references. It is handed to the
// validator, which resolves dependency ids against it and also rejects any
// chunkId already in it, so ids stay unique across the whole generation
// exactly as they already were within one section.
struct BatchAcceptance {
    uint32_t nextBatchIndex = 0;
    uint32_t totalChunks = 0;
    uint64_t triangles = 0, points = 0, vertices = 0;
    KnownChunkCatalog catalog;
    KnownChunkCatalog unresolved;
    KnownImageCatalog images;
    KnownSceneCatalog sceneCatalog;
    uint64_t textureBytes=0,texturePixels=0;
    bool haveTextureWarning=false, haveStatus=false;
    std::optional<model_core::SceneMetadata> scene;
    std::optional<std::array<double, 3>> origin;
    struct Region { model_core::ChunkDescriptor scan; bool coarse=false, fine=false; };
    std::unordered_map<uint32_t,Region> regions;
    uint64_t coarsePrimitives=0, coarseBytes=0;
    uint64_t coarseAllocationBytes=0;
    uint64_t previewPrimitives=0, previewBytes=0;
    uint64_t previewAllocationBytes=0;
    uint32_t coarseRegions=0;
    bool coarseComplete=false;

    void Record(const std::vector<ValidatedChunk>& chunks)
    {
        for (const auto& chunk : chunks) {
            // The validator already proved each id is new -- both within its
            // own section and against this catalog.
            catalog.emplace(chunk.descriptor.chunkId, chunk.descriptor.topology);
            if (chunk.descriptor.topology==model_core::ChunkTopology::Image) {
                model_core::ImagePayloadHeader header; std::memcpy(&header,chunk.payload.data(),sizeof(header));
                textureBytes+=header.pixelDataByteSize; texturePixels+=*model_core::ComputeImagePixelBytes(model_core::PixelFormatId::RGBA8_UNORM,header.width,header.height,header.mipLevels)/4;
                images.emplace(chunk.descriptor.chunkId,header);
                if (header.reserved0) { auto latest=header;latest.reserved0=0; images[header.reserved0]=latest; }
            }
            if (chunk.descriptor.topology==model_core::ChunkTopology::TextureWarning) haveTextureWarning=true;
            if (chunk.descriptor.topology==model_core::ChunkTopology::ImportStatus) haveStatus=true;
        }
        totalChunks += static_cast<uint32_t>(chunks.size());
        ++nextBatchIndex;
    }
};

enum class ProcessIdentity { Worker, CompatibilityHost, StepHost };

class ProcessCoordinator {
public:
    ProcessCoordinator(ProcessIdentity identity, size_t poolSize, bool exitAfterUse)
        : identity_(identity), poolSize_(poolSize), exitAfterUse_(exitAfterUse) {}

    ~ProcessCoordinator()
    {
        Shutdown();
    }

    void PrepareAsync(const std::wstring& workerExePath, uint64_t commitLimitBytes)
    {
        std::lock_guard lock(mutex_);
        if (initializing_ || ready_ || stopping_) return;
        initializing_ = true;
        initializer_ = std::thread([this, workerExePath, commitLimitBytes] {
            auto candidate = std::make_unique<WorkerPool>();
            std::wstring error;
            const auto& container = AcquireWorkerContainer(workerExePath);
            SandboxLimits limits{};
            limits.processMemoryLimitBytes = static_cast<SIZE_T>(commitLimitBytes);
            const bool ok = container.ready
                && candidate->InitializeBorrowed(workerExePath, container.sid.get(), limits,
                                                 poolSize_, error);
            std::lock_guard lock(mutex_);
            if (ok && !stopping_) {
                pool_ = std::move(candidate);
                ready_ = true;
            }
            initializing_ = false;
            changed_.notify_all();
        });
    }

    // Compatibility hosts are intentionally not prewarmed. This is called
    // only after UsdFallbackState accepts the exact fast-path
    // UnsupportedComposition transition, on the loader/broker lane.
    bool PrepareNow(const std::wstring& exePath, uint64_t commitLimitBytes,
                    const std::wstring& arguments,
                    const std::function<bool()>& cancelled)
    {
        {
            std::unique_lock lock(mutex_);
            while (initializing_ && !stopping_) {
                if (cancelled && cancelled()) return false;
                changed_.wait_for(lock, std::chrono::milliseconds(5));
            }
            if (ready_) return true;
            if (stopping_) return false;
            initializing_ = true;
        }

        auto candidate = std::make_unique<WorkerPool>();
        std::wstring error;
        const WorkerContainer& container = identity_ == ProcessIdentity::CompatibilityHost
            ? AcquireCompatibilityContainer(exePath)
            : identity_ == ProcessIdentity::StepHost
                ? AcquireStepContainer(exePath)
                : AcquireWorkerContainer(exePath);
        SandboxLimits limits{};
        limits.processMemoryLimitBytes = static_cast<SIZE_T>(commitLimitBytes);
        const bool ok = container.ready
            && candidate->InitializeBorrowedForTesting(
                exePath, container.sid.get(), limits, poolSize_, arguments, error);

        std::lock_guard lock(mutex_);
        if (ok && !stopping_) {
            pool_ = std::move(candidate);
            ready_ = true;
        }
        initializing_ = false;
        changed_.notify_all();
        return ready_;
    }

    struct Lease {
        Lease(ProcessCoordinator* ownerValue, WorkerPool* poolValue, size_t indexValue)
            : owner(ownerValue), pool(poolValue), index(indexValue) {}
        ProcessCoordinator* owner = nullptr;
        WorkerPool* pool = nullptr;
        size_t index = 0;
        bool reusable = false;
        ~Lease() { if (owner) owner->Release(*this); }
    };

    std::unique_ptr<Lease> Acquire(const std::function<bool()>& cancelled)
    {
        std::unique_lock lock(mutex_);
        while ((initializing_ || ready_) && !stopping_) {
            if (cancelled && cancelled()) return {};
            if (ready_) {
                if (auto index = pool_->AcquireIdle())
                    return std::make_unique<Lease>(this, pool_.get(), *index);
            }
            changed_.wait_for(lock, std::chrono::milliseconds(5));
        }
        return {};
    }

    void Shutdown()
    {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return;
            stopping_ = true;
            changed_.notify_all();
        }
        if (initializer_.joinable()) initializer_.join();
        std::unique_ptr<WorkerPool> pool;
        {
            std::lock_guard lock(mutex_);
            pool = std::move(pool_);
            ready_ = false;
        }
        if (pool) pool->Shutdown();
    }

private:
    void Release(Lease& lease)
    {
        if (exitAfterUse_) {
            std::unique_ptr<WorkerPool> finished;
            {
                std::lock_guard lock(mutex_);
                finished = std::move(pool_);
                ready_ = false;
                changed_.notify_all();
            }
            // A well-behaved host receives a bounded graceful Shutdown. A
            // crashed/hung/protocol-invalid host is killed immediately by
            // dropping the pool and its kill-on-close Job handle; it is never
            // replaced for the same generation.
            if (finished && lease.reusable) finished->Shutdown();
            return;
        }
        std::wstring error;
        const bool available = lease.reusable || lease.pool->TerminateAndReplace(lease.index, error);
        std::lock_guard lock(mutex_);
        if (available) lease.pool->Release(lease.index);
        changed_.notify_one();
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::unique_ptr<WorkerPool> pool_;
    std::thread initializer_;
    bool initializing_ = false;
    bool ready_ = false;
    bool stopping_ = false;
    ProcessIdentity identity_;
    size_t poolSize_ = 1;
    bool exitAfterUse_ = false;
};

ProcessCoordinator& WorkerCoordinator()
{
    static ProcessCoordinator coordinator(ProcessIdentity::Worker, 2, false);
    return coordinator;
}

ProcessCoordinator& CompatibilityCoordinator()
{
    // Size one means at most the current compatibility generation can own
    // the host. exitAfterUse is the strictest allowed bounded-idle policy:
    // the process exits as soon as that generation finishes.
    static ProcessCoordinator coordinator(ProcessIdentity::CompatibilityHost, 1, true);
    return coordinator;
}

ProcessCoordinator& StepHostCoordinator()
{
    // OCCT global state and peak B-rep memory are discarded deterministically:
    // one generation owns the host and it exits as soon as that generation
    // finishes, exactly like the USD compatibility host.
    static ProcessCoordinator coordinator(ProcessIdentity::StepHost, 1, true);
    return coordinator;
}

} // namespace

uint64_t DedicatedHostCommitLimitBytes()
{
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    const uint64_t fourGiB = 4ull * 1024ull * 1024ull * 1024ull;
    if (!GlobalMemoryStatusEx(&memory)) return fourGiB;
    const uint64_t thirtyFivePercent = memory.ullTotalPhys / 100ull * 35ull;
    return (std::min)(fourGiB, thirtyFivePercent);
}

bool PrepareImportSandbox(const std::wstring& workerExePath)
{
    return AcquireWorkerContainer(workerExePath).ready;
}

void PrepareImportWorkerPoolAsync(const std::wstring& workerExePath, uint64_t commitLimitBytes)
{
    WorkerCoordinator().PrepareAsync(workerExePath, commitLimitBytes);
}

void ShutdownImportWorkerPool()
{
    WorkerCoordinator().Shutdown();
}

void ShutdownCompatibilityHost()
{
    CompatibilityCoordinator().Shutdown();
}

ImportSessionResult RunImportSessionForProducer(const ImportSessionRequest& request,
                                                ImportProducer producer)
{
    // Producer/format pairing is a closed contract: the USD compatibility host
    // only ever serves USD and the dedicated STEP host only ever serves STEP.
    // A mismatch is a protocol violation, never a reason to fall through to
    // the general worker.
    if (producer == ImportProducer::CompatibilityHost && request.format != ImportFormat::Usd)
        return Fail(ImportStage::SendRequest, model_core::ImportErrorCode::ImportProtocolViolation);
    if (producer == ImportProducer::StepHost && request.format != ImportFormat::Step)
        return Fail(ImportStage::SendRequest, model_core::ImportErrorCode::ImportProtocolViolation);
    if (producer == ImportProducer::FastWorker && request.format == ImportFormat::Step)
        return Fail(ImportStage::SendRequest, model_core::ImportErrorCode::ImportProtocolViolation);
    // Cheapest possible cancellation: a generation already superseded before
    // it started launches no worker and touches no file at all.
    if (request.isCancelled && request.isCancelled()) {
        return Fail(ImportStage::Cancelled);
    }

    auto opened = OpenAndCanonicalizeSourceFile(request.sourcePath);
    if (!opened.file) {
        ImportSessionResult result = Fail(ImportStage::OpenSource, opened.errorCode);
        result.openError = opened.error;
        return result;
    }

    BY_HANDLE_FILE_INFORMATION sourceBefore{};
    if (!GetFileInformationByHandle(opened.file.get(), &sourceBefore))
        return Fail(ImportStage::OpenSource, model_core::ImportErrorCode::FileUnavailable);
    const uint64_t primaryBytes = (uint64_t(sourceBefore.nFileSizeHigh) << 32) | sourceBefore.nFileSizeLow;
    if (primaryBytes > 8ull * 1024 * 1024 * 1024)
        return Fail(ImportStage::OpenSource, model_core::ImportErrorCode::PrimarySourceLimit);
    FILE_ID_INFO primaryId{};
    if (!GetFileInformationByHandleEx(opened.file.get(), FileIdInfo, &primaryId, sizeof(primaryId)))
        return Fail(ImportStage::OpenSource, model_core::ImportErrorCode::FileUnavailable);
    model_core::FileIdentity sourceIdentity{};
    sourceIdentity.volumeSerialNumber = primaryId.VolumeSerialNumber;
    std::memcpy(sourceIdentity.fileId128.data(), primaryId.FileId.Identifier,
                sourceIdentity.fileId128.size());
    sourceIdentity.sizeBytes = primaryBytes;
    sourceIdentity.lastWriteTime = int64_t((uint64_t(sourceBefore.ftLastWriteTime.dwHighDateTime) << 32) |
                                           sourceBefore.ftLastWriteTime.dwLowDateTime);
    if (request.onSourceOpened) request.onSourceOpened(sourceIdentity);
    uint64_t allSourceBytes = primaryBytes;
    uint64_t accumulatedBytes = 0;
    std::vector<SourceChunkRange> sourceCatalog;
    platform::Win32Handle outputSection = CreateSharedSection(static_cast<SIZE_T>(request.sectionByteCapacity));
    if (!outputSection) {
        return Fail(ImportStage::CreateOutputSection);
    }

    const bool compatibility = producer == ImportProducer::CompatibilityHost;
    const bool stepHost = producer == ImportProducer::StepHost;
    const bool hostRoute = compatibility || stepHost;
    const std::wstring& childExePath = compatibility
        ? request.compatibilityHostExePath
        : stepHost ? request.stepHostExePath : request.workerExePath;
    const WorkerContainer& container = compatibility
        ? AcquireCompatibilityContainer(childExePath)
        : stepHost ? AcquireStepContainer(childExePath) : AcquireWorkerContainer(childExePath);
    if (!container.ready) {
        return Fail(ImportStage::CreateSandboxProfile);
    }

    platform::Win32Handle cancellationEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!cancellationEvent) return Fail(ImportStage::CreateControlChannel);

    std::unique_ptr<ProcessCoordinator::Lease> pooledLease;
    std::optional<SandboxProcess> proc;
    platform::Win32Handle controlInRead, controlInWrite, controlOutRead, controlOutWrite;
    platform::Win32Handle duplicatedFile, duplicatedCancellation;
    HANDLE workerProcess = nullptr, workerJob = nullptr, controlInput = nullptr, controlOutput = nullptr;
    uint64_t workerSource = 0, workerOutput = 0, workerCancellation = 0;

    if (hostRoute || (request.useWorkerPool && request.workerArgumentsOverride.empty())) {
        ProcessCoordinator& coordinator = compatibility
            ? CompatibilityCoordinator() : stepHost ? StepHostCoordinator() : WorkerCoordinator();
        const auto hostLimit = [&] {
            if (stepHost) {
                return request.stepHostCommitLimitBytes != 0
                    ? request.stepHostCommitLimitBytes : CompatibilityCommitLimitBytes();
            }
            return request.compatibilityHostCommitLimitBytes != 0
                ? request.compatibilityHostCommitLimitBytes : CompatibilityCommitLimitBytes();
        };
        const auto hostArguments = [&]() -> std::wstring {
            if (stepHost) {
                return request.stepHostArgumentsOverride.empty()
                    ? L"--pool" : request.stepHostArgumentsOverride;
            }
            return request.compatibilityHostArgumentsOverride.empty()
                ? L"--pool" : request.compatibilityHostArgumentsOverride;
        };
        if (hostRoute) {
            if (!coordinator.PrepareNow(childExePath, hostLimit(), hostArguments(),
                                        request.isCancelled))
                return Fail(request.isCancelled && request.isCancelled() ? ImportStage::Cancelled
                                                                          : ImportStage::LaunchWorker);
        }
        pooledLease = coordinator.Acquire(request.isCancelled);
        // A replacement generation may have waited behind the previous host
        // lease. That lease intentionally tears its host down on release, so
        // prepare/acquire once more rather than treating the bounded-idle exit
        // as a launch failure for the newer generation.
        if (!pooledLease && hostRoute
            && !(request.isCancelled && request.isCancelled())) {
            if (coordinator.PrepareNow(childExePath, hostLimit(), hostArguments(),
                                       request.isCancelled))
                pooledLease = coordinator.Acquire(request.isCancelled);
        }
        if (!pooledLease)
            return Fail(request.isCancelled && request.isCancelled() ? ImportStage::Cancelled
                                                                      : ImportStage::LaunchWorker);
        WorkerPool& pool = *pooledLease->pool;
        const size_t index = pooledLease->index;
        workerProcess = pool.ProcessHandle(index);
        workerJob = pool.JobHandle(index);
        controlInput = pool.ControlInput(index);
        controlOutput = pool.ControlOutput(index);
        auto sourceValue = DuplicateHandleIntoProcess(opened.file.get(), workerProcess);
        auto outputValue = DuplicateHandleIntoProcess(outputSection.get(), workerProcess);
        auto cancelValue = DuplicateHandleIntoProcess(cancellationEvent.get(), workerProcess);
        if (!sourceValue || !outputValue || !cancelValue) return Fail(ImportStage::DuplicateSourceHandle);
        workerSource = *sourceValue;
        workerOutput = *outputValue;
        workerCancellation = *cancelValue;
    } else {
        auto fileDuplicate = DuplicateInheritableHandle(opened.file.get());
        auto cancelDuplicate = DuplicateInheritableHandle(cancellationEvent.get());
        if (!fileDuplicate || !cancelDuplicate) return Fail(ImportStage::DuplicateSourceHandle);
        duplicatedFile = std::move(*fileDuplicate);
        duplicatedCancellation = std::move(*cancelDuplicate);

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
        HANDLE inReadRaw = nullptr, inWriteRaw = nullptr, outReadRaw = nullptr, outWriteRaw = nullptr;
        if (!CreatePipe(&inReadRaw, &inWriteRaw, &sa, 0) || !CreatePipe(&outReadRaw, &outWriteRaw, &sa, 0))
            return Fail(ImportStage::CreateControlChannel);
        controlInRead.reset(inReadRaw); controlInWrite.reset(inWriteRaw);
        controlOutRead.reset(outReadRaw); controlOutWrite.reset(outWriteRaw);
        SetHandleInformation(controlInWrite.get(), HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(controlOutRead.get(), HANDLE_FLAG_INHERIT, 0);

        std::wstring workerArgs = request.workerArgumentsOverride.empty()
            ? std::wstring(ParseFlagFor(request.format)) : request.workerArgumentsOverride;
        if (request.enableCoarseProxy && request.workerArgumentsOverride.empty()) workerArgs += L"-proxy";
        if (request.nextDetail && request.workerArgumentsOverride.empty()) workerArgs += L"-detail";
        std::wstring cmdLine = L"\"" + childExePath + L"\" " + workerArgs;
        HANDLE inherited[] = { controlInRead.get(), controlOutWrite.get(), duplicatedFile.get(),
                               outputSection.get(), duplicatedCancellation.get() };
        SandboxLimits limits{};
        limits.processMemoryLimitBytes = static_cast<SIZE_T>(compatibility
            ? (request.compatibilityHostCommitLimitBytes != 0
                ? request.compatibilityHostCommitLimitBytes : CompatibilityCommitLimitBytes())
            : request.commitLimitBytes);
        proc = LaunchSuspendedSandboxed(childExePath, cmdLine, inherited, controlOutWrite.get(), limits,
                                         container.sid, controlInRead.get());
        controlInRead.reset(); controlOutWrite.reset();
        if (!proc) return Fail(ImportStage::LaunchWorker);
        if (!ResumeSandboxProcess(*proc)) return Fail(ImportStage::ResumeWorker);
        workerProcess = proc->process.get(); workerJob = proc->job.get();
        controlInput = controlInWrite.get(); controlOutput = controlOutRead.get();
        workerSource = reinterpret_cast<uint64_t>(duplicatedFile.get());
        workerOutput = reinterpret_cast<uint64_t>(outputSection.get());
        workerCancellation = reinterpret_cast<uint64_t>(duplicatedCancellation.get());
    }

    const auto cpuBudgetAllows=[&] {
        if (!request.cpuBudgetAllows) return true;
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb=sizeof(memory);
        return K32GetProcessMemoryInfo(workerProcess,reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory))
            && request.cpuBudgetAllows(memory.PrivateUsage);
    };
    if (!SendStartRequest(request, producer, controlInput, workerSource, workerOutput, workerCancellation)) {
        return Fail(ImportStage::SendRequest);
    }

    // Map the output window once and reuse it for every batch. A generation
    // may now hand the window over repeatedly, and remapping per batch would
    // buy nothing: ValidateAndCopySection bulk-copies the bytes it needs out
    // of the view on every call and retains no reference into it afterwards,
    // so one mapping cannot carry state between batches.
    auto view = platform::MappedView::Map(outputSection.get(), FILE_MAP_READ,
                                           static_cast<SIZE_T>(request.sectionByteCapacity));
    if (!view) {
        return Fail(ImportStage::MapOutputSection);
    }

    // Services zero or more mid-generation messages -- RequestSidecarFile and
    // ChunkBatchReady, in whatever order the worker needs them -- before the
    // terminal ChunksReady/GenerationError reply. Degrades to exactly one
    // iteration for STL/PLY and any single-window GLB, which send neither.
    const auto replyTimeout = std::chrono::milliseconds(request.replyTimeoutMs);
    const auto cancelProbe = [&] {
        const bool cancelled = request.isCancelled && request.isCancelled();
        if (cancelled) SetEvent(cancellationEvent.get());
        return cancelled;
    };
    uint32_t sidecarRequestCount = 0;
    uint32_t stepProgressCount = 0;
    model_core::StepProgressNotice lastStepProgress{};
    BatchAcceptance acceptance;
    std::vector<ValidatedChunk> accumulated;
    std::optional<ImportSessionResult> failure;

    // Shared by the non-terminal ChunkBatchReady path and the terminal
    // ChunksReady one, so a batch is accepted by identical rules either way
    // and the terminal batch is never the weaker check.
    // Derived in 64-bit and clamped: maxChunkCount * maxChunkBatchesPerGeneration
    // overflows uint32 for large-but-legal inputs, and an overflowed cap
    // would be a smaller number than either factor rather than a larger one.
    const uint64_t derivedCap = static_cast<uint64_t>(request.maxChunkCount)
        * static_cast<uint64_t>(request.maxChunkBatchesPerGeneration);
    const uint32_t chunkCountCap = request.maxChunksPerGeneration != 0
        ? request.maxChunksPerGeneration
        : static_cast<uint32_t>(std::min<uint64_t>(derivedCap, (std::numeric_limits<uint32_t>::max)()));

    auto acceptBatch = [&](uint32_t chunkCount) -> bool {
        if (!cpuBudgetAllows()) {
            failure=Fail(ImportStage::ValidateSection,model_core::ImportErrorCode::ResourceLimit); return false;
        }
        if (acceptance.nextBatchIndex >= request.maxChunkBatchesPerGeneration) {
            failure = Fail(ImportStage::ChunkBatchLimit);
            return false;
        }

        // The catalog is passed only once a batch has actually preceded this
        // one, so a single-window import calls the validator exactly as it
        // was called before progressive delivery existed.
        const KnownChunkCatalog* prior = acceptance.nextBatchIndex > 0 ? &acceptance.catalog : nullptr;
        ValidationResult validation
            = ValidateAndCopySection(view.bytes(), request.generationId, request.maxChunkCount, prior, true,
                &acceptance.images,acceptance.textureBytes,acceptance.texturePixels,
                &acceptance.sceneCatalog);
        if (!validation.ok) {
            failure = Fail(ImportStage::ValidateSection, validation.errorCode);
            return false;
        }
        bool batchHasWarning=false, batchHasStatus=false;
        for (const auto& chunk : validation.chunks) {
            if (chunk.descriptor.topology==model_core::ChunkTopology::ImportStatus) {
                if (acceptance.haveStatus || batchHasStatus) {
                    failure=Fail(ImportStage::ValidateSection,model_core::ImportErrorCode::ImportProtocolViolation);return false;
                }
                batchHasStatus=true;
            }
            if (chunk.descriptor.topology==model_core::ChunkTopology::TextureWarning) {
                if (acceptance.haveTextureWarning || batchHasWarning) {
                    failure=Fail(ImportStage::ValidateSection,model_core::ImportErrorCode::MalformedData);return false;
                }
                batchHasWarning=true;
            }
            if (chunk.descriptor.topology == model_core::ChunkTopology::TriangleList
                || chunk.descriptor.topology == model_core::ChunkTopology::PointList) {
                const auto& geometry = chunk.descriptor;
                if (!acceptance.origin) acceptance.origin = std::array<double,3>{geometry.origin[0],geometry.origin[1],geometry.origin[2]};
                for (unsigned axis = 0; axis < 3; ++axis) {
                    // Keep the unchanged float camera's normalization and clip
                    // arithmetic in range, even across individually valid batches.
                    // Absolute double origins may be large; only scene span is capped.
                    const double offset = geometry.origin[axis] - (*acceptance.origin)[axis];
                    if (std::abs(offset + double(geometry.localMin[axis])) > 1e15
                        || std::abs(offset + double(geometry.localMax[axis])) > 1e15) {
                        failure = Fail(ImportStage::ValidateSection, model_core::ImportErrorCode::ResourceLimit);
                        return false;
                    }
                }
            }
            if (acceptance.scene && std::memcmp(&*acceptance.scene, &chunk.scene, sizeof(chunk.scene))) {
                failure = Fail(ImportStage::ValidateSection, model_core::ImportErrorCode::ImportProtocolViolation);
                return false;
            }
            acceptance.scene = chunk.scene;
            const bool expectedFormat = request.format == ImportFormat::Gltf
                ? (chunk.scene.format == model_core::SourceFormatId::Gltf
                   || chunk.scene.format == model_core::SourceFormatId::Glb)
                : request.format == ImportFormat::Stl
                    ? (chunk.scene.format == model_core::SourceFormatId::Stl
                       || chunk.scene.format == model_core::SourceFormatId::AsciiStl)
                    : request.format == ImportFormat::Ply
                        ? (chunk.scene.format == model_core::SourceFormatId::Ply
                           || chunk.scene.format == model_core::SourceFormatId::AsciiPly)
                        : request.format == ImportFormat::Obj
                            ? chunk.scene.format == model_core::SourceFormatId::Obj
                            : request.format == ImportFormat::Fbx
                                ? chunk.scene.format == model_core::SourceFormatId::Fbx
                                : request.format == ImportFormat::ThreeMf
                                    ? chunk.scene.format == model_core::SourceFormatId::ThreeMf
                                : request.format == ImportFormat::Step
                                    ? chunk.scene.format == model_core::SourceFormatId::Step
                                : (chunk.scene.format == model_core::SourceFormatId::Usda
                                   || chunk.scene.format == model_core::SourceFormatId::Usdc
                                   || chunk.scene.format == model_core::SourceFormatId::Usdz);
            // USD's detected encoding is security/dispatch state for the
            // fallback decision, so even hostile-worker test overrides must
            // not bypass its family check. Older format attack fixtures keep
            // their established override seam.
            if ((request.workerArgumentsOverride.empty() || request.format == ImportFormat::Usd)
                && !expectedFormat) {
                failure = Fail(ImportStage::ValidateSection, model_core::ImportErrorCode::ImportProtocolViolation);
                return false;
            }
        }
        const bool tierBFormat = acceptance.scene
            && (acceptance.scene->format == model_core::SourceFormatId::AsciiStl
                || acceptance.scene->format == model_core::SourceFormatId::AsciiPly
                || acceptance.scene->format == model_core::SourceFormatId::Obj
                || acceptance.scene->format == model_core::SourceFormatId::Fbx
                || acceptance.scene->format == model_core::SourceFormatId::Usda
                || acceptance.scene->format == model_core::SourceFormatId::Usdc
                || acceptance.scene->format == model_core::SourceFormatId::Usdz
                || acceptance.scene->format == model_core::SourceFormatId::ThreeMf
                || acceptance.scene->format == model_core::SourceFormatId::Step);
        const bool coarseProtocol = request.enableCoarseProxy && !tierBFormat;
        // The notice's own chunkCount is a claim; the validator re-derived the
        // authoritative one from the section header. Disagreement means the
        // two are describing different things, which is a protocol violation
        // whichever one is "right".
        if (validation.chunks.size() != chunkCount) {
            failure = Fail(ImportStage::ValidateSection, model_core::ImportErrorCode::ImportProtocolViolation);
            return false;
        }
        // Checked against the running total, not per batch: the per-section
        // cap already bounds one batch, and what needs bounding here is the
        // catalog across all of them.
        if (chunkCount > chunkCountCap - acceptance.totalChunks) {
            failure = Fail(ImportStage::ChunkCountLimit, model_core::ImportErrorCode::ResourceLimit);
            return false;
        }

        // Forward references are only material/image roles. Existing LOD
        // references must resolve to an already validated chunk. Every missing
        // role is bounded by the generation catalog cap and checked at terminal.
        KnownChunkCatalog available = acceptance.catalog;
        for (const auto& chunk : validation.chunks) available.emplace(chunk.descriptor.chunkId, chunk.descriptor.topology);
        for (const auto& chunk : validation.chunks) {
            auto pending = acceptance.unresolved.find(chunk.descriptor.chunkId);
            if (pending != acceptance.unresolved.end()) {
                if (pending->second != chunk.descriptor.topology) {
                    failure = Fail(ImportStage::ValidateSection, model_core::ImportErrorCode::MalformedData);
                    return false;
                }
                acceptance.unresolved.erase(pending);
            }
            const auto& desc = chunk.descriptor;
            for (uint32_t d = 0; d < model_core::kMaxDependencyIds; ++d) {
                const auto id = desc.dependencyIds[d];
                if (!id || available.contains(id)) continue;
                const auto expected = desc.topology == model_core::ChunkTopology::Material
                    ? model_core::ChunkTopology::Image : model_core::ChunkTopology::Material;
                if (desc.topology != model_core::ChunkTopology::Material && d != 0) {
                    failure = Fail(ImportStage::ValidateSection, model_core::ImportErrorCode::MalformedData); return false;
                }
                auto [it, inserted] = acceptance.unresolved.emplace(id, expected);
                if ((!inserted && it->second != expected) || acceptance.unresolved.size() > chunkCountCap) {
                    failure = Fail(ImportStage::ValidateSection, model_core::ImportErrorCode::MalformedData); return false;
                }
            }
        }
        uint64_t newTriangles = acceptance.triangles, newPoints = acceptance.points,
                 newVertices = acceptance.vertices;
        for (const auto& chunk : validation.chunks)
        {
            const auto& d = chunk.descriptor;
            if (coarseProtocol && d.lodLevel != model_core::kScanLod) continue;
            if (d.topology == model_core::ChunkTopology::TriangleList)
                newTriangles += d.indexCount / 3;
            if (d.topology == model_core::ChunkTopology::PointList)
                newPoints += d.vertexCount;
            if (d.topology == model_core::ChunkTopology::TriangleList ||
                d.topology == model_core::ChunkTopology::PointList)
                newVertices += d.vertexCount;
        }
        const uint64_t triangleLimit = tierBFormat ? model_core::kTierBTriangleLimit : model_core::kTierATriangleLimit;
        const uint64_t pointLimit = tierBFormat ? model_core::kTierBPointLimit : model_core::kTierAPointLimit;
        const uint64_t vertexLimit = tierBFormat ? model_core::kTierBVertexLimit : model_core::kTierAVertexLimit;
        if (newTriangles > triangleLimit || newPoints > pointLimit || newVertices > vertexLimit)
        {
            failure = Fail(ImportStage::ValidateSection, model_core::ImportErrorCode::ResourceLimit);
            return false;
        }
        acceptance.triangles = newTriangles;
        acceptance.points = newPoints;
        acceptance.vertices = newVertices;
        BY_HANDLE_FILE_INFORMATION currentSource{};
        if (!GetFileInformationByHandle(opened.file.get(), &currentSource) ||
            currentSource.nFileSizeHigh != sourceBefore.nFileSizeHigh ||
            currentSource.nFileSizeLow != sourceBefore.nFileSizeLow ||
            CompareFileTime(&currentSource.ftLastWriteTime, &sourceBefore.ftLastWriteTime))
        {
            failure = Fail(ImportStage::ValidateSection, model_core::ImportErrorCode::FileChanged);
            return false;
        }
        for (const auto& chunk : validation.chunks)
        {
            const auto& d = chunk.descriptor;
            if (d.topology != model_core::ChunkTopology::TriangleList &&
                d.topology != model_core::ChunkTopology::PointList)
                continue;
            if (!d.sourceRangeLength && chunk.scene.format != model_core::SourceFormatId::Unknown)
            {
                failure = Fail(ImportStage::ValidateSection, model_core::ImportErrorCode::MalformedData);
                return false;
            }
            if (d.sourceRangeLength)
            {
                if ((chunk.scene.format == model_core::SourceFormatId::Stl ||
                     chunk.scene.format == model_core::SourceFormatId::Ply ||
                     chunk.scene.format == model_core::SourceFormatId::AsciiStl ||
                     chunk.scene.format == model_core::SourceFormatId::AsciiPly) &&
                    (d.sourceRangeOffset > primaryBytes ||
                     d.sourceRangeLength > primaryBytes - d.sourceRangeOffset))
                {
                    failure = Fail(ImportStage::ValidateSection, model_core::ImportErrorCode::MalformedData);
                    return false;
                }
                if (chunk.scene.format == model_core::SourceFormatId::Stl &&
                    (d.sourceRangeOffset < 84 || (d.sourceRangeOffset - 84) % 50 ||
                     d.sourceRangeLength % 50 || d.indexCount / 3 > d.sourceRangeLength / 50))
                {
                    failure = Fail(ImportStage::ValidateSection, model_core::ImportErrorCode::MalformedData);
                    return false;
                }
                if ((chunk.scene.format == model_core::SourceFormatId::Gltf ||
                     chunk.scene.format == model_core::SourceFormatId::Glb) &&
                    ((d.sourceRangeOffset >> 32) >= model_core::kTierAObjectLimit ||
                     uint32_t(d.sourceRangeOffset) > 300000000 ||
                     d.sourceRangeLength > 300000000 - uint32_t(d.sourceRangeOffset) ||
                     (d.lodLevel != model_core::kCoarseLod && d.sourceRangeLength != d.indexCount)))
                {
                    failure = Fail(ImportStage::ValidateSection, model_core::ImportErrorCode::MalformedData);
                    return false;
                }
                const bool usdSource = chunk.scene.format == model_core::SourceFormatId::Usda
                    || chunk.scene.format == model_core::SourceFormatId::Usdc
                    || chunk.scene.format == model_core::SourceFormatId::Usdz;
                const bool boundedPackageSource = usdSource
                    || chunk.scene.format == model_core::SourceFormatId::ThreeMf;
                if (boundedPackageSource &&
                    ((d.sourceRangeOffset >> 32) >= model_core::kTierBObjectLimit ||
                     uint32_t(d.sourceRangeOffset) > model_core::kTierBIndexLimit ||
                     d.sourceRangeLength > model_core::kTierBIndexLimit - uint32_t(d.sourceRangeOffset) ||
                     d.sourceRangeLength != (d.topology == model_core::ChunkTopology::PointList
                         ? d.vertexCount : d.indexCount)))
                {
                    failure = Fail(ImportStage::ValidateSection, model_core::ImportErrorCode::MalformedData);
                    return false;
                }
                if ((chunk.scene.format == model_core::SourceFormatId::Obj
                     || chunk.scene.format == model_core::SourceFormatId::Fbx) &&
                    ((d.sourceRangeOffset >> 32) >= model_core::kTierBObjectLimit ||
                     uint32_t(d.sourceRangeOffset) > model_core::kTierBIndexLimit ||
                     d.sourceRangeLength > model_core::kTierBIndexLimit - uint32_t(d.sourceRangeOffset) ||
                     d.sourceRangeLength != d.indexCount))
                {
                    failure = Fail(ImportStage::ValidateSection, model_core::ImportErrorCode::MalformedData);
                    return false;
                }
                if (!coarseProtocol || d.lodLevel == model_core::kScanLod)
                    sourceCatalog.push_back({request.generationId, d});
            }
        }
        uint64_t coarseVertexBatch=0,coarseIndexBatch=0,previewVertexBatch=0,previewIndexBatch=0;
        bool havePreview=false,haveOtherGeometry=false;
        for (const auto& chunk:validation.chunks) {
            const auto& d=chunk.descriptor;
            if (d.topology!=model_core::ChunkTopology::TriangleList && d.topology!=model_core::ChunkTopology::PointList) continue;
            if (d.lodLevel==model_core::kPreviewLod) havePreview=true; else haveOtherGeometry=true;
        }
        // Provisional preview publications are separate from the validated
        // scan/coarse/full phases, including their packed GPU allocation budget.
        if (coarseProtocol && havePreview && haveOtherGeometry) {
            failure=Fail(ImportStage::ValidateSection,model_core::ImportErrorCode::MalformedData); return false;
        }
        for (const auto& chunk:validation.chunks) if (chunk.descriptor.lodLevel==model_core::kCoarseLod && coarseProtocol) {
            coarseVertexBatch+=uint64_t(chunk.descriptor.vertexCount)*model_core::VertexStrideForLayout(model_core::VertexLayoutId(chunk.descriptor.vertexLayoutId));
            coarseIndexBatch+=uint64_t(chunk.descriptor.indexCount)*4;
        }
        acceptance.coarseAllocationBytes+=(coarseVertexBatch+65535)/65536*65536+(coarseIndexBatch+65535)/65536*65536;
        for (const auto& chunk:validation.chunks) if (chunk.descriptor.lodLevel==model_core::kPreviewLod && coarseProtocol) {
            previewVertexBatch+=uint64_t(chunk.descriptor.vertexCount)*model_core::VertexStrideForLayout(model_core::VertexLayoutId(chunk.descriptor.vertexLayoutId));
            previewIndexBatch+=uint64_t(chunk.descriptor.indexCount)*4;
        }
        acceptance.previewAllocationBytes+=(previewVertexBatch+65535)/65536*65536+(previewIndexBatch+65535)/65536*65536;
        if (acceptance.previewAllocationBytes>model_core::kPreviewReservedBytes) {
            failure=Fail(ImportStage::ValidateSection,model_core::ImportErrorCode::ResourceLimit); return false;
        }
        if (acceptance.coarseAllocationBytes>model_core::kCoarseReservedBytes) {
            failure=Fail(ImportStage::ValidateSection,model_core::ImportErrorCode::ResourceLimit); return false;
        }
        for (const auto& chunk:validation.chunks) {
            const auto& d=chunk.descriptor;
            const bool geometry=d.topology==model_core::ChunkTopology::TriangleList || d.topology==model_core::ChunkTopology::PointList;
            if (!coarseProtocol) {
                if (d.lodLevel>=model_core::kScanLod || d.topology==model_core::ChunkTopology::CoarseComplete) {
                    failure=Fail(ImportStage::ValidateSection,model_core::ImportErrorCode::MalformedData); return false;
                }
                continue;
            }
            auto invalid=[&] { failure=Fail(ImportStage::ValidateSection,model_core::ImportErrorCode::MalformedData); return false; };
            if (geometry) {
                const uint32_t identity=d.chunkId & 0x0fffffffu;
                if (!identity) return invalid();
                if (d.lodLevel==model_core::kPreviewLod) {
                    if (d.chunkId!=(identity|model_core::kPreviewIdentity) || !acceptance.regions.empty()
                        || acceptance.coarseComplete || d.dependencyCount) return invalid();
                    acceptance.previewPrimitives+=d.topology==model_core::ChunkTopology::PointList ? d.vertexCount : d.indexCount/3;
                    acceptance.previewBytes+=d.byteSize;
                    if (acceptance.previewPrimitives>model_core::kPreviewPrimitiveLimit || acceptance.previewBytes>model_core::kPreviewByteLimit) return invalid();
                } else if (d.lodLevel==model_core::kScanLod) {
                    if (d.chunkId!=(identity|model_core::kScanIdentity) || acceptance.coarseComplete
                        || !acceptance.regions.emplace(identity,BatchAcceptance::Region{d}).second) return invalid();
                } else {
                    auto region=acceptance.regions.find(identity);
                    if (region==acceptance.regions.end()) return invalid();
                    const auto& scan=region->second.scan;
                    if (d.topology!=scan.topology || d.vertexLayoutId!=scan.vertexLayoutId || d.meshId!=scan.meshId
                        || d.nodeId!=scan.nodeId || d.geometryFlags!=scan.geometryFlags || d.sourceElementOffset!=scan.sourceElementOffset
                        || d.sourceRangeOffset!=scan.sourceRangeOffset || d.sourceRangeLength!=scan.sourceRangeLength
                        || std::memcmp(d.origin,scan.origin,sizeof(d.origin))
                        || std::memcmp(d.dependencyIds,scan.dependencyIds,sizeof(d.dependencyIds))) return invalid();
                    if (d.lodLevel==model_core::kCoarseLod) {
                        if (d.chunkId!=(identity|model_core::kCoarseIdentity) || region->second.coarse || acceptance.coarseComplete) return invalid();
                        for (unsigned axis=0;axis<3;++axis)
                            if (d.localMin[axis]<scan.localMin[axis] || d.localMax[axis]>scan.localMax[axis]) return invalid();
                        const uint64_t primitives=d.topology==model_core::ChunkTopology::PointList ? d.vertexCount : d.indexCount/3;
                        if (!primitives || primitives>(scan.topology==model_core::ChunkTopology::PointList ? scan.vertexCount : scan.indexCount/3)) return invalid();
                        acceptance.coarsePrimitives+=primitives; acceptance.coarseBytes+=d.byteSize;
                        if (acceptance.coarsePrimitives>model_core::kCoarsePrimitiveLimit || acceptance.coarseBytes>model_core::kCoarseReservedBytes) return invalid();
                        region->second.coarse=true; ++acceptance.coarseRegions;
                    } else {
                        if (d.chunkId!=identity || !acceptance.coarseComplete || region->second.fine
                            || d.chunkChecksum!=scan.chunkChecksum || d.indexCount!=scan.indexCount || d.vertexCount!=scan.vertexCount
                            || std::memcmp(d.localMin,scan.localMin,sizeof(d.localMin))
                            || std::memcmp(d.localMax,scan.localMax,sizeof(d.localMax))) return invalid();
                        region->second.fine=true;
                    }
                }
            } else if (d.topology==model_core::ChunkTopology::CoarseComplete) {
                model_core::CoarseCompletePayload complete; std::memcpy(&complete,chunk.payload.data(),sizeof(complete));
                if (acceptance.coarseComplete || complete.regions!=acceptance.regions.size() || complete.regions!=acceptance.coarseRegions
                    || complete.primitives!=acceptance.coarsePrimitives || complete.geometryBytes!=acceptance.coarseBytes
                    || complete.primitives>model_core::CoarsePrimitiveCap(acceptance.triangles+acceptance.points,acceptance.regions.size())
                    || !acceptance.unresolved.empty()) return invalid();
                acceptance.coarseComplete=true;
            }
        }
        acceptance.Record(validation.chunks);
        if (!request.onBatch)
        {
            for (const auto& chunk : validation.chunks)
            {
                if (chunk.payload.size() > 128ull * 1024 * 1024 - accumulatedBytes)
                {
                    failure = Fail(ImportStage::ValidateSection, model_core::ImportErrorCode::ResourceLimit);
                    return false;
                }
                accumulatedBytes += chunk.payload.size();
            }
        }
        if (request.onBatch) {
            request.onBatch(std::move(validation.chunks));
            if (cancelProbe()) {
                failure = Fail(ImportStage::Cancelled);
                return false;
            }
        } else if (accumulated.empty()) {
            accumulated = std::move(validation.chunks);
        } else {
            accumulated.insert(accumulated.end(), std::make_move_iterator(validation.chunks.begin()),
                                std::make_move_iterator(validation.chunks.end()));
        }
        return true;
    };

    model_core::ReceivedControlMessage received{};
    ControlWaitOutcome outcome = ReadControlMessageBounded(controlOutput, replyTimeout, received, cancelProbe);

    while (outcome == ControlWaitOutcome::Ready && !failure) {
        const auto opcode = static_cast<model_core::ControlOpcode>(received.header.opcode);

        if (opcode == model_core::ControlOpcode::RequestSidecarFile
            && received.payload.size() == sizeof(model_core::RequestSidecarFileNotice)) {
            if (++sidecarRequestCount > request.maxSidecarRequestsPerGeneration) {
                break; // never trust worker self-restraint -- treated as a protocol violation below
            }

            model_core::RequestSidecarFileNotice sidecar{};
            std::memcpy(&sidecar, received.payload.data(), sizeof(sidecar));
            auto serviced = ServiceSidecarRequest(workerProcess, opened.canonicalPath, sidecar,
                                                  request.maxSidecarFileBytes,
                                                  12ull * 1024 * 1024 * 1024 - allSourceBytes,
                                                  /*allowPackageBasenameLookup=*/true);

            bool sentReply = false;
            if (const auto* ready = std::get_if<model_core::SidecarFileReadyNotice>(&serviced)) {
                allSourceBytes += ready->sidecarByteLength;
                sentReply = model_core::WriteControlMessage(controlInput,
                                                             model_core::ControlOpcode::SidecarFileReady, ready,
                                                             sizeof(*ready));
            } else if (const auto* unavailable = std::get_if<model_core::SidecarFileUnavailableNotice>(&serviced)) {
                sentReply = model_core::WriteControlMessage(controlInput,
                                                             model_core::ControlOpcode::SidecarFileUnavailable,
                                                             unavailable, sizeof(*unavailable));
            }
            if (!sentReply) {
                outcome = ControlWaitOutcome::Eof;
                break;
            }

            outcome = ReadControlMessageBounded(controlOutput, replyTimeout, received, cancelProbe);
            continue;
        }

        if (opcode == model_core::ControlOpcode::ChunkBatchReady
            && received.payload.size() == sizeof(model_core::ChunkBatchReadyNotice)) {
            model_core::ChunkBatchReadyNotice notice{};
            std::memcpy(&notice, received.payload.data(), sizeof(notice));

            // Checked before the section is even looked at: an out-of-order
            // or foreign-generation batch is rejected on its claim alone, so
            // a replayed index never gets as far as re-presenting bytes the
            // host already accepted.
            if (notice.generationId != request.generationId || notice.batchIndex != acceptance.nextBatchIndex) {
                failure = Fail(ImportStage::ChunkBatchOutOfOrder, model_core::ImportErrorCode::ImportProtocolViolation);
                break;
            }
            if (!acceptBatch(notice.chunkCount)) {
                break;
            }

            // Only now may the worker touch the window again. The batch is
            // already in host-owned memory, so whatever it writes next cannot
            // disturb what was accepted.
            model_core::ChunkBatchConsumedNotice ack{};
            ack.generationId = request.generationId;
            ack.batchIndex = notice.batchIndex;
            if (!model_core::WriteControlMessage(controlInput,
                                                  model_core::ControlOpcode::ChunkBatchConsumed, &ack, sizeof(ack))) {
                failure = Fail(ImportStage::ChunkBatchAckFailed);
                break;
            }

            outcome = ReadControlMessageBounded(controlOutput, replyTimeout, received, cancelProbe);
            continue;
        }

        if (opcode == model_core::ControlOpcode::StepProgress
            && received.payload.size() == sizeof(model_core::StepProgressNotice)) {
            // STEP-005: only the dedicated STEP host may publish progress. Any
            // other producer sending it is a protocol violation, not a
            // tolerated extra.
            if (!stepHost) {
                failure = Fail(ImportStage::UnexpectedReply, model_core::ImportErrorCode::ImportProtocolViolation);
                break;
            }
            model_core::StepProgressNotice notice{};
            std::memcpy(&notice, received.payload.data(), sizeof(notice));
            const bool knownPhase = notice.phase >= model_core::kStepPhasePreflight
                && notice.phase <= model_core::kStepPhaseEmit;
            if (notice.generationId != request.generationId || !knownPhase || notice.reserved0
                || notice.definitionsMeshed > notice.definitionTotal) {
                failure = Fail(ImportStage::UnexpectedReply, model_core::ImportErrorCode::ImportProtocolViolation);
                break;
            }
            if (++stepProgressCount > request.maxStepProgressPerGeneration) {
                failure = Fail(ImportStage::StepProgressLimit, model_core::ImportErrorCode::ResourceLimit);
                break;
            }
            lastStepProgress = notice;
            if (request.onStepProgress) request.onStepProgress(notice);
            outcome = ReadControlMessageBounded(controlOutput, replyTimeout, received, cancelProbe);
            continue;
        }

        break; // terminal reply, or something this loop does not service
    }

    // No wait-for-exit here: `proc` owns the Job Object handle, and dropping
    // it on return is the kill (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE). The
    // previous WaitForSingleObject(..., 5000) existed to let the worker exit
    // before its per-import AppContainer profile was deleted; there is no
    // per-import profile any more. Validation below never depended on the
    // worker being dead either -- ValidateAndCopySection copies the whole
    // section into private memory before trusting any of it, which is
    // precisely what the hostile-worker suite proves.

    // Every early exit from here on reports how many batches were accepted
    // before it, so a caller can tell "rejected the first window" from
    // "accepted four and then the worker misbehaved".
    auto fail = [&](ImportStage stage, model_core::ImportErrorCode code = model_core::ImportErrorCode::None) {
        ImportSessionResult result = Fail(stage, code);
        result.batchCount = acceptance.nextBatchIndex;
        result.workerProcessId = GetProcessId(workerProcess);
        result.stepProgressCount = stepProgressCount;
        result.lastStepProgress = lastStepProgress;
        return result;
    };

    auto acknowledgeCancellation = [&] {
        model_core::ReceivedControlMessage cancellationReply{};
        const auto grace = ReadControlMessageBounded(controlOutput, std::chrono::milliseconds(500),
                                                       cancellationReply);
        if (pooledLease && grace == ControlWaitOutcome::Ready
            && cancellationReply.header.opcode == uint32_t(model_core::ControlOpcode::GenerationError)
            && cancellationReply.payload.size() == sizeof(model_core::GenerationErrorNotice)) {
            model_core::GenerationErrorNotice notice{};
            std::memcpy(&notice, cancellationReply.payload.data(), sizeof(notice));
            pooledLease->reusable = notice.generationId == request.generationId
                && notice.errorCode == uint32_t(model_core::ImportErrorCode::Cancelled);
        }
    };

    if (failure) {
        failure->batchCount = acceptance.nextBatchIndex;
        failure->workerProcessId = GetProcessId(workerProcess);
        failure->stepProgressCount = stepProgressCount;
        failure->lastStepProgress = lastStepProgress;
        if (failure->stage == ImportStage::Cancelled) acknowledgeCancellation();
        return *failure;
    }

    if (sidecarRequestCount > request.maxSidecarRequestsPerGeneration) {
        return fail(ImportStage::SidecarRequestLimit);
    }

    if (outcome == ControlWaitOutcome::Cancelled) {
        // Cooperative first: the event wakes parser/decode and batch/detail
        // waits. A pooled worker must acknowledge before reuse; otherwise its
        // lease destructor replaces it after this 500 ms grace period.
        acknowledgeCancellation();
        return fail(ImportStage::Cancelled);
    }
    if (outcome == ControlWaitOutcome::TimedOut) {
        return fail(ImportStage::ReplyTimedOut);
    }
    if (outcome == ControlWaitOutcome::ProtocolViolation) return fail(ImportStage::UnexpectedReply);
    if (outcome != ControlWaitOutcome::Ready) {
        JOBOBJECT_LIMIT_VIOLATION_INFORMATION violation{};
        if (QueryInformationJobObject(workerJob, JobObjectLimitViolationInformation, &violation, sizeof(violation), nullptr)
            && (violation.ViolationLimitFlags & JOB_OBJECT_LIMIT_PROCESS_MEMORY))
            return fail(ImportStage::AwaitReply, model_core::ImportErrorCode::ResourceLimit);
        // Some Windows builds reap the process before the Job violation flag
        // becomes observable. Preserve the same typed mapping for the closed
        // NT memory-exhaustion statuses; fail-fast/access faults remain an
        // ordinary crash and cannot masquerade as a limit.
        DWORD exitCode = STILL_ACTIVE;
        if (GetExitCodeProcess(workerProcess, &exitCode)
            && (exitCode == 0xC0000017u  // STATUS_NO_MEMORY
                || exitCode == 0xC000012Du // STATUS_COMMITMENT_LIMIT
                || exitCode == 0xC00000A1u)) // STATUS_WORKING_SET_QUOTA
            return fail(ImportStage::AwaitReply, model_core::ImportErrorCode::ResourceLimit);
        // A non-default host commit limit is exposed solely as a
        // qualification seam. If that capped host disappears without a
        // frame, report the cap rather than an indistinguishable generic
        // crash; production's derived multi-GiB limit stays on the Job/NT-
        // status evidence paths above.
        if ((compatibility && request.compatibilityHostCommitLimitBytes != 0)
            || (stepHost && request.stepHostCommitLimitBytes != 0))
            return fail(ImportStage::AwaitReply, model_core::ImportErrorCode::ResourceLimit);
        return fail(ImportStage::AwaitReply);
    }

    if (received.header.opcode == static_cast<uint32_t>(model_core::ControlOpcode::GenerationError)) {
        model_core::GenerationErrorNotice notice{};
        if (received.payload.size() != sizeof(notice))
            return fail(ImportStage::UnexpectedReply);
        std::memcpy(&notice, received.payload.data(), sizeof(notice));
        if (notice.generationId != request.generationId || notice.reserved0 > uint32_t(model_core::ImportFailurePhase::Textures)
            || !model_core::IsKnownImportErrorCode(notice.errorCode))
            return fail(ImportStage::UnexpectedReply);
        if (notice.errorCode == uint32_t(model_core::ImportErrorCode::Cancelled))
        {
            if (pooledLease) pooledLease->reusable = true;
            return fail(ImportStage::Cancelled);
        }
        // UnsupportedComposition is a pre-publication classification, not a
        // late parser failure. Once any fast-path batch has been accepted the
        // producer is committed; treating a later fallback signal as eligible
        // would let one generation mix TinyUSDZ and OpenUSD output.
        if (notice.errorCode == uint32_t(model_core::ImportErrorCode::UnsupportedComposition)
            && (producer != ImportProducer::FastWorker
                || request.format != ImportFormat::Usd || acceptance.nextBatchIndex != 0))
            return fail(ImportStage::UnexpectedReply,
                        model_core::ImportErrorCode::ImportProtocolViolation);
        if (pooledLease) pooledLease->reusable = true;
        auto result = fail(ImportStage::WorkerReportedError, static_cast<model_core::ImportErrorCode>(notice.errorCode));
        result.errorPhase = static_cast<model_core::ImportFailurePhase>(notice.reserved0);
        result.compatibilityFallbackRequired =
            producer == ImportProducer::FastWorker
            && notice.errorCode == uint32_t(model_core::ImportErrorCode::UnsupportedComposition);
        return result;
    }

    if (received.header.opcode != static_cast<uint32_t>(model_core::ControlOpcode::ChunksReady)
        || received.payload.size() != sizeof(model_core::ChunksReadyNotice)) {
        return fail(ImportStage::UnexpectedReply);
    }

    // The terminal batch, accepted by exactly the same rules as every
    // non-terminal one -- including the batch cap, so a lone ChunksReady
    // still counts as batch 0 and a default maxChunkBatchesPerGeneration of 1
    // permits it and nothing more.
    model_core::ChunksReadyNotice finalNotice{};
    std::memcpy(&finalNotice, received.payload.data(), sizeof(finalNotice));
    if (finalNotice.generationId != request.generationId || finalNotice.reserved0)
        return fail(ImportStage::UnexpectedReply);
    if (!acceptBatch(finalNotice.chunkCount)) {
        failure->batchCount = acceptance.nextBatchIndex;
        return *failure;
    }
    if (!acceptance.unresolved.empty())
        return fail(ImportStage::ValidateSection, model_core::ImportErrorCode::MalformedData);
    if (!acceptance.sceneCatalog.nodes.empty()
        && (!acceptance.scene || acceptance.sceneCatalog.nodes.size() != acceptance.scene->nodeCount))
        return fail(ImportStage::ValidateSection, model_core::ImportErrorCode::MalformedData);
    const bool tierBResult = acceptance.scene
        && (acceptance.scene->format == model_core::SourceFormatId::AsciiStl
            || acceptance.scene->format == model_core::SourceFormatId::AsciiPly
            || acceptance.scene->format == model_core::SourceFormatId::Obj
            || acceptance.scene->format == model_core::SourceFormatId::Fbx
            || acceptance.scene->format == model_core::SourceFormatId::Usda
            || acceptance.scene->format == model_core::SourceFormatId::Usdc
            || acceptance.scene->format == model_core::SourceFormatId::Usdz
            || acceptance.scene->format == model_core::SourceFormatId::ThreeMf
            || acceptance.scene->format == model_core::SourceFormatId::Step);
    if (request.enableCoarseProxy && !tierBResult) {
        if (!acceptance.coarseComplete) return fail(ImportStage::ValidateSection,model_core::ImportErrorCode::MalformedData);
        for (const auto& [id,region]:acceptance.regions)
            if (!region.coarse || (!request.nextDetail && !region.fine))
            return fail(ImportStage::ValidateSection,model_core::ImportErrorCode::MalformedData);
    }
    // No ack for the terminal batch: there is no next write to gate, and the
    // worker is already on its way out.

    BY_HANDLE_FILE_INFORMATION sourceAfter{};
    if (!GetFileInformationByHandle(opened.file.get(), &sourceAfter)
        || sourceBefore.nFileSizeHigh != sourceAfter.nFileSizeHigh || sourceBefore.nFileSizeLow != sourceAfter.nFileSizeLow
        || CompareFileTime(&sourceBefore.ftLastWriteTime, &sourceAfter.ftLastWriteTime))
        return fail(ImportStage::ValidateSection, model_core::ImportErrorCode::FileChanged);
    bool haveGeometry = false;
    for (const auto& [id, topology] : acceptance.catalog)
        haveGeometry |= topology == model_core::ChunkTopology::TriangleList || topology == model_core::ChunkTopology::PointList;
    if (!haveGeometry) return fail(ImportStage::WorkerReportedError, model_core::ImportErrorCode::EmptyGeometry);
    ImportSessionResult result;
    result.ok = true;
    result.stage = ImportStage::Completed;
    result.chunks = std::move(accumulated);
    result.batchCount = acceptance.nextBatchIndex;
    result.sourceCatalog = std::move(sourceCatalog);
    result.sourceIdentity = sourceIdentity;
    result.workerProcessId = GetProcessId(workerProcess);
    result.stepProgressCount = stepProgressCount;
    result.lastStepProgress = lastStepProgress;
    if (request.nextDetail && !tierBResult) {
        if (!request.enableCoarseProxy || !request.onBatch || !request.onInitialComplete)
            return fail(ImportStage::UnexpectedReply);
        request.onInitialComplete(sourceIdentity);
        // A single outstanding request owns the reused section. No new sidecar
        // requests are permitted: replay uses only the worker's pinned handles.
        for (;;) {
            if (cancelProbe()) {
                model_core::ReceivedControlMessage cancellationReply{};
                const auto grace = ReadControlMessageBounded(controlOutput, std::chrono::milliseconds(500),
                                                               cancellationReply);
                if (pooledLease && grace == ControlWaitOutcome::Ready
                    && cancellationReply.header.opcode == uint32_t(model_core::ControlOpcode::GenerationError))
                    pooledLease->reusable = true;
                return fail(ImportStage::Cancelled);
            }
            if (WaitForSingleObject(workerProcess, 0) == WAIT_OBJECT_0)
                return fail(ImportStage::AwaitReply, model_core::ImportErrorCode::WorkerCrashed);
            if (!cpuBudgetAllows()) return fail(ImportStage::ValidateSection,model_core::ImportErrorCode::ResourceLimit);
            const uint32_t identity = request.nextDetail();
            if (!identity) { Sleep(20); continue; }
            const auto region = acceptance.regions.find(identity);
            if (region == acceptance.regions.end()) return fail(ImportStage::UnexpectedReply);
            model_core::DetailRequest detail{request.generationId, region->second.scan};
            if (!model_core::WriteControlMessage(controlInput, model_core::ControlOpcode::RequestDetail, &detail, sizeof(detail)))
                return fail(ImportStage::AwaitReply);
            const auto detailOutcome = ReadControlMessageBounded(controlOutput, replyTimeout, received, cancelProbe);
            if (detailOutcome == ControlWaitOutcome::Cancelled) {
                model_core::ReceivedControlMessage cancellationReply{};
                const auto grace = ReadControlMessageBounded(controlOutput, std::chrono::milliseconds(500),
                                                               cancellationReply);
                if (pooledLease && grace == ControlWaitOutcome::Ready
                    && cancellationReply.header.opcode == uint32_t(model_core::ControlOpcode::GenerationError))
                    pooledLease->reusable = true;
                return fail(ImportStage::Cancelled);
            }
            if (detailOutcome != ControlWaitOutcome::Ready) return fail(ImportStage::AwaitReply);
            if (received.header.opcode == uint32_t(model_core::ControlOpcode::GenerationError)
                && received.payload.size() == sizeof(model_core::GenerationErrorNotice)) {
                model_core::GenerationErrorNotice error; std::memcpy(&error, received.payload.data(), sizeof(error));
                if (error.generationId != request.generationId || !model_core::IsKnownImportErrorCode(error.errorCode))
                    return fail(ImportStage::UnexpectedReply);
                return fail(ImportStage::WorkerReportedError, model_core::ImportErrorCode(error.errorCode));
            }
            if (received.header.opcode != uint32_t(model_core::ControlOpcode::ChunksReady)
                || received.payload.size() != sizeof(model_core::ChunksReadyNotice)) return fail(ImportStage::UnexpectedReply);
            model_core::ChunksReadyNotice ready; std::memcpy(&ready, received.payload.data(), sizeof(ready));
            if (ready.generationId != request.generationId || ready.reserved0 || ready.chunkCount != 1)
                return fail(ImportStage::UnexpectedReply);
            acceptance.catalog.erase(identity); // permit this exact immutable replacement only
            auto validation = ValidateAndCopySection(view.bytes(), request.generationId, 1, &acceptance.catalog, false);
            acceptance.catalog.emplace(identity, region->second.scan.topology);
            if (!validation.ok)
                return fail(ImportStage::ValidateSection, validation.errorCode);
            if (validation.chunks.size() != 1)
                return fail(ImportStage::ValidateSection, model_core::ImportErrorCode::ImportProtocolViolation);
            const auto& actual = validation.chunks.front().descriptor;
            auto expected = region->second.scan;
            expected.chunkId = identity; expected.lodLevel = model_core::kFineLod;
            expected.normalizedRangeOffset = actual.normalizedRangeOffset;
            expected.normalizedRangeLength = actual.normalizedRangeLength;
            expected.byteSize = actual.byteSize;
            if (std::memcmp(&actual, &expected, sizeof(expected))
                || std::memcmp(&validation.chunks.front().scene, &*acceptance.scene, sizeof(model_core::SceneMetadata)))
                return fail(ImportStage::ValidateSection, model_core::ImportErrorCode::ImportProtocolViolation);
            BY_HANDLE_FILE_INFORMATION current{};
            if (!GetFileInformationByHandle(opened.file.get(), &current)
                || current.nFileSizeHigh != sourceBefore.nFileSizeHigh || current.nFileSizeLow != sourceBefore.nFileSizeLow
                || CompareFileTime(&current.ftLastWriteTime, &sourceBefore.ftLastWriteTime))
                return fail(ImportStage::ValidateSection, model_core::ImportErrorCode::FileChanged);
            if (!cpuBudgetAllows()) return fail(ImportStage::ValidateSection,model_core::ImportErrorCode::ResourceLimit);
            request.onBatch(std::move(validation.chunks));
        }
    }
    if (pooledLease) pooledLease->reusable = true;
    return result;
}

namespace {

ImportSessionResult MapCompatibilityFailure(ImportSessionResult result)
{
    result.producer = ImportProducer::CompatibilityHost;
    result.compatibilityFallbackRequired = false;
    if (result.ok || result.errorCode == model_core::ImportErrorCode::Cancelled)
        return result;

    using E = model_core::ImportErrorCode;
    switch (result.errorCode) {
    case E::ResourceLimit:
    case E::OutOfMemory:
    case E::PrimarySourceLimit:
    case E::AggregateSourceLimit:
    case E::ScratchLimit:
    case E::ChunkCatalogLimit:
    case E::ArchiveLimit:
    case E::CompatibilityHostLimit:
        result.errorCode = E::CompatibilityHostLimit;
        break;
    default:
        // Child-provided text/status is never surfaced. Launch, payload
        // integrity, resolver, crash, timeout, protocol and importer faults
        // collapse to the closed host-owned fact required by the UI contract.
        result.errorCode = E::CompatibilityHostFailure;
        break;
    }
    return result;
}

ImportSessionResult MapStepFailure(ImportSessionResult result)
{
    result.producer = ImportProducer::StepHost;
    result.compatibilityFallbackRequired = false;
    if (result.ok || result.errorCode == model_core::ImportErrorCode::Cancelled)
        return result;

    using E = model_core::ImportErrorCode;
    // A host-reported classification is a bounded product fact (admission
    // syntax, unsupported external content, empty geometry, ...) and is
    // preserved. Only its resource family is normalized to StepHostLimit.
    if (result.stage == ImportStage::WorkerReportedError) {
        switch (result.errorCode) {
        case E::ResourceLimit:
        case E::OutOfMemory:
        case E::PrimarySourceLimit:
        case E::AggregateSourceLimit:
        case E::ScratchLimit:
        case E::ChunkCatalogLimit:
        case E::ArchiveLimit:
            result.errorCode = E::StepHostLimit;
            break;
        default:
            break;
        }
        return result;
    }

    switch (result.errorCode) {
    case E::ResourceLimit:
    case E::OutOfMemory:
    case E::PrimarySourceLimit:
    case E::AggregateSourceLimit:
    case E::ScratchLimit:
    case E::ChunkCatalogLimit:
    case E::ArchiveLimit:
    case E::CompatibilityHostLimit:
    case E::StepHostLimit:
        result.errorCode = E::StepHostLimit;
        break;
    default:
        // Child-provided text/status is never surfaced. Launch, payload
        // integrity, crash, timeout and broker-validation faults collapse to
        // the closed host-owned fact required by the UI contract.
        result.errorCode = E::StepHostFailure;
        break;
    }
    return result;
}

} // namespace

ImportSessionResult RunImportSession(const ImportSessionRequest& request)
{
    // STEP has no fast-worker candidate and no fallback: the dedicated host is
    // the only producer allowed to emit SourceFormatId::Step.
    if (request.format == ImportFormat::Step) {
        if (request.stepHostExePath.empty())
            return Fail(ImportStage::LaunchWorker, model_core::ImportErrorCode::StepHostFailure);
        return MapStepFailure(RunImportSessionForProducer(request, ImportProducer::StepHost));
    }

    ImportSessionResult fast = RunImportSessionForProducer(request, ImportProducer::FastWorker);
    fast.producer = ImportProducer::FastWorker;

    if (request.format != ImportFormat::Usd || !fast.compatibilityFallbackRequired
        || request.compatibilityHostExePath.empty()) {
        return fast;
    }

    UsdFallbackState fallback(request.generationId);
    if (fast.ok || fast.batchCount != 0 || !fast.chunks.empty()
        || fallback.ObserveError(request.generationId, UsdProducer::TinyUsdz,
                                 fast.errorCode)
            != UsdFallbackObservation::StartCompatibility) {
        fast.ok = false;
        fast.stage = ImportStage::UnexpectedReply;
        fast.errorCode = model_core::ImportErrorCode::ImportProtocolViolation;
        fast.compatibilityFallbackRequired = false;
        return fast;
    }

    // RunImportSessionForProducer creates a fresh section, catalog, source
    // handle duplication and process lease. Nothing from the discarded fast
    // attempt is passed to the compatibility producer except the immutable
    // request/generation and broker path authority.
    ImportSessionResult compatibility = MapCompatibilityFailure(
        RunImportSessionForProducer(request, ImportProducer::CompatibilityHost));

    if (compatibility.ok) {
        if (fallback.ObserveComplete(request.generationId, UsdProducer::OpenUsd)
            != UsdFallbackObservation::Accepted) {
            compatibility.ok = false;
            compatibility.stage = ImportStage::UnexpectedReply;
            compatibility.errorCode = model_core::ImportErrorCode::CompatibilityHostFailure;
        }
    } else if (compatibility.errorCode != model_core::ImportErrorCode::Cancelled) {
        // Feed a non-fallback error through the closed state machine. The
        // mapped host fact can never request another or reverse fallback.
        if (fallback.ObserveError(request.generationId, UsdProducer::OpenUsd,
                                  compatibility.errorCode)
            == UsdFallbackObservation::ProtocolViolation) {
            compatibility.errorCode = model_core::ImportErrorCode::CompatibilityHostFailure;
        }
    }
    return compatibility;
}

} // namespace import_broker
