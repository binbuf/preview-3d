#pragma once

// Host-side copy-then-validate acceptance path for a normalized-chunk
// shared section. Per .docs/design/02-system-architecture.md: "A shared
// section is not itself a trust boundary... the loader/broker pool
// therefore treats a chunk as untrusted right up until it has been copied."
//
// ValidateAndCopySection performs the full check sequence documented below
// before copying any payload bytes into host-owned memory, and never
// retains a reference into the caller's section view after returning -- the
// caller may safely UnmapViewOfFile immediately after this call returns.
//
// Fail-closed at the section level: any single check failure (header or any
// one chunk) rejects the whole batch, matching "the broker... does not
// retry the same failing stage in a loop." This validator is exercised only
// against honest (if malformed) input in this chunk; adversarial mutation/
// replay/spoofing proof is the deferred synthetic hostile-worker suite.

#include "model_core/ImportError.h"
#include "model_core/WireFormat.h"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace import_broker {

struct ValidatedChunk {
    model_core::ChunkDescriptor descriptor; // copied into private host memory
    std::vector<std::byte> payload;         // copied into private host memory
};

struct ValidationResult {
    bool ok = false;
    std::vector<ValidatedChunk> chunks;
    model_core::ImportErrorCode errorCode = model_core::ImportErrorCode::None;
    std::string diagnosticMessage; // developer-facing only, never parsed
};

// chunkId -> topology for every chunk already accepted in EARLIER batches of
// the same generation, when a model is delivered progressively across several
// writes of the output window (model_core::ControlOpcode::ChunkBatchReady).
//
// It exists so a later batch may reference an earlier one's chunk -- a mesh
// pointing at a material whose textures already crossed -- instead of every
// batch having to re-send the chunks it depends on. Self-contained batches
// were the alternative and are cheap for materials but not for images: at
// 03-file-formats-and-ingestion.md:140's 16 MiB chunk cap, one shared texture
// re-sent per batch is 16 MiB and one more GPU copy every time.
//
// This widens what a dependency id may resolve to; it does not weaken any
// check. Every entry here is a chunk that already passed this same validator
// in full and was copied into host-owned memory, and ids stay unique across
// the whole generation (a chunk whose id is already in the catalog is
// rejected, exactly as a duplicate inside one section is). Its size is
// bounded by the caller's per-generation chunk cap, not by this file.
using KnownChunkCatalog = std::unordered_map<uint32_t, model_core::ChunkTopology>;

// sectionView must be exactly the caller's actual MapViewOfFile size (never
// the section's own self-declared length).
//
// priorBatches is null for a single-window import, which makes this function
// behave exactly as it did before progressive delivery existed.
ValidationResult ValidateAndCopySection(std::span<const std::byte> sectionView,
                                         uint64_t expectedGenerationId, uint32_t maxChunkCount,
                                         const KnownChunkCatalog* priorBatches = nullptr);

} // namespace import_broker
