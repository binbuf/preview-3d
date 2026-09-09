#pragma once

#include "model_core/ImportError.h"

#include <cstdint>
#include <span>
#include <variant>

namespace import_worker {

struct PlyImportResult {
    uint32_t chunkCount = 0;
    uint64_t sectionBytesWritten = 0;
};

// Parses sourcePlyBytes as binary-little-endian or binary-big-endian PLY
// (ASCII PLY -- format "ascii" -- is Tier B, a later slice, and is rejected
// as MalformedData here). Emits exactly one chunk: TriangleList/
// PositionNormalUv0_F32 when the file declares a non-empty "face" element,
// or PointList/PositionOnly_F32 when it declares only a "vertex" element
// (or a "face" element with a declared count of 0). Mirrors StlAdapter.h's
// "compute everything, check total size once, write sequentially, header
// last" contract.
std::variant<PlyImportResult, model_core::ImportErrorCode> ImportPly(
    std::span<const std::byte> sourcePlyBytes, std::span<std::byte> destination,
    uint64_t generationId, uint32_t maxChunkCount);

} // namespace import_worker
