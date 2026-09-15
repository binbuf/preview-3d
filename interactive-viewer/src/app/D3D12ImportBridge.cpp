#include "D3D12ImportBridge.h"
#include <cstdio>

#include "import_broker/ImportSession.h"
#include "import_broker/SharedSection.h"
#include "model_core/PixelFormats.h"

#include <windows.h>

#include <cstring>
#include <cwctype>

namespace d3d12_import_bridge {

namespace {

std::wstring ToLower(std::wstring s)
{
    for (wchar_t& c : s) {
        c = static_cast<wchar_t>(towlower(c));
    }
    return s;
}

std::wstring ExtensionOf(const std::wstring& path)
{
    auto dot = path.find_last_of(L'.');
    auto slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) {
        return {};
    }
    return ToLower(path.substr(dot + 1));
}

std::wstring ResolveWorkerExePath()
{
    wchar_t modulePath[MAX_PATH]{};
    DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring path(modulePath, length);
    auto lastSlash = path.find_last_of(L"\\/");
    std::wstring directory = (lastSlash == std::wstring::npos) ? L"." : path.substr(0, lastSlash);
    return directory + L"\\Preview3DImportWorker.exe";
}

void DescribeImportError(model_core::ImportErrorCode code, std::wstring& summary, std::wstring& details)
{
    switch (code) {
    case model_core::ImportErrorCode::UnsupportedEncoding:
        summary = L"This encoding is not supported.";
        details = L"Use glTF 2.0, binary STL, or binary little/big-endian PLY. ASCII STL and PLY are deferred."; return;
    case model_core::ImportErrorCode::WorkerCrashed:
        summary = L"The sandboxed importer stopped unexpectedly.";
        details = L"The worker exited before completing this model. Retry or open another model."; return;
    case model_core::ImportErrorCode::UnsupportedRequiredFeature:
        summary = L"This model requires an unsupported feature.";
        details = L"Export a static glTF model using the supported extensions."; return;
    case model_core::ImportErrorCode::EmptyGeometry:
        summary = L"This model has no displayable geometry.";
        details = L"No valid triangles or points remain in the selected scene."; return;
    case model_core::ImportErrorCode::OutOfMemory:
        summary = L"There is not enough memory to preview this model.";
        details = L"Close other applications or export a smaller model, then retry."; return;
    case model_core::ImportErrorCode::FileChanged:
        summary = L"The source changed during import.";
        details = L"Save a stable local copy of the model and its sidecars, then retry."; return;
    case model_core::ImportErrorCode::ImportProtocolViolation:
        summary = L"The importer returned invalid data.";
        details = L"The sandbox response failed validation and was discarded."; return;
    case model_core::ImportErrorCode::MalformedData:
        summary = L"This file could not be read.";
        details = L"The importer found data that doesn't match the expected file format.";
        return;
    case model_core::ImportErrorCode::ResourceLimit:
        summary = L"This model is too large to preview.";
        details = L"The file exceeds a resource limit the sandboxed importer enforces.";
        return;
    case model_core::ImportErrorCode::UnsafeReference:
        summary = L"This model could not be previewed.";
        details = L"The file references another file in a way that isn't allowed.";
        return;
    case model_core::ImportErrorCode::FileUnavailable:
        summary = L"This model could not be previewed.";
        details = L"A file this model depends on could not be opened.";
        return;
    case model_core::ImportErrorCode::InternalImporterFailure:
    case model_core::ImportErrorCode::None:
    default:
        summary = L"This model could not be previewed.";
        details = L"The importer stopped unexpectedly while reading the file.";
        return;
    }
}

// Turns a typed session failure into the user-facing pair. Every host-side
// plumbing stage keeps the distinct wording it had when this sequence lived
// inline here; only the two stages that carry a worker/validator error code
// defer to DescribeImportError.
void DescribeSessionFailureInternal(const import_broker::ImportSessionResult& session, std::wstring& summary,
                             std::wstring& details)
{
    using import_broker::ImportStage;
    switch (session.stage) {
    case ImportStage::OpenSource:
        summary = L"This file could not be opened.";
        details = session.openError;
        return;
    case ImportStage::DuplicateSourceHandle:
        summary = L"This file could not be prepared for preview.";
        details = L"The file handle could not be shared with the sandboxed importer.";
        return;
    case ImportStage::CreateOutputSection:
        summary = L"This model could not be previewed.";
        details = L"A shared memory section for the importer's output could not be created.";
        return;
    case ImportStage::CreateSandboxProfile:
        summary = L"This model could not be previewed.";
        details = L"The sandbox container for the importer could not be created.";
        return;
    case ImportStage::CreateControlChannel:
        summary = L"This model could not be previewed.";
        details = L"The control channel to the sandboxed importer could not be created.";
        return;
    case ImportStage::LaunchWorker:
        summary = L"This model could not be previewed.";
        details = L"The sandboxed importer process could not be started.";
        return;
    case ImportStage::ResumeWorker:
        summary = L"This model could not be previewed.";
        details = L"The sandboxed importer process could not be resumed.";
        return;
    case ImportStage::SendRequest:
        summary = L"This model could not be previewed.";
        details = L"The import request could not be sent to the sandboxed importer.";
        return;
    case ImportStage::SidecarRequestLimit:
        summary = L"This model could not be previewed.";
        details = L"The sandboxed importer made too many file requests.";
        return;
    case ImportStage::AwaitReply:
        summary = L"This model could not be previewed.";
        details = L"The sandboxed importer did not respond.";
        return;
    case ImportStage::ReplyTimedOut:
        summary = L"This model could not be previewed.";
        details = L"The sandboxed importer stopped responding and was shut down.";
        return;
    case ImportStage::Cancelled:
        // Superseded or closing: the caller drops the result rather than
        // showing it, so this text exists only so no path returns empty.
        summary = L"This preview was cancelled.";
        details = L"A newer file was opened, or the window was closed.";
        return;
    case ImportStage::UnexpectedReply:
        summary = L"This model could not be previewed.";
        details = L"The sandboxed importer returned an unexpected response.";
        return;
    case ImportStage::MapOutputSection:
        summary = L"This model could not be previewed.";
        details = L"The importer's output could not be read.";
        return;
    case ImportStage::WorkerReportedError:
    case ImportStage::ValidateSection:
    case ImportStage::Completed:
    default:
        DescribeImportError(session.errorCode, summary, details);
        return;
    }
}

import_broker::ImportFormat ToBrokerFormat(SourceFormat format)
{
    switch (format) {
    case SourceFormat::Stl:
        return import_broker::ImportFormat::Stl;
    case SourceFormat::Ply:
        return import_broker::ImportFormat::Ply;
    case SourceFormat::Glb:
    default:
        return import_broker::ImportFormat::Gltf;
    }
}

constexpr uint32_t kMaxSidecarRequestsPerGeneration = 64;
constexpr uint64_t kMaxSidecarFileBytes = 256ull * 1024ull * 1024ull;

// Provisional finite round-trip ceiling retained for this slice. TSK-205
// derives replacement batch/catalog ceilings from normalized expansion;
// source bytes do not bound normalized bytes (e.g. instances/compression).
constexpr uint32_t kMaxChunkBatchesPerGeneration = 256;

} // namespace

