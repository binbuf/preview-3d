#include "StepHostImport.h"

#include "CadPreflight.h"
#include "FcstdPreflight.h"
#include "StepPart21Preflight.h"
#include "StepXdeAdapter.h"

#include "platform/MappedView.h"
#include "platform/Win32Handle.h"

#include <chrono>
#include <cstdint>

namespace step_host {
namespace {

double ElapsedMilliseconds(const std::chrono::steady_clock::time_point& start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

StepTessellationQuality MapImportQuality(model_core::StepImportQuality quality)
{
    switch (quality) {
    case model_core::StepImportQuality::Draft: return StepTessellationQuality::Draft;
    case model_core::StepImportQuality::Full: return StepTessellationQuality::Full;
    case model_core::StepImportQuality::Balanced: break;
    }
    return StepTessellationQuality::Balanced;
}

model_core::ParseStepFileRequest NarrowStepRequest(
    const model_core::ParseCadFileRequest& request)
{
    model_core::ParseStepFileRequest narrow{};
    narrow.generationId = request.generationId;
    narrow.sourceFileHandleValue = request.sourceFileHandleValue;
    narrow.sectionHandleValue = request.sectionHandleValue;
    narrow.sectionByteCapacity = request.sectionByteCapacity;
    narrow.maxChunkCount = request.maxChunkCount;
    narrow.requestFlags = request.requestFlags;
    narrow.cancellationEventHandleValue = request.cancellationEventHandleValue;
    return narrow;
}

struct PreflightOutcome {
    model_core::ImportErrorCode errorCode = model_core::ImportErrorCode::None;
    std::uint32_t unresolvedExternalDocuments = 0;
    std::uint64_t lexedBytes = 0;
    std::uint64_t milliseconds = 0;
};

PreflightOutcome RunCadPreflight(model_core::CadFormat format,
                                 std::span<const std::byte> mappedSource,
                                 HANDLE sourceHandle, std::uint64_t size)
{
    PreflightOutcome outcome;
    const auto start = std::chrono::steady_clock::now();
    switch (format) {
    case model_core::CadFormat::Step: {
        const StepPreflightResult preflight = !mappedSource.empty()
            ? StepPreflightBytes(mappedSource)
            : StepPreflightHandle(sourceHandle, size);
        outcome.errorCode = MapPreflightStatus(static_cast<std::uint32_t>(preflight.status));
        outcome.unresolvedExternalDocuments = preflight.externalDocuments;
        outcome.lexedBytes = preflight.lexedBytes;
        break;
    }
    case model_core::CadFormat::Iges: {
        const CadPreflightResult preflight = !mappedSource.empty()
            ? IgesPreflightBytes(mappedSource)
            : IgesPreflightHandle(sourceHandle, size);
        outcome.errorCode = MapCadPreflightStatus(static_cast<std::uint32_t>(preflight.status));
        outcome.lexedBytes = preflight.lexedBytes;
        break;
    }
    case model_core::CadFormat::Brep: {
        const CadPreflightResult preflight = !mappedSource.empty()
            ? BrepPreflightBytes(mappedSource)
            : BrepPreflightHandle(sourceHandle, size);
        outcome.errorCode = MapCadPreflightStatus(static_cast<std::uint32_t>(preflight.status));
        outcome.lexedBytes = preflight.lexedBytes;
        break;
    }
    case model_core::CadFormat::Xbf: {
        const CadPreflightResult preflight = !mappedSource.empty()
            ? XbfPreflightBytes(mappedSource)
            : XbfPreflightHandle(sourceHandle, size);
        outcome.errorCode = MapCadPreflightStatus(static_cast<std::uint32_t>(preflight.status));
        outcome.lexedBytes = preflight.lexedBytes;
        break;
    }
    case model_core::CadFormat::Fcstd: {
        // FCStd cannot be admitted from an unmapped handle: the bounded ZIP
        // scan and the later entry extraction both need the same private view.
        // The host always creates it, so an empty mapping is a host fault.
        if (mappedSource.empty()) {
            outcome.errorCode = model_core::ImportErrorCode::FileUnavailable;
            break;
        }
        const FcstdPreflightStatus preflight = InspectFcstd(mappedSource);
        outcome.errorCode = MapFcstdPreflightStatus(static_cast<std::uint32_t>(preflight));
        outcome.lexedBytes = mappedSource.size();
        break;
    }
    default:
        outcome.errorCode = model_core::ImportErrorCode::ImportProtocolViolation;
        break;
    }
    outcome.milliseconds = static_cast<std::uint64_t>(ElapsedMilliseconds(start));
    return outcome;
}

StepImportResult RunCadHostImportImpl(const model_core::ParseStepFileRequest& request,
                                      model_core::CadFormat cadFormat,
                                      std::span<std::byte> section, HANDLE sourceHandle,
                                      HANDLE cancellationEvent,
                                      const StepBatchPublisher& publish,
                                      const StepProgressSink& progress)
{
    StepImportResult result;
    const auto totalStart = std::chrono::steady_clock::now();
    if (!request.generationId || !request.sourceFileHandleValue || !request.sectionHandleValue
        || request.sectionByteCapacity < sizeof(model_core::SectionHeader)
        || section.size() < static_cast<std::size_t>(request.sectionByteCapacity)
        || !request.maxChunkCount) {
        result.errorCode = model_core::ImportErrorCode::ImportProtocolViolation;
        return result;
    }
    if (!model_core::IsKnownCadFormat(cadFormat)) {
        result.errorCode = model_core::ImportErrorCode::ImportProtocolViolation;
        return result;
    }
    model_core::StepImportQuality importQuality = model_core::StepImportQuality::Balanced;
    if (!model_core::StepImportQualityFromFlags(request.requestFlags, importQuality)) {
        result.errorCode = model_core::ImportErrorCode::ImportProtocolViolation;
        return result;
    }
    if (cancellationEvent && cancellationEvent != INVALID_HANDLE_VALUE
        && WaitForSingleObject(cancellationEvent, 0) == WAIT_OBJECT_0) {
        result.errorCode = model_core::ImportErrorCode::Cancelled;
        return result;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(sourceHandle, &size) || size.QuadPart <= 0) {
        result.errorCode = model_core::ImportErrorCode::FileUnavailable;
        return result;
    }

    // STEP-005 work item 4: map the inherited read-only handle once and run
    // both lexical admission and the OCCT transfer over that single view,
    // removing the pre-STEP-005 double full read. No path is ever opened; the
    // mapping is created from the duplicated handle. If mapping fails for any
    // reason the proven per-handle reads below remain the fallback.
    platform::Win32Handle mapping;
    platform::MappedView mappedView;
    {
        HANDLE rawMapping = CreateFileMappingW(sourceHandle, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (rawMapping) {
            mapping.reset(rawMapping);
            // Map exactly the file size: mapping to end-of-file would expose
            // the zero-filled tail of the final allocation granule to the
            // lexical scanner, which correctly rejects NUL as a control byte.
            mappedView = platform::MappedView::Map(
                mapping.get(), FILE_MAP_READ, static_cast<SIZE_T>(size.QuadPart));
        }
    }
    const std::span<const std::byte> mappedSource =
        mappedView ? mappedView.bytes() : std::span<const std::byte>{};

    // Product-owned lexical admission always runs before the OCCT semantic
    // transfer; the production reader cannot be reached until it passes.
    const PreflightOutcome preflight = RunCadPreflight(
        cadFormat, mappedSource, sourceHandle, static_cast<std::uint64_t>(size.QuadPart));
    result.timings.preflightMilliseconds = preflight.milliseconds;
    if (preflight.errorCode != model_core::ImportErrorCode::None) {
        result.errorCode = preflight.errorCode;
        return result;
    }
    if (progress) {
        StepProgressEvent event;
        event.phase = model_core::kStepPhasePreflight;
        event.preflightBytes = preflight.lexedBytes;
        event.phaseMilliseconds = result.timings.preflightMilliseconds;
        event.totalMilliseconds = static_cast<std::uint64_t>(ElapsedMilliseconds(totalStart));
        progress(event);
    }
    if (cancellationEvent && cancellationEvent != INVALID_HANDLE_VALUE
        && WaitForSingleObject(cancellationEvent, 0) == WAIT_OBJECT_0) {
        result.errorCode = model_core::ImportErrorCode::Cancelled;
        return result;
    }

    StepImportPolicy policy;
    policy.quality = MapImportQuality(importQuality);
    policy.unresolvedExternalDocuments = preflight.unresolvedExternalDocuments;
    const StepXdeResult scene = RunStepXdeAdapter(request, section, sourceHandle, cancellationEvent,
                                                  mappedSource, {}, publish, progress, policy,
                                                  cadFormat);
    result.errorCode = scene.errorCode;
    result.chunkCount = scene.chunkCount;
    result.batchCount = scene.batchCount;
    result.sectionBytesWritten = scene.sectionBytesWritten;
    result.timings = scene.timings;
    result.timings.preflightMilliseconds = preflight.milliseconds;
    result.timings.totalMilliseconds = static_cast<std::uint64_t>(ElapsedMilliseconds(totalStart));
    return result;
}

} // namespace

model_core::ImportErrorCode MapPreflightStatus(std::uint32_t status)
{
    switch (static_cast<StepPreflightStatus>(status)) {
    case StepPreflightStatus::Ok:
        return model_core::ImportErrorCode::None;
    case StepPreflightStatus::NotPart21:
    case StepPreflightStatus::MalformedSyntax:
    case StepPreflightStatus::DuplicateEntity:
        return model_core::ImportErrorCode::MalformedData;
    case StepPreflightStatus::UnsupportedEncoding:
        return model_core::ImportErrorCode::UnsupportedEncoding;
    case StepPreflightStatus::EntityLimit:
    case StepPreflightStatus::ReferenceLimit:
    case StepPreflightStatus::DepthLimit:
    case StepPreflightStatus::RecordLengthLimit:
    case StepPreflightStatus::StringLengthLimit:
    case StepPreflightStatus::SourceLimit:
        return model_core::ImportErrorCode::ResourceLimit;
    case StepPreflightStatus::ExternalDocument:
        return model_core::ImportErrorCode::UnsupportedRequiredFeature;
    case StepPreflightStatus::ReadFailure:
        return model_core::ImportErrorCode::FileUnavailable;
    }
    return model_core::ImportErrorCode::InternalImporterFailure;
}

model_core::ImportErrorCode MapCadPreflightStatus(std::uint32_t status)
{
    switch (static_cast<CadPreflightStatus>(status)) {
    case CadPreflightStatus::Ok:
        return model_core::ImportErrorCode::None;
    case CadPreflightStatus::NotIges:
    case CadPreflightStatus::NotBrep:
    case CadPreflightStatus::NotXbf:
    case CadPreflightStatus::MalformedSyntax:
        return model_core::ImportErrorCode::MalformedData;
    case CadPreflightStatus::UnsupportedEncoding:
        return model_core::ImportErrorCode::UnsupportedEncoding;
    case CadPreflightStatus::RecordLimit:
    case CadPreflightStatus::EntityLimit:
    case CadPreflightStatus::ReferenceLimit:
    case CadPreflightStatus::SourceLimit:
        return model_core::ImportErrorCode::ResourceLimit;
    case CadPreflightStatus::ReadFailure:
        return model_core::ImportErrorCode::FileUnavailable;
    }
    return model_core::ImportErrorCode::InternalImporterFailure;
}

StepImportResult RunStepHostImport(const model_core::ParseStepFileRequest& request,
                                   std::span<std::byte> section, HANDLE sourceHandle,
                                   HANDLE cancellationEvent, const StepBatchPublisher& publish,
                                   const StepProgressSink& progress)
{
    return RunCadHostImportImpl(request, model_core::CadFormat::Step, section, sourceHandle,
                                cancellationEvent, publish, progress);
}

StepImportResult RunCadHostImport(const model_core::ParseCadFileRequest& request,
                                  std::span<std::byte> section, HANDLE sourceHandle,
                                  HANDLE cancellationEvent,
                                  const StepBatchPublisher& publish,
                                  const StepProgressSink& progress)
{
    const model_core::CadFormat cadFormat = static_cast<model_core::CadFormat>(request.cadFormat);
    if (!model_core::IsKnownCadFormat(cadFormat)) {
        StepImportResult result;
        result.errorCode = model_core::ImportErrorCode::ImportProtocolViolation;
        return result;
    }
    return RunCadHostImportImpl(NarrowStepRequest(request), cadFormat, section, sourceHandle,
                                cancellationEvent, publish, progress);
}

} // namespace step_host
