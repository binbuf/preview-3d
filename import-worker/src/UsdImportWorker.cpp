#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "UsdImportWorker.h"

#include "ChunkBatchSink.h"
#include "SidecarFileClient.h"
#include "TextureDecodePolicy.h"
#include "UsdAdapter.h"
#include "UsdZipPreflight.h"
#include "model_core/ControlChannelIo.h"
#include "model_core/MappedFile.h"
#include "model_core/TierALimits.h"
#include "platform/MappedView.h"
#include "platform/Win32Handle.h"

#include "tinyusdz.hh"

#include <cstring>
#include <new>
#include <span>

namespace import_worker {
namespace {

using model_core::ImportErrorCode;
using model_core::SourceFormatId;

SourceFormatId DetectUsdEncoding(std::span<const std::byte> source)
{
    const auto* bytes = reinterpret_cast<const uint8_t*>(source.data());
    if (tinyusdz::IsUSDA(bytes, source.size())) return SourceFormatId::Usda;
    if (tinyusdz::IsUSDC(bytes, source.size())) return SourceFormatId::Usdc;
    if (tinyusdz::IsUSDZ(bytes, source.size())) return SourceFormatId::Usdz;
    return SourceFormatId::Unknown;
}

bool EncodingMatchesRequest(SourceFormatId detected, uint32_t flags)
{
    const uint32_t expected = flags & model_core::kImportRequestUsdExpectedMask;
    if (expected == 0) return true; // `.usd`: bytes are authoritative.
    if ((expected & (expected - 1)) != 0) return false;
    return (expected == model_core::kImportRequestUsdExpectedUsda && detected == SourceFormatId::Usda)
        || (expected == model_core::kImportRequestUsdExpectedUsdc && detected == SourceFormatId::Usdc)
        || (expected == model_core::kImportRequestUsdExpectedUsdz && detected == SourceFormatId::Usdz);
}

bool ReportError(HANDLE stdOut, uint64_t generationId, ImportErrorCode code,
                 model_core::ImportFailurePhase phase = model_core::ImportFailurePhase::Geometry)
{
    model_core::GenerationErrorNotice notice{};
    notice.generationId = generationId;
    notice.errorCode = uint32_t(code);
    notice.reserved0 = uint32_t(phase);
    model_core::WriteControlMessage(stdOut, model_core::ControlOpcode::GenerationError,
                                    &notice, sizeof(notice));
    return false;
}

bool ReportOutcome(HANDLE stdOut, uint64_t generationId, const UsdImportOutcome& outcome)
{
    if (const auto* failure = std::get_if<UsdImportFailure>(&outcome))
        return ReportError(stdOut, generationId, failure->code, failure->phase);
    const auto& result = std::get<UsdImportResult>(outcome);
    model_core::ChunksReadyNotice notice{};
    notice.generationId = generationId;
    notice.chunkCount = result.chunkCount;
    notice.sectionBytesWritten = result.sectionBytesWritten;
    model_core::WriteControlMessage(stdOut, model_core::ControlOpcode::ChunksReady,
                                    &notice, sizeof(notice));
    return true;
}

} // namespace

bool HandleUsdImportFileRequest(HANDLE stdIn, HANDLE stdOut,
                                const model_core::ParseUsdFileRequest& request)
{
    const uint32_t allowedFlags = model_core::kImportRequestUsdExpectedMask;
    if ((request.requestFlags & ~allowedFlags) != 0)
        return ReportError(stdOut, request.generationId, ImportErrorCode::ImportProtocolViolation);
    if (request.cancellationEventHandleValue) {
        HANDLE cancellation = reinterpret_cast<HANDLE>(
            static_cast<uintptr_t>(request.cancellationEventHandleValue));
        if (WaitForSingleObject(cancellation, 0) == WAIT_OBJECT_0)
            return ReportError(stdOut, request.generationId, ImportErrorCode::Cancelled);
    }

    HANDLE rawFile = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.sourceFileHandleValue));
    auto opened = model_core::MappedFile::FromHandle(platform::Win32Handle(rawFile));
    if (!opened.file)
        return ReportError(stdOut, request.generationId, ImportErrorCode::InternalImporterFailure);
    if (opened.file->SizeBytes() > model_core::kTierBPrimarySourceBytes)
        return ReportError(stdOut, request.generationId, ImportErrorCode::PrimarySourceLimit);
    std::wstring mapError;
    auto source = opened.file->MapWhole(mapError);
    if (!source)
        return ReportError(stdOut, request.generationId, ImportErrorCode::InternalImporterFailure);

    const SourceFormatId format = DetectUsdEncoding(source.Bytes());
    if (format == SourceFormatId::Unknown)
        return ReportError(stdOut, request.generationId, ImportErrorCode::MalformedData);
    if (!EncodingMatchesRequest(format, request.requestFlags))
        return ReportError(stdOut, request.generationId, ImportErrorCode::UnsupportedEncoding);
    UsdzArchiveView archive;
    if (format == SourceFormatId::Usdz) {
        HANDLE cancellation = reinterpret_cast<HANDLE>(
            static_cast<uintptr_t>(request.cancellationEventHandleValue));
        const auto preflight = InspectUsdz(source.Bytes(), &archive, {}, [cancellation] {
            return cancellation && WaitForSingleObject(cancellation, 0) == WAIT_OBJECT_0;
        });
        if (preflight == UsdzPreflightError::Cancelled)
            return ReportError(stdOut, request.generationId, ImportErrorCode::Cancelled);
        if (preflight != UsdzPreflightError::None)
            return ReportError(stdOut, request.generationId, ImportErrorCode::ArchiveLimit);
    }

    platform::Win32Handle outputSection(reinterpret_cast<HANDLE>(
        static_cast<uintptr_t>(request.sectionHandleValue)));
    auto output = platform::MappedView::Map(outputSection.get(), FILE_MAP_WRITE | FILE_MAP_READ,
                                            static_cast<SIZE_T>(request.sectionByteCapacity));
    if (!output)
        return ReportError(stdOut, request.generationId, ImportErrorCode::InternalImporterFailure);

    platform::Win32Handle cancellationEvent(reinterpret_cast<HANDLE>(
        static_cast<uintptr_t>(request.cancellationEventHandleValue)));
    ChunkBatchSink sink(stdIn, stdOut, request.generationId, request.requestFlags,
                        cancellationEvent.get());
    SidecarFileClient sidecars(stdIn, stdOut, request.generationId);
    sidecars.EnablePinnedReplay();
    TextureDecodeOptions textureOptions;
    textureOptions.isCancelled = [&sink] { return sink.Cancelled(); };
    UsdImportOptions options;
    options.format = format;
    options.isCancelled = [&sink] { return sink.Cancelled(); };
    options.sidecars = &sidecars;
    options.textureOptions = &textureOptions;
    options.archive = format == SourceFormatId::Usdz ? &archive : nullptr;
    try {
        return ReportOutcome(stdOut, request.generationId,
            ImportUsd(source.Bytes(), output.bytes(), request.generationId,
                      request.maxChunkCount, &sink, options));
    } catch (const std::bad_alloc&) {
        return ReportError(stdOut, request.generationId, ImportErrorCode::OutOfMemory);
    } catch (...) {
        return ReportError(stdOut, request.generationId,
                           ImportErrorCode::InternalImporterFailure);
    }
}

int RunUsdImport()
{
    HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE), stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!stdIn || stdIn == INVALID_HANDLE_VALUE || !stdOut || stdOut == INVALID_HANDLE_VALUE)
        return 1;
    auto message = model_core::ReadControlMessage(stdIn);
    if (!message
        || message->header.opcode != uint32_t(model_core::ControlOpcode::StartUsdImportFromFile)
        || message->payload.size() != sizeof(model_core::ParseUsdFileRequest)) return 1;
    model_core::ParseUsdFileRequest request{};
    std::memcpy(&request, message->payload.data(), sizeof(request));
    return HandleUsdImportFileRequest(stdIn, stdOut, request) ? 0 : 1;
}

} // namespace import_worker
