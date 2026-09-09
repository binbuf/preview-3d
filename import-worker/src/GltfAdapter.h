#pragma once

// Real glTF/GLB parsing, using fastgltf, translated into the model_core wire
// format. Core geometry: POSITION/NORMAL/TEXCOORD_0, triangle-list
// primitives, node-transform baking -- either from ordinary accessors or,
// when KHR_draco_mesh_compression is present on a primitive, via a real
// bounded draco::Decoder call (DracoDecodeAdapter.h). Materials: flat
// PBR factors always; a KHR_texture_basisu base-color texture is
// transcoded (TextureTranscodeAdapter.h) into an Image chunk the material
// depends on -- a plain PNG/JPEG/WebP base-color texture is skipped
// (material keeps its factors, no image dependency), not a failure. No
// skins or animation. See .docs/design/04-rendering-and-streaming.md
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
