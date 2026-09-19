#include "StepHostImport.h"

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

StepImportResult RunStepHostImport(const model_core::ParseStepFileRequest& request,
                                   std::span<std::byte> section, HANDLE sourceHandle,
                                   HANDLE cancellationEvent, const StepBatchPublisher& publish,
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
            // lexical scanner, which correctly rejects NUL as a non-Part-21
            // control byte.
            mappedView = platform::MappedView::Map(
                mapping.get(), FILE_MAP_READ, static_cast<SIZE_T>(size.QuadPart));
        }
    }
    const std::span<const std::byte> mappedSource =
        mappedView ? mappedView.bytes() : std::span<const std::byte>{};

    // Product-owned lexical admission always runs before the OCCT semantic
    // transfer; the production XDE reader cannot be reached until it passes.
    const auto preflightStart = std::chrono::steady_clock::now();
    const StepPreflightResult preflight = mappedView
        ? StepPreflightBytes(mappedSource)
        : StepPreflightHandle(sourceHandle, static_cast<std::uint64_t>(size.QuadPart));
    result.timings.preflightMilliseconds = static_cast<std::uint64_t>(ElapsedMilliseconds(preflightStart));
    if (!preflight.ok()) {
        result.errorCode = MapPreflightStatus(static_cast<std::uint32_t>(preflight.status));
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

    const StepXdeResult scene = RunStepXdeAdapter(request, section, sourceHandle, cancellationEvent,
                                                  mappedSource, {}, publish, progress);
    result.errorCode = scene.errorCode;
    result.chunkCount = scene.chunkCount;
    result.batchCount = scene.batchCount;
    result.sectionBytesWritten = scene.sectionBytesWritten;
    result.timings = scene.timings;
    result.timings.preflightMilliseconds = static_cast<std::uint64_t>(ElapsedMilliseconds(preflightStart));
    result.timings.totalMilliseconds = static_cast<std::uint64_t>(ElapsedMilliseconds(totalStart));
    return result;
}

} // namespace step_host
