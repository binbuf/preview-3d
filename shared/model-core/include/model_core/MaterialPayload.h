#pragma once

// Fixed-size payload for ChunkTopology::Material chunks (see WireFormat.h).
// Carries the full normalized-Material contract from
// .docs/design/03-file-formats-and-ingestion.md (base color, metallic,
// roughness, emissive, uv transform, alpha, double-sided/unlit) so a later
// slice adding more texture maps doesn't need to widen this struct -- but
// this chunk's adapter only ever populates dependencyIds[0] (base color) on
// the ChunkDescriptor that carries this payload; see GltfAdapter.cpp.

#include <cstdint>

namespace model_core {

enum class AlphaModeId : uint32_t {
    Opaque = 0,
    Mask = 1,
    Blend = 2,
};

constexpr uint32_t kMaterialFlagDoubleSided = 1u << 0;
constexpr uint32_t kMaterialFlagUnlit = 1u << 1;
// Bits 2-31 reserved, must be 0 -- SharedSectionValidator rejects any set.
constexpr uint32_t kMaterialFlagsKnownMask = kMaterialFlagDoubleSided | kMaterialFlagUnlit;

#pragma pack(push, 1)

struct MaterialPayload {
    float baseColorFactor[4]; // RGBA, linear, default {1,1,1,1}
    float metallicFactor;     // default 1
    float roughnessFactor;    // default 1
    float emissiveFactor[3];  // default {0,0,0}
    float uvOffset[2];        // KHR_texture_transform on baseColorTexture only; default {0,0}
    float uvScale[2];         // default {1,1}
    float uvRotation;         // radians, default 0
    uint32_t alphaMode;       // AlphaModeId
    float alphaCutoff;        // default 0.5
    uint32_t flags;           // kMaterialFlag*
    uint32_t reserved0;       // must be 0
};
static_assert(sizeof(MaterialPayload) == 72, "MaterialPayload wire layout changed");

#pragma pack(pop)

} // namespace model_core
