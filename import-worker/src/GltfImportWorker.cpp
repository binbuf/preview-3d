#include "GltfImportWorker.h"

#include "GltfAdapter.h"

#include "model_core/ControlChannelIo.h"
#include "model_core/ControlProtocol.h"
#include "platform/MappedView.h"

#include <windows.h>

#include <cstring>
#include <variant>

namespace import_worker {

int RunGltfImport()
{
    HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (stdIn == nullptr || stdIn == INVALID_HANDLE_VALUE || stdOut == nullptr
        || stdOut == INVALID_HANDLE_VALUE) {
        return 1;
    }

    auto received = model_core::ReadControlMessage(stdIn);
    if (!received
        || received->header.opcode != static_cast<uint32_t>(model_core::ControlOpcode::StartGltfImport)
        || received->payload.size() != sizeof(model_core::ParseGltfRequest)) {
        // Honest-worker protocol sanity check, not adversarial handling.
        return 1;
    }

    model_core::ParseGltfRequest request{};
    std::memcpy(&request, received->payload.data(), sizeof(request));

    auto sendError = [&](model_core::ImportErrorCode code) {
        model_core::GenerationErrorNotice notice{};
        notice.generationId = request.generationId;
        notice.errorCode = static_cast<uint32_t>(code);
        model_core::WriteControlMessage(stdOut, model_core::ControlOpcode::GenerationError, &notice,
                                         sizeof(notice));
    };

    HANDLE sourceSection = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.sourceHandleValue));
    auto sourceView = platform::MappedView::Map(sourceSection, FILE_MAP_READ,
                                                 static_cast<SIZE_T>(request.sourceByteLength));
    if (!sourceView) {
        sendError(model_core::ImportErrorCode::InternalImporterFailure);
        return 1;
    }

    HANDLE outputSection = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.sectionHandleValue));
    auto outputView = platform::MappedView::Map(outputSection, FILE_MAP_WRITE | FILE_MAP_READ,
                                                 static_cast<SIZE_T>(request.sectionByteCapacity));
    if (!outputView) {
        sendError(model_core::ImportErrorCode::InternalImporterFailure);
        return 1;
    }

    auto result = ImportGltf(sourceView.bytes(), outputView.bytes(), request.generationId,
                              request.maxChunkCount);

    if (const auto* errorCode = std::get_if<model_core::ImportErrorCode>(&result)) {
        sendError(*errorCode);
        return 1;
    }

    const auto& info = std::get<GltfImportResult>(result);
    model_core::ChunksReadyNotice notice{};
    notice.generationId = request.generationId;
    notice.chunkCount = info.chunkCount;
    notice.sectionBytesWritten = info.sectionBytesWritten;
    model_core::WriteControlMessage(stdOut, model_core::ControlOpcode::ChunksReady, &notice,
                                     sizeof(notice));
    return 0;
}

} // namespace import_worker
