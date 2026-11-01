#include "AssimpImportWorker.h"

#include "AssimpAdapter.h"
#include "BoundedChunkWriter.h"
#include "ChunkBatchSink.h"
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

bool ReportResult(HANDLE stdOut, uint64_t generationId, const AssimpImportOutcome& result)
{
    if (const auto* error = std::get_if<model_core::ImportErrorCode>(&result)) {
        model_core::GenerationErrorNotice notice{};
        notice.generationId = generationId;
        notice.errorCode = uint32_t(*error);
        notice.reserved0 = uint32_t(*error == model_core::ImportErrorCode::UnsafeReference
            || *error == model_core::ImportErrorCode::FileUnavailable
            ? model_core::ImportFailurePhase::Sidecars : model_core::ImportFailurePhase::Geometry);
        model_core::WriteControlMessage(stdOut, model_core::ControlOpcode::GenerationError,
                                        &notice, sizeof(notice));
        return false;
    }
    const auto& value = std::get<AssimpImportResult>(result);
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
    AssimpImportOutcome outcome(error);
    return ReportResult(stdOut, generationId, outcome);
}

} // namespace

bool HandleAssimpImportFileRequest(HANDLE stdIn, HANDLE stdOut,
                                   const model_core::ParseAssimpFileRequest& request)
{
    const auto format = static_cast<model_core::AssimpFormat>(request.assimpFormat);
    if (!model_core::IsKnownAssimpFormat(format))
        return ReportError(stdOut, request.generationId,
                           model_core::ImportErrorCode::ImportProtocolViolation);

    HANDLE rawFile = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.sourceFileHandleValue));
    auto opened = model_core::MappedFile::FromHandle(platform::Win32Handle(rawFile));
    if (!opened.file) return ReportError(stdOut, request.generationId,
        model_core::ImportErrorCode::InternalImporterFailure);
    if (opened.file->SizeBytes() > model_core::kTierBPrimarySourceBytes)
        return ReportError(stdOut, request.generationId,
                           model_core::ImportErrorCode::PrimarySourceLimit);
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

    ChunkBatchSink sink(stdIn, stdOut, request.generationId, request.requestFlags,
                        cancellationEvent.get());
    SidecarFileClient sidecars(stdIn, stdOut, request.generationId);
    sidecars.EnablePinnedReplay();

    AssimpImportOptions options;
    options.format = format;
    options.textureOptions.isCancelled = [&sink] { return sink.Cancelled(); };
    options.requestSidecar = [&sidecars](std::string_view reference, uint64_t maxBytes) {
        AssimpSidecarResult result;
        auto requested = sidecars.RequestSidecarBytes(std::string(reference), maxBytes);
        result.error = requested.errorCode;
        result.bytes = std::move(requested.bytes);
        return result;
    };
    if (request.requestFlags & model_core::kImportRequestAssimpTinyLimitsForTesting) {
        options.maxObjects = 8;
        options.maxMaterials = 8;
        options.maxTriangles = 128;
        options.maxVertices = 384;
    }

    try {
        auto result = ImportAssimp(source.Bytes(), output.bytes(), request.generationId,
                                   request.maxChunkCount, &sink, options);
        return ReportResult(stdOut, request.generationId, result);
    } catch (const std::bad_alloc&) {
        return ReportError(stdOut, request.generationId, model_core::ImportErrorCode::OutOfMemory);
    } catch (...) {
        return ReportError(stdOut, request.generationId,
                           model_core::ImportErrorCode::InternalImporterFailure);
    }
}

int RunAssimpImport()
{
    HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE), stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!stdIn || stdIn == INVALID_HANDLE_VALUE || !stdOut || stdOut == INVALID_HANDLE_VALUE)
        return 1;
    auto message = model_core::ReadControlMessage(stdIn);
    if (!message
        || message->header.opcode != uint32_t(model_core::ControlOpcode::StartAssimpImportFromFile)
        || message->payload.size() != sizeof(model_core::ParseAssimpFileRequest)) return 1;
    model_core::ParseAssimpFileRequest request{};
    std::memcpy(&request, message->payload.data(), sizeof(request));
    return HandleAssimpImportFileRequest(stdIn, stdOut, request) ? 0 : 1;
}

} // namespace import_worker
