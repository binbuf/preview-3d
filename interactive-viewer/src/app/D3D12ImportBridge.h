#pragma once

// App-facing (trusted-process) bridge from a real on-disk file to the
// sandboxed Preview3DImportWorker.exe pipeline. All application imports use
// this bridge and reach the exclusive D3D12 rendering path.
//
// The launch/converse/validate sequence itself now lives in
// import_broker::RunImportSession (shared/import-broker/ImportSession.h), so
// it is reachable from tests/import-isolation/; this file keeps only what is
// genuinely app-layer: extension -> format classification, mapping a typed
// session failure to user-facing text, and unpacking validated chunks into
// the renderer-facing structs below.
//
#include "model_core/MaterialPayload.h"
#include "model_core/PixelFormats.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"
#include "import_broker/ImportSession.h"

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
    Obj,
    Fbx,
    ThreeMf,
    Usd,
    // Dedicated STEP/STP host route. Intentionally NOT returned by
    // ClassifyByExtension until STEP-006 enables product discovery, so no
    // public picker, drag/drop, or activation path recognizes STEP yet.
    Step,
};

// nullopt for any extension this slice doesn't recognize.
std::optional<SourceFormat> ClassifyByExtension(const std::wstring& path);

// One validated chunk, ready for the upload coordinator. payload is
// vertex bytes immediately followed by index bytes (index bytes only when
// topology == TriangleList), matching GltfAdapter.cpp/StlAdapter.cpp/
// PlyAdapter.cpp's wire packing -- no further chunk-table bookkeeping is
// needed at this layer.
struct ImportedMesh {
    model_core::ChunkTopology topology = model_core::ChunkTopology::Unknown;
    model_core::VertexLayoutId vertexLayoutId = model_core::VertexLayoutId::Unknown;
    uint32_t chunkId = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    std::vector<std::byte> payload;
    uint32_t materialChunkId = 0; // 0 = none; generation-wide material identity
    model_core::ChunkDescriptor geometry{};
};

// Material/image ids refer to the generation-wide catalog, including bounded
// forward references. The broker rejects unresolved or incorrectly typed
// terminal catalogs; the renderer uses neutral fallback until dependencies arrive.
struct ImportedMaterial {
    uint32_t chunkId = 0;
    model_core::MaterialPayload data{};
    // All four slots are populated and consumed: GltfAdapter.cpp resolves
    // metallicRoughness/normal/emissive alongside base color. 0 = none.
    uint32_t baseColorImageChunkId = 0;
    uint32_t metallicRoughnessImageChunkId = 0;
    uint32_t normalImageChunkId = 0;
    uint32_t emissiveImageChunkId = 0;
};

struct ImportedImage {
    uint32_t chunkId = 0;
    uint32_t logicalChunkId = 0;
    model_core::PixelFormatId pixelFormat = model_core::PixelFormatId::Unknown;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 0;
    model_core::ColorSpaceId colorSpace = model_core::ColorSpaceId::Linear;
    std::vector<std::byte> pixelBytes; // mip 0..N-1 tightly packed per PixelFormats.h's layout
};

struct ImportedNode {
    model_core::NodePayload data{};
};

struct ImportedInstance {
    model_core::MeshInstancePayload data{};
    double worldTransform[16]{};
    bool resolvedVisible = false;
    bool mirrored = false;
};

struct ImportResult {
    bool detail = false;
    bool coarseComplete = false;
    bool ok = false;
    model_core::SceneMetadata scene{};
    model_core::FileIdentity sourceIdentity{};
    uint32_t textureWarningCount = 0;
    model_core::ImportStatusPayload status{};
    bool forceUploadFailureForTesting = false;
    model_core::ImportErrorCode errorCode = model_core::ImportErrorCode::None;
    import_broker::ImportStage errorStage = import_broker::ImportStage::Completed;
    model_core::ImportFailurePhase errorPhase = model_core::ImportFailurePhase::Unspecified;
    std::vector<ImportedMesh> meshes;
    std::vector<ImportedMaterial> materials;
    std::vector<ImportedImage> images;
    std::vector<ImportedNode> nodes;
    std::vector<ImportedInstance> instances;
    std::wstring errorSummary;
    std::wstring errorDetails;
};

std::wstring SourceFormatLabel(const std::wstring& path);
std::wstring StageLabel(import_broker::ImportStage stage);
std::wstring FailurePhaseLabel(const ImportResult& result);
std::wstring DiagnosticDetails(const std::wstring& path, const ImportResult& result);
void DescribeSessionFailure(const import_broker::ImportSessionResult& session,
    std::wstring& summary, std::wstring& details);

// With onBatch, transfers each host-owned batch without retaining payloads;
// the terminal result contains only status. Without it, returns accumulated data.
// Synchronous -- call from one of BeginOpen's owned background threads.
//
// isCancelled is polled while waiting on the worker; returning true signals
// cooperative cancellation and falls back to bounded worker replacement.
ImportResult RunImport(SourceFormat format, const std::wstring& path, uint64_t generationId,
                        std::function<bool()> isCancelled = {},
                        std::function<void(ImportResult)> onBatch = {},
                        uint64_t sectionBytes = 64ull * 1024 * 1024,
                        bool delayBatchesForTesting = false, uint32_t faultForTesting = 0,
                        std::function<uint32_t()> nextDetail = {},
                        std::function<void(const model_core::FileIdentity&)> onInitialComplete = {},
                        std::function<bool(uint64_t)> cpuBudgetAllows = {});

// Creates the import worker's AppContainer profile and grants it
// read+execute on the worker's own directory -- without this the sandboxed
// worker cannot load its own .exe. Idempotent and cheap after the first
// call. RunImport does this itself; calling it at startup only keeps the
// one-time cost off the first open. A real installed build provisions the
// profile and its ACL at install time instead (Gate 7).
void EnsureImportSandboxPrepared();

} // namespace d3d12_import_bridge
