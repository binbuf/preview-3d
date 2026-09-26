#include "UfbxMaterialConversion.h"

#include "ufbx.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

namespace import_worker {
namespace {

bool Finite(double value)
{
    return std::isfinite(value) && std::abs(value) <= 1.0e30;
}

// The FBX material model has no dedicated glass/transmission channel: these
// exports author window glass as an opaque white Phong surface with a bright
// specular and zero reflection, which is otherwise indistinguishable from
// opaque white plastic. When the material name identifies it as glass, carry
// it through as transmissive so the shader's Fresnel glass path (see
// kMaterialFlagTransmissive) can make the window see-through. Names are
// already used elsewhere in this adapter for the common base-color recovery,
// and the match is deliberately limited to unambiguous glass tokens across
// the exporters' common languages.
bool LooksLikeGlass(std::string_view name)
{
    static constexpr std::string_view kTokens[] = {
        "glass", "cristal", "crystal", "vidrio", "verre", "vetro", "cristallo", "glas"
    };
    std::string lower;
    lower.reserve(name.size());
    for (const char c : name) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    for (const std::string_view token : kTokens) {
        if (lower.find(token) != std::string::npos) return true;
    }
    return false;
}

float Saturate(double value, float fallback = 1.0f)
{
    return Finite(value) ? static_cast<float>(std::clamp(value, 0.0, 1.0)) : fallback;
}

float SafeFloat(double value, float fallback = 0.0f)
{
    return Finite(value) && value >= -(std::numeric_limits<float>::max)()
        && value <= (std::numeric_limits<float>::max)()
        ? static_cast<float>(value) : fallback;
}

void Warn(uint32_t& warnings)
{
    warnings = (std::min)(64u, warnings + 1);
}

} // namespace

model_core::MaterialPayload ConvertUfbxMaterial(const ufbx_material& material,
                                                 bool fbxPolicy,
                                                 uint32_t& optionalWarnings)
{
    using namespace model_core;
    MaterialPayload result{};
    const bool usePbrBase = !fbxPolicy || material.pbr.base_color.has_value;
    const auto base = usePbrBase ? material.pbr.base_color.value_vec4
                                 : material.fbx.diffuse_color.value_vec4;
    const double baseFactor = material.pbr.base_factor.has_value
        ? material.pbr.base_factor.value_real
        : (fbxPolicy && material.fbx.diffuse_factor.has_value
            ? material.fbx.diffuse_factor.value_real : 1.0);
    result.baseColorFactor[0] = Saturate(base.x * baseFactor);
    result.baseColorFactor[1] = Saturate(base.y * baseFactor);
    result.baseColorFactor[2] = Saturate(base.z * baseFactor);
    // 3ds Max exports write an explicit scalar Opacity alongside the
    // TransparencyFactor that FBX normally inverts; when both are present the
    // authored Opacity is the surface opacity. Trusting TransparencyFactor
    // alone reads every one of those materials as fully transparent (the
    // exporter writes TransparencyFactor=1 on opaque materials), which makes
    // the whole model invisible in the shaded view while clay/wireframe, which
    // ignore material alpha, still draw it.
    double opacity = 1.0;
    if (material.pbr.opacity.has_value) {
        opacity = material.pbr.opacity.value_real;
    } else if (const ufbx_prop* authoredOpacity = ufbx_find_prop(&material.props, "Opacity");
               authoredOpacity && Finite(authoredOpacity->value_real)) {
        opacity = authoredOpacity->value_real;
    } else if (material.fbx.transparency_factor.has_value) {
        opacity = 1.0 - material.fbx.transparency_factor.value_real;
    }
    result.baseColorFactor[3] = Saturate(opacity);
    result.metallicFactor = material.pbr.metalness.has_value
        ? Saturate(material.pbr.metalness.value_real, 0.0f) : 0.0f;
    result.roughnessFactor = material.pbr.roughness.has_value
        ? Saturate(material.pbr.roughness.value_real) : 1.0f;
    // FBX Phong has no metallic channel: reflectivity is authored as
    // ReflectionColor scaled by ReflectionFactor (the classic 3ds Max
    // reflection slot), which the normalized contract has no separate field
    // for. Carry it in the one scalar metalness slot, where the shader uses
    // the base color as F0 and drops the diffuse lobe -- exactly how a mirror,
    // chrome trim, reflective car paint, or a headlight reflector reads.
    // Without this every such surface kept metalness 0 and the shader's fixed
    // 4% dielectric F0, so its environment reflection was imperceptible and it
    // rendered as flat opaque plastic. The default FBX property template
    // authors ReflectionColor=black with ReflectionFactor=1, so a non-black
    // reflection color is required: ordinary Phong dielectrics (glass, cloth,
    // matte plastic) keep metalness 0 and their authored dielectric look.
    if (fbxPolicy && !material.pbr.metalness.has_value) {
        const ufbx_vec3 reflectionColor = material.fbx.reflection_color.value_vec3;
        const double reflectivity = (std::max)({ reflectionColor.x, reflectionColor.y,
                                                 reflectionColor.z });
        if (Finite(reflectivity) && reflectivity > 1.0e-3
            && material.fbx.reflection_factor.has_value) {
            result.metallicFactor = Saturate(material.fbx.reflection_factor.value_real, 0.0f);
        }
    }
    const bool usePbrEmission = !fbxPolicy || material.pbr.emission_color.has_value;
    const auto emission = usePbrEmission ? material.pbr.emission_color.value_vec3
                                         : material.fbx.emission_color.value_vec3;
    const double emissionFactor = material.pbr.emission_factor.has_value
        ? material.pbr.emission_factor.value_real : (material.fbx.emission_factor.has_value
            && fbxPolicy ? material.fbx.emission_factor.value_real : 1.0);
    result.emissiveFactor[0] = SafeFloat(emission.x * emissionFactor);
    result.emissiveFactor[1] = SafeFloat(emission.y * emissionFactor);
    result.emissiveFactor[2] = SafeFloat(emission.z * emissionFactor);
    result.uvScale[0] = result.uvScale[1] = 1.0f;
    result.alphaMode = uint32_t(result.baseColorFactor[3] < 0.999f
        ? AlphaModeId::Blend : AlphaModeId::Opaque);
    result.alphaCutoff = 0.5f;
    // ufbx preserves the FBX/OBJ convention that a texture's V axis points up
    // (bottom-left origin), while the D3D12 samplers and every glTF/3MF/USD
    // texture carry a top-left origin. Flip the final sampling coordinate so
    // authored atlases land on their intended UV islands instead of a vertical
    // mirror of them.
    result.flags = kMaterialFlagDoubleSided | kMaterialFlagFlipV;

    if (fbxPolicy) {
        if (const ufbx_prop* alphaMode = ufbx_find_prop(
                &material.props, "3dsMax|main|alphaMode")) {
            if (alphaMode->value_int >= int64_t(AlphaModeId::Opaque)
                && alphaMode->value_int <= int64_t(AlphaModeId::Blend)) {
                result.alphaMode = static_cast<uint32_t>(alphaMode->value_int);
            } else {
                Warn(optionalWarnings);
            }
        }
        if (const ufbx_prop* alphaCutoff = ufbx_find_prop(
                &material.props, "3dsMax|main|alphaCutoff")) {
            if (!Finite(alphaCutoff->value_real)
                || alphaCutoff->value_real < 0.0 || alphaCutoff->value_real > 1.0)
                Warn(optionalWarnings);
            result.alphaCutoff = Saturate(alphaCutoff->value_real, 0.5f);
        }
        if (material.features.double_sided.is_explicit && !material.features.double_sided.enabled)
            result.flags &= ~kMaterialFlagDoubleSided;
        if (material.features.unlit.is_explicit && material.features.unlit.enabled)
            result.flags |= kMaterialFlagUnlit;
        if ((material.features.transmission.is_explicit && material.features.transmission.enabled)
            || (material.features.coat.is_explicit && material.features.coat.enabled)
            || (material.features.sheen.is_explicit && material.features.sheen.enabled)
            || (material.features.thin_walled.is_explicit && material.features.thin_walled.enabled)
            || (material.features.caustics.is_explicit && material.features.caustics.enabled))
            Warn(optionalWarnings);
        // Recover window/glass surfaces the exporter left as opaque. Only an
        // opaque, non-reflective Phong material whose name identifies it as
        // glass is affected; an authored alpha mode, opacity, or reflectivity
        // is always left untouched.
        const std::string_view materialName(material.name.data ? material.name.data : "",
                                            material.name.data ? material.name.length : 0);
        if (result.alphaMode == uint32_t(AlphaModeId::Opaque)
            && result.baseColorFactor[3] >= 0.999f
            && result.metallicFactor == 0.0f
            && LooksLikeGlass(materialName)) {
            result.transmissionFactor = 1.0f;
            result.flags |= kMaterialFlagTransmissive;
            result.alphaMode = uint32_t(AlphaModeId::Blend);
        }
    }
    return result;
}

} // namespace import_worker
