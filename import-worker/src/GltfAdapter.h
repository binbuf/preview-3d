#pragma once

// Real glTF/GLB/.gltf parsing, using fastgltf, translated into the
// model_core wire format. Core geometry: POSITION/NORMAL/TEXCOORD_0,
// triangle-list primitives, node-transform baking -- either from ordinary
// accessors or, when KHR_draco_mesh_compression is present on a primitive,
// via a real bounded draco::Decoder call (DracoDecodeAdapter.h). Materials:
// flat PBR factors always; all four PBR texture slots (baseColor/
// metallicRoughness/normal/emissive) are resolved through
// KHR_texture_basisu (TextureTranscodeAdapter.h) or a plain PNG/JPEG/BMP/
// TIFF raster (WicImageDecodeAdapter.h) -- GLB-embedded or, when
// sidecarClient is non-null, an external sidecar file (SidecarFileClient.h)
// -- into an Image chunk the material depends on; a texture this chunk
// can't decode is simply skipped (material keeps its factors, no image
// dependency), not a failure. No skins or animation. See
// .docs/design/04-rendering-and-streaming.md (vertex-layout set) and
// .docs/PROGRESS.md for the fastgltf-API findings this implementation
// relies on.

#include "model_core/ImportError.h"

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

// Parses sourceGlbBytes (a .glb container OR a plain-JSON .gltf -- auto-
// detected) and writes header+descriptors+payload into destination, per
// the model_core wire format -- same "compute everything, check total size
// once, then write sequentially with the header written last" structure as
// SyntheticSceneGenerator::GenerateSyntheticScene. sidecarClient is nullptr
// for the always-self-contained shared-section ParseGltfRequest path (a
// .glb never needs sidecars); non-null for the real-file
// ParseGltfFileRequest path, letting a plain .gltf resolve its external
// .bin/.png/.jpg/.jpeg/.webp/.ktx2 siblings. Returns an ImportErrorCode
// instead of a GltfImportResult on any parse failure, resource-limit
// violation, or malformed/unsupported content.
//
// batchSink is nullptr for a caller that can only take one section: the whole
// model must then fit the window or the result is ResourceLimit, which is
// what this function always did. Non-null allows progressive delivery -- the
// model is split across as many windows as it needs, each handed over through
// the sink -- and only then can a model larger than the window be imported at
// all.
//
// Either way the FINAL batch is left sitting in `destination` and described
// by the returned GltfImportResult; the caller sends the terminal ChunksReady
// for it. So the single-window case emits exactly the traffic it always did.
std::variant<GltfImportResult, model_core::ImportErrorCode> ImportGltf(
    std::span<const std::byte> sourceGlbBytes, std::span<std::byte> destination,
    uint64_t generationId, uint32_t maxChunkCount, SidecarFileClient* sidecarClient = nullptr,
    ChunkBatchSink* batchSink = nullptr);

} // namespace import_worker
