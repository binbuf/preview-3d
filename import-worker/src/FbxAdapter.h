#pragma once

#include "model_core/ImportError.h"

#include <cstdint>
#include <functional>
#include <span>
#include <variant>

namespace import_worker {

class ChunkBatchSink;

struct FbxImportResult {
    uint32_t chunkCount = 0;
    uint64_t sectionBytesWritten = 0;
};

struct FbxImportFailure {
    model_core::ImportErrorCode code = model_core::ImportErrorCode::InternalImporterFailure;
    model_core::ImportFailurePhase phase = model_core::ImportFailurePhase::Geometry;
};

struct FbxImportOptions {
    std::function<bool()> isCancelled;
    bool Cancelled() const { return isCancelled && isCancelled(); }
};

using FbxImportOutcome = std::variant<FbxImportResult, FbxImportFailure>;

// Tier-B static FBX import. This FBX-003 boundary intentionally emits only
// undeformed polygon geometry and neutral materials; FBX-004 and FBX-005 own
// deformation and material/image evaluation respectively.
FbxImportOutcome ImportFbx(std::span<const std::byte> sourceBytes,
                           std::span<std::byte> destination, uint64_t generationId,
                           uint32_t maxChunkCount, ChunkBatchSink* batchSink,
                           const FbxImportOptions& options = {});

} // namespace import_worker
