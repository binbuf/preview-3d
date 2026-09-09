#pragma once

// Real binary-STL parsing, a product-owned parser (per ADR-006 -- no
// third-party library) translated into the existing (unmodified)
// model_core wire format. Scope: binary STL only -- ASCII STL is Tier B
// (".docs/design/03-file-formats-and-ingestion.md": "text parsing touches
// and converts the complete source") and is a later, separate slice.

#include "model_core/ImportError.h"

#include <cstdint>
#include <span>
#include <variant>

namespace import_worker {

struct StlImportResult {
    uint32_t chunkCount = 0;
    uint64_t sectionBytesWritten = 0;
};

// Parses sourceStlBytes (binary STL: 80-byte header + uint32 LE triangle
// count + N x 50-byte facets) and writes header+descriptor+payload into
// destination, per the model_core wire format -- same "compute everything,
// check total size once, then write sequentially with the header written
// last" structure GltfAdapter.cpp/SyntheticSceneGenerator already use.
// Always emits exactly one chunk on success (or an ImportErrorCode on any
// parse failure, resource-limit violation, or empty-after-validation
// result -- e.g. every facet dropped as degenerate/non-finite).
std::variant<StlImportResult, model_core::ImportErrorCode> ImportStl(
    std::span<const std::byte> sourceStlBytes, std::span<std::byte> destination,
    uint64_t generationId, uint32_t maxChunkCount);

} // namespace import_worker
