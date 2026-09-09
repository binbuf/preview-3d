#pragma once

// Real binary- and ASCII-STL parsing, a product-owned parser (per ADR-006
// -- no third-party library) translated into the existing (unmodified)
// model_core wire format. Gate 3 slice 4 added ASCII on top of the
// already-shipped binary path (slice 2); the two dialects share the same
// per-facet validation and wire-writing logic, differing only in how a
// facet's numbers are sourced from the file.

#include "model_core/ImportError.h"

#include <cstdint>
#include <span>
#include <variant>

namespace import_worker {

struct StlImportResult {
    uint32_t chunkCount = 0;
    uint64_t sectionBytesWritten = 0;
};

// Detects binary vs. ASCII STL and parses sourceStlBytes accordingly,
// writing header+descriptor+payload into destination per the model_core
// wire format -- same "compute everything, check total size once, then
// write sequentially with the header written last" structure
// GltfAdapter.cpp/SyntheticSceneGenerator already use. Always emits exactly
// one chunk on success (or an ImportErrorCode on any parse failure,
// resource-limit violation, or empty-after-validation result -- e.g. every
// facet dropped as degenerate/non-finite).
//
// Detection: a file whose 80-byte header's declared triangle count matches
// a binary layout consistent with the actual byte length (trailing bytes
// tolerated, same as before) is always parsed as binary, even if it also
// happens to start with the ASCII keyword "solid" (some binary STL writers
// put a "solid <name>"-style comment in the free-form header). Otherwise, a
// file starting with "solid" is parsed as ASCII text. Anything else is
// rejected -- same MalformedData/ResourceLimit outcomes the binary-only
// version of this function already produced for a non-binary-shaped input.
std::variant<StlImportResult, model_core::ImportErrorCode> ImportStl(
    std::span<const std::byte> sourceStlBytes, std::span<std::byte> destination,
    uint64_t generationId, uint32_t maxChunkCount);

} // namespace import_worker
