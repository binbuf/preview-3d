#pragma once

#include "TextureDecodePolicy.h"
#include "model_core/ImportError.h"

#include <cstdint>
#include <span>
#include <variant>

namespace import_worker {

class ChunkBatchSink;
class SidecarFileClient;

struct ObjImportResult {
    uint32_t chunkCount = 0;
    uint64_t sectionBytesWritten = 0;
};

// Tier-B Wavefront OBJ/MTL import. ufbx materializes the bounded source in
// the sandbox; normalized mesh/material/image chunks are then published in
// bounded batches through ChunkBatchSink.
std::variant<ObjImportResult, model_core::ImportErrorCode> ImportObj(
    std::span<const std::byte> sourceBytes, std::span<std::byte> destination,
    uint64_t generationId, uint32_t maxChunkCount, SidecarFileClient& sidecars,
    ChunkBatchSink* batchSink, const TextureDecodeOptions& textureOptions = {});

} // namespace import_worker
