#pragma once

#include "OpenUsdSpikeProtocol.h"
#include "model_core/ControlProtocol.h"
#include "model_core/ImportError.h"

#include <cstddef>
#include <cstdint>

extern "C" __declspec(dllexport) int __cdecl Preview3DRunOpenUsdSpike(
    compatibility_host::OpenUsdSpikeSection* output, const std::byte* section,
    std::size_t sectionSize, const wchar_t* payloadDirectory);

// Production bootstrap hook used by USD-006 before any stage is opened. The
// implementation hashes the closed resource inventory selected by USD-002.
extern "C" __declspec(dllexport) int __cdecl Preview3DAuditOpenUsdPayload(
    const wchar_t* payloadDirectory);

namespace compatibility_host {
struct OpenUsdImportResult {
    model_core::ImportErrorCode errorCode = model_core::ImportErrorCode::InternalImporterFailure;
    model_core::ImportFailurePhase phase = model_core::ImportFailurePhase::Geometry;
    std::uint32_t chunkCount = 0;
    std::uint64_t sectionBytesWritten = 0;
};
} // namespace compatibility_host

// Production entry point. All handles are borrowed for the duration of the
// call. The implementation owns stage composition and normalized section
// writes; the bootstrap remains free of OpenUSD imports.
extern "C" __declspec(dllexport) int __cdecl Preview3DRunOpenUsdImport(
    const model_core::ParseOpenUsdFileRequest* request,
    std::byte* outputSection, std::size_t outputSectionSize,
    void* controlInput, void* controlOutput,
    const wchar_t* payloadDirectory, compatibility_host::OpenUsdImportResult* result);
