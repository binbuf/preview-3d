#pragma once

// T10 bounded Assimp compatibility adapter.
//
// This is a compatibility importer for formats with no native adapter
// (.dae/.3ds/.dxf/.x/.off/.amf/.x3d). It is worker-only: Assimp types never
// cross this header. Every external open goes through the brokers' sidecar
// protocol via AssimpImportOptions::requestSidecar; there is deliberately no
// path, default IOSystem, plug-in load or filesystem fallback here.
//
// The adapter materializes the Assimp scene inside the sandbox, validates it
// against the Tier-B object/material/triangle/vertex ceilings, and normalizes
// it into the same ChunkTopology/VertexLayout contract the native adapters
// emit.

#include "TextureDecodePolicy.h"
#include "model_core/ControlProtocol.h"
#include "model_core/ImportError.h"
#include "model_core/TierALimits.h"

#include <cstdint>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace import_worker {

class ChunkBatchSink;

struct AssimpSidecarResult {
    std::optional<std::vector<std::byte>> bytes;
    model_core::ImportErrorCode error = model_core::ImportErrorCode::None;
};

// Requests one broker-approved sidecar by its authored relative reference.
struct AssimpImportOptions {
    std::function<AssimpSidecarResult(std::string_view relativeReferenceUtf8, uint64_t maxBytes)>
        requestSidecar;
    TextureDecodeOptions textureOptions{};
    model_core::AssimpFormat format = model_core::AssimpFormat::Dae;
    // Tier-B ceilings. The qualification seam lowers these so a small fixture
    // can prove typed rejection without a multi-hundred-megabyte asset.
    uint32_t maxObjects = model_core::kTierBObjectLimit;
    uint32_t maxMaterials = model_core::kTierBMaterialLimit;
    uint64_t maxTriangles = model_core::kTierBTriangleLimit;
    uint64_t maxVertices = model_core::kTierBVertexLimit;
    uint32_t maxSidecarRequests = 256;
    bool Cancelled() const { return textureOptions.isCancelled && textureOptions.isCancelled(); }
};

struct AssimpImportResult {
    uint32_t chunkCount = 0;
    uint64_t sectionBytesWritten = 0;
};

using AssimpImportOutcome = std::variant<AssimpImportResult, model_core::ImportErrorCode>;

AssimpImportOutcome ImportAssimp(std::span<const std::byte> sourceBytes,
                                 std::span<std::byte> destination,
                                 uint64_t generationId, uint32_t maxChunkCount,
                                 ChunkBatchSink* batchSink,
                                 const AssimpImportOptions& options = {});

// Kept public for the worker's request validation and the unit tests.
bool IsSafeAssimpSidecarReference(std::string_view reference) noexcept;

} // namespace import_worker
