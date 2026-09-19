#pragma once

#include "model_core/ImportError.h"
#include "model_core/PixelFormats.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <variant>

namespace Lib3MF { class CModel; using PModel = std::shared_ptr<CModel>; }

namespace import_worker {

struct ThreeMfDisplayCatalog;

class ChunkBatchSink;

struct ThreeMfImportResult {
    uint32_t chunkCount = 0;
    uint64_t sectionBytesWritten = 0;
};

struct ThreeMfImportFailure {
    model_core::ImportErrorCode code = model_core::ImportErrorCode::InternalImporterFailure;
    model_core::ImportFailurePhase phase = model_core::ImportFailurePhase::Geometry;
};

using ThreeMfImportOutcome = std::variant<ThreeMfImportResult, ThreeMfImportFailure>;

struct ThreeMfImportOptions {
    std::function<bool()> isCancelled;
    const ThreeMfDisplayCatalog* displayCatalog = nullptr;
    uint64_t maxAggregateTextureBytes = model_core::kMaxAggregateTextureBytes;
    uint64_t maxAggregateTexturePixels = model_core::kMaxAggregateTexturePixels;
    bool Cancelled() const { return isCancelled && isCancelled(); }
};

// Normalizes only the standard root build.  The caller owns lib3mf loading;
// this adapter retains neither the model nor source package data after return.
ThreeMfImportOutcome ImportThreeMf(const Lib3MF::PModel& model,
                                   std::span<std::byte> destination,
                                   uint64_t generationId, uint32_t maxChunkCount,
                                   ChunkBatchSink* batchSink,
                                   const ThreeMfImportOptions& options);

} // namespace import_worker
