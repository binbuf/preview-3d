#include "model_core/TierALimits.h"
#include "D3D12ImportBridge.h"
#include <cstdio>

#include "import_broker/ImportSession.h"
#include "import_broker/SharedSection.h"
#include "model_core/PixelFormats.h"

#include <windows.h>

#include <cstring>
#include <cwctype>
#include <functional>
#include <unordered_map>

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
    // Portable releases keep the AppContainer payload in its own directory.
    // ImportSession grants read/execute to the worker executable's directory,
    // so this layout avoids granting the sandbox access to the viewer, docs,
    // or cleanup tooling. Developer/test builds retain the shared-OutDir
    // fallback used by the solution.
    const std::wstring packaged = directory + L"\\worker\\Preview3DImportWorker.exe";
    if (GetFileAttributesW(packaged.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return packaged;
    }
    return directory + L"\\Preview3DImportWorker.exe";
}

std::wstring ResolveCompatibilityHostExePath()
{
    wchar_t modulePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    const std::wstring path(modulePath, length);
    const auto lastSlash = path.find_last_of(L"\\/");
    const std::wstring directory = (lastSlash == std::wstring::npos) ? L"." : path.substr(0, lastSlash);
    // Both packaged and solution builds keep OpenUSD's executable, DLLs, and
    // hash-verified resources in this private sibling directory.
    return directory + L"\\OpenUsdHost\\Preview3DImportHost.exe";
}

std::wstring ResolveStepHostExePath()
{
    wchar_t modulePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    const std::wstring path(modulePath, length);
    const auto lastSlash = path.find_last_of(L"\\/");
    const std::wstring directory = (lastSlash == std::wstring::npos) ? L"." : path.substr(0, lastSlash);
    // The dedicated OCCT STEP host and its signed payload closure live in
    // their own sibling directory, separate from the viewer, the general
    // worker, and the USD compatibility host.
    return directory + L"\\StepHost\\Preview3DStepHost.exe";
}

void DescribeImportError(model_core::ImportErrorCode code, std::wstring& summary, std::wstring& details)
{
    switch (code) {
    case model_core::ImportErrorCode::UnsupportedEncoding:
        summary = L"This encoding is not supported.";
        details = L"Use a supported glTF, STL, PLY, OBJ, FBX, USDA, USDC, or USDZ encoding."; return;
    case model_core::ImportErrorCode::WorkerCrashed:
        summary = L"The sandboxed importer stopped unexpectedly.";
        details = L"The worker exited before completing this model. Retry or open another model."; return;
    case model_core::ImportErrorCode::UnsupportedRequiredFeature:
        summary = L"This model requires an unsupported feature.";
        details = L"Export a static model using the documented supported subset."; return;
    case model_core::ImportErrorCode::UnsupportedComposition:
        summary = L"This USD stage requires compatibility import.";
        details = L"The fast importer requested the isolated compatibility host."; return;
    case model_core::ImportErrorCode::CompatibilityHostFailure:
        summary = L"The USD compatibility importer stopped unexpectedly.";
        details = L"The isolated compatibility host could not complete this model."; return;
    case model_core::ImportErrorCode::CompatibilityHostLimit:
        summary = L"This USD stage is too large or complex to preview.";
        details = L"The compatibility host reached a bounded resource limit."; return;
    case model_core::ImportErrorCode::StepHostFailure:
        summary = L"The STEP importer stopped unexpectedly.";
        details = L"The isolated STEP host could not complete this model."; return;
    case model_core::ImportErrorCode::StepHostLimit:
        summary = L"This STEP model is too large or complex to preview.";
        details = L"The STEP host reached a bounded resource limit."; return;
    case model_core::ImportErrorCode::TessellationFailed:
        summary = L"This STEP model could not be tessellated.";
        details = L"One or more shapes exceeded the supported CAD tessellation budget. Export a simpler solid or assembly."; return;
    case model_core::ImportErrorCode::ArchiveLimit:
        summary = L"This model archive is not supported.";
        details = L"The archive violates a path, structure, compression, or expansion limit."; return;
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
    case model_core::ImportErrorCode::PrimarySourceLimit:
        summary = L"The primary source exceeds the import limit.";
        details = L"ASCII STL/PLY files are limited to 2 GiB; Tier A primary files are limited to 8 GiB.";
        return;
    case model_core::ImportErrorCode::AggregateSourceLimit:
        summary = L"The model and sidecars exceed the import limit.";
        details = L"Combined local source files are limited to 12 GiB.";
        return;
    case model_core::ImportErrorCode::ScratchLimit:
        summary = L"The importer scratch budget was exceeded.";
        details = L"Import scratch is limited to 1 GiB or 25% of physical RAM, whichever is lower.";
        return;
    case model_core::ImportErrorCode::ChunkCatalogLimit:
        summary = L"The model has too many source ranges.";
        details = L"The bounded geometry, material and image catalog cannot accept more entries.";
        return;
    case model_core::ImportErrorCode::DracoPrimitiveLimit:
        summary = L"A compressed primitive exceeds the decode limit.";
        details =
            L"One Draco primitive is limited to 512 MiB of estimated decode work and 10 million triangles.";
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
    case SourceFormat::Obj:
        return import_broker::ImportFormat::Obj;
    case SourceFormat::Fbx:
        return import_broker::ImportFormat::Fbx;
    case SourceFormat::ThreeMf:
        return import_broker::ImportFormat::ThreeMf;
    case SourceFormat::Usd:
        return import_broker::ImportFormat::Usd;
    case SourceFormat::Step:
        return import_broker::ImportFormat::Step;
    case SourceFormat::Glb:
    default:
        return import_broker::ImportFormat::Gltf;
    }
}

