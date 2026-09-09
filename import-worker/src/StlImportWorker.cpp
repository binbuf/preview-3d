#include "StlImportWorker.h"

#include "StlAdapter.h"

#include "model_core/ControlChannelIo.h"
#include "model_core/MappedFile.h"
#include "platform/MappedView.h"
#include "platform/Win32Handle.h"

#include <windows.h>

#include <cstring>
#include <variant>

namespace import_worker {

namespace {

bool ReportResult(HANDLE stdOut, uint64_t generationId,
                   const std::variant<StlImportResult, model_core::ImportErrorCode>& result)
{
    if (const auto* errorCode = std::get_if<model_core::ImportErrorCode>(&result)) {
        model_core::GenerationErrorNotice notice{};
        notice.generationId = generationId;
        notice.errorCode = static_cast<uint32_t>(*errorCode);
        model_core::WriteControlMessage(stdOut, model_core::ControlOpcode::GenerationError, &notice,
                                         sizeof(notice));
        return false;
    }

    const auto& info = std::get<StlImportResult>(result);
    model_core::ChunksReadyNotice notice{};
    notice.generationId = generationId;
    notice.chunkCount = info.chunkCount;
    notice.sectionBytesWritten = info.sectionBytesWritten;
    model_core::WriteControlMessage(stdOut, model_core::ControlOpcode::ChunksReady, &notice, sizeof(notice));
    return true;
}

bool ReportError(HANDLE stdOut, uint64_t generationId, model_core::ImportErrorCode code)
{
    std::variant<StlImportResult, model_core::ImportErrorCode> result = code;
    return ReportResult(stdOut, generationId, result);
}

} // namespace

bool HandleStlImportFileRequest(HANDLE stdOut, const model_core::ParseStlFileRequest& request)
{
    HANDLE rawFile = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.sourceFileHandleValue));
    auto openResult = model_core::MappedFile::FromHandle(platform::Win32Handle(rawFile));
    if (!openResult.file.has_value()) {
        return ReportError(stdOut, request.generationId, model_core::ImportErrorCode::InternalImporterFailure);
    }

    std::wstring mapError;
    auto lease = openResult.file->MapWhole(mapError);
    if (!lease) {
        return ReportError(stdOut, request.generationId, model_core::ImportErrorCode::InternalImporterFailure);
    }

    HANDLE outputSection = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.sectionHandleValue));
    auto outputView = platform::MappedView::Map(outputSection, FILE_MAP_WRITE | FILE_MAP_READ,
                                                 static_cast<SIZE_T>(request.sectionByteCapacity));
    if (!outputView) {
        return ReportError(stdOut, request.generationId, model_core::ImportErrorCode::InternalImporterFailure);
    }

    auto result = ImportStl(lease.Bytes(), outputView.bytes(), request.generationId, request.maxChunkCount);
    return ReportResult(stdOut, request.generationId, result);
}

int RunStlImport()
{
    HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (stdIn == nullptr || stdIn == INVALID_HANDLE_VALUE || stdOut == nullptr
        || stdOut == INVALID_HANDLE_VALUE) {
        return 1;
    }

    auto received = model_core::ReadControlMessage(stdIn);
    if (!received
        || received->header.opcode != static_cast<uint32_t>(model_core::ControlOpcode::StartStlImportFromFile)
        || received->payload.size() != sizeof(model_core::ParseStlFileRequest)) {
        // Honest-worker protocol sanity check, not adversarial handling.
        return 1;
    }

    model_core::ParseStlFileRequest request{};
    std::memcpy(&request, received->payload.data(), sizeof(request));

    return HandleStlImportFileRequest(stdOut, request) ? 0 : 1;
}

} // namespace import_worker
