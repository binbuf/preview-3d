#pragma once

// Host-side (trusted-process) bridge from a real on-disk file to the
// sandboxed Preview3DImportWorker.exe pipeline, for the opt-in --d3d12 path
// only -- the default D3D11 path keeps using Model.cpp's in-process parser
// unchanged. RunImport() reproduces, generalized over format, the exact
// real-file sequence tests/import-isolation/{Gltf,Stl,Ply}ImportTests.cpp
// already prove end-to-end: open+canonicalize, duplicate an inheritable
// handle, create an output shared section, launch one fresh one-shot
// sandboxed worker, send the file-based ControlOpcode, and validate+copy the
// resulting chunks. No WorkerPool (no way yet to duplicate a *new* source
// file handle into an already-running pooled worker) and no persistent
// AppContainer profile -- both explicitly deferred to a later, streaming-
// focused slice.

#include "model_core/MaterialPayload.h"
#include "model_core/PixelFormats.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"

#include <cstdint>
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
    uint32_t baseColorImageChunkId = 0;        // 0 = none
    uint32_t metallicRoughnessImageChunkId = 0; // always 0 this slice, carried for forward-compat
    uint32_t normalImageChunkId = 0;            // always 0 this slice
    uint32_t emissiveImageChunkId = 0;          // always 0 this slice
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
ImportResult RunImport(SourceFormat format, const std::wstring& path, uint64_t generationId);

// Grants the AppContainer capability SIDs read+execute on
// Preview3DImportWorker.exe's directory -- without this, the sandboxed
// worker fails to even load its own .exe. Idempotent (runs at most once per
// process); safe to call before every import. This is a dev-loop
// placeholder (a runtime icacls shell-out): a real installed/MSIX build
// needs this provisioned at install time instead, not at first --d3d12
// launch -- revisit before shipping.
void EnsureAppContainerDirectoryAccessGranted();

} // namespace d3d12_import_bridge