constexpr uint32_t kMaxSidecarRequestsPerGeneration = 64;
constexpr uint64_t kMaxSidecarFileBytes = 8ull * 1024ull * 1024ull * 1024ull;

// Derived from normalized expansion, logical occurrence tails, material slots
// and immutable texture replacements; also safe for one-chunk test windows.
constexpr uint32_t kMaxChunkBatchesPerGeneration = model_core::kTierABatchLimit;

} // namespace

void DescribeSessionFailure(const import_broker::ImportSessionResult& session, std::wstring& summary, std::wstring& details)
{
    DescribeSessionFailureInternal(session, summary, details);
    if (session.stage == import_broker::ImportStage::ValidateSection
        && session.errorCode == model_core::ImportErrorCode::MalformedData)
        DescribeImportError(model_core::ImportErrorCode::ImportProtocolViolation, summary, details);
    // Codes with a specific document explanation override generic stage text.
    if (session.errorCode == model_core::ImportErrorCode::FileChanged ||
        session.errorCode == model_core::ImportErrorCode::OutOfMemory ||
        session.errorCode == model_core::ImportErrorCode::WorkerCrashed ||
        session.errorCode == model_core::ImportErrorCode::ResourceLimit ||
        session.errorCode == model_core::ImportErrorCode::StepHostFailure ||
        session.errorCode == model_core::ImportErrorCode::StepHostLimit ||
        session.errorCode == model_core::ImportErrorCode::TessellationFailed ||
        (session.errorCode >= model_core::ImportErrorCode::PrimarySourceLimit &&
         session.errorCode <= model_core::ImportErrorCode::ArchiveLimit))
        DescribeImportError(session.errorCode, summary, details);
}

std::wstring SourceFormatLabel(const std::wstring& path)
{
    const auto ext = ExtensionOf(path);
    if (ext == L"gltf") return L"glTF";
    if (ext == L"glb") return L"GLB";
    if (ext == L"stl") return L"STL";
    if (ext == L"ply") return L"PLY";
    if (ext == L"obj") return L"OBJ";
    if (ext == L"fbx") return L"FBX";
    if (ext == L"3mf") return L"3MF";
    if (ext == L"step" || ext == L"stp") return L"STEP";
    if (ext == L"usd" || ext == L"usda" || ext == L"usdc") return L"USD";
    if (ext == L"usdz") return L"USDZ";
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
    if (ext == L"obj") return SourceFormat::Obj;
    if (ext == L"fbx") return SourceFormat::Fbx;
    if (ext == L"3mf") return SourceFormat::ThreeMf;
    if (ext == L"usd" || ext == L"usda" || ext == L"usdc" || ext == L"usdz")
        return SourceFormat::Usd;
    // `.step`/`.stp` are deliberately absent until STEP-006: no public
    // picker, drag/drop, or activation path may recognize STEP yet, and an
    // extension alone must never bypass STEP-002 byte admission.
    return std::nullopt;
}

void EnsureImportSandboxPrepared()
{
    import_broker::PrepareImportWorkerPoolAsync(ResolveWorkerExePath());
}

