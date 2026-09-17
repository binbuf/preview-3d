#include "FbxImportWorker.h"

#include "ChunkBatchSink.h"
#include "FbxAdapter.h"
#include "model_core/ControlChannelIo.h"
#include "model_core/MappedFile.h"
#include "model_core/TierALimits.h"
#include "platform/MappedView.h"
#include "platform/Win32Handle.h"

#include <cstring>
#include <new>

namespace import_worker {
namespace {

bool ReportOutcome(HANDLE stdOut, uint64_t generationId, const FbxImportOutcome& outcome)
{
    if (const auto* failure = std::get_if<FbxImportFailure>(&outcome)) {
        model_core::GenerationErrorNotice notice{};
        notice.generationId = generationId;
        notice.errorCode = uint32_t(failure->code);
        notice.reserved0 = uint32_t(failure->phase);
        model_core::WriteControlMessage(stdOut, model_core::ControlOpcode::GenerationError,
                                        &notice, sizeof(notice));
        return false;
    }
    const auto& result = std::get<FbxImportResult>(outcome);
    model_core::ChunksReadyNotice notice{};
    notice.generationId = generationId;
    notice.chunkCount = result.chunkCount;
    notice.sectionBytesWritten = result.sectionBytesWritten;
    model_core::WriteControlMessage(stdOut, model_core::ControlOpcode::ChunksReady,
                                    &notice, sizeof(notice));
    return true;
}

bool ReportError(HANDLE stdOut, uint64_t generationId, model_core::ImportErrorCode code)
{
    return ReportOutcome(stdOut, generationId,
                         FbxImportFailure{code, model_core::ImportFailurePhase::Geometry});
}

} // namespace

bool HandleFbxImportFileRequest(HANDLE stdIn, HANDLE stdOut,
                                const model_core::ParseFbxFileRequest& request)
{
    HANDLE rawFile = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.sourceFileHandleValue));
    auto opened = model_core::MappedFile::FromHandle(platform::Win32Handle(rawFile));
    if (!opened.file)
        return ReportError(stdOut, request.generationId,
                           model_core::ImportErrorCode::InternalImporterFailure);
    if (opened.file->SizeBytes() > model_core::kTierBPrimarySourceBytes)
        return ReportError(stdOut, request.generationId,
                           model_core::ImportErrorCode::PrimarySourceLimit);
    std::wstring mapError;
    auto source = opened.file->MapWhole(mapError);
    if (!source)
        return ReportError(stdOut, request.generationId,
                           model_core::ImportErrorCode::InternalImporterFailure);

    platform::Win32Handle outputSection(reinterpret_cast<HANDLE>(
        static_cast<uintptr_t>(request.sectionHandleValue)));
    platform::Win32Handle cancellationEvent(reinterpret_cast<HANDLE>(
        static_cast<uintptr_t>(request.cancellationEventHandleValue)));
    auto output = platform::MappedView::Map(outputSection.get(), FILE_MAP_WRITE | FILE_MAP_READ,
                                            static_cast<SIZE_T>(request.sectionByteCapacity));
    if (!output)
        return ReportError(stdOut, request.generationId,
                           model_core::ImportErrorCode::InternalImporterFailure);

    ChunkBatchSink sink(stdIn, stdOut, request.generationId, 0, cancellationEvent.get());
    FbxImportOptions options;
    options.isCancelled = [&sink] { return sink.Cancelled(); };
    if (request.requestFlags & model_core::kImportRequestFbxTinyEvaluationLimitForTesting)
        options.evaluationAllocatorLimit = 1024;
    try {
        return ReportOutcome(stdOut, request.generationId,
            ImportFbx(source.Bytes(), output.bytes(), request.generationId,
                      request.maxChunkCount, &sink, options));
    } catch (const std::bad_alloc&) {
        return ReportError(stdOut, request.generationId, model_core::ImportErrorCode::OutOfMemory);
    } catch (...) {
        return ReportError(stdOut, request.generationId,
                           model_core::ImportErrorCode::InternalImporterFailure);
    }
}

int RunFbxImport()
{
    HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE), stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!stdIn || stdIn == INVALID_HANDLE_VALUE || !stdOut || stdOut == INVALID_HANDLE_VALUE)
        return 1;
    auto message = model_core::ReadControlMessage(stdIn);
    if (!message
        || message->header.opcode != uint32_t(model_core::ControlOpcode::StartFbxImportFromFile)
        || message->payload.size() != sizeof(model_core::ParseFbxFileRequest)) return 1;
    model_core::ParseFbxFileRequest request{};
    std::memcpy(&request, message->payload.data(), sizeof(request));
    return HandleFbxImportFileRequest(stdIn, stdOut, request) ? 0 : 1;
}

} // namespace import_worker
