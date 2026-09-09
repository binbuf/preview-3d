#include "D3D12ImportBridge.h"

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
    case model_core::ImportErrorCode::ImportProtocolViolation:
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
void DescribeSessionFailure(const import_broker::ImportSessionResult& session, std::wstring& summary,
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

constexpr uint32_t kMaxChunkCount = 64;
constexpr uint32_t kMaxSidecarRequestsPerGeneration = 64;
constexpr uint64_t kMaxSidecarFileBytes = 256ull * 1024ull * 1024ull;

} // namespace

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
                        std::function<bool()> isCancelled)
{
    ImportResult result;

    import_broker::ImportSessionRequest sessionRequest;
    sessionRequest.isCancelled = std::move(isCancelled);
    sessionRequest.workerExePath = ResolveWorkerExePath();
    sessionRequest.sourcePath = path;
    sessionRequest.format = ToBrokerFormat(format);
    sessionRequest.generationId = generationId;
    sessionRequest.sectionByteCapacity = import_broker::kSyntheticSectionBytes;
    sessionRequest.maxChunkCount = kMaxChunkCount;
    sessionRequest.maxSidecarRequestsPerGeneration = kMaxSidecarRequestsPerGeneration;
    sessionRequest.maxSidecarFileBytes = kMaxSidecarFileBytes;

    import_broker::ImportSessionResult session = import_broker::RunImportSession(sessionRequest);
    if (!session.ok) {
        DescribeSessionFailure(session, result.errorSummary, result.errorDetails);
        return result;
    }

    for (auto& chunk : session.chunks) {
        switch (chunk.descriptor.topology) {
        case model_core::ChunkTopology::TriangleList:
        case model_core::ChunkTopology::PointList: {
            ImportedMesh mesh;
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
                for (const auto& other : session.chunks) {
                    if (other.descriptor.chunkId == targetId
                        && other.descriptor.topology == model_core::ChunkTopology::Material) {
                        mesh.materialChunkId = targetId;
                        break;
                    }
                }
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
            if (chunk.descriptor.dependencyCount >= 1) material.baseColorImageChunkId = chunk.descriptor.dependencyIds[0];
            if (chunk.descriptor.dependencyCount >= 2) material.metallicRoughnessImageChunkId = chunk.descriptor.dependencyIds[1];
            if (chunk.descriptor.dependencyCount >= 3) material.normalImageChunkId = chunk.descriptor.dependencyIds[2];
            if (chunk.descriptor.dependencyCount >= 4) material.emissiveImageChunkId = chunk.descriptor.dependencyIds[3];
            result.materials.push_back(std::move(material));
            break;
        }
        case model_core::ChunkTopology::Image: {
            ImportedImage image;
            image.chunkId = chunk.descriptor.chunkId;
            if (chunk.payload.size() >= sizeof(model_core::ImagePayloadHeader)) {
                model_core::ImagePayloadHeader header{};
                std::memcpy(&header, chunk.payload.data(), sizeof(header));
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
        default:
            break; // unrecognized topology already rejected by the validator; never reached
        }
    }
    result.ok = true;
    return result;
}

} // namespace d3d12_import_bridge
