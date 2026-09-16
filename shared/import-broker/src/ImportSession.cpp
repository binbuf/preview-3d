#include "model_core/TierALimits.h"
#include "import_broker/ImportSession.h"
#include "model_core/VertexLayouts.h"

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

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

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
    if (code == model_core::ImportErrorCode::None) {
        using E = model_core::ImportErrorCode;
        switch (stage) {
        case ImportStage::Cancelled: code = E::Cancelled; break;
        case ImportStage::OpenSource: code = E::FileUnavailable; break;
        case ImportStage::ReplyTimedOut: code = E::WorkerTimedOut; break;
        case ImportStage::SendRequest: case ImportStage::AwaitReply: case ImportStage::ChunkBatchAckFailed: code = E::WorkerCrashed; break;
        case ImportStage::SidecarRequestLimit: case ImportStage::ChunkBatchLimit: case ImportStage::ChunkCountLimit: code = E::ResourceLimit; break;
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

    std::wstring workerArgs = request.workerArgumentsOverride.empty()
        ? std::wstring(ParseFlagFor(request.format))
        : request.workerArgumentsOverride;
    if (request.enableCoarseProxy && request.workerArgumentsOverride.empty()) workerArgs += L"-proxy";
    std::wstring cmdLine = L"\"" + request.workerExePath + L"\" " + workerArgs;

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
    uint32_t sidecarRequestCount = 0;
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
                &acceptance.images,acceptance.textureBytes,acceptance.texturePixels);
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
            const auto expectedFormat = request.format == ImportFormat::Gltf ? model_core::SourceFormatId::Gltf
                : request.format == ImportFormat::Stl ? model_core::SourceFormatId::Stl : model_core::SourceFormatId::Ply;
            if (request.workerArgumentsOverride.empty() && chunk.scene.format != expectedFormat
                && !(request.format == ImportFormat::Gltf && chunk.scene.format == model_core::SourceFormatId::Glb)) {
                failure = Fail(ImportStage::ValidateSection, model_core::ImportErrorCode::ImportProtocolViolation);
                return false;
            }
        }
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
            if (request.enableCoarseProxy && d.lodLevel != model_core::kScanLod) continue;
            if (d.topology == model_core::ChunkTopology::TriangleList)
                newTriangles += d.indexCount / 3;
            if (d.topology == model_core::ChunkTopology::PointList)
                newPoints += d.vertexCount;
            if (d.topology == model_core::ChunkTopology::TriangleList ||
                d.topology == model_core::ChunkTopology::PointList)
                newVertices += d.vertexCount;
        }
        if (newTriangles > model_core::kTierATriangleLimit || newPoints > model_core::kTierAPointLimit ||
            newVertices > model_core::kTierAVertexLimit)
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
                     chunk.scene.format == model_core::SourceFormatId::Ply) &&
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
                if (!request.enableCoarseProxy || d.lodLevel == model_core::kScanLod)
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
        if (request.enableCoarseProxy && havePreview && haveOtherGeometry) {
            failure=Fail(ImportStage::ValidateSection,model_core::ImportErrorCode::MalformedData); return false;
        }
        for (const auto& chunk:validation.chunks) if (chunk.descriptor.lodLevel==model_core::kCoarseLod && request.enableCoarseProxy) {
            coarseVertexBatch+=uint64_t(chunk.descriptor.vertexCount)*model_core::VertexStrideForLayout(model_core::VertexLayoutId(chunk.descriptor.vertexLayoutId));
            coarseIndexBatch+=uint64_t(chunk.descriptor.indexCount)*4;
        }
        acceptance.coarseAllocationBytes+=(coarseVertexBatch+65535)/65536*65536+(coarseIndexBatch+65535)/65536*65536;
        for (const auto& chunk:validation.chunks) if (chunk.descriptor.lodLevel==model_core::kPreviewLod && request.enableCoarseProxy) {
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
            if (!request.enableCoarseProxy) {
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
                        || d.nodeId!=scan.nodeId || d.geometryFlags!=scan.geometryFlags
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
            if (request.isCancelled && request.isCancelled()) {
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
    ControlWaitOutcome outcome = ReadControlMessageBounded(controlOutRead.get(), replyTimeout, received, request.isCancelled);

    while (outcome == ControlWaitOutcome::Ready && !failure) {
        const auto opcode = static_cast<model_core::ControlOpcode>(received.header.opcode);

        if (opcode == model_core::ControlOpcode::RequestSidecarFile
            && received.payload.size() == sizeof(model_core::RequestSidecarFileNotice)) {
            if (++sidecarRequestCount > request.maxSidecarRequestsPerGeneration) {
                break; // never trust worker self-restraint -- treated as a protocol violation below
            }

            model_core::RequestSidecarFileNotice sidecar{};
            std::memcpy(&sidecar, received.payload.data(), sizeof(sidecar));
            auto serviced = ServiceSidecarRequest(proc->process.get(), opened.canonicalPath, sidecar,
                                                  request.maxSidecarFileBytes,
                                                  12ull * 1024 * 1024 * 1024 - allSourceBytes);

            bool sentReply = false;
            if (const auto* ready = std::get_if<model_core::SidecarFileReadyNotice>(&serviced)) {
                allSourceBytes += ready->sidecarByteLength;
                sentReply = model_core::WriteControlMessage(controlInWrite.get(),
                                                             model_core::ControlOpcode::SidecarFileReady, ready,
                                                             sizeof(*ready));
            } else if (const auto* unavailable = std::get_if<model_core::SidecarFileUnavailableNotice>(&serviced)) {
                sentReply = model_core::WriteControlMessage(controlInWrite.get(),
                                                             model_core::ControlOpcode::SidecarFileUnavailable,
                                                             unavailable, sizeof(*unavailable));
            }
            if (!sentReply) {
                outcome = ControlWaitOutcome::Eof;
                break;
            }

            outcome = ReadControlMessageBounded(controlOutRead.get(), replyTimeout, received, request.isCancelled);
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
            if (!model_core::WriteControlMessage(controlInWrite.get(),
                                                  model_core::ControlOpcode::ChunkBatchConsumed, &ack, sizeof(ack))) {
                failure = Fail(ImportStage::ChunkBatchAckFailed);
                break;
            }

            outcome = ReadControlMessageBounded(controlOutRead.get(), replyTimeout, received, request.isCancelled);
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
        return result;
    };

    if (failure) {
        failure->batchCount = acceptance.nextBatchIndex;
        return *failure;
    }

    if (sidecarRequestCount > request.maxSidecarRequestsPerGeneration) {
        return fail(ImportStage::SidecarRequestLimit);
    }

    if (outcome == ControlWaitOutcome::Cancelled) {
        return fail(ImportStage::Cancelled);
    }
    if (outcome == ControlWaitOutcome::TimedOut) {
        return fail(ImportStage::ReplyTimedOut);
    }
    if (outcome == ControlWaitOutcome::ProtocolViolation) return fail(ImportStage::UnexpectedReply);
    if (outcome != ControlWaitOutcome::Ready) {
        JOBOBJECT_LIMIT_VIOLATION_INFORMATION violation{};
        if (QueryInformationJobObject(proc->job.get(), JobObjectLimitViolationInformation, &violation, sizeof(violation), nullptr)
            && (violation.ViolationLimitFlags & JOB_OBJECT_LIMIT_PROCESS_MEMORY))
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
            return fail(ImportStage::Cancelled);
        auto result = fail(ImportStage::WorkerReportedError, static_cast<model_core::ImportErrorCode>(notice.errorCode));
        result.errorPhase = static_cast<model_core::ImportFailurePhase>(notice.reserved0);
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
    if (request.enableCoarseProxy) {
        if (!acceptance.coarseComplete) return fail(ImportStage::ValidateSection,model_core::ImportErrorCode::MalformedData);
        for (const auto& [id,region]:acceptance.regions) if (!region.coarse || !region.fine)
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
    return result;
}

} // namespace import_broker
