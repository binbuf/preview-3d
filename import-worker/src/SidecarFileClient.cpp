#include "SidecarFileClient.h"

#include "model_core/ControlChannelIo.h"
#include "model_core/ControlProtocol.h"
#include "model_core/MappedFile.h"
#include "platform/Win32Handle.h"

#include <cstring>

namespace import_worker {

SidecarFileClient::Result SidecarFileClient::RequestSidecarBytes(const std::string& relativePathUtf8)
{
    Result result;

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
    auto openResult = model_core::MappedFile::FromHandle(platform::Win32Handle(rawFile));
    if (!openResult.file.has_value()) {
        result.errorCode = model_core::ImportErrorCode::InternalImporterFailure;
        return result;
    }

    std::wstring mapError;
    auto lease = openResult.file->MapWhole(mapError);
    if (!lease) {
        result.errorCode = model_core::ImportErrorCode::InternalImporterFailure;
        return result;
    }

    // Copy once here rather than threading a MappingLease through
    // GltfAdapter.cpp's WalkState -- a deliberate simplicity-over-micro-
    // optimization call; sidecar files are individually size-capped by the
    // host (ready.sidecarByteLength, never trusted beyond what the mapping
    // itself proves), so this is bounded.
    result.bytes = std::vector<std::byte>(lease.Bytes().begin(), lease.Bytes().end());
    return result;
}

} // namespace import_worker
