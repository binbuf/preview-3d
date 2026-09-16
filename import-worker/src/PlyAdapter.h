#pragma once

#include "model_core/ImportError.h"
#include "model_core/MappedFile.h"

#include <cstdint>
#include <span>
#include <variant>

namespace import_worker {
class ChunkBatchSink;

struct PlyImportResult {
    uint32_t chunkCount = 0;
    uint64_t sectionBytesWritten = 0;
};

// Binary little/big-endian meshes and point clouds use bounded cluster buffers.
// mappedSource supplies the complete file while sourcePlyBytes holds only the
// bounded header. Indexed faces resolve vertices through two mapped windows;
// variable vertex records use a capped checkpoint catalog. Vertex-before-face
// layouts are supported; face-before-vertex binary layouts fail UnsupportedEncoding.
// ASCII is available only to explicitly opted-in developer callers.
// The final section stays in destination; intermediate sections use batchSink.
std::variant<PlyImportResult, model_core::ImportErrorCode> ImportPly(
    std::span<const std::byte> sourcePlyBytes, std::span<std::byte> destination, uint64_t generationId,
    uint32_t maxChunkCount, bool allowAscii = true, ChunkBatchSink* batchSink = nullptr,
    model_core::MappedFile* mappedSource = nullptr);

} // namespace import_worker
