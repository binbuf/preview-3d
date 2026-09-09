#pragma once

#include <cstdint>

namespace model_core {

// A deliberately small subset of the full error taxonomy in
// .docs/design/03-file-formats-and-ingestion.md ("Error taxonomy") -- just
// enough for the honest-worker-reports-a-real-problem and
// validator-rejects-malformed-input cases this chunk covers. The full
// enumeration belongs to later work integrating with interactive-viewer.
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
};

} // namespace model_core