void DescribeSessionFailure(const import_broker::ImportSessionResult& session, std::wstring& summary, std::wstring& details)
{
    DescribeSessionFailureInternal(session, summary, details);
    if (session.stage == import_broker::ImportStage::ValidateSection
        && session.errorCode == model_core::ImportErrorCode::MalformedData)
        DescribeImportError(model_core::ImportErrorCode::ImportProtocolViolation, summary, details);
    // Codes with a specific document explanation override generic stage text.
    if (session.errorCode == model_core::ImportErrorCode::FileChanged
        || session.errorCode == model_core::ImportErrorCode::OutOfMemory
        || session.errorCode == model_core::ImportErrorCode::WorkerCrashed
        || session.errorCode == model_core::ImportErrorCode::ResourceLimit)
        DescribeImportError(session.errorCode, summary, details);
}

std::wstring SourceFormatLabel(const std::wstring& path)
{
    const auto ext = ExtensionOf(path);
    if (ext == L"gltf") return L"glTF";
    if (ext == L"glb") return L"GLB";
    if (ext == L"stl") return L"STL";
    if (ext == L"ply") return L"PLY";
    // Extension only, capped and restricted to printable alphanumerics.
    if (ext.empty() || ext.size() > 16) return L"Unknown";
    std::wstring label;
    for (auto c : ext) {
        if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'))) return L"Unknown";
        label += wchar_t(towupper(c));
    }
    return label;
}

