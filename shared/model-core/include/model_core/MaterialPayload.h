#pragma once

// Fixed-size payload for ChunkTopology::Material chunks (see WireFormat.h).
// Carries the full normalized-Material contract from
// .docs/design/03-file-formats-and-ingestion.md (base color, metallic,
// roughness, emissive, uv transform, alpha, double-sided/unlit). The carrying
// ChunkDescriptor's four dependency slots identify the base-color,
// metallic/roughness, normal, and emissive images respectively.

#include <cstdint>

namespace model_core {

enum class AlphaModeId : uint32_t {
    Opaque = 0,
    Mask = 1,
    Blend = 2,
};

constexpr uint32_t kMaterialFlagDoubleSided = 1u << 0;
constexpr uint32_t kMaterialFlagUnlit = 1u << 1;
// USD defines (0,0) at an image's lower-left, while Direct3D samples (0,0)
// at the upper-left. Flip after the authored UV transform for USD materials.
constexpr uint32_t kMaterialFlagFlipV = 1u << 2;
// 3MF texture2d carries independent U/V addressing plus nearest/linear intent.
// Zero remains the existing wrap/linear default, so older producers retain
// their exact behavior. Texture-layer and mix distinguish the cases where
// texture alpha participates in a 3MF multiproperties blend. Vertex-sRGB asks
// the shader to interpolate an authored color in sRGB before linearization.
constexpr uint32_t kMaterialAddressUShift = 3;
constexpr uint32_t kMaterialAddressVShift = 5;
constexpr uint32_t kMaterialAddressMask = 0x3u;
enum class TextureAddressId : uint32_t { Wrap = 0, Mirror = 1, Clamp = 2, None = 3 };
constexpr uint32_t kMaterialFlagNearest = 1u << 7;
constexpr uint32_t kMaterialFlagTextureLayer = 1u << 8;
constexpr uint32_t kMaterialFlagTextureMix = 1u << 9;
constexpr uint32_t kMaterialFlagVertexSrgb = 1u << 10;
// KHR_materials_transmission is rendered without a refraction pass: the shader
// suppresses transmitted diffuse, adds a view-dependent Fresnel term to output
// alpha, and lets the environment reflection pass through unchanged. The
// accompanying transmissionFactor is meaningful only when this flag is set.
constexpr uint32_t kMaterialFlagTransmissive = 1u << 11;
constexpr uint32_t MaterialAddressFlags(TextureAddressId u, TextureAddressId v) noexcept
{
    return (uint32_t(u) << kMaterialAddressUShift)
        | (uint32_t(v) << kMaterialAddressVShift);
}
constexpr uint32_t kMaterialSamplerFlags =
    (kMaterialAddressMask << kMaterialAddressUShift)
    | (kMaterialAddressMask << kMaterialAddressVShift) | kMaterialFlagNearest;
// Bits 12-31 reserved, must be 0 -- SharedSectionValidator rejects any set.
constexpr uint32_t kMaterialFlagsKnownMask =
    kMaterialFlagDoubleSided | kMaterialFlagUnlit | kMaterialFlagFlipV
    | kMaterialSamplerFlags | kMaterialFlagTextureLayer
    | kMaterialFlagTextureMix | kMaterialFlagVertexSrgb
    | kMaterialFlagTransmissive;

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
    // KHR_materials_transmission factor [0,1]; 0 and ignored unless
    // kMaterialFlagTransmissive is set. This reuses the former reserved word to
    // avoid growing the 72-byte wire record.
    float transmissionFactor;
};
static_assert(sizeof(MaterialPayload) == 72, "MaterialPayload wire layout changed");

#pragma pack(pop)

} // namespace model_core
