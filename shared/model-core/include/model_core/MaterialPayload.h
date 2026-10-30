#pragma once

// Fixed-size payload for ChunkTopology::Material chunks (see WireFormat.h).
// Carries the full normalized-Material contract from
// .docs/design/03-file-formats-and-ingestion.md (base color, metallic,
// roughness, emissive, uv transform, alpha, double-sided/unlit) plus the
// protocol-v12 PBR lobes (emissive strength, clearcoat, sheen, specular/IOR and
// occlusion strength). The carrying ChunkDescriptor's four dependency slots
// identify the base-color, metallic/roughness, normal, and emissive images
// respectively.
//
// Protocol v12 note: this record grew from 72 to 140 bytes. It is a wire
// change, not an in-place reinterpretation of the former reserved words, so
// every producer/consumer and cache identity moves together with
// kCurrentProtocolVersion.

#include <algorithm>
#include <cmath>
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

constexpr uint32_t kMaterialReservedWordCount = 4;
inline constexpr float kDefaultMaterialIor = 1.5f;
inline constexpr float kDefaultDielectricF0 = 0.04f;

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
    // kMaterialFlagTransmissive is set.
    float transmissionFactor;
    // KHR_materials_emissive_strength multiplier, applied to emissiveFactor
    // before tone mapping. Default 1 (no authored change).
    float emissiveStrength = 1.0f;
    // KHR_materials_clearcoat second GGX lobe. Default 0 = absent.
    float clearcoatFactor = 0.0f;
    float clearcoatRoughness = 0.0f;
    // KHR_materials_sheen velvet lobe, linear color and roughness. Default
    // black = absent.
    float sheenColor[3] = {0.0f, 0.0f, 0.0f};
    float sheenRoughness = 0.0f;
    // KHR_materials_specular scales/tints the dielectric F0. IOR-driven F0 is
    // ((ior - 1) / (ior + 1))^2; the shader combines it with these fields.
    float specularFactor = 1.0f;
    float specularColor[3] = {1.0f, 1.0f, 1.0f};
    // KHR_materials_ior. Default 1.5 -> F0 = 0.04.
    float ior = kDefaultMaterialIor;
    // glTF occlusionStrength [0,1]; applied with the ORM R channel. Default 1
    // preserves the pre-v12 full-strength behavior.
    float occlusionStrength = 1.0f;
    // Reserved for future anisotropy/iridescence/volume/dispersion lobes.
    // Must be exactly zero in protocol v12; the validator rejects any other
    // value so a later additive meaning cannot be smuggled through.
    uint32_t reserved[kMaterialReservedWordCount] = {0, 0, 0, 0};
};
static_assert(sizeof(MaterialPayload) == 140, "MaterialPayload wire layout changed");

#pragma pack(pop)

// IOR-derived dielectric F0. Clamped to the physically meaningful [0,1] range;
// ior <= 1 means no optical interface (F0 = 0).
inline float IorToF0(float ior) noexcept
{
    if (!std::isfinite(ior) || ior <= 1.0f) return 0.0f;
    const float ratio = (ior - 1.0f) / (ior + 1.0f);
    return std::clamp(ratio * ratio, 0.0f, 1.0f);
}

// The normalized dielectric specular color used by both the CPU reference and
// the shader. KHR_materials_specular multiplies the IOR/0.04 base F0 by the
// scalar factor and the authored color.
inline void MaterialDielectricF0(const MaterialPayload& material, float out[3]) noexcept
{
    const float base = std::clamp(IorToF0(material.ior) * material.specularFactor, 0.0f, 1.0f);
    for (int channel = 0; channel < 3; ++channel)
        out[channel] = std::clamp(base * std::clamp(material.specularColor[channel], 0.0f, 1.0f),
                                  0.0f, 1.0f);
}

// Soft velvet sheen weight at a view/normal angle. It is deliberately a
// grazing-only lobe so the sheen does not turn into an isotropic emissive tint.
inline float SheenLobeWeight(const MaterialPayload& material, float nv) noexcept
{
    if (material.sheenColor[0] <= 0.0f && material.sheenColor[1] <= 0.0f
        && material.sheenColor[2] <= 0.0f)
        return 0.0f;
    const float grazing = std::pow(std::clamp(1.0f - nv, 0.0f, 1.0f), 3.0f);
    const float tightness = 0.35f + 0.65f * std::clamp(material.sheenRoughness, 0.0f, 1.0f);
    return grazing * tightness;
}

// Clearcoat and sheen are layered over the base rather than added on top of an
// unchanged base specular response. Base specular is scaled by
// (1 - clearcoatFactor) so the coat does not double-count the same energy.
inline float ClearcoatBaseSpecularScale(const MaterialPayload& material) noexcept
{
    return 1.0f - std::clamp(material.clearcoatFactor, 0.0f, 1.0f);
}

// Single validation entry point shared by the hostile/worker wire validator and
// the [material-lobes] unit tests. Returns nullptr when every v12 lobe field is
// finite and in range, otherwise a stable diagnostic string.
inline const char* ValidateMaterialLobeFields(const MaterialPayload& material) noexcept
{
    auto finite = [](float value) { return std::isfinite(value); };
    if (!finite(material.emissiveStrength) || material.emissiveStrength < 0.0f
        || material.emissiveStrength > 1024.0f)
        return "material emissiveStrength out of range";
    if (!finite(material.clearcoatFactor) || material.clearcoatFactor < 0.0f
        || material.clearcoatFactor > 1.0f)
        return "material clearcoatFactor out of range";
    if (!finite(material.clearcoatRoughness) || material.clearcoatRoughness < 0.0f
        || material.clearcoatRoughness > 1.0f)
        return "material clearcoatRoughness out of range";
    for (float value : material.sheenColor)
        if (!finite(value) || value < 0.0f || value > 1.0f)
            return "material sheenColor out of range";
    if (!finite(material.sheenRoughness) || material.sheenRoughness < 0.0f
        || material.sheenRoughness > 1.0f)
        return "material sheenRoughness out of range";
    if (!finite(material.specularFactor) || material.specularFactor < 0.0f
        || material.specularFactor > 1.0f)
        return "material specularFactor out of range";
    for (float value : material.specularColor)
        if (!finite(value) || value < 0.0f || value > 1.0f)
            return "material specularColor out of range";
    if (!finite(material.ior) || material.ior < 1.0f || material.ior > 4.0f)
        return "material ior out of range";
    if (!finite(material.occlusionStrength) || material.occlusionStrength < 0.0f
        || material.occlusionStrength > 1.0f)
        return "material occlusionStrength out of range";
    for (uint32_t word : material.reserved)
        if (word != 0) return "material reserved word must be zero";
    return nullptr;
}

} // namespace model_core