std::wstring StageLabel(import_broker::ImportStage stage)
{
    using import_broker::ImportStage;
    switch (stage) {
    case ImportStage::Completed: return L"complete";
    case ImportStage::OpenSource: return L"opening source";
    case ImportStage::WorkerReportedError: return L"parsing / decoding";
    case ImportStage::ValidateSection: case ImportStage::UnexpectedReply:
    case ImportStage::ChunkBatchOutOfOrder: return L"validating import";
    case ImportStage::ReplyTimedOut: case ImportStage::AwaitReply: return L"waiting for importer";
    case ImportStage::SidecarRequestLimit: return L"resolving sidecars";
    case ImportStage::ChunkBatchLimit: case ImportStage::ChunkCountLimit: return L"accepting batches";
    case ImportStage::ChunkBatchAckFailed: return L"acknowledging batch";
    case ImportStage::Upload: return L"uploading geometry / textures";
    case ImportStage::Cancelled: return L"cancelled";
    default: return L"preparing sandbox";
    }
}

std::wstring DiagnosticDetails(const std::wstring& path, const ImportResult& result)
{
    return result.errorSummary + L"\r\n\r\n" + result.errorDetails + L"\r\n\r\nFormat: "
        + SourceFormatLabel(path) + L"\r\nPhase: " + FailurePhaseLabel(result)
        + L"\r\nCode: " + std::to_wstring(uint32_t(result.errorCode));
}

std::wstring FailurePhaseLabel(const ImportResult& result)
{
    if (result.errorStage == import_broker::ImportStage::WorkerReportedError) {
        switch (result.errorPhase) {
        case model_core::ImportFailurePhase::Geometry: return L"parsing geometry";
        case model_core::ImportFailurePhase::Sidecars: return L"resolving sidecars";
        case model_core::ImportFailurePhase::Textures: return L"decoding textures";
        default: break;
        }
    }
    return StageLabel(result.errorStage);
}

std::optional<SourceFormat> ClassifyByExtension(const std::wstring& path)
{
    std::wstring ext = ExtensionOf(path);
    if (ext == L"glb" || ext == L"gltf") return SourceFormat::Glb;
    if (ext == L"stl") return SourceFormat::Stl;
    if (ext == L"ply") return SourceFormat::Ply;
    return std::nullopt;
}

void EnsureImportSandboxPrepared()
{
    import_broker::PrepareImportSandbox(ResolveWorkerExePath());
}

