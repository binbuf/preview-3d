#include "import_broker/SidecarRequestServicer.h"

#include "import_broker/SidecarPathResolver.h"
#include "import_broker/SourceFileAccess.h"

namespace import_broker {

std::variant<model_core::SidecarFileReadyNotice, model_core::SidecarFileUnavailableNotice>
ServiceSidecarRequest(HANDLE workerProcess, const std::wstring& primaryCanonicalPath,
                       const model_core::RequestSidecarFileNotice& request, uint64_t maxSidecarFileBytes)
{
    model_core::SidecarFileUnavailableNotice unavailable{};
    unavailable.generationId = request.generationId;

    if (request.relativePathLength > model_core::kMaxSidecarRelativePathBytes) {
        unavailable.errorCode = static_cast<uint32_t>(model_core::ImportErrorCode::UnsafeReference);
        return unavailable;
    }
    std::string relativeReferenceUtf8(reinterpret_cast<const char*>(request.relativePathUtf8),
                                       request.relativePathLength);

    SidecarResolution resolution = ResolveSidecarPath(primaryCanonicalPath, relativeReferenceUtf8, maxSidecarFileBytes);
    if (!resolution.file) {
        unavailable.errorCode = static_cast<uint32_t>(resolution.rejectionCode);
        return unavailable;
    }

    auto duplicated = DuplicateHandleIntoProcess(resolution.file.get(), workerProcess);
    // resolution.file goes out of scope and closes here regardless -- the
    // worker's just-duplicated handle is an independent reference to the
    // same file object, per this file's header comment.
    if (!duplicated) {
        unavailable.errorCode = static_cast<uint32_t>(model_core::ImportErrorCode::FileUnavailable);
        return unavailable;
    }

    model_core::SidecarFileReadyNotice ready{};
    ready.generationId = request.generationId;
    ready.sidecarFileHandleValue = *duplicated;
    ready.sidecarByteLength = resolution.fileSizeBytes;
    return ready;
}

} // namespace import_broker
