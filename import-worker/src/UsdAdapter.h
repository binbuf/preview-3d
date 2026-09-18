#pragma once

#include "model_core/ImportError.h"
#include "model_core/WireFormat.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <variant>

namespace import_worker {

class ChunkBatchSink;

struct UsdImportResult {
    uint32_t chunkCount = 0;
    uint64_t sectionBytesWritten = 0;
};

struct UsdImportFailure {
    model_core::ImportErrorCode code = model_core::ImportErrorCode::InternalImporterFailure;
    model_core::ImportFailurePhase phase = model_core::ImportFailurePhase::Geometry;
};

using UsdImportOutcome = std::variant<UsdImportResult, UsdImportFailure>;

struct UsdImportOptions {
    model_core::SourceFormatId format = model_core::SourceFormatId::Unknown;
    std::function<bool()> isCancelled;

    bool Cancelled() const { return isCancelled && isCancelled(); }
};

// Normalizes the USD-001 self-contained static subset. The adapter owns no
// source bytes after it returns and publishes only protocol-v10 records.
UsdImportOutcome ImportUsd(std::span<const std::byte> sourceBytes,
                           std::span<std::byte> destination,
                           uint64_t generationId,
                           uint32_t maxChunkCount,
                           ChunkBatchSink* batchSink,
                           const UsdImportOptions& options);

} // namespace import_worker
