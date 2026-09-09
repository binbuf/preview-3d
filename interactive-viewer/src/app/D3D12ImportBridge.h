#pragma once

// App-facing (trusted-process) bridge from a real on-disk file to the
// sandboxed Preview3DImportWorker.exe pipeline, for the opt-in --d3d12 path
// only -- the default D3D11 path keeps using Model.cpp's in-process parser
// unchanged.
//
// The launch/converse/validate sequence itself now lives in
// import_broker::RunImportSession (shared/import-broker/ImportSession.h), so
// it is reachable from tests/import-isolation/; this file keeps only what is
// genuinely app-layer: extension -> format classification, mapping a typed
// session failure to user-facing text, and unpacking validated chunks into
// the renderer-facing structs below.
//
// Still deferred to a later, streaming-focused slice: WorkerPool-based reuse
// (there is no way yet to duplicate a *new* source file handle into an
// already-running pooled worker) and a persistent AppContainer profile.

#include "model_core/MaterialPayload.h"
#include "model_core/PixelFormats.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace d3d12_import_bridge {

enum class SourceFormat {
    Glb,
    Stl,
    Ply,
};

// nullopt for any extension this slice doesn't recognize.
std::optional<SourceFormat> ClassifyByExtension(const std::wstring& path);

// One validated chunk, ready for D3D12ViewerPath::UploadModel. payload is
// vertex bytes immediately followed by index bytes (index bytes only when
// topology == TriangleList), matching GltfAdapter.cpp/StlAdapter.cpp/
// PlyAdapter.cpp's wire packing -- no further chunk-table bookkeeping is
// needed at this layer.
struct ImportedMesh {
    model_core::ChunkTopology topology = model_core::ChunkTopology::Unknown;
    model_core::VertexLayoutId vertexLayoutId = model_core::VertexLayoutId::Unknown;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    std::vector<std::byte> payload;
    uint32_t materialChunkId = 0; // 0 = none; resolves via ImportResult::materials' chunkId
};

// Ids here are already validator-confirmed (SharedSectionValidator) to
// cross-reference correctly within one ImportResult -- this struct is
// purely mechanical unpacking of a Material-topology chunk's payload, no
// re-validation needed.
struct ImportedMaterial {
    uint32_t chunkId = 0;
    model_core::MaterialPayload data{};
    // All four slots are populated: GltfAdapter.cpp resolves
    // metallicRoughness/normal/emissive alongside base color. 0 = none.
    // (The renderer still samples only base color -- that gap is in the
    // shaders, not here.)
    uint32_t baseColorImageChunkId = 0;
    uint32_t metallicRoughnessImageChunkId = 0;
    uint32_t normalImageChunkId = 0;
    uint32_t emissiveImageChunkId = 0;
};

struct ImportedImage {
    uint32_t chunkId = 0;
    model_core::PixelFormatId pixelFormat = model_core::PixelFormatId::Unknown;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 0;
    model_core::ColorSpaceId colorSpace = model_core::ColorSpaceId::Linear;
    std::vector<std::byte> pixelBytes; // mip 0..N-1 tightly packed per PixelFormats.h's layout
};

struct ImportResult {
    bool ok = false;
    std::vector<ImportedMesh> meshes;
    std::vector<ImportedMaterial> materials;
    std::vector<ImportedImage> images;
    std::wstring errorSummary;
    std::wstring errorDetails;
};

// Synchronous -- call from a detached background thread, mirroring
// BeginOpen's existing std::thread(...).detach() pattern for LoadGlb.
//
// isCancelled is polled while waiting on the worker; returning true abandons
// the import (the worker is killed by its Job Object) and yields a result
// whose text the caller is expected to drop rather than display.
ImportResult RunImport(SourceFormat format, const std::wstring& path, uint64_t generationId,
                        std::function<bool()> isCancelled = {});

// Creates the import worker's AppContainer profile and grants it
// read+execute on the worker's own directory -- without this the sandboxed
// worker cannot load its own .exe. Idempotent and cheap after the first
// call. RunImport does this itself; calling it at startup only keeps the
// one-time cost off the first open. A real installed build provisions the
// profile and its ACL at install time instead (Gate 7).
void EnsureImportSandboxPrepared();

} // namespace d3d12_import_bridge
