#pragma once

// Real glTF/GLB/.gltf parsing, using fastgltf, translated into the
// model_core wire format. Core geometry: POSITION/NORMAL/TEXCOORD_0,
// triangle-list primitives, node-transform baking -- either from ordinary
// accessors or, when KHR_draco_mesh_compression is present on a primitive,
// via a real bounded draco::Decoder call (DracoDecodeAdapter.h). Materials:
// flat PBR factors always; all four PBR texture slots (baseColor/
// metallicRoughness/normal/emissive) are resolved through
// KHR_texture_basisu (TextureTranscodeAdapter.h) or a plain PNG/JPEG
// raster (WicImageDecodeAdapter.h) -- GLB-embedded or, when
// sidecarClient is non-null, an external sidecar file (SidecarFileClient.h)
// -- into an Image chunk the material depends on; a texture this chunk
// cannot decode uses deterministic checker/neutral fallbacks and a bounded
// warning, preserving valid geometry. No skins or animation. See
// .docs/design/04-rendering-and-streaming.md (vertex-layout set) and
// .docs/PROGRESS.md for the fastgltf-API findings this implementation
// relies on.

#include "model_core/ImportError.h"
#include "TextureDecodePolicy.h"

#include <cstdint>
#include <span>
#include <variant>

namespace import_worker {

class ChunkBatchSink;
class SidecarFileClient;

struct GltfImportResult {
    uint32_t chunkCount = 0;
    uint64_t sectionBytesWritten = 0;
};

// Parses a read-only mapped GLB or bounded JSON glTF. GLB BIN and brokered
// geometry sidecars remain mapped; only padded metadata and bounded image
// encodings are copied. With a batch sink, ordinary primitives are normalized,
// remapped, emitted and freed one cluster at a time. Draco remains an
// independently capped decode unit and is split after decode. The bounded
// single-section developer path fails if its normalized scratch/window fills.
//
// The final section stays in destination for the caller's ChunksReady notice.
// Progressive sections wait for host ownership/capacity before window reuse.
std::variant<GltfImportResult, model_core::ImportErrorCode> ImportGltf(
    std::span<const std::byte> sourceGlbBytes, std::span<std::byte> destination,
    uint64_t generationId, uint32_t maxChunkCount, SidecarFileClient* sidecarClient = nullptr,
    ChunkBatchSink* batchSink = nullptr, const TextureDecodeOptions& textureOptions = {});

} // namespace import_worker
