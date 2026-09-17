#include "ObjImportWorker.h"

#include "BoundedChunkWriter.h"
#include "ChunkBatchSink.h"
#include "ObjAdapter.h"
#include "SidecarFileClient.h"
#include "model_core/ControlChannelIo.h"
#include "model_core/MappedFile.h"
#include "model_core/TierALimits.h"
#include "platform/MappedView.h"
#include "platform/Win32Handle.h"

#include <cstring>
#include <new>
#include <variant>

namespace import_worker {
namespace {

bool ReportResult(HANDLE stdOut, uint64_t generationId,
                  const std::variant<ObjImportResult, model_core::ImportErrorCode>& result)
{
    if (const auto* error = std::get_if<model_core::ImportErrorCode>(&result)) {
        model_core::GenerationErrorNotice notice{};
        notice.generationId = generationId;
        notice.errorCode = uint32_t(*error);
        notice.reserved0 = uint32_t(*error == model_core::ImportErrorCode::UnsafeReference ||
            *error == model_core::ImportErrorCode::FileUnavailable
            ? model_core::ImportFailurePhase::Sidecars : model_core::ImportFailurePhase::Geometry);
        model_core::WriteControlMessage(stdOut, model_core::ControlOpcode::GenerationError,
                                        &notice, sizeof(notice));
        return false;
    }
    const auto& value = std::get<ObjImportResult>(result);
    model_core::ChunksReadyNotice notice{};
    notice.generationId = generationId;
    notice.chunkCount = value.chunkCount;
    notice.sectionBytesWritten = value.sectionBytesWritten;
    model_core::WriteControlMessage(stdOut, model_core::ControlOpcode::ChunksReady,
                                    &notice, sizeof(notice));
    return true;
}

bool ReportError(HANDLE stdOut, uint64_t generationId, model_core::ImportErrorCode error)
{
    return ReportResult(stdOut, generationId,
        std::variant<ObjImportResult, model_core::ImportErrorCode>(error));
}

} // namespace

bool HandleObjImportFileRequest(HANDLE stdIn, HANDLE stdOut,
                                const model_core::ParseObjFileRequest& request)
{
    HANDLE rawFile = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.sourceFileHandleValue));
    auto opened = model_core::MappedFile::FromHandle(platform::Win32Handle(rawFile));
    if (!opened.file) return ReportError(stdOut, request.generationId,
        model_core::ImportErrorCode::InternalImporterFailure);
    if (opened.file->SizeBytes() > model_core::kTierBPrimarySourceBytes)
        return ReportError(stdOut, request.generationId, model_core::ImportErrorCode::PrimarySourceLimit);
    std::wstring mapError;
    auto source = opened.file->MapWhole(mapError);
    if (!source) return ReportError(stdOut, request.generationId,
        model_core::ImportErrorCode::InternalImporterFailure);
    platform::Win32Handle outputSection(reinterpret_cast<HANDLE>(
        static_cast<uintptr_t>(request.sectionHandleValue)));
    platform::Win32Handle cancellationEvent(reinterpret_cast<HANDLE>(
        static_cast<uintptr_t>(request.cancellationEventHandleValue)));
    auto output = platform::MappedView::Map(outputSection.get(), FILE_MAP_WRITE | FILE_MAP_READ,
                                            static_cast<SIZE_T>(request.sectionByteCapacity));
    if (!output) return ReportError(stdOut, request.generationId,
        model_core::ImportErrorCode::InternalImporterFailure);
    ChunkBatchSink sink(stdIn, stdOut, request.generationId, 0, cancellationEvent.get());
    SidecarFileClient sidecars(stdIn, stdOut, request.generationId);
    sidecars.EnablePinnedReplay();
    TextureDecodeOptions textureOptions;
    textureOptions.isCancelled = [&sink] { return sink.Cancelled(); };
    try {
        auto result = ImportObj(source.Bytes(), output.bytes(), request.generationId,
                                request.maxChunkCount, sidecars, &sink, textureOptions);
        return ReportResult(stdOut, request.generationId, result);
    } catch (const std::bad_alloc&) {
        return ReportError(stdOut, request.generationId, model_core::ImportErrorCode::OutOfMemory);
    } catch (...) {
        return ReportError(stdOut, request.generationId, model_core::ImportErrorCode::InternalImporterFailure);
    }
}

int RunObjImport()
{
    HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE), stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!stdIn || stdIn == INVALID_HANDLE_VALUE || !stdOut || stdOut == INVALID_HANDLE_VALUE) return 1;
    auto message = model_core::ReadControlMessage(stdIn);
    if (!message || message->header.opcode != uint32_t(model_core::ControlOpcode::StartObjImportFromFile)
        || message->payload.size() != sizeof(model_core::ParseObjFileRequest)) return 1;
    model_core::ParseObjFileRequest request{};
    std::memcpy(&request, message->payload.data(), sizeof(request));
    return HandleObjImportFileRequest(stdIn, stdOut, request) ? 0 : 1;
}

} // namespace import_worker
