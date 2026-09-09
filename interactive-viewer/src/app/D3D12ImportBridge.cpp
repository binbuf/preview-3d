#include "D3D12ImportBridge.h"

#include "import_broker/SandboxLauncher.h"
#include "import_broker/SharedSection.h"
#include "import_broker/SharedSectionValidator.h"
#include "import_broker/SourceFileAccess.h"
#include "model_core/ControlChannelIo.h"
#include "model_core/ControlProtocol.h"
#include "platform/AppContainerSid.h"
#include "platform/MappedView.h"
#include "platform/Win32Handle.h"

#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <mutex>

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

std::wstring MakeUniqueContainerName()
{
    auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return L"Preview3DD3D12Import-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(ticks);
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
    case model_core::ImportErrorCode::ImportProtocolViolation:
    case model_core::ImportErrorCode::InternalImporterFailure:
    case model_core::ImportErrorCode::None:
    default:
        summary = L"This model could not be previewed.";
        details = L"The importer stopped unexpectedly while reading the file.";
        return;
    }
}

constexpr uint32_t kMaxChunkCount = 64;

// Every real-file request struct (ParseGltfFileRequest/ParseStlFileRequest/
// ParsePlyFileRequest) is field-for-field identical -- generationId,
// sourceFileHandleValue, sectionHandleValue, sectionByteCapacity,
// maxChunkCount, reserved0 -- but each is its own named type per this
// codebase's "small deliberate duplication over cross-format coupling"
// precedent, so this helper is a template rather than one shared struct.
template <typename Request>
Request MakeFileRequest(uint64_t generationId, HANDLE sourceFileHandle, HANDLE sectionHandle)
{
    Request request{};
    request.generationId = generationId;
    request.sourceFileHandleValue = reinterpret_cast<uint64_t>(sourceFileHandle);
    request.sectionHandleValue = reinterpret_cast<uint64_t>(sectionHandle);
    request.sectionByteCapacity = import_broker::kSyntheticSectionBytes;
    request.maxChunkCount = kMaxChunkCount;
    return request;
}

std::once_flag g_directoryAccessGrantOnce;

} // namespace

std::optional<SourceFormat> ClassifyByExtension(const std::wstring& path)
{
    std::wstring ext = ExtensionOf(path);
    if (ext == L"glb" || ext == L"gltf") return SourceFormat::Glb;
    if (ext == L"stl") return SourceFormat::Stl;
    if (ext == L"ply") return SourceFormat::Ply;
    return std::nullopt;
}

void EnsureAppContainerDirectoryAccessGranted()
{
    std::call_once(g_directoryAccessGrantOnce, [] {
        std::wstring workerExePath = ResolveWorkerExePath();
        auto lastSlash = workerExePath.find_last_of(L"\\/");
        std::wstring directory = (lastSlash == std::wstring::npos) ? L"." : workerExePath.substr(0, lastSlash);

        std::wstring command = L"icacls \"" + directory
            + L"\" /grant *S-1-15-2-1:(OI)(CI)RX /grant *S-1-15-2-2:(OI)(CI)RX /Q";
        _wsystem(command.c_str());
    });
}

