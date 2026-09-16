#pragma once

// Real binary- and ASCII-STL parsing, a product-owned parser (per ADR-006
// -- no third-party library) translated into the existing (unmodified)
// model_core wire format. Gate 3 slice 4 added ASCII on top of the
// already-shipped binary path (slice 2); the two dialects share the same
// per-facet validation and wire-writing logic, differing only in how a
// facet's numbers are sourced from the file.

#include "model_core/ImportError.h"
#include "model_core/MappedFile.h"

#include <cstdint>
#include <span>
#include <variant>

namespace import_worker {
class ChunkBatchSink;

struct StlImportResult {
    uint32_t chunkCount = 0;
    uint64_t sectionBytesWritten = 0;
};

// Binary facets are normalized in bounded mapped ranges and emitted as split
// clusters. sourceStlBytes may be only the 84-byte prefix when mappedSource is
// supplied. A binary-shaped file takes precedence over a leading "solid" name;
// declared count, source extent and Tier A limits are checked before allocation.
// ASCII remains an opted-in developer path. The final section is left in
// destination; intermediate sections wait for batchSink acknowledgement.
std::variant<StlImportResult, model_core::ImportErrorCode> ImportStl(
    std::span<const std::byte> sourceStlBytes, std::span<std::byte> destination, uint64_t generationId,
    uint32_t maxChunkCount, bool allowAscii = true, ChunkBatchSink* batchSink = nullptr,
    model_core::MappedFile* mappedSource = nullptr);

} // namespace import_worker
