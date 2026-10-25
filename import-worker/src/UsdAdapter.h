#pragma once

#include "model_core/ImportError.h"
#include "model_core/TierALimits.h"
#include "model_core/WireFormat.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <variant>

namespace import_worker {

// Admits a face-varying primvar expansion only if its corner count fits the
// Tier B per-scene triangle/vertex caps. The adapter calls this before it
// reserves the expanded buffer; EmitMesh re-checks the cumulative budget after
// the writer exists, but that is too late to bound the allocation. Kept here
// (header, no allocation) so the boundary is unit-testable without a fixture
// large enough to exceed 20M triangles.
inline model_core::ImportErrorCode PrimvarExpansionLimit(uint64_t corners)
{
    if (corners / 3 > model_core::kTierBTriangleLimit
        || corners > model_core::kTierBVertexLimit)
        return model_core::ImportErrorCode::ResourceLimit;
    return model_core::ImportErrorCode::None;
}

// Admits a flattened texture-coordinate primvar only if its sample count fits
// the Tier B vertex budget. PrimvarExpansionLimit bounds the expanded corner
// buffer, but that cap keys on the mesh corner count: a small mesh can carry an
// indexed or Varying primvar whose flattened sample count dwarfs its corners, so
// the sample buffer itself needs this pre-resize admission check. Reuses
// kTierBVertexLimit (no new public limit).
inline model_core::ImportErrorCode PrimvarSampleLimit(uint64_t samples)
{
    if (samples > model_core::kTierBVertexLimit)
        return model_core::ImportErrorCode::ResourceLimit;
    return model_core::ImportErrorCode::None;
}

class ChunkBatchSink;
class SidecarFileClient;
struct TextureDecodeOptions;
struct UsdzArchiveView;

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
    SidecarFileClient* sidecars = nullptr;
    const TextureDecodeOptions* textureOptions = nullptr;
    const UsdzArchiveView* archive = nullptr;
    uint64_t maxAggregateDependencyBytes = 512ull * 1024 * 1024;
    uint32_t maxDependencyCount = 64;

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