ImportResult RunImport(SourceFormat format, const std::wstring& path, uint64_t generationId,
                        std::function<bool()> isCancelled, std::function<void(ImportResult)> onBatch, uint64_t sectionBytes, bool delayBatchesForTesting, uint32_t faultForTesting,
                        std::function<uint32_t()> nextDetail,
                        std::function<void(const model_core::FileIdentity&)> onInitialComplete, std::function<bool(uint64_t)> cpuBudgetAllows)
{
    ImportResult result;
    // Startup normally prewarms this pool, but direct/retry callers must not
    // depend on that timing. The coordinator makes repeated preparation cheap.
    EnsureImportSandboxPrepared();

    import_broker::ImportSessionRequest sessionRequest;
    sessionRequest.enableCoarseProxy = !delayBatchesForTesting && format != SourceFormat::Obj
        && format != SourceFormat::Fbx && format != SourceFormat::ThreeMf
        && format != SourceFormat::Usd && format != SourceFormat::Step;
    sessionRequest.useWorkerPool = !faultForTesting;
    sessionRequest.cpuBudgetAllows=std::move(cpuBudgetAllows);
    if (!delayBatchesForTesting && !faultForTesting && format != SourceFormat::ThreeMf
        && format != SourceFormat::Usd && format != SourceFormat::Step) {
        sessionRequest.nextDetail = std::move(nextDetail);
        sessionRequest.onInitialComplete = std::move(onInitialComplete);
    }
    sessionRequest.isCancelled = std::move(isCancelled);
    sessionRequest.workerExePath = ResolveWorkerExePath();
    if (format == SourceFormat::Usd)
        sessionRequest.compatibilityHostExePath = ResolveCompatibilityHostExePath();
    if (format == SourceFormat::Step)
        sessionRequest.stepHostExePath = ResolveStepHostExePath();
    sessionRequest.sourcePath = path;
    sessionRequest.format = ToBrokerFormat(format);
    sessionRequest.generationId = generationId;
    sessionRequest.sectionByteCapacity = sectionBytes;
    if (delayBatchesForTesting && format == SourceFormat::Glb)
        sessionRequest.workerArgumentsOverride = L"--parse-gltf-delayed-batches";
    sessionRequest.maxChunksPerGeneration = model_core::kTierACatalogLimit;
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
    if (faultForTesting == 6) {
        sessionRequest.commitLimitBytes = 1ull * 1024 * 1024;
        sessionRequest.replyTimeoutMs = 500;
    }
    import_broker::KnownChunkCatalog catalog;
    std::unordered_map<uint32_t, model_core::NodePayload> nodeCatalog;
    struct ResolvedNode { double world[16]{}; bool visible=false; bool active=false; };
    std::unordered_map<uint32_t,ResolvedNode> resolvedNodes;
    model_core::FileIdentity openedIdentity;
    bool initialComplete = false;
    if (sessionRequest.onInitialComplete) {
        auto complete = std::move(sessionRequest.onInitialComplete);
        sessionRequest.onInitialComplete = [&, complete](const auto& identity) {
            initialComplete = true; complete(identity);
        };
    }
    sessionRequest.onSourceOpened=[&](const auto& identity) { openedIdentity=identity; };
    auto unpack = [&](std::vector<import_broker::ValidatedChunk> chunks) {
        ImportResult result;
        result.detail = initialComplete;
        result.sourceIdentity=openedIdentity;
        result.forceUploadFailureForTesting = faultForTesting == 4;
        if (!chunks.empty()) result.scene = chunks.front().scene;
        for (const auto& chunk : chunks) catalog.emplace(chunk.descriptor.chunkId, chunk.descriptor.topology);
        for (auto& chunk : chunks) {
            switch (chunk.descriptor.topology) {
            case model_core::ChunkTopology::Node: {
                ImportedNode node;
                std::memcpy(&node.data, chunk.payload.data(), sizeof(node.data));
                nodeCatalog.emplace(node.data.nodeId,node.data);
                result.nodes.push_back(node);
                break;
            }
            case model_core::ChunkTopology::MeshInstance: {
                ImportedInstance instance;
                std::memcpy(&instance.data, chunk.payload.data(), sizeof(instance.data));
                result.instances.push_back(instance);
                break;
            }
            case model_core::ChunkTopology::CoarseComplete:
                result.coarseComplete = true;
                break;
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
        std::function<bool(uint32_t)> resolveNode=[&](uint32_t id) {
            if(auto found=resolvedNodes.find(id);found!=resolvedNodes.end()&&!found->second.active)return true;
            auto source=nodeCatalog.find(id);if(source==nodeCatalog.end())return false;
            auto& resolved=resolvedNodes[id];if(resolved.active)return false;resolved.active=true;
            std::memcpy(resolved.world,source->second.localTransform,sizeof(resolved.world));
            resolved.visible=(source->second.flags&model_core::kSceneRecordVisible)!=0;
            if(source->second.parentNodeId){if(!resolveNode(source->second.parentNodeId))return false;
                double world[16]{};const auto& parent=resolvedNodes.at(source->second.parentNodeId);
                for(uint32_t row=0;row<4;++row)for(uint32_t column=0;column<4;++column)for(uint32_t k=0;k<4;++k)
                    world[row*4+column]+=source->second.localTransform[row*4+k]*parent.world[k*4+column];
                std::memcpy(resolved.world,world,sizeof(world));resolved.visible=resolved.visible&&parent.visible;}
            resolved.active=false;return true;
        };
        for(auto& instance:result.instances){if(resolveNode(instance.data.nodeId)){const auto& node=resolvedNodes.at(instance.data.nodeId);
            std::memcpy(instance.worldTransform,node.world,sizeof(instance.worldTransform));
            instance.resolvedVisible=node.visible&&(instance.data.flags&model_core::kSceneRecordVisible);
            const auto* m=instance.worldTransform;const double determinant=m[0]*(m[5]*m[10]-m[6]*m[9])
                -m[1]*(m[4]*m[10]-m[6]*m[8])+m[2]*(m[4]*m[9]-m[5]*m[8]);instance.mirrored=determinant<0;}}
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
        if (format == SourceFormat::Fbx) {
            if (session.errorCode == model_core::ImportErrorCode::UnsupportedRequiredFeature) {
                result.errorDetails = L"Export or bake this FBX as static polygon geometry using the supported material and deformation subset.";
            } else if (session.errorCode == model_core::ImportErrorCode::PrimarySourceLimit) {
                result.errorDetails = L"FBX files are limited to the bounded Tier B source size.";
            } else if (session.errorCode == model_core::ImportErrorCode::ScratchLimit) {
                result.errorDetails = L"FBX parsing or static-pose evaluation exceeded the bounded importer scratch budget.";
            }
        } else if (format == SourceFormat::ThreeMf) {
            if (session.errorCode == model_core::ImportErrorCode::UnsupportedRequiredFeature) {
                result.errorDetails = L"This viewer supports static 3MF Core, Materials and Properties, Production, and bounded Beam Lattice content. Other required extensions or features cannot be previewed.";
            } else if (session.errorCode == model_core::ImportErrorCode::PrimarySourceLimit) {
                result.errorDetails = L"3MF files are limited to the bounded Tier B primary-source size.";
            } else if (session.errorCode == model_core::ImportErrorCode::ArchiveLimit) {
                result.errorDetails = L"The 3MF package exceeded a bounded archive, relationship, or expansion limit.";
            } else if (session.errorCode == model_core::ImportErrorCode::ScratchLimit) {
                result.errorDetails = L"3MF package parsing or normalization exceeded the bounded Tier B scratch budget.";
            }
        } else if (format == SourceFormat::Usd) {
            if (session.errorCode == model_core::ImportErrorCode::UnsupportedEncoding) {
                result.errorDetails = L"Use USDA, USDC, or USDZ content whose encoding matches the explicit suffix; .usd is detected by bytes.";
            } else if (session.errorCode == model_core::ImportErrorCode::UnsupportedRequiredFeature) {
                result.errorDetails = L"Export a static USD stage using supported meshes, primvars, instances, and USD Preview Surface materials.";
            } else if (session.errorCode == model_core::ImportErrorCode::PrimarySourceLimit) {
                result.errorDetails = L"USD files are limited to the bounded Tier B primary-source size.";
            } else if (session.errorCode == model_core::ImportErrorCode::ScratchLimit) {
                result.errorDetails = L"USD parsing, composition, or normalization exceeded the bounded Tier B scratch budget.";
            }
        } else if (format == SourceFormat::Step) {
            if (session.errorCode == model_core::ImportErrorCode::UnsupportedRequiredFeature) {
                result.errorDetails = L"Export a self-contained ISO 10303-21 STEP file. Required external STEP documents are not supported yet.";
            } else if (session.errorCode == model_core::ImportErrorCode::PrimarySourceLimit) {
                result.errorDetails = L"STEP files are limited to the bounded Tier B primary-source size.";
            } else if (session.errorCode == model_core::ImportErrorCode::ScratchLimit) {
                result.errorDetails = L"STEP parsing or tessellation exceeded the bounded Tier B scratch budget.";
            }
        }
        return result;
    }
    if (!onBatch)
    {
        result = unpack(std::move(session.chunks));
        result.sourceIdentity = session.sourceIdentity;
        return result;
    }
    result.sourceIdentity = session.sourceIdentity;
    result.ok = true;
    return result;
}

} // namespace d3d12_import_bridge
