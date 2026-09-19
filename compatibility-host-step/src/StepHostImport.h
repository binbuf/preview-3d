#pragma once

// STEP-002/STEP-003 production host route. It performs bounded Part-21
// admission on the inherited read-only source handle and, on success, runs the
// STEP-003 `StepXdeAdapter` XDE traversal into the normalized protocol-v10
// scene. The OCCT reader is never reached before admission succeeds.

#include "model_core/ControlProtocol.h"
#include "model_core/ImportError.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace step_host {

struct StepImportResult {
    model_core::ImportErrorCode errorCode = model_core::ImportErrorCode::None;
    std::uint32_t chunkCount = 0;
    std::uint64_t sectionBytesWritten = 0;
};

// Maps a preflight outcome into the closed product error taxonomy. Exposed so
// the mapping is unit-testable without launching the host.
model_core::ImportErrorCode MapPreflightStatus(std::uint32_t status);

// Runs admission then the bounded synthetic scene. `section` is the mapped
// output section and must be exactly `request.sectionByteCapacity` bytes.
StepImportResult RunStepHostImport(const model_core::ParseStepFileRequest& request,
                                   std::span<std::byte> section, HANDLE sourceHandle,
                                   HANDLE cancellationEvent);

} // namespace step_host
