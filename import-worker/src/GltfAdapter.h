#pragma once

// Real glTF/GLB parsing, using fastgltf, translated into the existing
// (unmodified) model_core wire format. Scope: core untextured geometry only
// -- POSITION/NORMAL/TEXCOORD_0, triangle-list primitives, node-transform
// baking. No materials, textures, Draco/meshopt/quantization extensions,
// skins, or animation. See .docs/design/04-rendering-and-streaming.md
// (vertex-layout set) and .docs/PROGRESS.md for the fastgltf-API findings
// this implementation relies on.

#include "model_core/ImportError.h"

#include <cstdint>
#include <span>
#include <variant>

namespace import_worker {

struct GltfImportResult {
    uint32_t chunkCount = 0;
    uint64_t sectionBytesWritten = 0;
};

// Parses sourceGlbBytes and writes header+descriptors+payload into
// destination, per the model_core wire format -- same "compute everything,
// check total size once, then write sequentially with the header written
// last" structure as SyntheticSceneGenerator::GenerateSyntheticScene.
// Returns an ImportErrorCode instead of a GltfImportResult on any parse
// failure, resource-limit violation, or malformed/unsupported content.
std::variant<GltfImportResult, model_core::ImportErrorCode> ImportGltf(
    std::span<const std::byte> sourceGlbBytes, std::span<std::byte> destination,
    uint64_t generationId, uint32_t maxChunkCount);

} // namespace import_worker
