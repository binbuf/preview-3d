#include "PlyImportWorker.h"

#include "PlyAdapter.h"
#include "BoundedChunkWriter.h"
#include "ChunkBatchSink.h"

#include "model_core/ControlChannelIo.h"
#include "model_core/MappedFile.h"
#include "platform/MappedView.h"
#include "platform/Win32Handle.h"

#include <windows.h>

#include <cstring>
#include <new>
#include <variant>

namespace import_worker {

namespace {

bool ReportResult(HANDLE stdOut, uint64_t generationId,
                   const std::variant<PlyImportResult, model_core::ImportErrorCode>& result)
{
    if (const auto* errorCode = std::get_if<model_core::ImportErrorCode>(&result)) {
        model_core::GenerationErrorNotice notice{};
        notice.generationId = generationId;
        notice.errorCode = static_cast<uint32_t>(*errorCode);
        notice.reserved0 = uint32_t(*errorCode == model_core::ImportErrorCode::UnsafeReference || *errorCode == model_core::ImportErrorCode::FileUnavailable
            ? model_core::ImportFailurePhase::Sidecars : model_core::ImportFailurePhase::Geometry);
        model_core::WriteControlMessage(stdOut, model_core::ControlOpcode::GenerationError, &notice,
                                         sizeof(notice));
        return false;
    }

    const auto& info = std::get<PlyImportResult>(result);
    model_core::ChunksReadyNotice notice{};
    notice.generationId = generationId;
    notice.chunkCount = info.chunkCount;
    notice.sectionBytesWritten = info.sectionBytesWritten;
    model_core::WriteControlMessage(stdOut, model_core::ControlOpcode::ChunksReady, &notice, sizeof(notice));
    return true;
}

bool ReportError(HANDLE stdOut, uint64_t generationId, model_core::ImportErrorCode code)
{
    std::variant<PlyImportResult, model_core::ImportErrorCode> result = code;
    return ReportResult(stdOut, generationId, result);
}

} // namespace

bool HandlePlyImportFileRequest(HANDLE stdOut, const model_core::ParsePlyFileRequest& request, bool allowAsciiForTesting)
{
    HANDLE rawFile = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.sourceFileHandleValue));
    auto openResult = model_core::MappedFile::FromHandle(platform::Win32Handle(rawFile));
    if (!openResult.file.has_value()) {
        return ReportError(stdOut, request.generationId, model_core::ImportErrorCode::InternalImporterFailure);
    }

    if (openResult.file->SizeBytes() > kTierAPrimaryBytes)
        return ReportError(stdOut, request.generationId, model_core::ImportErrorCode::PrimarySourceLimit);
    std::wstring mapError;
    auto lease =
        allowAsciiForTesting
            ? openResult.file->MapWhole(mapError)
            : openResult.file->MapWindow(0, (std::min)(openResult.file->SizeBytes(), 64ull * 1024), mapError);
    if (!lease) {
        return ReportError(stdOut, request.generationId, model_core::ImportErrorCode::InternalImporterFailure);
    }

    HANDLE outputSection = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.sectionHandleValue));
    auto outputView = platform::MappedView::Map(outputSection, FILE_MAP_WRITE | FILE_MAP_READ,
                                                 static_cast<SIZE_T>(request.sectionByteCapacity));
    if (!outputView) {
        return ReportError(stdOut, request.generationId, model_core::ImportErrorCode::InternalImporterFailure);
    }

    ChunkBatchSink batchSink(GetStdHandle(STD_INPUT_HANDLE), stdOut, request.generationId);
    try {
        auto result =
            ImportPly(lease.Bytes(), outputView.bytes(), request.generationId, request.maxChunkCount,
                      allowAsciiForTesting, &batchSink, allowAsciiForTesting ? nullptr : &*openResult.file);
        if (batchSink.Preview()) {
            if (const auto* preview=std::get_if<PlyImportResult>(&result)) {
                if (!batchSink.PublishBatch(preview->chunkCount,preview->sectionBytesWritten))
                    return ReportError(stdOut,request.generationId,model_core::ImportErrorCode::Cancelled);
            } else if (std::get<model_core::ImportErrorCode>(result)!=model_core::ImportErrorCode::EmptyGeometry)
                return ReportResult(stdOut,request.generationId,result);
            batchSink.BeginScan();
            result=ImportPly(lease.Bytes(),outputView.bytes(),request.generationId,request.maxChunkCount,false,&batchSink,&*openResult.file);
        }
        if (batchSink.ProxyEnabled() && std::holds_alternative<PlyImportResult>(result)) {
            const auto first=std::get<PlyImportResult>(result);
            if (!batchSink.PublishBatch(first.chunkCount,first.sectionBytesWritten))
                return ReportError(stdOut,request.generationId,model_core::ImportErrorCode::Cancelled);
            batchSink.BeginRefinement();
            result=ImportPly(lease.Bytes(),outputView.bytes(),request.generationId,request.maxChunkCount,
                             false,&batchSink,&*openResult.file);
        }
        return ReportResult(stdOut, request.generationId, result);
    } catch (const std::bad_alloc&) {
        return ReportError(stdOut, request.generationId, model_core::ImportErrorCode::OutOfMemory);
    } catch (...) {
        return ReportError(stdOut, request.generationId, model_core::ImportErrorCode::InternalImporterFailure);
    }
}

int RunPlyImport(bool allowAsciiForTesting)
{
    HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (stdIn == nullptr || stdIn == INVALID_HANDLE_VALUE || stdOut == nullptr
        || stdOut == INVALID_HANDLE_VALUE) {
        return 1;
    }

    auto received = model_core::ReadControlMessage(stdIn);
    if (!received
        || received->header.opcode != static_cast<uint32_t>(model_core::ControlOpcode::StartPlyImportFromFile)
        || received->payload.size() != sizeof(model_core::ParsePlyFileRequest)) {
        // Honest-worker protocol sanity check, not adversarial handling.
        return 1;
    }

    model_core::ParsePlyFileRequest request{};
    std::memcpy(&request, received->payload.data(), sizeof(request));

    return HandlePlyImportFileRequest(stdOut, request, allowAsciiForTesting) ? 0 : 1;
}

} // namespace import_worker
