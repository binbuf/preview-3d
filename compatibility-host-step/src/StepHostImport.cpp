#include "StepHostImport.h"

#include "StepPart21Preflight.h"
#include "StepXdeAdapter.h"

#include <cstdint>

namespace step_host {

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
                                   HANDLE cancellationEvent)
{
    StepImportResult result;
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

    // Product-owned lexical admission always runs before the OCCT semantic
    // transfer; the production XDE reader cannot be reached until it passes.
    const StepPreflightResult preflight
        = StepPreflightHandle(sourceHandle, static_cast<std::uint64_t>(size.QuadPart));
    if (!preflight.ok()) {
        result.errorCode = MapPreflightStatus(static_cast<std::uint32_t>(preflight.status));
        return result;
    }
    if (cancellationEvent && cancellationEvent != INVALID_HANDLE_VALUE
        && WaitForSingleObject(cancellationEvent, 0) == WAIT_OBJECT_0) {
        result.errorCode = model_core::ImportErrorCode::Cancelled;
        return result;
    }

    const StepXdeResult scene
        = RunStepXdeAdapter(request, section, sourceHandle, cancellationEvent);
    result.errorCode = scene.errorCode;
    result.chunkCount = scene.chunkCount;
    result.sectionBytesWritten = scene.sectionBytesWritten;
    return result;
}

} // namespace step_host