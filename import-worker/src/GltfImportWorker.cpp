#include "GltfImportWorker.h"

#include "ChunkBatchSink.h"
#include "GltfAdapter.h"
#include "SidecarFileClient.h"

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
                   const std::variant<GltfImportResult, model_core::ImportErrorCode>& result)
{
    if (const auto* errorCode = std::get_if<model_core::ImportErrorCode>(&result)) {
        model_core::GenerationErrorNotice notice{};
        notice.generationId = generationId;
        notice.errorCode = static_cast<uint32_t>(*errorCode);
        model_core::WriteControlMessage(stdOut, model_core::ControlOpcode::GenerationError, &notice,
                                         sizeof(notice));
        return false;
    }

    const auto& info = std::get<GltfImportResult>(result);
    model_core::ChunksReadyNotice notice{};
    notice.generationId = generationId;
    notice.chunkCount = info.chunkCount;
    notice.sectionBytesWritten = info.sectionBytesWritten;
    model_core::WriteControlMessage(stdOut, model_core::ControlOpcode::ChunksReady, &notice, sizeof(notice));
    return true;
}

bool ReportError(HANDLE stdOut, uint64_t generationId, model_core::ImportErrorCode code)
{
    std::variant<GltfImportResult, model_core::ImportErrorCode> result = code;
    return ReportResult(stdOut, generationId, result);
}

} // namespace

// The existing path: a pre-made, whole-source shared/pagefile section the
// host copied the GLB bytes into (StartGltfImport/ParseGltfRequest).
bool HandleGltfImportRequest(HANDLE stdOut, const model_core::ParseGltfRequest& request)
{
    HANDLE sourceSection = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.sourceHandleValue));
    auto sourceView = platform::MappedView::Map(sourceSection, FILE_MAP_READ,
                                                 static_cast<SIZE_T>(request.sourceByteLength));
    if (!sourceView) {
        return ReportError(stdOut, request.generationId, model_core::ImportErrorCode::InternalImporterFailure);
    }

    HANDLE outputSection = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.sectionHandleValue));
    auto outputView = platform::MappedView::Map(outputSection, FILE_MAP_WRITE | FILE_MAP_READ,
                                                 static_cast<SIZE_T>(request.sectionByteCapacity));
    if (!outputView) {
        return ReportError(stdOut, request.generationId, model_core::ImportErrorCode::InternalImporterFailure);
    }

    auto result = ImportGltf(sourceView.bytes(), outputView.bytes(), request.generationId, request.maxChunkCount);
    return ReportResult(stdOut, request.generationId, result);
}

// The real pipeline: a raw FILE handle the broker duplicated in from the
// trusted process's own CreateFileW/GetFinalPathNameByHandleW-canonicalized
// open (StartGltfImportFromFile/ParseGltfFileRequest). The worker builds
// its own model_core::MappedFile from that handle, per
// .docs/design/03-file-formats-and-ingestion.md's "Mapped-file abstraction"
// step 1 ("...or, for a duplicated handle received from the broker, reopen
// a mapping directly from that handle without a fresh CreateFileW call").
bool HandleGltfImportFileRequest(HANDLE stdIn, HANDLE stdOut, const model_core::ParseGltfFileRequest& request)
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

    SidecarFileClient sidecarClient(stdIn, stdOut, request.generationId);
    // Progressive delivery is offered on the real-file path only, which is
    // the product's own path and the one that meets models too big for a
    // single window. The pre-made-section path above keeps its one-shot shape
    // -- its source is a section the host already sized to hold the whole
    // file, so it has no large-model case to serve.
    ChunkBatchSink batchSink(stdIn, stdOut, request.generationId);
    auto result = ImportGltf(lease.Bytes(), outputView.bytes(), request.generationId, request.maxChunkCount,
                              &sidecarClient, &batchSink);
    return ReportResult(stdOut, request.generationId, result);
}

int RunGltfImport()
{
    HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (stdIn == nullptr || stdIn == INVALID_HANDLE_VALUE || stdOut == nullptr
        || stdOut == INVALID_HANDLE_VALUE) {
        return 1;
    }

    auto received = model_core::ReadControlMessage(stdIn);
    if (!received) {
        // Honest-worker protocol sanity check, not adversarial handling.
        return 1;
    }

    if (received->header.opcode == static_cast<uint32_t>(model_core::ControlOpcode::StartGltfImport)
        && received->payload.size() == sizeof(model_core::ParseGltfRequest)) {
        model_core::ParseGltfRequest request{};
        std::memcpy(&request, received->payload.data(), sizeof(request));
        return HandleGltfImportRequest(stdOut, request) ? 0 : 1;
    }

    if (received->header.opcode == static_cast<uint32_t>(model_core::ControlOpcode::StartGltfImportFromFile)
        && received->payload.size() == sizeof(model_core::ParseGltfFileRequest)) {
        model_core::ParseGltfFileRequest request{};
        std::memcpy(&request, received->payload.data(), sizeof(request));
        return HandleGltfImportFileRequest(stdIn, stdOut, request) ? 0 : 1;
    }

    return 1;
}

} // namespace import_worker