ImportResult RunImport(SourceFormat format, const std::wstring& path, uint64_t generationId,
                        std::function<bool()> isCancelled, std::function<void(ImportResult)> onBatch, uint64_t sectionBytes, bool delayBatchesForTesting, uint32_t faultForTesting)
{
    ImportResult result;

    import_broker::ImportSessionRequest sessionRequest;
    sessionRequest.isCancelled = std::move(isCancelled);
    sessionRequest.workerExePath = ResolveWorkerExePath();
    sessionRequest.sourcePath = path;
    sessionRequest.format = ToBrokerFormat(format);
    sessionRequest.generationId = generationId;
    sessionRequest.sectionByteCapacity = sectionBytes;
    if (delayBatchesForTesting && format == SourceFormat::Glb)
        sessionRequest.workerArgumentsOverride = L"--parse-gltf-delayed-batches";
    sessionRequest.maxChunksPerGeneration = 65536;
    sessionRequest.maxChunkCount = import_broker::kImportMaxChunkCount;
    sessionRequest.maxSidecarRequestsPerGeneration = kMaxSidecarRequestsPerGeneration;
    sessionRequest.maxSidecarFileBytes = kMaxSidecarFileBytes;
    sessionRequest.maxChunkBatchesPerGeneration = kMaxChunkBatchesPerGeneration;
    if (faultForTesting == 1) sessionRequest.workerArgumentsOverride = L"--child-noop";
    if (faultForTesting == 2) {
        sessionRequest.workerArgumentsOverride = L"--test-hang-import";
        sessionRequest.replyTimeoutMs = 500;
    }
    if (faultForTesting == 3) sessionRequest.maxChunkCount = 0;
    if (faultForTesting == 5) sessionRequest.workerArgumentsOverride = L"--test-invalid-import-reply";
    import_broker::KnownChunkCatalog catalog;
    auto unpack = [&](std::vector<import_broker::ValidatedChunk> chunks) {
        ImportResult result;
        result.forceUploadFailureForTesting = faultForTesting == 4;
        if (!chunks.empty()) result.scene = chunks.front().scene;
        for (const auto& chunk : chunks) catalog.emplace(chunk.descriptor.chunkId, chunk.descriptor.topology);
        for (auto& chunk : chunks) {
            switch (chunk.descriptor.topology) {
            case model_core::ChunkTopology::TriangleList:
            case model_core::ChunkTopology::PointList: {
                ImportedMesh mesh;
                mesh.geometry = chunk.descriptor;
                mesh.chunkId = chunk.descriptor.chunkId;
                mesh.topology = chunk.descriptor.topology;
                mesh.vertexLayoutId = static_cast<model_core::VertexLayoutId>(chunk.descriptor.vertexLayoutId);
                mesh.vertexCount = chunk.descriptor.vertexCount;
                mesh.indexCount = chunk.descriptor.indexCount;
                mesh.payload = std::move(chunk.payload);
                // Per WireFormat.h: a mesh's dependencyIds[0] is only a
                // material reference when the target chunk's own topology is
                // Material -- the same slot predates this and is also used for
                // an unrelated LOD/derivation relationship elsewhere, so the
                // target's topology (not the slot position) determines meaning.
                if (chunk.descriptor.dependencyCount >= 1) {
                    uint32_t targetId = chunk.descriptor.dependencyIds[0];
                    auto target = catalog.find(targetId);
                    if (target == catalog.end() || target->second == model_core::ChunkTopology::Material)
                        mesh.materialChunkId = targetId;
                }
                result.meshes.push_back(std::move(mesh));
                break;
            }
            case model_core::ChunkTopology::Material: {
                ImportedMaterial material;
                material.chunkId = chunk.descriptor.chunkId;
                if (chunk.payload.size() == sizeof(model_core::MaterialPayload)) {
                    std::memcpy(&material.data, chunk.payload.data(), sizeof(material.data));
                }
                if (chunk.descriptor.dependencyIds[0]) material.baseColorImageChunkId = chunk.descriptor.dependencyIds[0];
                if (chunk.descriptor.dependencyIds[1]) material.metallicRoughnessImageChunkId = chunk.descriptor.dependencyIds[1];
                if (chunk.descriptor.dependencyIds[2]) material.normalImageChunkId = chunk.descriptor.dependencyIds[2];
                if (chunk.descriptor.dependencyIds[3]) material.emissiveImageChunkId = chunk.descriptor.dependencyIds[3];
                result.materials.push_back(std::move(material));
                break;
            }
            case model_core::ChunkTopology::Image: {
                ImportedImage image;
                image.chunkId = chunk.descriptor.chunkId;
                if (chunk.payload.size() >= sizeof(model_core::ImagePayloadHeader)) {
                    model_core::ImagePayloadHeader header{};
                    std::memcpy(&header, chunk.payload.data(), sizeof(header));
                    image.logicalChunkId=header.reserved0;
                    image.pixelFormat = static_cast<model_core::PixelFormatId>(header.pixelFormat);
                    image.width = header.width;
                    image.height = header.height;
                    image.mipLevels = header.mipLevels;
                    image.colorSpace = static_cast<model_core::ColorSpaceId>(header.colorSpace);
                    image.pixelBytes.assign(chunk.payload.begin() + sizeof(header), chunk.payload.end());
                }
                result.images.push_back(std::move(image));
                break;
            }
            case model_core::ChunkTopology::ImportStatus:
                std::memcpy(&result.status,chunk.payload.data(),sizeof(result.status));
                break;
            case model_core::ChunkTopology::TextureWarning:
                std::memcpy(&result.textureWarningCount,chunk.payload.data(),sizeof(uint32_t));
                break;
            default:
                break; // unrecognized topology already rejected by the validator; never reached
            }
        }
        result.ok = true;
        return result;
    };
    if (onBatch) sessionRequest.onBatch = [&](auto chunks) { onBatch(unpack(std::move(chunks))); };
    auto session = import_broker::RunImportSession(sessionRequest);
    if (!session.ok) {
        if (delayBatchesForTesting && session.stage!=import_broker::ImportStage::Cancelled) std::fprintf(stderr,"Import smoke failure: stage %u code %u\n",unsigned(session.stage),unsigned(session.errorCode));
        result.errorCode = session.errorCode;
        if (session.stage == import_broker::ImportStage::ValidateSection && result.errorCode == model_core::ImportErrorCode::MalformedData)
            result.errorCode = model_core::ImportErrorCode::ImportProtocolViolation;
        result.errorStage = session.stage;
        result.errorPhase = session.errorPhase;
        DescribeSessionFailure(session, result.errorSummary, result.errorDetails);
        return result;
    }
    if (!onBatch) return unpack(std::move(session.chunks));
    result.ok = true;
    return result;
}

} // namespace d3d12_import_bridge