ImportResult RunImport(SourceFormat format, const std::wstring& path, uint64_t generationId)
{
    ImportResult result;

    auto opened = import_broker::OpenAndCanonicalizeSourceFile(path);
    if (!opened.file) {
        result.errorSummary = L"This file could not be opened.";
        result.errorDetails = opened.error;
        return result;
    }

    auto duplicatedFile = import_broker::DuplicateInheritableHandle(opened.file.get());
    if (!duplicatedFile) {
        result.errorSummary = L"This file could not be prepared for preview.";
        result.errorDetails = L"The file handle could not be shared with the sandboxed importer.";
        return result;
    }

    platform::Win32Handle outputSection = import_broker::CreateSharedSection(import_broker::kSyntheticSectionBytes);
    if (!outputSection) {
        result.errorSummary = L"This model could not be previewed.";
        result.errorDetails = L"A shared memory section for the importer's output could not be created.";
        return result;
    }

    std::wstring containerName = MakeUniqueContainerName();
    platform::AppContainerSid sid = platform::AppContainerSid::CreateOrOpen(
        containerName, L"Preview3D D3D12 Import", L"Sandboxed import for the --d3d12 preview path");
    if (!sid) {
        result.errorSummary = L"This model could not be previewed.";
        result.errorDetails = L"The sandbox container for the importer could not be created.";
        return result;
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
        platform::AppContainerSid::Delete(containerName);
        result.errorSummary = L"This model could not be previewed.";
        result.errorDetails = L"The control channel to the sandboxed importer could not be created.";
        return result;
    }
    platform::Win32Handle controlInRead(inReadRaw);
    platform::Win32Handle controlInWrite(inWriteRaw);
    platform::Win32Handle controlOutRead(outReadRaw);
    platform::Win32Handle controlOutWrite(outWriteRaw);
    SetHandleInformation(controlInWrite.get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(controlOutRead.get(), HANDLE_FLAG_INHERIT, 0);

    std::wstring workerExePath = ResolveWorkerExePath();
    const wchar_t* parseFlag = format == SourceFormat::Glb  ? L"--parse-gltf"
        : format == SourceFormat::Stl                       ? L"--parse-stl"
                                                              : L"--parse-ply";
    std::wstring cmdLine = L"\"" + workerExePath + L"\" " + parseFlag;

    HANDLE inherited[] = { controlInRead.get(), controlOutWrite.get(), duplicatedFile->get(), outputSection.get() };
    import_broker::SandboxLimits limits{};
    auto proc = import_broker::LaunchSuspendedSandboxed(workerExePath, cmdLine, inherited, controlOutWrite.get(),
                                                          limits, sid, controlInRead.get());
    controlInRead.reset();
    controlOutWrite.reset();
    if (!proc) {
        platform::AppContainerSid::Delete(containerName);
        result.errorSummary = L"This model could not be previewed.";
        result.errorDetails = L"The sandboxed importer process could not be started.";
        return result;
    }
    if (!import_broker::ResumeSandboxProcess(*proc)) {
        platform::AppContainerSid::Delete(containerName);
        result.errorSummary = L"This model could not be previewed.";
        result.errorDetails = L"The sandboxed importer process could not be resumed.";
        return result;
    }

    bool sent = false;
    switch (format) {
    case SourceFormat::Glb: {
        auto request = MakeFileRequest<model_core::ParseGltfFileRequest>(generationId, duplicatedFile->get(),
                                                                          outputSection.get());
        sent = model_core::WriteControlMessage(controlInWrite.get(), model_core::ControlOpcode::StartGltfImportFromFile,
                                                &request, sizeof(request));
        break;
    }
    case SourceFormat::Stl: {
        auto request = MakeFileRequest<model_core::ParseStlFileRequest>(generationId, duplicatedFile->get(),
                                                                         outputSection.get());
        sent = model_core::WriteControlMessage(controlInWrite.get(), model_core::ControlOpcode::StartStlImportFromFile,
                                                &request, sizeof(request));
        break;
    }
    case SourceFormat::Ply: {
        auto request = MakeFileRequest<model_core::ParsePlyFileRequest>(generationId, duplicatedFile->get(),
                                                                         outputSection.get());
        sent = model_core::WriteControlMessage(controlInWrite.get(), model_core::ControlOpcode::StartPlyImportFromFile,
                                                &request, sizeof(request));
        break;
    }
    }

    if (!sent) {
        platform::AppContainerSid::Delete(containerName);
        result.errorSummary = L"This model could not be previewed.";
        result.errorDetails = L"The import request could not be sent to the sandboxed importer.";
        return result;
    }

    auto received = model_core::ReadControlMessage(controlOutRead.get());
    WaitForSingleObject(proc->process.get(), 5000);
    platform::AppContainerSid::Delete(containerName);

    if (!received) {
        result.errorSummary = L"This model could not be previewed.";
        result.errorDetails = L"The sandboxed importer did not respond.";
        return result;
    }

    if (received->header.opcode == static_cast<uint32_t>(model_core::ControlOpcode::GenerationError)) {
        model_core::GenerationErrorNotice notice{};
        if (received->payload.size() == sizeof(notice)) {
            std::memcpy(&notice, received->payload.data(), sizeof(notice));
        }
        DescribeImportError(static_cast<model_core::ImportErrorCode>(notice.errorCode), result.errorSummary,
                             result.errorDetails);
        return result;
    }

    if (received->header.opcode != static_cast<uint32_t>(model_core::ControlOpcode::ChunksReady)) {
        result.errorSummary = L"This model could not be previewed.";
        result.errorDetails = L"The sandboxed importer returned an unexpected response.";
        return result;
    }

    auto view = platform::MappedView::Map(outputSection.get(), FILE_MAP_READ, import_broker::kSyntheticSectionBytes);
    if (!view) {
        result.errorSummary = L"This model could not be previewed.";
        result.errorDetails = L"The importer's output could not be read.";
        return result;
    }
    import_broker::ValidationResult validation
        = import_broker::ValidateAndCopySection(view.bytes(), generationId, kMaxChunkCount);
    if (!validation.ok) {
        DescribeImportError(validation.errorCode, result.errorSummary, result.errorDetails);
        return result;
    }

    for (auto& chunk : validation.chunks) {
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
                for (const auto& other : validation.chunks) {
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
