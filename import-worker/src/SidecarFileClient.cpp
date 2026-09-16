#include "SidecarFileClient.h"

#include "model_core/ControlChannelIo.h"
#include "model_core/ControlProtocol.h"
#include "model_core/MappedFile.h"
#include "platform/Win32Handle.h"

#include <cstring>

namespace import_worker {

SidecarFileClient::Result SidecarFileClient::RequestSidecarBytes(const std::string& relativePathUtf8,
                                                                 uint64_t maxBytes, bool mappedOnly)
{
    Result result;
    auto existing=pinned_.find(relativePathUtf8);
    if (existing!=pinned_.end()) {
        HANDLE duplicate=nullptr;
        if (!DuplicateHandle(GetCurrentProcess(),existing->second.handle.get(),GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS)) {
            result.errorCode=model_core::ImportErrorCode::FileUnavailable; return result;
        }
        auto opened=model_core::MappedFile::FromHandle(platform::Win32Handle(duplicate));
        if (!opened.file || opened.file->Identity()!=existing->second.identity || !opened.file->IsUnchanged()) {
            result.errorCode=model_core::ImportErrorCode::FileChanged; return result;
        }
        if (opened.file->SizeBytes()>maxBytes) { result.errorCode=model_core::ImportErrorCode::ResourceLimit; return result; }
        std::wstring error; auto lease=opened.file->MapWhole(error);
        if (!lease) { result.errorCode=model_core::ImportErrorCode::FileUnavailable; return result; }
        if (mappedOnly) { result.file=std::move(opened.file); result.mapping=std::move(lease); }
        else result.bytes=std::vector<std::byte>(lease.Bytes().begin(),lease.Bytes().end());
        return result;
    }

    if (relativePathUtf8.size() > model_core::kMaxSidecarRelativePathBytes) {
        result.errorCode = model_core::ImportErrorCode::UnsafeReference;
        return result;
    }

    model_core::RequestSidecarFileNotice request{};
    request.generationId = generationId_;
    request.relativePathLength = static_cast<uint32_t>(relativePathUtf8.size());
    std::memcpy(request.relativePathUtf8, relativePathUtf8.data(), relativePathUtf8.size());

    if (!model_core::WriteControlMessage(stdOut_, model_core::ControlOpcode::RequestSidecarFile, &request,
                                          sizeof(request))) {
        result.errorCode = model_core::ImportErrorCode::ImportProtocolViolation;
        return result;
    }

    auto received = model_core::ReadControlMessage(stdIn_);
    if (!received) {
        result.errorCode = model_core::ImportErrorCode::ImportProtocolViolation;
        return result;
    }

    if (received->header.opcode == static_cast<uint32_t>(model_core::ControlOpcode::SidecarFileUnavailable)
        && received->payload.size() == sizeof(model_core::SidecarFileUnavailableNotice)) {
        model_core::SidecarFileUnavailableNotice notice{};
        std::memcpy(&notice, received->payload.data(), sizeof(notice));
        result.errorCode = static_cast<model_core::ImportErrorCode>(notice.errorCode);
        return result;
    }

    if (received->header.opcode != static_cast<uint32_t>(model_core::ControlOpcode::SidecarFileReady)
        || received->payload.size() != sizeof(model_core::SidecarFileReadyNotice)) {
        result.errorCode = model_core::ImportErrorCode::ImportProtocolViolation;
        return result;
    }

    model_core::SidecarFileReadyNotice ready{};
    std::memcpy(&ready, received->payload.data(), sizeof(ready));

    HANDLE rawFile = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(ready.sidecarFileHandleValue));
    platform::Win32Handle pinnedHandle;
    if (pinnedReplay_) {
        HANDLE duplicate=nullptr;
        if (pinned_.size()>=64 || !DuplicateHandle(GetCurrentProcess(),rawFile,GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS)) {
            CloseHandle(rawFile); result.errorCode=model_core::ImportErrorCode::ResourceLimit; return result;
        }
        pinnedHandle.reset(duplicate);
    }
    auto openResult = model_core::MappedFile::FromHandle(platform::Win32Handle(rawFile));
    if (!openResult.file.has_value()) {
        result.errorCode = model_core::ImportErrorCode::InternalImporterFailure;
        return result;
    }

    if (openResult.file->SizeBytes()>maxBytes) {
        result.errorCode=model_core::ImportErrorCode::ResourceLimit;return result;
    }
    if (pinnedHandle) pinned_.emplace(relativePathUtf8,Pinned{std::move(pinnedHandle),openResult.file->Identity()});
    std::wstring mapError;
    auto lease = openResult.file->MapWhole(mapError);
    if (!lease) {
        result.errorCode = model_core::ImportErrorCode::InternalImporterFailure;
        return result;
    }

    // Geometry sidecars retain a read-only lease; bounded encoded images
    // retain an owned copy for decoder APIs. Neither path grants new path authority.
    if (mappedOnly)
    {
        result.file = std::move(openResult.file);
        result.mapping = std::move(lease);
    }
    else
        result.bytes = std::vector<std::byte>(lease.Bytes().begin(), lease.Bytes().end());
    return result;
}

} // namespace import_worker
