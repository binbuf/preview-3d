#pragma once

#include <cstdint>

namespace model_core {

// Closed error taxonomy shared by sandbox reports and host/UI diagnostics.
enum class ImportFailurePhase : uint32_t { Unspecified = 0, Geometry = 1, Sidecars = 2, Textures = 3 };
enum class ImportErrorCode : uint32_t {
    None = 0,
    MalformedData = 1,
    ResourceLimit = 2,
    ImportProtocolViolation = 3,
    InternalImporterFailure = 4,
    // Both names taken verbatim from the design doc's own "Error taxonomy"
    // list ("FileUnavailable, FileChanged, UnsafeReference; ..."), added
    // for the sidecar resolver: UnsafeReference is a containment-policy
    // rejection (absolute/UNC/traversal/wrong-extension/escapes-directory);
    // FileUnavailable covers not-found, zero-length, and over-size.
    UnsafeReference = 5,
    FileUnavailable = 6,
    Cancelled = 7,
    UnsupportedFormat = 8,
    UnsupportedEncoding = 9,
    UnsupportedRequiredFeature = 10,
    EmptyGeometry = 11,
    OutOfMemory = 12,
    FileChanged = 13,
    WorkerCrashed = 14,
    WorkerTimedOut = 15,
    UploadFailure = 16,
};

constexpr bool IsKnownImportErrorCode(uint32_t code)
{
    return code >= uint32_t(ImportErrorCode::MalformedData)
        && code <= uint32_t(ImportErrorCode::UploadFailure);
}

} // namespace model_core
