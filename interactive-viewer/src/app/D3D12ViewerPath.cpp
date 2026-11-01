#include "D3D12ViewerPath.h"
#include "DetailView.h"
#include "GroundShadow.h"
#include "LightRig.h"
#include "ScreenSpaceAo.h"
#include "StudioEnvironment.h"

#include <d3dcompiler.h>
#include <windows.h>
#include <DirectXPackedVector.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <functional>
#include <utility>
#include <vector>
#include <unordered_map>
#include <unordered_set>

using Microsoft::WRL::ComPtr;

namespace {
constexpr DWORD kIdleWaitTimeoutMs = 5000;

// T15's linear HDR intermediate. Feature-level 11_0 guarantees render-target
// and shader-resource support for R16G16B16A16_FLOAT.
constexpr DXGI_FORMAT kSceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// The catalog preset changes view state only. CAD gets a slightly lighter
// neutral backdrop so untextured parts read against it; point clouds get a
// darker backdrop so vertex colors/splats are not washed out. PBR keeps the
// historical Gate 1 #1C1C1E clear.
inline void PresentationClearColor(model_core::PresentationProfile profile, float color[4])
{
    switch (profile)
    {
    case model_core::PresentationProfile::StudioCad:
        color[0] = 0x23 / 255.0f; color[1] = 0x23 / 255.0f;
        color[2] = 0x26 / 255.0f; color[3] = 1.0f;
        break;
    case model_core::PresentationProfile::PointCloud:
        color[0] = 0x11 / 255.0f; color[1] = 0x11 / 255.0f;
        color[2] = 0x13 / 255.0f; color[3] = 1.0f;
        break;
    default:
        color[0] = 0x1C / 255.0f; color[1] = 0x1C / 255.0f;
        color[2] = 0x1E / 255.0f; color[3] = 1.0f;
        break;
    }
}

// SceneSnapshotPublisher keys a published resource by (clusterId, lodLevel)
// and replaces an entry that repeats the pair, so mesh buffers and textures
// must not share an id space. Meshes count up from 0; textures start here.
constexpr uint32_t kTextureClusterIdBase = 1u << 20;

struct DrawConstants {
    DirectX::XMFLOAT4X4 localToCamera{};
    DirectX::XMFLOAT4X4 normalToCamera{};
    uint32_t pickId = 0;
};

DrawConstants BuildDrawConstants(const double instance[16], const double origin[3],
                                 const double sceneOrigin[3], const double cameraTarget[3],
                                 const DirectX::XMFLOAT4X4& axis)
{
    DrawConstants result{};
    const auto transform=BuildCameraRelativeInstanceTransform(instance,origin,sceneOrigin,cameraTarget,axis);
    result.localToCamera=transform.localToCamera;result.normalToCamera=transform.normalToCamera;
    return result;
}

// T22 per-material CBV. The layout matches the HLSL MaterialConstants cbuffer
// byte for byte; it must stay float4-aligned because HLSL packs cbuffers that
// way. The per-slot packed word is addressU | addressV<<2 | nearest<<4 |
// uvSet<<5, with bit 9 marking an authored uv set this build cannot sample.
struct MaterialConstantData
{
    float baseColorFactor[4]{};
    float materialFactors[4]{};
    float emissiveFactor[4]{};
    float uvLegacy[4]{};
    float uvLegacy2[4]{};
    float lobes0[4]{};
    float lobes1[4]{};
    float lobes2[4]{};
    float extras[4]{};
    float slotUv[model_core::kMaterialTextureSlotCount][4]{};
    float slotParams[model_core::kMaterialTextureSlotCount][4]{};
};
static_assert(sizeof(MaterialConstantData) <= D3D12ViewerPath::kMaterialConstantStride,
              "material constant data exceeds its CBV stride");

constexpr uint32_t kRenderSlotFlagUnsupportedUvSet = 1u << 9;

// Root-signature parameter for each fixed material texture slot. Slots 0..3
// are t0..t3, slot 4 is t5 (t4 stays the Studio cubemap), and the eight T25
// extension maps are the appended tables 10..17 (t7..t14).
constexpr UINT kMaterialSlotRootParameter[model_core::kMaterialTextureSlotCount] = {
    1, 2, 3, 4, 6, 10, 11, 12, 13, 14, 15, 16, 17};

uint32_t RenderMaterialFlags(const model_core::MaterialPayload& material, bool twoSided) noexcept
{
    return (twoSided ? 1u : 0u)
        | ((material.flags & model_core::kMaterialFlagUnlit) ? 2u : 0u)
        | (material.alphaMode == uint32_t(model_core::AlphaModeId::Mask) ? 4u : 0u)
        | ((material.flags & model_core::kMaterialFlagFlipV) ? 8u : 0u)
        | ((material.flags & model_core::kMaterialSamplerFlags) << 1)
        | ((material.flags & model_core::kMaterialFlagTextureLayer) ? 512u : 0u)
        | ((material.flags & model_core::kMaterialFlagTextureMix) ? 1024u : 0u)
        | ((material.flags & model_core::kMaterialFlagVertexSrgb) ? 2048u : 0u)
        | ((material.flags & model_core::kMaterialFlagTransmissive) ? 4096u : 0u)
        // T26: only BLEND draws carry coverage alpha. OPAQUE and MASK writes
        // are opaque after the authored cutout, so the resolve must not leak
        // the background through a low alpha texel in an opaque material.
        | (material.alphaMode == uint32_t(model_core::AlphaModeId::Blend) ? 8192u : 0u);
}

uint32_t RenderSlotPacked(const model_core::MaterialPayload& material, uint32_t slot) noexcept
{
    uint32_t addressU = 0, addressV = 0, uvSet = 0;
    bool nearest = false;
    const uint32_t slotFlags = material.slotFlags[slot];
    if (slotFlags == 0 && (material.flags & model_core::kMaterialSamplerFlags) != 0) {
        // Producers predating per-slot metadata keep their material-wide
        // addressing/nearest intent rather than silently becoming wrap/linear.
        addressU = (material.flags >> model_core::kMaterialAddressUShift)
            & model_core::kMaterialAddressMask;
        addressV = (material.flags >> model_core::kMaterialAddressVShift)
            & model_core::kMaterialAddressMask;
        nearest = (material.flags & model_core::kMaterialFlagNearest) != 0;
    } else {
        addressU = (slotFlags >> model_core::kMaterialSlotAddressUShift)
            & model_core::kMaterialSlotAddressMask;
        addressV = (slotFlags >> model_core::kMaterialSlotAddressVShift)
            & model_core::kMaterialSlotAddressMask;
        nearest = (slotFlags & model_core::kMaterialSlotNearest) != 0;
        uvSet = model_core::MaterialSlotUvSet(slotFlags);
    }
    uint32_t packed = addressU | (addressV << 2) | (nearest ? 16u : 0u)
        | ((std::min)(uvSet, 15u) << 5);
    if (uvSet > model_core::kMaterialMaxSupportedUvSet) packed |= kRenderSlotFlagUnsupportedUvSet;
    return packed;
}

void FillMaterialConstantData(const model_core::MaterialPayload& material, uint32_t mapMask,
                              bool twoSided, bool mirrored, MaterialConstantData& data) noexcept
{
    std::memcpy(data.baseColorFactor, material.baseColorFactor, sizeof(data.baseColorFactor));
    data.materialFactors[0] = material.metallicFactor;
    data.materialFactors[1] = material.roughnessFactor;
    data.materialFactors[2] = material.alphaCutoff;
    data.materialFactors[3] = float(RenderMaterialFlags(material, twoSided));
    data.emissiveFactor[0] = material.emissiveFactor[0];
    data.emissiveFactor[1] = material.emissiveFactor[1];
    data.emissiveFactor[2] = material.emissiveFactor[2];
    data.emissiveFactor[3] = material.emissiveStrength;
    data.uvLegacy[0] = material.uvOffset[0];
    data.uvLegacy[1] = material.uvOffset[1];
    data.uvLegacy[2] = material.uvScale[0];
    data.uvLegacy[3] = material.uvScale[1];
    data.uvLegacy2[0] = std::cos(material.uvRotation);
    data.uvLegacy2[1] = std::sin(material.uvRotation);
    data.uvLegacy2[2] = float(mapMask);
    data.uvLegacy2[3] = material.transmissionFactor;
    data.lobes0[0] = material.clearcoatFactor;
    data.lobes0[1] = material.clearcoatRoughness;
    data.lobes0[2] = material.occlusionStrength;
    data.lobes0[3] = material.sheenRoughness;
    data.lobes1[0] = material.sheenColor[0];
    data.lobes1[1] = material.sheenColor[1];
    data.lobes1[2] = material.sheenColor[2];
    data.lobes1[3] = material.specularFactor;
    data.lobes2[0] = material.ior;
    data.lobes2[1] = material.specularColor[0];
    data.lobes2[2] = material.specularColor[1];
    data.lobes2[3] = material.specularColor[2];
    data.extras[0] = material.normalScale;
    data.extras[1] = mirrored ? -1.0f : 1.0f;
    data.extras[2] = material.clearcoatNormalScale;
    for (uint32_t slot = 0; slot < model_core::kMaterialTextureSlotCount; ++slot) {
        float offsetU = 0.0f, offsetV = 0.0f, scaleU = 1.0f, scaleV = 1.0f, rotation = 0.0f;
        if (slot == model_core::kMaterialSlotIndexBaseColor) {
            offsetU = material.uvOffset[0]; offsetV = material.uvOffset[1];
            scaleU = material.uvScale[0]; scaleV = material.uvScale[1];
            rotation = material.uvRotation;
        } else {
            offsetU = material.slotUvOffset[slot - 1][0];
            offsetV = material.slotUvOffset[slot - 1][1];
            scaleU = material.slotUvScale[slot - 1][0];
            scaleV = material.slotUvScale[slot - 1][1];
            rotation = material.slotUvRotation[slot - 1];
        }
        data.slotUv[slot][0] = offsetU;
        data.slotUv[slot][1] = offsetV;
        data.slotUv[slot][2] = scaleU;
        data.slotUv[slot][3] = scaleV;
        data.slotParams[slot][0] = std::cos(rotation);
        data.slotParams[slot][1] = std::sin(rotation);
        data.slotParams[slot][2] = float(RenderSlotPacked(material, slot));
        data.slotParams[slot][3] = 0.0f;
    }
}

// D3D12 root constants are limited to 64 DWORDs per root signature. The
// textured signature uploads a transposed 3x4 local transform (12 DWORDs) and
// a 3x3 normal matrix (12 DWORDs, three padded rows) instead of two 4x4
// matrices, plus the pick id: 12 + 12 + 1 = 25 DWORDs for b1. T22 moved the
// 32-DWORD material block into a b2 CBV (2 DWORDs) so six single-descriptor
// SRV tables, the b0 frame CBV and b2 fit: 2 + 6 + 25 + 2 = 35 DWORDs.
struct TexturedDrawConstants {
    float localToCamera[12]{};
    float normalToCamera[12]{};
    uint32_t pickId = 0;
};
static_assert(sizeof(TexturedDrawConstants) == 25 * sizeof(uint32_t));

TexturedDrawConstants BuildTexturedDrawConstants(const DrawConstants& source)
{
    TexturedDrawConstants result{};
    // HLSL reads a row_major float3x4 as mul(matrix, position), i.e. the
    // transpose of the 4x4 row-vector transform, truncated to three rows.
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
            result.localToCamera[row * 4 + column] = source.localToCamera.m[column][row];
        }
    }
    // The shader dots the authored normal with three stored columns, so keep
    // the transpose of the normal matrix's upper-left 3x3.
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            result.normalToCamera[column * 4 + row] = source.normalToCamera.m[row][column];
        }
    }
    result.pickId = source.pickId;
    return result;
}

void TransformGridBounds(const DirectX::XMFLOAT3& minimum, const DirectX::XMFLOAT3& maximum,
                         DirectX::FXMMATRIX transform, DirectX::XMFLOAT3& outMinimum,
                         DirectX::XMFLOAT3& outMaximum)
{
    const DirectX::XMVECTOR corners[8] = {
        DirectX::XMVectorSet(minimum.x, minimum.y, minimum.z, 1),
        DirectX::XMVectorSet(maximum.x, minimum.y, minimum.z, 1),
        DirectX::XMVectorSet(minimum.x, maximum.y, minimum.z, 1),
        DirectX::XMVectorSet(maximum.x, maximum.y, minimum.z, 1),
        DirectX::XMVectorSet(minimum.x, minimum.y, maximum.z, 1),
        DirectX::XMVectorSet(maximum.x, minimum.y, maximum.z, 1),
        DirectX::XMVectorSet(minimum.x, maximum.y, maximum.z, 1),
        DirectX::XMVectorSet(maximum.x, maximum.y, maximum.z, 1),
    };
    DirectX::XMVECTOR transformedMinimum = DirectX::g_XMFltMax;
    DirectX::XMVECTOR transformedMaximum = -DirectX::g_XMFltMax;
    for (const auto& corner : corners) {
        const auto transformed = DirectX::XMVector3TransformCoord(corner, transform);
        transformedMinimum = DirectX::XMVectorMin(transformedMinimum, transformed);
        transformedMaximum = DirectX::XMVectorMax(transformedMaximum, transformed);
    }
    DirectX::XMStoreFloat3(&outMinimum, transformedMinimum);
    DirectX::XMStoreFloat3(&outMaximum, transformedMaximum);
}

// Embedded HLSL, compiled at runtime via D3DCompile -- same convention
// Renderer.cpp's D3D11 shader strings already use; no .hlsl files on disk.
// The compact shader handles position-only and position/normal STEP geometry;
// complete vertices use the textured material shader below, while points use
// the splat shaders.
constexpr char kVertexShaderSource[] = R"(
cbuffer FrameConstants : register(b0)
{
    row_major float4x4 gViewProjection;
    float4 gEyeSelection;
    float4 gViewport;
    float4 gLighting; // mode, legacy azimuth/elevation, reserved
};

cbuffer DrawConstants : register(b1)
{
    row_major float4x4 gLocalToCamera;
    row_major float4x4 gNormalToCamera;
    uint gPickId;
};
struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD1;
    float3 normal : NORMAL;
    nointerpolation uint pickId : TEXCOORD7;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    float4 relativePosition = mul(float4(input.position, 1.0f), gLocalToCamera);
    output.position = mul(relativePosition, gViewProjection);
    output.worldPosition = relativePosition.xyz;
    output.normal = mul(float4(input.normal, 0.0f), gNormalToCamera).xyz;
    output.pickId = gPickId;
    return output;
}
)";

constexpr char kPixelShaderSource[] = R"(
cbuffer FrameConstants : register(b0)
{
    row_major float4x4 gViewProjection;
    float4 gEyeSelection;
    float4 gViewport;
    float4 gLighting;
    // T30: the remaining FrameConstants prefix is declared so this compact
    // shader can read the world-space sun at the same offset as the full
    // material shader. All fields are float4-aligned; nothing here is used.
    float4 gWorldFromCamera;
    float4 gEnvironment;
    float4 gSh[9];
    float4 gPresentation;
    float4 gTransmissionFrame;
    float4 gSunDirection;
};
cbuffer MaterialConstants : register(b2) { float4 gBaseColorFactor; };
struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD1;
    float3 normal : NORMAL;
    nointerpolation uint pickId : TEXCOORD7;
};

#ifdef AO_INDIRECT
struct PixelOutput { float4 color : SV_TARGET0; float4 indirect : SV_TARGET1;
                     uint pick : SV_TARGET2; };
#else
struct PixelOutput { float4 color : SV_TARGET0; uint pick : SV_TARGET1; };
#endif
struct OitOutput { float4 accum : SV_TARGET0; float4 reveal : SV_TARGET1; };
float OitWeight(float viewDepth)
{
    float z=max(viewDepth,1e-3f)/max(gViewport.z,1e-3f);
    return clamp(0.03f/(1e-5f+pow(z,4.0f)),1e-2f,300.0f);
}
#ifdef OIT_PASS
OitOutput PSMain(PSInput input)
#else
PixelOutput PSMain(PSInput input)
#endif
{
    if ((input.pickId&0x80000000u)!=0) {
#ifdef OIT_PASS
        OitOutput wire;wire.accum=float4(.72f,.76f,.82f,1);wire.reveal=float4(0.0f,0.0f,0.0f,0.0f);return wire;
#else
        PixelOutput wire;wire.color=float4(.72f,.76f,.82f,1);
#ifdef AO_INDIRECT
        wire.indirect=float4(0.0f,0.0f,0.0f,0.0f);
#endif
        wire.pick=input.pickId&0x7fffffffu;return wire;
#endif
    }
    float3 n = normalize(input.normal);
    float3 albedo = gLighting.x>0.5f && gLighting.x<1.5f
        ? float3(.58f,.58f,.58f) : gBaseColorFactor.rgb;
    // T32: split the compact shader's ambient/hemispherical floor from its
    // direct terms so the same indirect-only AO resolve applies to legacy
    // position-only CAD geometry as to the complete material path.
    float3 indirect;
    float3 direct;
    if (gLighting.x > 1.5f) {
        // Same fixed world frame as the material shader; the sun is already
        // world-anchored, so no camera rotation belongs here.
        float3 lightDir=normalize(gSunDirection.xyz);
        indirect=albedo*0.08f;
        direct=albedo*(1.05f*saturate(dot(n,lightDir)));
    } else {
        float key=saturate(dot(n,normalize(float3(.45f,-.55f,.70f))));
        float fill=saturate(dot(n,normalize(float3(-.70f,.25f,.38f))));
        float rim=saturate(dot(n,normalize(float3(.18f,.76f,.28f))));
        float hemi=saturate(n.z*.5f+.5f);
        indirect=albedo*(0.10f+0.18f*hemi);
        direct=albedo*(0.62f*key+0.30f*fill+0.16f*rim);
    }
    float3 color=direct+indirect;
    float3 viewDir = normalize(gEyeSelection.xyz - input.worldPosition);
    float outline = pow(1.0f - saturate(dot(n,viewDir)), 2.0f);
    color += gEyeSelection.w * (outline * 0.45f * float3(0.36f,0.62f,1.0f) + 0.03f);
#ifdef OIT_PASS
    float coverage=saturate(gBaseColorFactor.a);
    float weight=OitWeight(length(input.worldPosition));
    OitOutput output;
    output.accum=float4(max(color,0.0f)*coverage*weight,coverage*weight);
    output.reveal=float4(coverage,0.0f,0.0f,coverage);
    return output;
#else
    PixelOutput output; output.color = float4(saturate(color),
        gLighting.x>0.5f && gLighting.x<1.5f ? 1.0f : gBaseColorFactor.a);
#ifdef AO_INDIRECT
    output.indirect = float4(max(indirect,0.0f),1.0f);
#endif
    output.pick = input.pickId; return output;
#endif
}
)";

// Complete material variant. The shader consumes every normalized material
// field and texture slot while keeping the legacy shader available for older
// position-only geometry.
constexpr char kTexturedVertexShaderSource[] = R"(
cbuffer FrameConstants : register(b0)
{
    row_major float4x4 gViewProjection;
    float4 gEyeSelection;
    float4 gViewport;
    float4 gLighting;
};

cbuffer DrawConstants : register(b1)
{
    // Explicit float4 rows keep the cbuffer layout unambiguous: three rows of
    // the transposed 3x4 local transform, three columns of the normal matrix,
    // then the pick id (25 DWORDs total).
    float4 gLocalToCamera0;
    float4 gLocalToCamera1;
    float4 gLocalToCamera2;
    float4 gNormalToCamera0;
    float4 gNormalToCamera1;
    float4 gNormalToCamera2;
    uint gPickId;
};
struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 tangent : TANGENT;
    float4 color : COLOR0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD1;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 tangent : TANGENT;
    float4 color : COLOR0;
    nointerpolation uint pickId : TEXCOORD7;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    const float4 position = float4(input.position, 1.0f);
    float4 relativePosition = float4(
        dot(gLocalToCamera0, position),
        dot(gLocalToCamera1, position),
        dot(gLocalToCamera2, position), 1.0f);
    output.position = mul(relativePosition, gViewProjection);
    output.worldPosition = relativePosition.xyz;
    output.normal = float3(dot(input.normal, gNormalToCamera0.xyz),
                           dot(input.normal, gNormalToCamera1.xyz),
                           dot(input.normal, gNormalToCamera2.xyz));
    // T22: the raw authored TEXCOORD set travels to the pixel shader. Each
    // texture slot applies its own authored transform and sampler, so a single
    // vertex-stage uv transform cannot represent the material any more.
    output.uv=input.uv;
    output.tangent=float4(normalize(float3(dot(input.tangent.xyz,gNormalToCamera0.xyz),
                                          dot(input.tangent.xyz,gNormalToCamera1.xyz),
                                          dot(input.tangent.xyz,gNormalToCamera2.xyz))),input.tangent.w);
    output.color=input.color;
    output.pickId=gPickId;
    return output;
}
)";

constexpr char kTexturedPixelShaderSource[] = R"(
cbuffer FrameConstants : register(b0)
{
    row_major float4x4 gViewProjection;
    float4 gEyeSelection;
    float4 gViewport;
    float4 gLighting;
    float4 gWorldFromCamera; // world-from-camera rotation quaternion
    float4 gEnvironment;     // intensity, yaw, exposure, last specular mip
    float4 gSh[9];           // diffuse irradiance/pi, RGB coefficients
    float4 gPresentation;    // key/fill/rim direct-light multipliers
    // T27: (1/sceneWidth, 1/sceneHeight, last transmission mip, usable flag)
    float4 gTransmissionFrame;
    // T30: one world-space sun direction (xyz, unit vector towards the light).
    // The shading vectors share this Z-up world frame (camera-relative
    // origin only); gViewProjection applies the camera rotation later.
    float4 gSunDirection;
};
Texture2D gBaseColor : register(t0);
Texture2D gMetallicRoughness : register(t1);
Texture2D gNormal : register(t2);
Texture2D gEmissive : register(t3);
TextureCube gStudioEnvironment : register(t4);
// T22: glTF occlusionTexture is an independent binding. The metallic-roughness
// red channel stays unused per ORM; it is never aliased to AO.
Texture2D gOcclusion : register(t5);
// T24 split-sum environment BRDF integration LUT: x = NoV, y = roughness.
Texture2D<float2> gBrdfLut : register(t6);
// T25 extension-lobe maps. Each extension fixes its channel: clearcoat R,
// clearcoat roughness G, clearcoat normal RGB, specular A, specular color RGB,
// sheen color RGB, sheen roughness A and transmission R.
Texture2D gClearcoat : register(t7);
Texture2D gClearcoatRoughness : register(t8);
Texture2D gClearcoatNormal : register(t9);
Texture2D gSpecular : register(t10);
Texture2D gSpecularColor : register(t11);
Texture2D gSheenColor : register(t12);
Texture2D gSheenRoughness : register(t13);
Texture2D gTransmission : register(t14);
// T27 scene-color transmission pyramid: a per-frame copy of the opaque HDR
// scene and its bounded mip chain. Roughness selects a filtered mip.
Texture2D gTransmissionSource : register(t15);
SamplerState gLinearSampler : register(s0);
SamplerState gNearestSampler : register(s1);
SamplerState gEnvironmentSampler : register(s2);
// The per-material record is a CBV now, not root constants, so the extra
// per-slot transforms fit inside D3D12's 64-DWORD root-signature budget.
cbuffer MaterialConstants : register(b2)
{
    float4 gBaseColorFactor;
    float4 gMaterialFactors;        // metallic, roughness, alpha cutoff, flags-as-float
    float4 gEmissiveFactor;         // rgb; .a carries emissive strength
    float4 gUvLegacy;               // base-color offset.xy, scale.xy
    float4 gUvLegacy2;              // cos, sin, map mask, transmission
    float4 gMaterialLobes0;         // clearcoat factor/roughness, occlusion strength, sheen roughness
    float4 gMaterialLobes1;         // sheen rgb, specular factor
    float4 gMaterialLobes2;         // IOR, specular color rgb
    float4 gMaterialExtras;         // normal scale, mirrored sign, clearcoat normal scale
    float4 gSlotUv[13];             // per-slot offset.xy, scale.xy
    float4 gSlotParams[13];         // per-slot cos, sin, packed sampler/uv-set
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD1;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 tangent : TANGENT;
    float4 color : COLOR0;
    nointerpolation uint pickId : TEXCOORD7;
    bool frontFace : SV_IsFrontFace;
};

struct SlotUv
{
    float2 uv;
    bool valid;
    bool nearest;
};

// Applies one slot's authored KHR_texture_transform and address mode. The
// packed word carries addressU | addressV<<2 | nearest<<4 | uvSet<<5 with bit
// 9 set when the authored UV set cannot be sampled by this layout. An
// unsupported set is never aliased to UV0: the slot is reported invalid and
// the caller skips it.
SlotUv ResolveSlotUv(uint slot, float2 raw)
{
    const float4 xform=gSlotUv[slot];
    const float4 params=gSlotParams[slot];
    const uint packed=(uint)params.z;
    const uint addressU=packed&3u;
    const uint addressV=(packed>>2)&3u;
    const uint uvSet=(packed>>5)&0xFu;
    SlotUv result;
    result.nearest=(packed&16u)!=0;
    float2 scaled=raw*xform.zw;
    float2 uv=float2(scaled.x*params.x-scaled.y*params.y,
                     scaled.x*params.y+scaled.y*params.x)+xform.xy;
    if (((uint)gMaterialFactors.w&8u)!=0) uv.y=1.0f-uv.y;
    result.uv=uv;
    const bool supported=((packed&512u)==0u)&&uvSet==0u;
    const bool uInside=(addressU!=3u)||(uv.x>=0.0f&&uv.x<=1.0f);
    const bool vInside=(addressV!=3u)||(uv.y>=0.0f&&uv.y<=1.0f);
    result.valid=supported&&uInside&&vInside;
    result.uv.x=addressU==0u?frac(uv.x):addressU==1u?1.0f-abs(frac(uv.x*.5f)*2.0f-1.0f):saturate(uv.x);
    result.uv.y=addressV==0u?frac(uv.y):addressV==1u?1.0f-abs(frac(uv.y*.5f)*2.0f-1.0f):saturate(uv.y);
    return result;
}
float4 SampleSlot(Texture2D tex,SlotUv uv)
{
    return uv.nearest?tex.Sample(gNearestSampler,uv.uv):tex.Sample(gLinearSampler,uv.uv);
}
float2 TransformSlotUvUnwrapped(uint slot,float2 raw)
{
    const float4 xform=gSlotUv[slot];
    const float4 params=gSlotParams[slot];
    float2 scaled=raw*xform.zw;
    float2 uv=float2(scaled.x*params.x-scaled.y*params.y,
                     scaled.x*params.y+scaled.y*params.x)+xform.xy;
    if (((uint)gMaterialFactors.w&8u)!=0) uv.y=1.0f-uv.y;
    return uv;
}

#ifdef AO_INDIRECT
// T32 opaque material layout: full linear HDR colour, the environment/IBL
// share for the AO resolve, and the pick id. Keeping indirect separate is what
// lets screen AO darken only ambient/IBL terms instead of the whole lit image.
struct PixelOutput { float4 color : SV_TARGET0; float4 indirect : SV_TARGET1;
                     uint pick : SV_TARGET2; };
#else
struct PixelOutput { float4 color : SV_TARGET0; uint pick : SV_TARGET1; };
#endif
struct OitOutput { float4 accum : SV_TARGET0; float4 reveal : SV_TARGET1; };
// Weighted blended OIT weight (McGuire-Bavoil family). gViewport.z carries the
// camera distance so the curve is scene-relative rather than a fixed unit
// assumption. Nearer fragments dominate; the clamps keep every fragment
// finite and the accumulation commutative.
float OitWeight(float viewDepth)
{
    float z=max(viewDepth,1e-3f)/max(gViewport.z,1e-3f);
    return clamp(0.03f/(1e-5f+pow(z,4.0f)),1e-2f,300.0f);
}

// Trowbridge-Reitz GGX for the squared perceptual roughness, matching the CPU
// StudioEnvironment bake and the Khronos reference renderer.
float DistributionGGX(float noH,float roughness)
{
    float a=roughness*roughness, a2=a*a;
    float f=noH*noH*(a2-1.0f)+1.0f;
    return a2/max(0.0001f,3.14159265f*f*f);
}
// Height-correlated Smith GGX visibility V = G / (4 NoL NoV).
float GeometryVisibilityGGX(float noL,float noV,float roughness)
{
    float a=roughness*roughness, a2=a*a;
    float gv=noL*sqrt(noV*noV*(1.0f-a2)+a2);
    float gl=noV*sqrt(noL*noL*(1.0f-a2)+a2);
    float g=gv+gl;
    return g>0.0f?0.5f/g:0.0f;
}
// IOR-derived dielectric F0. ior <= 1 has no optical interface.
float IorToF0(float ior)
{
    if (!(ior>1.0f)) return 0.0f;
    float ratio=(ior-1.0f)/(ior+1.0f);
    return saturate(ratio*ratio);
}
float3 FresnelSchlick(float3 f0,float3 f90,float cosTheta)
{
    return f0+(f90-f0)*pow(saturate(1.0f-cosTheta),5.0f);
}
float FresnelSchlick(float f0,float f90,float cosTheta)
{
    return f0+(f90-f0)*pow(saturate(1.0f-cosTheta),5.0f);
}
// T24 split-sum DFG LUT (x = NoV, y = perceptual roughness). The CPU bake in
// StudioEnvironment.cpp is the reference implementation.
float2 SampleBrdfLut(float nv,float roughness)
{
    return gBrdfLut.SampleLevel(gLinearSampler,float2(saturate(nv),saturate(roughness)),0).rg;
}
// Khronos environment Fresnel: F0*A + B plus the Fdez-Aguera multiple-
// scattering compensation. F0 = 1, weight = 1 is the white-furnace identity.
float3 EnvironmentBrdfFresnel(float3 f0,float nv,float roughness,float specularWeight)
{
    float2 ab=SampleBrdfLut(nv,roughness);
    float3 fr=max((1.0f-roughness).xxx,f0)-f0;
    float3 kS=f0+fr*pow(saturate(1.0f-nv),5.0f);
    float3 fssEss=specularWeight*(kS*ab.x+ab.y);
    float ems=1.0f-(ab.x+ab.y);
    float3 fAvg=specularWeight*(f0+(1.0f-f0)/21.0f);
    float3 fmsEms=ems*fssEss*fAvg/max(1e-4f,1.0f-fAvg*ems);
    return max(fssEss+fmsEms,0.0f);
}
// Khronos glTF punctual BRDF. f0/f90 carry KHR_materials_ior/specular: the
// IOR/specularColor set normal-incidence F0 while specularFactor sets F90, so
// normal and grazing response stay independent. outDiffuse/outSpecular split
// the same combined expression for the T21/T24 diagnostics.
float3 EvaluateLightSplit(float3 n,float3 v,float3 l,float3 base,float metallic,float roughness,
                          float3 f0,float3 f90,float intensity,
                          out float3 outDiffuse,out float3 outSpecular)
{
    float3 h=normalize(v+l);
    float nl=saturate(dot(n,l)), nv=saturate(dot(n,v));
    float nh=saturate(dot(n,h)), vh=saturate(dot(v,h));
    float specNl=intensity*nl*GeometryVisibilityGGX(nl,nv,roughness)*DistributionGGX(nh,roughness);
    float3 fd=FresnelSchlick(f0,f90,vh);
    float3 fm=FresnelSchlick(base,float3(1,1,1),vh);
    float3 lDiffuse=intensity*nl*base/3.14159265f;
    float3 dielectric=lerp(lDiffuse,specNl.xxx,fd);
    float3 metal=fm*specNl;
    float3 combined=lerp(dielectric,metal,metallic);
    outDiffuse=(1.0f-metallic)*lDiffuse*(1.0f-fd);
    outSpecular=max(combined-outDiffuse,0.0f);
    return combined;
}
// Clearcoat is a second dielectric GGX lobe. Like the Khronos reference it
// does NOT carry its own Fresnel here: the caller mixes the base toward this
// lobe by clearcoatFactor * F_schlick(coatF0, 1, NoV), which is what makes the
// attenuation energy-consistent.
float3 EvaluateClearcoatLight(float3 n,float3 v,float3 l,float roughness,float intensity)
{
    float3 h=normalize(v+l);
    float nl=saturate(dot(n,l)), nv=saturate(dot(n,v));
    float nh=saturate(dot(n,h));
    float s=intensity*nl*GeometryVisibilityGGX(nl,nv,roughness)*DistributionGGX(nh,roughness);
    return s.xxx;
}
// The vertex shader subtracts the camera origin in double precision but keeps
// directions in the fixed Z-up world frame. The IBL/reflection path applies
// this existing world-from-camera conversion before sampling the procedural
// studio cubemap; the direct lighting and the T30 direction use the
// world-frame normal/view vectors directly.
float3 RotateWorldFromCamera(float3 v)
{
    float3 q=gWorldFromCamera.xyz;
    return v+2.0f*cross(q,cross(q,v)+gWorldFromCamera.w*v);
}
// T30 coordinate convention: BuildCameraRelativeInstanceTransform (below the
// shader) subtracts the double-precision camera origin but does not rotate by
// the camera. `worldPosition`, the shading normal and `viewDir` therefore live
// in the fixed Z-up world frame; the camera rotation is applied by
// gViewProjection after this shader. A world-anchored sun is used directly:
// orbiting the camera changes viewDir (and the view matrix) while the sun
// vector, lit face and gizmo stay put. RotateWorldFromCamera above remains the
// environment-frame conversion used by the IBL/reflection path.
float3 EnvironmentDirection(float3 worldDir)
{
    // Studio yaw is a view preference, not part of the baked cubemap.
    float c=cos(gEnvironment.y), s=sin(gEnvironment.y);
    return float3(c*worldDir.x+s*worldDir.y,-s*worldDir.x+c*worldDir.y,worldDir.z);
}
// SH9 diffuse irradiance (already divided by pi on the CPU).
float3 EvaluateStudioIrradiance(float3 n)
{
    float3 result=gSh[0].rgb*0.282095f;
    result+=gSh[1].rgb*(0.488603f*n.y);
    result+=gSh[2].rgb*(0.488603f*n.z);
    result+=gSh[3].rgb*(0.488603f*n.x);
    result+=gSh[4].rgb*(1.092548f*n.x*n.y);
    result+=gSh[5].rgb*(1.092548f*n.y*n.z);
    result+=gSh[6].rgb*(0.315392f*(3.0f*n.z*n.z-1.0f));
    result+=gSh[7].rgb*(1.092548f*n.x*n.z);
    result+=gSh[8].rgb*(0.546274f*(n.x*n.x-n.y*n.y));
    return max(result,0.0f);
}
// GGX-prefiltered specular cubemap. Roughness 1 selects the coarsest mip.
float3 SampleStudioSpecular(float3 worldNormal,float3 worldView,float roughness)
{
    float3 refl=reflect(-worldView,worldNormal);
    return gStudioEnvironment.SampleLevel(gEnvironmentSampler,
        EnvironmentDirection(refl),roughness*gEnvironment.w).rgb;
}
// Material writes raw linear HDR only. The full-screen resolve owns exposure,
// the tone curve and the single sRGB display encoding; duplicating any of them
// here would encode twice.
float3 SrgbToLinear(float3 color)
{
    float3 low=color/12.92f;
    float3 high=pow((color+0.055f)/1.055f,2.4f);
    return lerp(high,low,step(color,0.04045f));
}
// glTF occlusion strength: 1 + strength * (sample - 1). The authored
// occlusionStrength now travels in MaterialPayload.gMaterialLobes0.z; a
// missing map samples 1.0 and leaves the factor at 1.0.
float OcclusionFactor(float sample,float strength)
{
    return saturate(1.0f+clamp(strength,0.0f,1.0f)*(saturate(sample)-1.0f));
}
#ifdef OIT_PASS
OitOutput PSMain(PSInput input)
#else
PixelOutput PSMain(PSInput input)
#endif
{
    if ((input.pickId&0x80000000u)!=0) {
#ifdef OIT_PASS
        OitOutput wire;wire.accum=float4(.72f,.76f,.82f,1);wire.reveal=float4(0.0f,0.0f,0.0f,0.0f);return wire;
#else
        PixelOutput wire;wire.color=float4(.72f,.76f,.82f,1);
#ifdef AO_INDIRECT
        wire.indirect=float4(0.0f,0.0f,0.0f,0.0f);
#endif
        wire.pick=input.pickId&0x7fffffffu;return wire;
#endif
    }
    uint flags=(uint)gMaterialFactors.w;
    uint maps=(uint)gUvLegacy2.z;
    bool textureLayer=(flags&512u)!=0;
    bool textureMix=(flags&1024u)!=0;
    bool clay=gLighting.x>0.5f && gLighting.x<1.5f;
    bool doubleSided=(flags&1u)!=0;
    // T27: the scene-color path is a frame resource. When it is unavailable
    // the same material renders as its authored dielectric surface (coverage
    // and reflection only) and fields 259-263 record the degradation.
    bool transmissionAvailable=gTransmissionFrame.w>0.5f;
    bool transmissiveFlag=!clay && (flags&4096u)!=0u;
    bool transmissive=transmissiveFlag && transmissionAvailable;
    // A double-sided transmissive surface shades whichever authored face the
    // camera sees. A single plane is therefore one thin interface and stays
    // visible from both sides; a closed shell contributes its two surfaces as
    // two bounded thin interfaces to the OIT composite.
    // Every map slot resolves its own uv set, transform, address mode and
    // sampler. Slots 0..4 mirror the material chunk's image dependency order.
    SlotUv baseUv=ResolveSlotUv(0,input.uv);
    float4 vertexColor=input.color;
    if ((flags&2048u)!=0) vertexColor.rgb=SrgbToLinear(vertexColor.rgb);
    // T29 Clay is a geometry-review mode, not a low-specular material preset.
    // The CPU rewrites every triangle into the opaque bucket and the shader
    // replaces authored shading with one neutral matte. The only authored
    // coverage that survives is a MASK cutout's texture alpha: those holes are
    // modelled geometry, so they are sampled independently of the gray
    // shading. BLEND/transmissive coverage is deliberately discarded here
    // because the same override draws that surface opaque; retaining authored
    // alpha would leak the background through a surface the depth pass treats
    // as solid.
    float4 base=clay ? float4(.58f,.58f,.58f,1.0f) : gBaseColorFactor*vertexColor;
    float cutoutAlpha=1.0f;
    if (clay) {
        if ((flags&4u)!=0u) {
            if ((maps&1u)!=0u && baseUv.valid) cutoutAlpha=SampleSlot(gBaseColor,baseUv).a;
            else if ((maps&1u)!=0u && textureLayer) cutoutAlpha=0.0f;
            if (cutoutAlpha<gMaterialFactors.z) discard;
        }
    } else {
        if ((maps&1) && baseUv.valid) {
            float4 tex=SampleSlot(gBaseColor,baseUv);
            // Authored base-color alpha is significant for MASK cutouts and BLEND
            // coverage. OPAQUE/MASK writes force display alpha to 1 after the
            // cutoff, so keeping the sampled alpha here cannot leak background
            // through an opaque material; it is what makes a textured cutout work.
            if (textureMix) {
                base.rgb=tex.rgb*tex.a+base.rgb*(1.0f-tex.a);
                base.a=tex.a+base.a*(1.0f-tex.a);
            } else base*=tex;
        } else if ((maps&1) && textureLayer) {
            // An authored address mode of None outside [0,1] means the layer
            // contributes no coverage; it must not fall back to UV0 sampling.
            base.a=0.0f;
        }
        if ((flags&4)!=0 && base.a<gMaterialFactors.z) discard;
    }
    // Authored coverage: BLEND keeps its alpha, OPAQUE/MASK covers the pixel
    // completely. Optical transmission and interface Fresnel never enter this
    // value (T27). Clay always covers the pixel: its surfaces are opaque by
    // construction, and only the MASK cutout above may discard.
    float effectiveCoverage=clay?1.0f:(((flags&8192u)!=0u)?saturate(base.a):1.0f);
    float3 geometricN=normalize(input.normal);
    // glTF double-sided back faces: reverse the geometric normal before the
    // tangent basis is formed. Flipping the perturbed normal toward the eye
    // instead would invert the normal map's slope on back faces.
    if (doubleSided && !input.frontFace) geometricN=-geometricN;
    float3 n=geometricN;
    // The coat layer owns a separate authored normal map, so build one shared
    // tangent basis and perturb base and coat independently. The fallback
    // derivative frame uses whichever normal-map slot is active.
    float3 tangentDir=float3(1.0f,0.0f,0.0f);
    float3 bitangentDir=float3(0.0f,1.0f,0.0f);
    if (!clay && ((maps&4)||(maps&128))) {
        tangentDir=input.tangent.xyz;
        if (dot(tangentDir,tangentDir)<1e-10f) {
            // PositionNormalUv0 layouts carry no tangent; recover one from the
            // screen-space derivatives of the (transformed) normal-map uv so
            // the map still perturbs in the right frame.
            uint fallbackSlot=(maps&4)!=0u ? 2u : 7u;
            float2 uv=TransformSlotUvUnwrapped(fallbackSlot,input.uv);
            float3 dpdx=ddx(input.worldPosition),dpdy=ddy(input.worldPosition);
            float2 duvdx=ddx(uv),duvdy=ddy(uv);
            float det=duvdx.x*duvdy.y-duvdx.y*duvdy.x;
            if (abs(det)>1e-8f) {
                tangentDir=(dpdx*duvdy.y-dpdy*duvdx.y)/det;
            } else {
                tangentDir=cross(geometricN,float3(0.0f,0.0f,1.0f));
                if (dot(tangentDir,tangentDir)<1e-8f)
                    tangentDir=cross(geometricN,float3(0.0f,1.0f,0.0f));
            }
        }
        tangentDir=normalize(tangentDir);
        // A mirrored instance reverses tangent handedness; multiply by the
        // instance determinant sign so the reconstructed bitangent matches the
        // transformed authored basis.
        float tangentSign=input.tangent.w*gMaterialExtras.y;
        bitangentDir=normalize(cross(geometricN,tangentDir))*tangentSign;
    }
    if (!clay && (maps&4)) {
        SlotUv normalUv=ResolveSlotUv(2,input.uv);
        if (normalUv.valid) {
            float3 sampled=SampleSlot(gNormal,normalUv).xyz*2-1;
            // normalTexture.scale multiplies the tangent-space XY only.
            sampled.xy*=gMaterialExtras.x;
            n=normalize(sampled.x*tangentDir+sampled.y*bitangentDir+sampled.z*geometricN);
        }
    }
    float3 coatN=geometricN;
    if (!clay && (maps&128)) {
        SlotUv coatNormalUv=ResolveSlotUv(7,input.uv);
        if (coatNormalUv.valid) {
            float3 sampled=SampleSlot(gClearcoatNormal,coatNormalUv).xyz*2-1;
            sampled.xy*=gMaterialExtras.z;
            coatN=normalize(sampled.x*tangentDir+sampled.y*bitangentDir+sampled.z*geometricN);
        }
    }
    float3 viewDir = normalize(gEyeSelection.xyz - input.worldPosition);
    float metallic=clay?0.0f:gMaterialFactors.x, roughness=clay?.82f:gMaterialFactors.y;
    // ORM convention leaves the metallic-roughness red channel unused. AO is
    // sampled only from the independent occlusion slot and scaled by the
    // authored occlusionStrength; it darkens indirect diffuse/environment
    // light only, so direct-light specular stays unoccluded.
    float occlusion=1.0f;
    if (!clay && (maps&2)) {
        SlotUv mrUv=ResolveSlotUv(1,input.uv);
        if (mrUv.valid) {
            float4 mr=SampleSlot(gMetallicRoughness,mrUv);
            metallic*=mr.b; roughness*=mr.g;
        }
    }
    if (!clay && (maps&16)) {
        SlotUv aoUv=ResolveSlotUv(4,input.uv);
        if (aoUv.valid) occlusion=OcclusionFactor(SampleSlot(gOcclusion,aoUv).r,gMaterialLobes0.z);
    }
    roughness=clamp(roughness,.045f,1.0f);
    // T18/T24 PBR lobes. Clearcoat is a second dielectric GGX layer. The base
    // is mixed toward it by clearcoatFactor * F_schlick(coatF0, 1, NoV), which
    // is the Khronos energy-consistent layering: no implicit (1-clearcoat)
    // removal of the base F0. Sheen is a grazing-only velvet term. Clay
    // suppresses both so it stays a geometry-review mode.
    float clearcoat=clay?0.0f:saturate(gMaterialLobes0.x);
    float clearcoatRoughness=gMaterialLobes0.y;
    float3 sheenColor=clay?float3(0.0f,0.0f,0.0f):saturate(gMaterialLobes1.rgb);
    float sheenRoughness=gMaterialLobes0.w;
    // KHR_materials_ior/specular. specularColor (with IOR) sets F0 at normal
    // incidence; specularFactor sets F90 at grazing. They stay independent at
    // normal and grazing angles as authored; each extension map multiplies its
    // own scalar factor, per the Khronos channel definitions.
    float specularWeight=saturate(gMaterialLobes1.w);
    float3 specularColor=saturate(gMaterialLobes2.yzw);
    // KHR_materials_transmission. Coverage (base.a) and optical transmission
    // stay independent inputs: coverage is the authored alpha for BLEND and
    // opaque coverage otherwise; transmission selects how much transmitted
    // scene radiance replaces the surface's diffuse response. The interface
    // Fresnel is applied once, to both the reflection and the transmitted
    // weight, never folded into coverage.
    float authoredTransmission=transmissiveFlag?clamp(gUvLegacy2.w,0.0f,1.0f):0.0f;
    float transmission=authoredTransmission;
    if (!clay) {
        if ((maps&32)!=0u) {
            SlotUv uv=ResolveSlotUv(5,input.uv);
            if (uv.valid) clearcoat*=SampleSlot(gClearcoat,uv).r;
        }
        if ((maps&64)!=0u) {
            SlotUv uv=ResolveSlotUv(6,input.uv);
            if (uv.valid) clearcoatRoughness*=SampleSlot(gClearcoatRoughness,uv).g;
        }
        if ((maps&256)!=0u) {
            SlotUv uv=ResolveSlotUv(8,input.uv);
            if (uv.valid) specularWeight*=SampleSlot(gSpecular,uv).a;
        }
        if ((maps&512)!=0u) {
            SlotUv uv=ResolveSlotUv(9,input.uv);
            if (uv.valid) specularColor*=SampleSlot(gSpecularColor,uv).rgb;
        }
        if ((maps&1024)!=0u) {
            SlotUv uv=ResolveSlotUv(10,input.uv);
            if (uv.valid) sheenColor*=SampleSlot(gSheenColor,uv).rgb;
        }
        if ((maps&2048)!=0u) {
            SlotUv uv=ResolveSlotUv(11,input.uv);
            if (uv.valid) sheenRoughness*=SampleSlot(gSheenRoughness,uv).a;
        }
        if ((maps&4096)!=0u) {
            SlotUv uv=ResolveSlotUv(12,input.uv);
            if (uv.valid) transmission*=SampleSlot(gTransmission,uv).r;
        }
    }
    clearcoat=saturate(clearcoat);
    clearcoatRoughness=clamp(clearcoatRoughness,.045f,1.0f);
    sheenColor=saturate(sheenColor);
    sheenRoughness=clamp(sheenRoughness,0.0f,1.0f);
    specularWeight=saturate(specularWeight);
    specularColor=saturate(specularColor);
    authoredTransmission=saturate(transmission);
    transmission=transmissive?authoredTransmission:0.0f;
    float iorF0=IorToF0(gMaterialLobes2.x);
    float3 dielectricF0=saturate(iorF0*specularColor);
    // Clay has no specular lobe: a neutral matte BRDF keeps form readability
    // independent of authored metal/specular/coat values and avoids washed-out
    // highlights on small curved geometry.
    float3 f0=clay?float3(0.0f,0.0f,0.0f):lerp(dielectricF0,base.rgb,metallic);
    float3 f90=clay?float3(0.0f,0.0f,0.0f)
        :lerp(specularWeight.xxx,float3(1.0f,1.0f,1.0f),metallic);
    float nv=saturate(dot(n,viewDir));
    float coatNv=saturate(dot(coatN,viewDir));
    float diffuseScale=1.0f-transmission;
    // The transmitted share is attenuated by the IOR-only dielectric interface
    // Fresnel, independent of coverage and of the specularColor tint applied to
    // reflection. An IOR at or below 1 has no optical interface: it neither
    // reflects nor attenuates transmission.
    float interfaceF0=IorToF0(gMaterialLobes2.x);
    float glassReflectance=interfaceF0>0.0f
        ? lerp(interfaceF0,1.0f,pow(1.0f-nv,5.0f)) : 0.0f;
    float3 worldN=RotateWorldFromCamera(n);
    float3 worldView=RotateWorldFromCamera(viewDir);
    float envIntensity=gEnvironment.x;
    // One Fresnel-weighted IBL path shared by Studio, Clay and Directional.
    // Split-sum DFG weight, then the Khronos dielectric/metal mix. Occlusion
    // darkens the environment response only; direct light stays unoccluded.
    float specularEnvironmentScale=clay?0.0f:1.0f;
    float3 diffuseEnvironment=base.rgb*EvaluateStudioIrradiance(worldN)*envIntensity*diffuseScale*occlusion;
    float3 specularEnvironment=SampleStudioSpecular(worldN,worldView,roughness)
        *envIntensity*specularEnvironmentScale;
    float3 dielectricIblFresnel=EnvironmentBrdfFresnel(dielectricF0,nv,roughness,specularWeight);
    float3 metalIblFresnel=EnvironmentBrdfFresnel(base.rgb,nv,roughness,1.0f);
    float3 envBrdfScale=lerp(dielectricIblFresnel,metalIblFresnel,metallic);
    float3 dielectricDiffuse=diffuseEnvironment*(1.0f-dielectricIblFresnel);
    float3 dielectricSpecular=specularEnvironment*dielectricIblFresnel;
    float3 metalSpecular=specularEnvironment*metalIblFresnel;
    float3 iblDiffuse=lerp(dielectricDiffuse,float3(0,0,0),metallic);
    float3 iblSpecular=lerp(dielectricSpecular,metalSpecular,metallic);
    // T27 bounded scene-color transmission in linear HDR. The fragment samples
    // the opaque scene copy at its own screen position through a roughness-
    // selected mip, so rough glass filters the transmitted radiance. When the
    // copy has no opaque surface at that pixel (alpha 0) the studio environment
    // in the transmitted direction is the documented fallback. There is no
    // refraction offset, no inter-layer transmission and no invented volume
    // thickness: this is a thin-surface approximation.
    float3 transmittedRadiance=float3(0,0,0);
    float3 transmissionSample=float3(0,0,0);
    float transmissionOpaqueCoverage=0.0f;
    float transmissionWeight=0.0f;
    float3 environmentTransmission=float3(0,0,0);
    if (transmissive && transmission>0.0f) {
        float2 transmissionUv=input.position.xy*gTransmissionFrame.xy;
        float transmissionMip=gTransmissionFrame.z*roughness;
        float4 sampled=gTransmissionSource.SampleLevel(gLinearSampler,transmissionUv,
            transmissionMip);
        transmissionSample=sampled.rgb;
        environmentTransmission=gStudioEnvironment.SampleLevel(gEnvironmentSampler,
            EnvironmentDirection(-worldView),roughness*gEnvironment.w).rgb*envIntensity;
        transmissionOpaqueCoverage=saturate(sampled.a);
        float3 backgroundRadiance=lerp(environmentTransmission,transmissionSample,
            transmissionOpaqueCoverage);
        transmissionWeight=transmission*(1.0f-saturate(glassReflectance));
        transmittedRadiance=backgroundRadiance*base.rgb*transmissionWeight;
    }
    // Coat IBL is the same prefiltered environment at the coat roughness. It
    // is weighted once by the layer mix below; it must not carry a second
    // Fresnel factor.
    float coatFresnel=FresnelSchlick(clamp(iorF0,0.0f,1.0f),1.0f,coatNv);
    float coatWeight=clearcoat*coatFresnel;
    float3 coatWorldN=RotateWorldFromCamera(coatN);
    float3 coatEnvironment=SampleStudioSpecular(coatWorldN,worldView,clearcoatRoughness)
        *envIntensity*occlusion;
    // T21/T24 diagnostic contributions. Every split term is accumulated with
    // exactly the arithmetic that reaches `color`, so channel 20 remains the
    // sum of channels 14..19 while exposing the individual lobes.
    float3 directDiffuse=float3(0,0,0),directSpecular=float3(0,0,0);
    float3 envDiffuse=float3(0,0,0),envSpecular=float3(0,0,0);
    float3 coatContribution=float3(0,0,0),emission=float3(0,0,0);
    // T32: the only portion screen AO is allowed to touch. It is accumulated
    // with exactly the arithmetic that reaches `color`, so the resolve's
    // subtraction cannot leak into direct light, emission or transmission.
    float3 indirectContribution=float3(0,0,0);
    float3 baseDirect=float3(0,0,0),coatDirect=float3(0,0,0);
    float3 dd=float3(0,0,0),ds=float3(0,0,0);
    float3 sheen=float3(0,0,0);
    float sheenWeight=0.0f;
    if (!clay && any(sheenColor>0.0f)) {
        sheenWeight=pow(1.0f-nv,3.0f)*(0.35f+0.65f*sheenRoughness);
        // T30: sheen is an environment-side term in this renderer. It scales
        // with the same environment intensity as the IBL and the direct rig,
        // so intensity zero removes it instead of leaving a flat ambient lift.
        sheen=sheenColor*sheenWeight*envIntensity;
    }
    bool unlitMaterial=false;
    if (!clay && (flags&2)) {
        // glTF unlit: authored base color only, no environment or coat.
        unlitMaterial=true;
        iblDiffuse=base.rgb*diffuseScale;
        iblSpecular=float3(0,0,0);
        sheen=float3(0,0,0);
        coatEnvironment=float3(0,0,0);
        coatWeight=0.0f;
    } else if (gLighting.x>1.5f) {
        // Directional: one coherent sun for base and coat. gSunDirection is
        // the documented world-space unit vector and the shading vectors are
        // already in that same world frame (camera-relative origin only), so
        // no extra rotation is applied here. The IBL above is identical to
        // Studio, so both modes share one material BRDF and one ambient
        // reflection floor.
        float3 lightDir=normalize(gSunDirection.xyz);
        float3 d,s;
        baseDirect=EvaluateLightSplit(n,viewDir,lightDir,base.rgb*diffuseScale,metallic,
            roughness,f0,f90,3.4f*envIntensity*gPresentation.x,d,s);
        dd+=d;ds+=s;
        coatDirect=EvaluateClearcoatLight(coatN,viewDir,lightDir,clearcoatRoughness,
            3.4f*envIntensity*gPresentation.x);
    } else {
        // World-stable Studio IBL plus the camera-relative key/fill/rim rig.
        // The base rig intensities live in StudioEnvironment.h; the CPU passes
        // them pre-multiplied by the preset/user key/fill/rim scales, so the
        // shader has no second, undocumented intensity source. Clay softens the
        // same rig (less key/rim, more fill) so cavities and curved form stay
        // readable without a hard, washed-out highlight; it does not change
        // the user's exposure or the Studio/Directional rig itself.
        float keyScale=gPresentation.x, fillScale=gPresentation.y, rimScale=gPresentation.z;
        if (clay) { keyScale*=.72f; fillScale*=1.18f; rimScale*=.55f; }
        float3 d0,s0,d1,s1,d2,s2;
        float3 key=EvaluateLightSplit(n,viewDir,normalize(float3(.45f,-.55f,.70f)),
            base.rgb*diffuseScale,metallic,roughness,f0,f90,envIntensity*keyScale,d0,s0);
        float3 fill=EvaluateLightSplit(n,viewDir,normalize(float3(-.70f,.25f,.38f)),
            base.rgb*diffuseScale,metallic,roughness,f0,f90,envIntensity*fillScale,d1,s1);
        float3 rim=EvaluateLightSplit(n,viewDir,normalize(float3(.18f,.76f,.28f)),
            base.rgb*diffuseScale,metallic,roughness,f0,f90,envIntensity*rimScale,d2,s2);
        baseDirect=key+fill+rim;
        dd=d0+d1+d2;ds=s0+s1+s2;
        // Coat direct lights use the same Studio directions as the base rig.
        coatDirect=EvaluateClearcoatLight(coatN,viewDir,normalize(float3(.45f,-.55f,.70f)),
                clearcoatRoughness,envIntensity*gPresentation.x)
            +EvaluateClearcoatLight(coatN,viewDir,normalize(float3(-.70f,.25f,.38f)),
                clearcoatRoughness,envIntensity*gPresentation.y)
            +EvaluateClearcoatLight(coatN,viewDir,normalize(float3(.18f,.76f,.28f)),
                clearcoatRoughness,envIntensity*gPresentation.z);
    }
    // Clearcoat layering: base (diffuse+specular+sheen) is attenuated by
    // (1 - coatWeight) and the coat lobe fills that energy. Emission beneath
    // the coat is attenuated the same way.
    float attenuation=1.0f-coatWeight;
    float3 coatTotal=coatEnvironment+coatDirect;
    directDiffuse=dd*attenuation;
    directSpecular=ds*attenuation;
    envDiffuse=iblDiffuse*attenuation;
    envSpecular=(iblSpecular+sheen)*attenuation;
    coatContribution=coatTotal*coatWeight;
    float3 emissive=float3(0,0,0);
    if (!clay) {
        emissive=gEmissiveFactor.rgb*gEmissiveFactor.a;
        if (maps&8) {
            SlotUv emissiveUv=ResolveSlotUv(3,input.uv);
            if (emissiveUv.valid) emissive*=SampleSlot(gEmissive,emissiveUv).rgb;
        }
    }
    emission=emissive*attenuation;
    // T32 indirect share: environment diffuse + environment specular/sheen +
    // the coat's environment lobe. Coat direct light, base direct light,
    // emission, selection outline and transmitted radiance are excluded.
    indirectContribution=unlitMaterial ? float3(0.0f,0.0f,0.0f)
        : envDiffuse+envSpecular+coatEnvironment*coatWeight;
    // Transmitted radiance sits beneath the coat like the base lobes; the
    // interface Fresnel already removed the reflected share.
    float3 transmittedContribution=transmittedRadiance*attenuation;
    float3 color=directDiffuse+directSpecular+envDiffuse+envSpecular+coatContribution
        +emission+transmittedContribution;
    float outline = pow(1.0f - saturate(dot(n,viewDir)), 2.0f);
    color += gEyeSelection.w * (outline * 0.45f * float3(0.36f,0.62f,1.0f) + 0.03f);
    // T21 Debug inspection channels travel in gPresentation.w. They are raw
    // linear values; the display resolve performs the only clamp. Unsupported
    // channels return a deliberate magenta marker rather than a neutral value.
    uint debugChannel=(uint)gPresentation.w;
    if (debugChannel!=0u) {
        float3 dbg;
        const float3 unsupported=float3(1.0f,0.0f,1.0f);
        if (debugChannel==1u) dbg=base.rgb;
        else if (debugChannel==2u) dbg=normalize(input.normal)*0.5f+0.5f;
        else if (debugChannel==3u) dbg=n*0.5f+0.5f;
        else if (debugChannel==4u) dbg=float3(TransformSlotUvUnwrapped(0,input.uv),0.0f);
        else if (debugChannel==5u) dbg=unsupported; // UV1 is not on the wire
        else if (debugChannel==6u) dbg=metallic.xxx;
        else if (debugChannel==7u) dbg=roughness.xxx;
        else if (debugChannel==8u) dbg=occlusion.xxx;
        else if (debugChannel==9u) dbg=effectiveCoverage.xxx;
        else if (debugChannel==10u) dbg=transmission.xxx;
        else if (debugChannel==11u) dbg=float3(gMaterialLobes2.x*0.25f,iorF0,0.0f);
        else if (debugChannel==12u) dbg=clearcoat.xxx;
        else if (debugChannel==13u) dbg=sheenColor*sheenWeight;
        else if (debugChannel==14u) dbg=directDiffuse;
        else if (debugChannel==15u) dbg=directSpecular;
        else if (debugChannel==16u) dbg=envDiffuse;
        else if (debugChannel==17u) dbg=envSpecular;
        else if (debugChannel==18u) dbg=coatContribution;
        else if (debugChannel==19u) dbg=emission;
        else if (debugChannel==20u) dbg=color;
        else if (debugChannel==21u) dbg=envBrdfScale;
        else if (debugChannel==22u) dbg=float3(base.a,glassReflectance,authoredTransmission);
        // Fixed linear patch used to check the resolve's tone-map/display
        // contract without depending on scene lighting or sampling.
        else if (debugChannel==23u) dbg=float3(0.3f,0.3f,0.3f);
        // T25 effective extension-lobe inputs, after map x factor. Channel 24
        // packs clearcoat factor and roughness; 25 is the specular grazing
        // weight; 26 the IOR/specularColor F0; 27 the sheen color; 28 the
        // transmission factor; 29 the coat normal; 30 the sheen roughness.
        else if (debugChannel==24u) dbg=float3(clearcoat,clearcoatRoughness,0.0f);
        else if (debugChannel==25u) dbg=specularWeight.xxx;
        else if (debugChannel==26u) dbg=dielectricF0;
        else if (debugChannel==27u) dbg=sheenColor;
        else if (debugChannel==28u) dbg=authoredTransmission.xxx;
        else if (debugChannel==29u) dbg=coatN*0.5f+0.5f;
        else if (debugChannel==30u) dbg=sheenRoughness.xxx;
        // T27 transmission terms: 31 is the final tinted/weighted transmitted
        // radiance and 32 the raw (untinted, unweighted) scene sample, so tint
        // and coverage can be checked independently of lighting.
        else if (debugChannel==31u) dbg=transmittedRadiance;
        else if (debugChannel==32u) dbg=transmissionSample;
        else dbg=float3(0.0f,1.0f,1.0f);
#ifdef OIT_PASS
        OitOutput debugOutput; debugOutput.accum=float4(max(dbg,0.0f),1.0f);
        debugOutput.reveal=float4(0.0f,0.0f,0.0f,0.0f); return debugOutput;
#else
        PixelOutput debugOutput; debugOutput.color=float4(max(dbg,0.0f),1.0f);
#ifdef AO_INDIRECT
        debugOutput.indirect=float4(0.0f,0.0f,0.0f,0.0f);
#endif
        debugOutput.pick=input.pickId; return debugOutput;
#endif
    }
    // Linear HDR output. T15's resolve pass owns exposure, tone mapping and the
    // sRGB transfer so transparency, transmission and emission all blend in the
    // same color space before the display transform.
#ifdef OIT_PASS
    // One order-independent accumulation record per transparent fragment.
    // Coverage (base.a) is kept separate from the lit colour: the revealage
    // product is the coverage input, while reflection and transmission remain
    // distinct material terms inside `color` for T27's transmitted-scene pass.
    // Coverage is the authored alpha only for BLEND; OPAQUE and MASK geometry
    // covers the pixel completely even when it is optically transmissive. The
    // interface Fresnel is never written into coverage.
    float coverage=effectiveCoverage;
    float weight=OitWeight(length(input.worldPosition));
    OitOutput output;
    output.accum=float4(max(color,0.0f)*coverage*weight,coverage*weight);
    output.reveal=float4(coverage,0.0f,0.0f,coverage);
    return output;
#else
    // Only BLEND materials carry coverage here; OPAQUE/MASK writes opaque
    // alpha after the authored cutout so the resolve cannot leak background.
    float opacity=effectiveCoverage;
    PixelOutput output; output.color = float4(max(color,0.0f), opacity);
#ifdef AO_INDIRECT
    // The indirect target stores camera-space linear HDR, never coverage: AO
    // must not alter the resolve's alpha/background composite.
    output.indirect = float4(max(indirectContribution,0.0f),1.0f);
#endif
    output.pick = input.pickId; return output;
#endif
}
)";

// The grid is generated from SV_VertexID, so it needs no persistent vertex
// buffer or upload. Draw constants carry its camera-relative center, ground
// height, and span; this preserves the large-coordinate precision policy used
// by model draws while keeping the grid in the viewer's fixed Z-up world.
// LineList uses the same bounded vertex payload as meshes but only the
// position is needed. A dedicated depth-biased PSO keeps authored curves and
// wireframe-like line geometry visible on coplanar surfaces; the pick target
// still receives the identity so a line can be selected.
constexpr char kLineVertexShaderSource[] = R"(
cbuffer FrameConstants : register(b0)
{
    row_major float4x4 gViewProjection;
    float4 gEyeSelection;
    float4 gViewport;
    float4 gLighting;
};
cbuffer DrawConstants : register(b1)
{
    row_major float4x4 gLocalToCamera;
    row_major float4x4 gNormalToCamera;
    uint gPickId;
};
struct VSInput { float3 position : POSITION; };
struct PSInput { float4 position : SV_POSITION; nointerpolation uint pickId : TEXCOORD7; };
PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = mul(mul(float4(input.position, 1.0f), gLocalToCamera), gViewProjection);
    output.pickId = gPickId;
    return output;
}
)";

constexpr char kLinePixelShaderSource[] = R"(
struct PSInput { float4 position : SV_POSITION; nointerpolation uint pickId : TEXCOORD7; };
struct PixelOutput { float4 color : SV_TARGET0; uint pick : SV_TARGET1; };
PixelOutput PSMain(PSInput input)
{
    PixelOutput output;
    // Stable cool grey-blue, matching the wireframe overlay family.
    output.color = float4(0.62f, 0.74f, 0.92f, 1.0f);
    output.pick = input.pickId;
    return output;
}
)";

constexpr char kGridVertexShaderSource[] = R"(
cbuffer FrameConstants : register(b0)
{
    row_major float4x4 gViewProjection;
    float4 gEyeSelection;
    float4 gViewport;
};
cbuffer GridConstants : register(b1)
{
    float4 gGrid; // center x, center y, ground z (camera relative), span
};
struct GridOutput { float4 position : SV_POSITION; float4 color : COLOR0; };
GridOutput VSMain(uint vertexId : SV_VertexID)
{
    const uint divisions = 20;
    uint lineIndex = vertexId / 4;
    uint endpoint = vertexId % 4;
    float offset = lerp(-gGrid.w, gGrid.w, (float)lineIndex / (float)divisions);
    float3 position;
    if (endpoint < 2)
        position = float3(gGrid.x + (endpoint == 0 ? -gGrid.w : gGrid.w), gGrid.y + offset, gGrid.z);
    else
        position = float3(gGrid.x + offset, gGrid.y + (endpoint == 2 ? -gGrid.w : gGrid.w), gGrid.z);
    bool major = lineIndex == divisions / 2 || lineIndex % 5 == 0;
    GridOutput output;
    output.position = mul(float4(position, 1), gViewProjection);
    output.color = major ? float4(.22, .25, .30, .52) : float4(.16, .18, .22, .36);
    return output;
}
)";

constexpr char kGridPixelShaderSource[] = R"(
struct GridInput { float4 position : SV_POSITION; float4 color : COLOR0; };
struct GridOutput { float4 color : SV_TARGET0; uint pick : SV_TARGET1; };
GridOutput PSMain(GridInput input)
{
    GridOutput output;
    output.color = input.color;
    output.pick = 0;
    return output;
}
)";

// T16 ground contact shadow. The plane and its analytic soft ellipse are drawn
// as their own layer: hiding the grid cannot hide the shadow, and hiding the
// shadow leaves the grid untouched. The vertex shader generates a camera-
// relative quad from SV_VertexID; the pixel shader mirrors GroundShadow.cpp's
// ShadowMask so the CPU reference tests and the GPU image cannot drift.
constexpr char kGroundVertexShaderSource[] = R"(
cbuffer FrameConstants : register(b0)
{
    row_major float4x4 gViewProjection;
    float4 gEyeSelection;
    float4 gViewport;
};
cbuffer GroundConstants : register(b1)
{
    float4 gGround;       // center.xy, plane z, half size
    float4 gShadow;       // footprint center.xy, radii.xy
    float4 gShadowParams; // opacity, softness, highlight strength, shadow enabled
    float4 gGroundColor;  // linear rgb, center alpha
};
struct GroundOutput { float4 position : SV_POSITION; float3 worldPosition : TEXCOORD0; };
GroundOutput VSMain(uint vertexId : SV_VertexID)
{
    float2 corner = float2(
        (vertexId == 1 || vertexId == 4 || vertexId == 5) ? 1.0f : -1.0f,
        (vertexId == 2 || vertexId == 3 || vertexId == 5) ? 1.0f : -1.0f);
    float3 position = float3(gGround.xy + corner * gGround.w, gGround.z);
    GroundOutput output;
    output.position = mul(float4(position, 1.0f), gViewProjection);
    output.worldPosition = position;
    return output;
}
)";

constexpr char kGroundPixelShaderSource[] = R"(
cbuffer GroundConstants : register(b1)
{
    float4 gGround;
    float4 gShadow;
    float4 gShadowParams;
    float4 gGroundColor;
};
struct GroundInput { float4 position : SV_POSITION; float3 worldPosition : TEXCOORD0; };
struct GroundOutput { float4 color : SV_TARGET0; uint pick : SV_TARGET1; };
GroundOutput PSMain(GroundInput input)
{
    float2 p = input.worldPosition.xy;
    float2 shadowDelta = (p - gShadow.xy) / max(gShadow.zw, float2(1e-6f, 1e-6f));
    float distanceToShadow = length(shadowDelta);
    float softness = clamp(gShadowParams.y, 0.001f, 1.0f);
    float shadow = (1.0f - smoothstep(1.0f - softness, 1.0f, distanceToShadow)) * gShadowParams.x;
    // T21a: gShadowParams.w gates the contact shadow independently of the
    // illuminated area, so turning shadows off does not remove the highlight.
    shadow *= saturate(gShadowParams.w);
    // The illuminated area is a separate presentation layer. gShadowParams.z
    // is 0 when the user turned the highlight off; the shadow still applies.
    float highlight = saturate(gShadowParams.z);
    float2 groundDelta = (p - gGround.xy) / max(gGround.w, 1e-6f);
    float groundDistance = length(groundDelta);
    float fade = 1.0f - smoothstep(0.72f, 1.0f, groundDistance);
    float3 ground = gGroundColor.rgb * (0.88f + 0.12f * (1.0f - saturate(groundDistance)));
    // Highlight off leaves only the contact shadow (black over the backdrop).
    float3 color = ground * highlight;
    color *= (1.0f - 0.95f * saturate(shadow));
    float alpha = saturate(max(shadow, highlight * fade * gGroundColor.a));
    GroundOutput output;
    // Premultiplied over the display-space backdrop target.
    output.color = float4(max(color * alpha, 0.0f), alpha);
    output.pick = 0;
    return output;
}
)";

// T31 geometry-derived contact shadow. The caster pass writes only depth (plus
// one R8 attenuation value for the nearest caster), so transparent geometry
// never contributes colour and clear glass cannot paint an opaque silhouette.
// Its root constants are the world-from-model matrix (b1), the fitted light
// view-projection and caster attenuation (b3), and the material record (b2).
constexpr char kShadowVertexShaderSource[] = R"(
cbuffer ShadowDrawConstants : register(b1)
{
    row_major float4x4 gLocalToWorld;
};
cbuffer ShadowFrameConstants : register(b3)
{
    row_major float4x4 gShadowViewProjection;
    float4 gShadowCaster; // x = approximate transmission attenuation
};
struct ShadowVSInput { float3 position : POSITION; float2 uv : TEXCOORD0; };
struct ShadowVSOutput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
ShadowVSOutput VSMain(ShadowVSInput input)
{
    ShadowVSOutput output;
    float4 world = mul(float4(input.position, 1.0f), gLocalToWorld);
    output.position = mul(world, gShadowViewProjection);
    output.uv = input.uv;
    return output;
}
)";

constexpr char kShadowOpaqueVertexShaderSource[] = R"(
cbuffer ShadowDrawConstants : register(b1)
{
    row_major float4x4 gLocalToWorld;
};
cbuffer ShadowFrameConstants : register(b3)
{
    row_major float4x4 gShadowViewProjection;
    float4 gShadowCaster;
};
struct ShadowVSInput { float3 position : POSITION; };
struct ShadowVSOutput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
ShadowVSOutput VSMain(ShadowVSInput input)
{
    ShadowVSOutput output;
    float4 world = mul(float4(input.position, 1.0f), gLocalToWorld);
    output.position = mul(world, gShadowViewProjection);
    output.uv = float2(0.0f, 0.0f);
    return output;
}
)";

constexpr char kShadowPixelShaderSource[] = R"(
cbuffer ShadowFrameConstants : register(b3)
{
    row_major float4x4 gShadowViewProjection;
    float4 gShadowCaster;
};
struct ShadowPSInput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
float4 PSMain(ShadowPSInput input) : SV_TARGET
{
    // The mask target records how strongly this caster darkens the ground:
    // 1 for opaque geometry, the bounded transmission attenuation for glass.
    return float4(gShadowCaster.x, 0.0f, 0.0f, 1.0f);
}
)";

constexpr char kShadowMaskPixelShaderSource[] = R"(
cbuffer ShadowFrameConstants : register(b3)
{
    row_major float4x4 gShadowViewProjection;
    float4 gShadowCaster;
};
cbuffer ShadowMaterial : register(b2)
{
    float4 gBaseColorFactor;
    float4 gMaterialFactors; // metallic, roughness, alpha cutoff, flags
    float4 gUvLegacy;        // base-colour offset.xy, scale.xy
};
Texture2D gBaseColor : register(t0);
SamplerState gLinearSampler : register(s0);
struct ShadowPSInput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
float4 PSMain(ShadowPSInput input) : SV_TARGET
{
    float2 uv = input.uv * gUvLegacy.zw + gUvLegacy.xy;
    float alpha = gBaseColor.SampleLevel(gLinearSampler, uv, 0).a * gBaseColorFactor.a;
    // Preserve authored MASK holes: the shadow must not fill a strap loop or a
    // perforated sheet that the visible surface cuts away.
    clip(alpha - gMaterialFactors.z);
    return float4(gShadowCaster.x, 0.0f, 0.0f, 1.0f);
}
)";

constexpr char kShadowGroundPixelShaderSource[] = R"(
cbuffer ShadowGroundConstants : register(b1)
{
    float4 gGround;       // center.xy, plane z, half size (camera relative)
    float4 gShadowParams; // opacity, pcf radius in texels, enabled
    float4 gShadowTexel;  // 1/width, 1/height, normalized depth bias
    row_major float4x4 gShadowMatrix; // camera-relative -> light clip
};
Texture2D<float> gShadowDepth : register(t0);
Texture2D<float> gShadowMask : register(t1);
SamplerComparisonState gShadowCompare : register(s0);
SamplerState gShadowPoint : register(s1);
struct GroundInput { float4 position : SV_POSITION; float3 worldPosition : TEXCOORD0; };
struct GroundOutput { float4 color : SV_TARGET0; uint pick : SV_TARGET1; };
GroundOutput PSMain(GroundInput input)
{
    GroundOutput output;
    float4 clip = mul(float4(input.worldPosition, 1.0f), gShadowMatrix);
    float2 uv = clip.xy * float2(0.5f, -0.5f) + 0.5f;
    bool inside = all(uv >= 0.0f) && all(uv <= 1.0f) && clip.w > 0.0f;
    float visibility = 1.0f;
    float mask = 0.0f;
    if (inside && gShadowParams.z > 0.5f) {
        float reference = clip.z - gShadowTexel.z;
        float sum = 0.0f;
        [unroll] for (int y = -1; y <= 1; ++y) {
            [unroll] for (int x = -1; x <= 1; ++x) {
                float2 offset = float2(x, y) * gShadowTexel.xy * gShadowParams.y;
                sum += gShadowDepth.SampleCmpLevelZero(gShadowCompare, uv + offset, reference);
            }
        }
        visibility = sum / 9.0f;
        float2 clampedUv = saturate(uv);
        mask = gShadowMask.SampleLevel(gShadowPoint, clampedUv, 0).r;
    }
    float alpha = saturate((1.0f - visibility) * mask * gShadowParams.x);
    // Premultiplied coverage over the linear HDR scene. The resolve composites
    // this black coverage over the display backdrop, so exposure/tone-map
    // changes cannot invert the contact cue.
    output.color = float4(0.0f, 0.0f, 0.0f, alpha);
    output.pick = 0;
    return output;
}
)";


constexpr char kPointVertexShaderSource[] = R"(
cbuffer FrameConstants:register(b0){row_major float4x4 gViewProjection;float4 gEyeSelection;float4 gViewport;float4 gLighting;};
cbuffer DrawConstants:register(b1){row_major float4x4 gLocalToCamera;row_major float4x4 gNormalToCamera;uint gPickId;};
struct V{float3 p:POSITION;}; struct O{float4 p:SV_POSITION;float4 c:COLOR0;nointerpolation uint id:TEXCOORD7;};
O VSMain(V v){O o;o.p=mul(mul(float4(v.p,1),gLocalToCamera),gViewProjection);o.c=float4(.76,.78,.84,1);o.id=gPickId;return o;}
)";
constexpr char kColoredPointVertexShaderSource[] = R"(
cbuffer FrameConstants:register(b0){row_major float4x4 gViewProjection;float4 gEyeSelection;float4 gViewport;float4 gLighting;};
cbuffer DrawConstants:register(b1){row_major float4x4 gLocalToCamera;row_major float4x4 gNormalToCamera;uint gPickId;};
struct V{float3 p:POSITION;float3 n:NORMAL;float2 uv:TEXCOORD0;float4 t:TANGENT;float4 c:COLOR0;};
struct O{float4 p:SV_POSITION;float4 c:COLOR0;nointerpolation uint id:TEXCOORD7;};
O VSMain(V v){O o;o.p=mul(mul(float4(v.p,1),gLocalToCamera),gViewProjection);o.c=v.c;o.id=gPickId;return o;}
)";
constexpr char kPointGeometryShaderSource[] = R"(
cbuffer FrameConstants:register(b0){row_major float4x4 gViewProjection;float4 gEyeSelection;float4 gViewport;float4 gLighting;};
struct I{float4 p:SV_POSITION;float4 c:COLOR0;nointerpolation uint id:TEXCOORD7;}; struct O{float4 p:SV_POSITION;float4 c:COLOR0;float2 q:TEXCOORD0;nointerpolation uint id:TEXCOORD7;};
[maxvertexcount(4)] void GSMain(point I i[1],inout TriangleStream<O> s){
 float radius=clamp(7.0/sqrt(max(.25,abs(i[0].p.w))),2.0,12.0);float2 clip=2.0*radius/max(gViewport.xy,float2(1,1));
 float2 q[4]={float2(-1,-1),float2(-1,1),float2(1,-1),float2(1,1)};
 [unroll]for(uint k=0;k<4;k++){O o;o.p=i[0].p;o.p.xy+=q[k]*clip*i[0].p.w;o.c=i[0].c;o.q=q[k];o.id=i[0].id;s.Append(o);} }
)";
constexpr char kPointPixelShaderSource[] = R"(
struct I{float4 p:SV_POSITION;float4 c:COLOR0;float2 q:TEXCOORD0;nointerpolation uint id:TEXCOORD7;};struct O{float4 c:SV_TARGET0;uint pick:SV_TARGET1;};
O PSMain(I i){if(dot(i.q,i.q)>1)discard;O o;o.c=i.c;o.pick=i.id;return o;}
)";

// T15 full-screen resolve. Reads the linear HDR scene target, composites the
// background in the same color space, applies exposure and the selected tone
// curve, then writes sRGB into the UNORM back buffer. The D2D/D3D11On12
// overlay still runs after this pass.
constexpr char kResolveVertexShaderSource[] = R"(
struct ResolveOutput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
ResolveOutput VSMain(uint vertexId : SV_VertexID)
{
    float2 p = float2((vertexId << 1) & 2, vertexId & 2);
    ResolveOutput output;
    output.uv = p;
    output.position = float4(p * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}
)";

constexpr char kResolvePixelShaderSource[] = R"(
cbuffer ResolveConstants : register(b0)
{
    float4 gResolve;    // exposure, tone-map mode, background mode, checker size
    float4 gBackground; // linear background rgb (transparent/checker ignore rgb)
    // T32: x = AO mask applied this frame, y = effective strength,
    // z/w = viewport pixels (diagnostic evidence only).
    float4 gAo;
    // T33: x = edge AA enabled, y = tap count, z/w = inverse viewport texel.
    float4 gResolve2;
};
Texture2D<float4> gSceneColor : register(t0);
Texture2D<float4> gBackdrop : register(t1);
// T26 bounded weighted blended OIT: premultiplied weighted colour and the
// revealage product. gBackground.w is 1 when this frame used OIT and 0 when it
// used the recorded sorted fallback.
Texture2D<float4> gOitAccum : register(t2);
Texture2D<float4> gOitReveal : register(t3);
// T32 indirect-only AO: the opaque material pass writes its environment/IBL
// share separately and the depth-derived mask says how much survives. The
// resolve subtracts only that share, before tone mapping.
Texture2D<float4> gIndirect : register(t4);
Texture2D<float> gAoMask : register(t5);
// T33 single-sample D32 depth. The edge filter reads it; it never becomes a
// render target here and the pick target is not involved.
Texture2D<float> gDepth : register(t6);
SamplerState gLinearSampler : register(s0);

float3 NeutralToneMap(float3 linearColor,float exposure)
{
    float3 color=max(linearColor*exposure,0.0f);
    const float startCompression=0.8f-0.04f;
    const float desaturation=0.15f;
    float minimum=min(color.r,min(color.g,color.b));
    float offset=minimum<0.08f ? minimum-6.25f*minimum*minimum : 0.04f;
    color-=offset;
    float peak=max(color.r,max(color.g,color.b));
    if (peak>=startCompression) {
        float d=1.0f-startCompression;
        float newPeak=1.0f-d*d/(peak+d-startCompression);
        color*=newPeak/max(peak,1e-6f);
        float grey=1.0f-1.0f/(desaturation*(peak-newPeak)+1.0f);
        color=lerp(color,newPeak.xxx,grey);
    }
    return saturate(color);
}
float3 AcesToneMap(float3 color)
{
    const float a=2.51f,b=0.03f,c=2.43f,d=0.59f,e=0.14f;
    color=max(color,0.0f);
    return saturate((color*(a*color+b))/(color*(c*color+d)+e));
}
float HableCurve(float x)
{
    const float A=0.15f,B=0.50f,C=0.10f,D=0.20f,E=0.02f,F=0.30f;
    return ((x*(A*x+C*B)+D*E)/(x*(A*x+B)+D*F))-E/F;
}
float3 FilmicToneMap(float3 color)
{
    const float white=11.2f;
    float3 mapped=float3(HableCurve(max(color.r,0.0f)),HableCurve(max(color.g,0.0f)),HableCurve(max(color.b,0.0f)));
    return saturate(mapped/HableCurve(white));
}
float3 ApplyToneMap(float3 linearColor, float exposure, uint mode)
{
    if (mode==1u) return AcesToneMap(linearColor*exposure);
    if (mode==2u) return FilmicToneMap(linearColor*exposure);
    if (mode==3u) return saturate(linearColor*exposure);
    return NeutralToneMap(linearColor,exposure);
}
float3 SrgbEncode(float3 color)
{
    float3 low=color*12.92f;
    float3 high=1.055f*pow(max(color,0.0f),1.0f/2.4f)-0.055f;
    return lerp(high,low,step(color,0.0031308f));
}
float3 BackdropAt(float2 uv, float2 position)
{
    uint backgroundMode=(uint)gResolve.z;
    float3 backdrop=gBackdrop.Sample(gLinearSampler,uv).rgb;
    if (backgroundMode==3u) {
        // Explicit checkerboard choice, in display space so it is stable.
        float2 cell=floor(position.xy/max(4.0f,gResolve.w));
        float parity=fmod(cell.x+cell.y,2.0f);
        backdrop=lerp(float3(0.10f,0.10f,0.11f),float3(0.18f,0.18f,0.19f),parity);
    } else if (backgroundMode==2u) {
        backdrop=float3(0.0f,0.0f,0.0f);
    }
    return backdrop;
}

// Full product composite for one sample: HDR scene + AO + OIT + exposure +
// tone map + display-space backdrop. Kept as one function so the edge filter's
// blend taps composite exactly like the centre pixel. Diagnostic views return
// their raw inspection value and never enter the AA path.
float4 ComputeComposite(float2 uv, float2 position, uint resolveDiagnostic)
{
    float4 scene=gSceneColor.Sample(gLinearSampler,uv);
    uint backgroundMode=(uint)gResolve.z;
    // The HDR target stores straight-alpha scene color premultiplied over a
    // black clear. Recover the straight color before the display transform so
    // glass does not get darker just because its coverage is low.
    float alpha=saturate(scene.a);
    float3 sceneStraight=alpha>1e-5f ? scene.rgb/alpha : float3(0.0f,0.0f,0.0f);
    // T32: AO is a subtraction from the indirect share, never a multiplier on
    // the whole image. Direct light, emission, the selection outline and the
    // transparent composite remain untouched; the display transform runs
    // afterwards on the already-adjusted linear HDR value.
    float aoMask=1.0f;
    float3 indirectShare=float3(0.0f,0.0f,0.0f);
    if (gAo.x>0.5f) {
        aoMask=saturate(gAoMask.Sample(gLinearSampler,uv).r);
        indirectShare=max(gIndirect.Sample(gLinearSampler,uv).rgb,0.0f);
        sceneStraight=max(sceneStraight-indirectShare*(1.0f-aoMask),0.0f);
    }
    // Diagnostic AO-only views bypass the tone map so masks and indirect
    // shares can be inspected and measured directly.
    if (resolveDiagnostic==5u) return float4(aoMask.xxx,1.0f);
    if (resolveDiagnostic==6u) return float4(indirectShare,1.0f);
    if (resolveDiagnostic==7u) return float4(indirectShare*aoMask,1.0f);
    float3 combined=sceneStraight*alpha;
    float coverage=alpha;
    if (gBackground.w>0.5f) {
        // Weighted blended OIT resolve in linear HDR, before tone mapping.
        // `accum.a` is the sum of coverage*weight; the average transparent
        // colour is accum.rgb/accum.a and revealage is the product of
        // (1-coverage). The opaque scene is the background this layer covers.
        float4 accum=gOitAccum.Sample(gLinearSampler,uv);
        float reveal=saturate(gOitReveal.Sample(gLinearSampler,uv).r);
        // Debug-only inspection of the bounded OIT targets.
        if (resolveDiagnostic==1u) return float4(accum.rgb,1.0f);
        if (resolveDiagnostic==2u) return float4(reveal.xxx,1.0f);
        if (resolveDiagnostic==3u) return float4(scene.rgb,1.0f);
        if (resolveDiagnostic==4u) return float4(accum.a.xxx,1.0f);
        float transparentCoverage=1.0f-reveal;
        float3 transparentColor=accum.a>1e-5f ? accum.rgb/accum.a : float3(0.0f,0.0f,0.0f);
        combined=sceneStraight*alpha*reveal+transparentColor*transparentCoverage;
        coverage=saturate(alpha+(1.0f-alpha)*transparentCoverage);
    }
    float3 mapped=SrgbEncode(ApplyToneMap(combined,gResolve.x,(uint)gResolve.y));
    float3 backdrop=BackdropAt(uv,position);
    float4 output;
    if (backgroundMode==2u) {
        // Transparent capture: premultiplied display color and preserved
        // coverage alpha. The backdrop/ground contributes no alpha.
        output.rgb=mapped*coverage;
        output.a=coverage;
    } else {
        output.rgb=lerp(backdrop,mapped,coverage);
        output.a=1.0f;
    }
    return output;
}

// Luma/coverage-only probe for the four axial neighbours. It includes the
// display-space backdrop so a silhouette against the product background is a
// real luma edge. AO is omitted here: it is a low-frequency indirect term and
// cannot move an edge across the filter threshold.
float2 EdgeProbe(float2 uv, float2 position)
{
    float4 scene=gSceneColor.Sample(gLinearSampler,uv);
    float alpha=saturate(scene.a);
    float3 sceneStraight=alpha>1e-5f ? scene.rgb/alpha : float3(0.0f,0.0f,0.0f);
    float3 combined=sceneStraight*alpha;
    float coverage=alpha;
    if (gBackground.w>0.5f) {
        float4 accum=gOitAccum.Sample(gLinearSampler,uv);
        float reveal=saturate(gOitReveal.Sample(gLinearSampler,uv).r);
        float transparentCoverage=1.0f-reveal;
        float3 transparentColor=accum.a>1e-5f ? accum.rgb/accum.a : float3(0.0f,0.0f,0.0f);
        combined=sceneStraight*alpha*reveal+transparentColor*transparentCoverage;
        coverage=saturate(alpha+(1.0f-alpha)*transparentCoverage);
    }
    float3 mapped=SrgbEncode(ApplyToneMap(combined,gResolve.x,(uint)gResolve.y));
    uint backgroundMode=(uint)gResolve.z;
    float3 display=backgroundMode==2u ? mapped*coverage
        : lerp(BackdropAt(uv,position),mapped,coverage);
    return float2(dot(display,float3(0.2126f,0.7152f,0.0722f)),coverage);
}

bool DepthEdge(float centreDepth, float neighbourDepth)
{
    // `1 - depth` is linear for orthographic and monotonic for perspective, so
    // one relative rule covers both. The floor rejects quantisation noise on a
    // single flat surface.
    float centreKey=saturate(1.0f-centreDepth);
    float neighbourKey=saturate(1.0f-neighbourDepth);
    float threshold=max(0.0005f,centreKey*0.01f);
    return abs(centreKey-neighbourKey)>threshold;
}

float4 PSMain(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
    uint resolveDiagnostic=(uint)gResolve.w;
    float4 centre=ComputeComposite(uv,position.xy,resolveDiagnostic);
    // Diagnostics and the explicit off path stay byte-identical to the
    // pre-T33 resolve. AA runs only on the product image.
    if (gResolve2.x<0.5f || resolveDiagnostic!=12u) return centre;

    float2 texel=float2(gResolve2.z,gResolve2.w);
    int2 pixel=int2(position.xy);
    float2 c=EdgeProbe(uv,position.xy);
    float2 west=EdgeProbe(uv+float2(-texel.x,0.0f),position.xy);
    float2 east=EdgeProbe(uv+float2( texel.x,0.0f),position.xy);
    float2 north=EdgeProbe(uv+float2(0.0f,-texel.y),position.xy);
    float2 south=EdgeProbe(uv+float2(0.0f, texel.y),position.xy);
    float lumaMin=min(c.x,min(min(west.x,east.x),min(north.x,south.x)));
    float lumaMax=max(c.x,max(max(west.x,east.x),max(north.x,south.x)));
    float contrast=lumaMax-lumaMin;
    if (contrast<0.045f) return centre;

    float centreDepth=gDepth.Load(int3(pixel,0));
    bool depthEdge=
        DepthEdge(centreDepth,gDepth.Load(int3(pixel+int2(-1,0),0)))
        ||DepthEdge(centreDepth,gDepth.Load(int3(pixel+int2(1,0),0)))
        ||DepthEdge(centreDepth,gDepth.Load(int3(pixel+int2(0,-1),0)))
        ||DepthEdge(centreDepth,gDepth.Load(int3(pixel+int2(0,1),0)));
    bool coverageEdge=
        abs(c.y-west.y)>0.15f||abs(c.y-east.y)>0.15f
        ||abs(c.y-north.y)>0.15f||abs(c.y-south.y)>0.15f;
    // A high-contrast texture interior gets only the small detail-preserving
    // weight. Mild texture edges are left untouched, so authored lettering and
    // engraving keep their original contrast.
    if (!depthEdge && !coverageEdge && contrast<0.22f) return centre;

    float gradientX=abs(east.x-west.x);
    float gradientY=abs(north.x-south.x);
    float2 direction=gradientX>=gradientY ? float2(texel.x,0.0f) : float2(0.0f,texel.y);
    float4 blendA=ComputeComposite(uv-direction*1.5f,position.xy,12u);
    float4 blendB=ComputeComposite(uv+direction*1.5f,position.xy,12u);
    float reach=saturate((contrast-0.045f)/0.955f);
    float weight=((depthEdge||coverageEdge)?0.55f:0.12f)*reach;
    return lerp(centre,(blendA+blendB)*0.5f,weight);
}
)";

// T27 transparent-radiance mip reduction. One full-screen draw per level; the
// caller binds the previous level as SRV and this level as RTV.
constexpr char kTransmissionMipPixelShaderSource[] = R"(
Texture2D<float4> gTransmissionSource : register(t0);
SamplerState gLinearSampler : register(s0);
struct MipInput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
float4 PSMain(MipInput input) : SV_TARGET
{
    return gTransmissionSource.SampleLevel(gLinearSampler,input.uv,0);
}
)";

// T32 scale-aware screen-space ambient occlusion. The depth buffer is the only
// geometry input: camera-relative positions are reconstructed with the inverse
// view-projection, so perspective, orthographic and the blended projection all
// use the same code. The kernel is a fixed 12-sample spiral; scene scale
// converts the world radius into a bounded pixel radius measured from the
// reconstructed frame itself, never from an assumed model size.
constexpr char kAoRawPixelShaderSource[] = R"(
cbuffer AoConstants : register(b0)
{
    row_major float4x4 gInvViewProjection;
    float4 gViewport;   // physical target width, height, left, top
    float4 gAoParams;   // world radius, max pixel radius, bias, strength
    float4 gAoParams2;  // sample count, filter radius, unused, unused
    // Camera position in the same camera-target-relative frame as the
    // reconstructed positions. The camera is not at the origin.
    float4 gAoEye;
};
Texture2D<float> gDepth : register(t0);
SamplerState gPoint : register(s0);

float3 Reconstruct(float2 uv, float depth)
{
    // DirectX depth is [0,1]; uv.y grows down while NDC y grows up.
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 clip = float4(ndc, depth, 1.0f);
    float4 relative = mul(clip, gInvViewProjection);
    return relative.xyz / max(relative.w, 1e-6f);
}

float PSMain(float4 position : SV_POSITION) : SV_TARGET
{
    const float2 targetSize = max(gViewport.xy, float2(1.0f, 1.0f));
    const float2 texel = 1.0f / targetSize;
    const float2 uv = position.xy / targetSize;
    const float worldRadius = max(gAoParams.x, 1e-5f);
    const float pixelRadiusMax = max(gAoParams.y, 1.0f);
    const float bias = saturate(gAoParams.z);
    const float strength = saturate(gAoParams.w);

    float depth = gDepth.SampleLevel(gPoint, uv, 0).r;
    if (depth >= 0.999999f) return 1.0f;
    // Debug view 1: geometry coverage (white) versus background (black).
    if (gAoParams2.z > 0.5f && gAoParams2.z < 1.5f) return 1.0f;

    float depthR = gDepth.SampleLevel(gPoint, uv + float2(texel.x, 0.0f), 0).r;
    float depthL = gDepth.SampleLevel(gPoint, uv - float2(texel.x, 0.0f), 0).r;
    float depthD = gDepth.SampleLevel(gPoint, uv + float2(0.0f, texel.y), 0).r;
    float depthU = gDepth.SampleLevel(gPoint, uv - float2(0.0f, texel.y), 0).r;
    // Debug view 5: how many of the four neighbours are background.
    float debugView = gAoParams2.z;
    if (debugView > 4.5f) {
        float background = (depthR >= 0.999999f ? 1.0f : 0.0f)
            + (depthL >= 0.999999f ? 1.0f : 0.0f)
            + (depthD >= 0.999999f ? 1.0f : 0.0f)
            + (depthU >= 0.999999f ? 1.0f : 0.0f);
        return background * 0.25f;
    }
    // A silhouette or background neighbour invalidates the reconstructed
    // normal. Returning an unoccluded pixel preserves the silhouette instead of
    // painting a halo around it.
    if (depthR >= 0.999999f || depthL >= 0.999999f
        || depthD >= 0.999999f || depthU >= 0.999999f) return 1.0f;

    float3 p = Reconstruct(uv, depth);
    float3 pR = Reconstruct(uv + float2(texel.x, 0.0f), depthR);
    float3 pL = Reconstruct(uv - float2(texel.x, 0.0f), depthL);
    float3 pD = Reconstruct(uv + float2(0.0f, texel.y), depthD);
    float3 pU = Reconstruct(uv - float2(0.0f, texel.y), depthU);

    float viewDepth = max(abs(p.z), 1e-4f);
    float gap = max(max(length(pR - p), length(pL - p)),
                    max(length(pD - p), length(pU - p)));
    if (debugView > 2.5f && debugView < 3.5f)
        return saturate(gap / max(worldRadius * 3.0f, 1e-6f));
    if (!(gap <= max(worldRadius * 3.0f, viewDepth * 0.08f))) return 1.0f;

    float3 n = cross(pR - pL, pD - pU);
    float lengthN = length(n);
    if (!(lengthN > 1e-12f)) return 1.0f;
    n /= lengthN;
    if (debugView > 1.5f && debugView < 2.5f) return n.x * 0.5f + 0.5f;
    // Orient toward the camera. Positions are relative to the camera target,
    // so the eye is gAoEye, not the origin. Two-sided and mirrored instances
    // reconstruct the same facing normal here.
    if (dot(n, gAoEye.xyz - p) < 0.0f) n = -n;

    float worldPerPixel = max(length(pR - p), length(pD - p));
    float pixelRadius = clamp(worldRadius / max(worldPerPixel, 1e-7f),
                              1.0f, pixelRadiusMax);
    if (debugView > 3.5f) return saturate(pixelRadius / max(pixelRadiusMax, 1.0f));

    // A fixed interleaved-gradient rotation gives a stable, repeatable pattern
    // for a stationary camera without temporal history or reprojection.
    float noise = frac(52.9829189f * frac(0.06711056f * position.x
                                          + 0.00583715f * position.y));
    float occlusion = 0.0f;
    float accepted = 0.0f;
    float facingSum = 0.0f;
    [unroll]
    for (uint index = 0u; index < 12u; ++index) {
        float angle = noise * 6.2831853f + float(index) * 2.39996323f;
        float radius = sqrt((float(index) + 0.5f) / 12.0f);
        float2 offset = float2(cos(angle), sin(angle)) * radius * pixelRadius;
        float2 sampleUv = uv + offset * texel;
        if (sampleUv.x <= 0.0f || sampleUv.x >= 1.0f
            || sampleUv.y <= 0.0f || sampleUv.y >= 1.0f) continue;
        float sampleDepth = gDepth.SampleLevel(gPoint, sampleUv, 0).r;
        if (sampleDepth >= 0.999999f) continue;
        float3 samplePosition = Reconstruct(sampleUv, sampleDepth);
        float3 delta = samplePosition - p;
        float distance = length(delta);
        if (!(distance > 1e-6f) || distance > worldRadius * 2.5f) continue;
        float facing = dot(n, delta / distance);
        // A sample that lies closer to the eye is occluding geometry even when
        // it sits on a perpendicular wall (facing ~ 0), which is exactly the
        // wall-to-wall/narrow-gap case a pure horizon test misses. Same-plane
        // samples have closing ~ 0 and are still rejected by the bias.
        float closing = saturate(dot(delta, gAoEye.xyz - p)
                                 / max(distance * length(gAoEye.xyz - p), 1e-6f));
        if (facing <= bias && closing <= 0.15f) continue;
        accepted += 1.0f;
        facingSum += facing;
        float weight = max(saturate((facing - bias) / max(1.0f - bias, 1e-4f)),
                           closing * 0.7f);
        occlusion += weight * saturate(1.0f - distance / worldRadius);
    }
    if (debugView > 7.5f && debugView < 8.5f) return accepted / 12.0f;
    if (debugView > 8.5f) return saturate(facingSum / max(accepted, 1.0f));
    // Divide by the fixed kernel size, not the surviving count: a sample that
    // falls on background must reduce occlusion rather than renormalize a
    // sparser (and darker) kernel.
    float normalized = saturate(occlusion / 12.0f);
    if (debugView > 6.5f) return normalized;
    return saturate(1.0f - normalized * strength * 2.2f);
}
)";

constexpr char kAoBlurPixelShaderSource[] = R"(
cbuffer AoConstants : register(b0)
{
    row_major float4x4 gInvViewProjection;
    float4 gViewport;
    float4 gAoParams;
    float4 gAoParams2;
    float4 gAoEye;
};
Texture2D<float> gAoRaw : register(t0);
Texture2D<float> gDepth : register(t1);
SamplerState gPoint : register(s0);

float3 Reconstruct(float2 uv, float depth)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 clip = float4(ndc, depth, 1.0f);
    float4 relative = mul(clip, gInvViewProjection);
    return relative.xyz / max(relative.w, 1e-6f);
}

float PSMain(float4 position : SV_POSITION) : SV_TARGET
{
    const float2 targetSize = max(gViewport.xy, float2(1.0f, 1.0f));
    const float2 texel = 1.0f / targetSize;
    const float2 uv = position.xy / targetSize;
    const float worldRadius = max(gAoParams.x, 1e-5f);

    float depth = gDepth.SampleLevel(gPoint, uv, 0).r;
    if (depth >= 0.999999f) return 1.0f;
    float3 center = Reconstruct(uv, depth);

    float sum = 0.0f;
    float weightSum = 0.0f;
    [unroll]
    for (int y = -2; y <= 2; ++y) {
        [unroll]
        for (int x = -2; x <= 2; ++x) {
            float2 sampleUv = uv + float2(x, y) * texel;
            if (sampleUv.x <= 0.0f || sampleUv.x >= 1.0f
                || sampleUv.y <= 0.0f || sampleUv.y >= 1.0f) continue;
            float sampleDepth = gDepth.SampleLevel(gPoint, sampleUv, 0).r;
            if (sampleDepth >= 0.999999f) continue;
            float3 samplePosition = Reconstruct(sampleUv, sampleDepth);
            float distance = length(samplePosition - center);
            // Depth/normal discontinuity rejection: a neighbour across a
            // silhouette or a separate surface gets no weight, so filtering
            // cannot bleed darkening into the background.
            if (distance > worldRadius * 2.0f) continue;
            float spatial = 1.0f - float(abs(x) + abs(y)) / 8.0f;
            float range = exp(-distance / max(worldRadius * 0.35f, 1e-5f));
            float weight = max(spatial, 0.05f) * range;
            sum += gAoRaw.SampleLevel(gPoint, sampleUv, 0).r * weight;
            weightSum += weight;
        }
    }
    if (!(weightSum > 0.0f)) return gAoRaw.SampleLevel(gPoint, uv, 0).r;
    return saturate(sum / weightSum);
}
)";

bool CompileShader(const char* source, size_t sourceLength, const char* entryPoint, const char* target,
                    ComPtr<ID3DBlob>& outBlob, std::wstring& error,
                    const D3D_SHADER_MACRO* defines = nullptr)
{
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompile(source, sourceLength, nullptr, defines, nullptr, entryPoint, target, flags, 0,
                             &outBlob, &errorBlob);
    if (FAILED(hr)) {
        error = L"A D3D12 shader failed to compile.";
        if (errorBlob) {
            error += L" ";
            error += std::wstring(static_cast<const char*>(errorBlob->GetBufferPointer()),
                                   static_cast<const char*>(errorBlob->GetBufferPointer()) + errorBlob->GetBufferSize())
                         .c_str();
        }
        return false;
    }
    return true;
}

// Never guesses: nullopt for any (format, colorSpace) combination not
// explicitly listed, matching model_core::PixelFormatBlockInfo's "never
// guess" doctrine. SharedSectionValidator has already rejected
// BC5_UNORM+Srgb before this is ever reached (no DXGI _SRGB variant).
std::optional<DXGI_FORMAT> DxgiFormatFor(model_core::PixelFormatId pixelFormat, model_core::ColorSpaceId colorSpace)
{
    using model_core::ColorSpaceId;
    using model_core::PixelFormatId;
    bool srgb = colorSpace == ColorSpaceId::Srgb;
    switch (pixelFormat) {
    case PixelFormatId::RGBA8_UNORM:
        return srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    case PixelFormatId::BC7_UNORM:
        return srgb ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
    case PixelFormatId::BC5_UNORM:
        return DXGI_FORMAT_BC5_UNORM; // no _SRGB variant exists
    case PixelFormatId::BC3_UNORM:
        return srgb ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM;
    case PixelFormatId::BC1_UNORM:
        return srgb ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
    default:
        return std::nullopt;
    }
}

ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, uint64_t sizeBytes, D3D12_HEAP_TYPE heapType,
                                     D3D12_RESOURCE_STATES initialState, HRESULT* result = nullptr)
{
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = heapType;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = sizeBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> resource;
    const HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
                                     IID_PPV_ARGS(&resource));
    if (result) *result = hr;
    return resource;
}

} // namespace

const char* D3D12ViewerPath::TexturedPixelShaderSource()
{
    return kTexturedPixelShaderSource;
}

const char* D3D12ViewerPath::TexturedVertexShaderSource()
{
    return kTexturedVertexShaderSource;
}

const char* D3D12ViewerPath::ResolvePixelShaderSource()
{
    return kResolvePixelShaderSource;
}

const char* D3D12ViewerPath::GroundPixelShaderSource()
{
    return kGroundPixelShaderSource;
}

const char* D3D12ViewerPath::ShadowDepthPixelShaderSource()
{
    return kShadowMaskPixelShaderSource;
}

const char* D3D12ViewerPath::ShadowGroundPixelShaderSource()
{
    return kShadowGroundPixelShaderSource;
}

const char* D3D12ViewerPath::AoRawPixelShaderSource()
{
    return kAoRawPixelShaderSource;
}

const char* D3D12ViewerPath::AoBlurPixelShaderSource()
{
    return kAoBlurPixelShaderSource;
}

uint64_t D3D12ViewerPath::AoTargetBytesForExtent(uint32_t width, uint32_t height) noexcept
{
    return viewer_ao::TargetBytesForExtent(width, height);
}

float D3D12ViewerPath::OitWeightForDepth(float viewDepth, float sceneScale) noexcept
{
    const float z = (std::max)(viewDepth, 1e-3f) / (std::max)(sceneScale, 1e-3f);
    const float weight = 0.03f / (1e-5f + std::pow(z, 4.0f));
    return std::clamp(weight, 1e-2f, 300.0f);
}

float D3D12ViewerPath::OitResolveCoverage(float revealage) noexcept
{
    return std::clamp(1.0f - revealage, 0.0f, 1.0f);
}

float D3D12ViewerPath::OitResolveAlpha(float accumulatedAlpha) noexcept
{
    return accumulatedAlpha > 1e-5f ? accumulatedAlpha : 0.0f;
}

uint64_t D3D12ViewerPath::OitTargetBytesForExtent(uint32_t width, uint32_t height) noexcept
{
    // Two RGBA16F targets, with a conservative 64 KiB per-target allocation
    // granularity so the public estimate never under-reports what the driver
    // may commit.
    const uint64_t pixels = uint64_t(width) * uint64_t(height);
    const uint64_t accum = ((pixels * 8ull) + 65535ull) / 65536ull * 65536ull;
    const uint64_t reveal = ((pixels * 8ull) + 65535ull) / 65536ull * 65536ull;
    return accum + reveal;
}

uint32_t D3D12ViewerPath::TransmissionMipLevel(float roughness, uint32_t mipCount) noexcept
{
    if (mipCount == 0) return 0;
    const float clamped = std::clamp(roughness, 0.0f, 1.0f);
    const uint32_t last = mipCount - 1;
    return std::min(last, static_cast<uint32_t>(clamped * float(last) + 0.5f));
}

float D3D12ViewerPath::TransmissionInterfaceFresnel(float f0, float nv) noexcept
{
    // f0 is the IOR-derived normal-incidence reflectance. A non-positive f0
    // means no optical interface (IOR at or below 1), not a grazing-only one.
    if (!(f0 > 0.0f)) return 0.0f;
    const float clampedF0 = std::clamp(f0, 0.0f, 1.0f);
    const float cosine = std::clamp(nv, 0.0f, 1.0f);
    return clampedF0 + (1.0f - clampedF0) * std::pow(1.0f - cosine, 5.0f);
}

float D3D12ViewerPath::TransmissionTintWeight(float transmission, float fresnel) noexcept
{
    return std::clamp(transmission, 0.0f, 1.0f) * (1.0f - std::clamp(fresnel, 0.0f, 1.0f));
}

float D3D12ViewerPath::TransmissionCoverage(bool blend, float authoredAlpha) noexcept
{
    return blend ? std::clamp(authoredAlpha, 0.0f, 1.0f) : 1.0f;
}

uint64_t D3D12ViewerPath::TransmissionTargetBytesForExtent(uint32_t width, uint32_t height) noexcept
{
    if (width == 0 || height == 0) return 0;
    // RGBA16F base level plus the bounded mip pyramid, rounded to 64 KiB.
    uint64_t bytes = 0;
    uint32_t levelWidth = width;
    uint32_t levelHeight = height;
    for (uint32_t mip = 0; mip < kTransmissionMipCount; ++mip) {
        const uint64_t pixels = uint64_t(levelWidth) * uint64_t(levelHeight);
        bytes += ((pixels * 8ull) + 65535ull) / 65536ull * 65536ull;
        levelWidth = (std::max)(1u, levelWidth >> 1);
        levelHeight = (std::max)(1u, levelHeight >> 1);
    }
    return bytes;
}

bool D3D12ViewerPath::Initialize(HWND window, std::wstring& error)
{
    // A device recovery constructs a new device. Release every viewport- and
    // device-scoped resource before CreateShadowTargets can reuse a stale
    // allocation.
    shadow.depth.Reset();
    shadow.mask.Reset();
    shadow.dsvHeap.Reset();
    shadow.rtvHeap.Reset();
    shadow.srvHeap.Reset();
    shadow.available = false;
    shadow.size = 0;
    shadow.depthBytes = 0;
    shadow.maskBytes = 0;
    shadowAllocationBytes = 0;
    ao.indirect.Reset();
    ao.raw.Reset();
    ao.filtered.Reset();
    ao.rtvHeap.Reset();
    ao.srvHeap.Reset();
    ao.indirectBytes = ao.rawBytes = ao.filteredBytes = 0;
    ao.available = false;
    ao.fallbackReason = AoFallbackDeviceLoss;
    aoAllocationBytes = 0;
    aoQueryHeap.Reset();
    aoQueryReadback.Reset();
    aoQueryFence = 0;
    aoQueryInFlight = false;
    aoQueryRecorded = false;
    lastAoGpuMilliseconds = 0.0;
#if defined(PREVIEW3D_ENABLE_TEST_CONTROL)
    // T26 low-memory fallback evidence. A Debug test-control launch can force
    // the OIT target allocation to fail so the recorded sorted fallback is
    // exercised without exhausting the real device.
    wchar_t oitFailure[2]{};
    if (GetEnvironmentVariableW(L"PREVIEW3D_TEST_OIT_ALLOCATION_FAILURE", oitFailure, 2) == 1
        && oitFailure[0] == L'1') {
        oitAllocationForcedFailure = true;
    }
    wchar_t transmissionFailure[2]{};
    if (GetEnvironmentVariableW(L"PREVIEW3D_TEST_TRANSMISSION_ALLOCATION_FAILURE",
                                transmissionFailure, 2) == 1
        && transmissionFailure[0] == L'1') {
        transmissionAllocationForcedFailure = true;
    }
    // T31: exercise the documented analytic fallback without exhausting the
    // device. It is a Debug-only launch switch, like the OIT/transmission ones.
    wchar_t shadowFailure[2]{};
    if (GetEnvironmentVariableW(L"PREVIEW3D_TEST_SHADOW_ALLOCATION_FAILURE",
                                shadowFailure, 2) == 1
        && shadowFailure[0] == L'1') {
        shadowAllocationForcedFailure = true;
    }
    wchar_t aoFailure[2]{};
    if (GetEnvironmentVariableW(L"PREVIEW3D_TEST_AO_ALLOCATION_FAILURE", aoFailure, 2) == 1
        && aoFailure[0] == L'1') {
        aoAllocationForcedFailure = true;
    }
#endif
    auto deviceResult = device.Initialize(deviceOptions);
    if (!deviceResult.success) {
        error = L"The D3D12 device could not be created (HRESULT " + std::to_wstring(deviceResult.hr) + L").";
        return false;
    }

    if (!directQueue.Initialize(*device.Device(), D3D12_COMMAND_LIST_TYPE_DIRECT, L"Direct")) {
        error = L"The D3D12 direct command queue could not be created.";
        return false;
    }

    RECT clientRect{};
    GetClientRect(window, &clientRect);
    D3D12SwapChain::CreateOptions swapChainOptions;
    swapChainOptions.width = static_cast<UINT>(clientRect.right - clientRect.left);
    swapChainOptions.height = static_cast<UINT>(clientRect.bottom - clientRect.top);
    auto swapChainResult = swapChain.Initialize(device, directQueue, window, swapChainOptions);
    if (!swapChainResult.success) {
        error = L"The D3D12 swap chain could not be created (HRESULT " + std::to_wstring(swapChainResult.hr) + L").";
        return false;
    }

    for (UINT i = 0; i < kFrameCount; ++i) {
        if (FAILED(device.Device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                              IID_PPV_ARGS(&frames[i].allocator)))) {
            error = L"The D3D12 command allocator could not be created.";
            return false;
        }
    }
    if (FAILED(device.Device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames[0].allocator.Get(),
                                                    nullptr, IID_PPV_ARGS(&commandList)))) {
        error = L"The D3D12 command list could not be created.";
        return false;
    }
    // CreateCommandList returns it open; close before the first Reset(),
    // matching the FrameRecorder convention SwapChainTests.cpp established.
    commandList->Close();

    // The coordinator initializes a separate upload-only path sharing this device.
    if (!CreateDepthBuffer(swapChainOptions.width, swapChainOptions.height, error)) return false;
    if (!CreatePipeline(error)) return false;
    if (!CreateTexturedPipeline(error)) return false;
    if (!CreateAoPipelines(error)) return false;
    // Device-scoped AO GPU timing. Failure only disables the timing evidence;
    // the AO feature and its recorded pass time remain available.
    D3D12_QUERY_HEAP_DESC queryDesc{};
    queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queryDesc.Count = 2;
    if (FAILED(device.Device()->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&aoQueryHeap)))) {
        aoQueryHeap.Reset();
    }
    if (!aoQueryReadback) {
        aoQueryReadback = CreateBuffer(device.Device(), 256, D3D12_HEAP_TYPE_READBACK,
                                       D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (!CreateShadowPipelines(error)) return false;
    if (!CreateResolvePipeline(error)) return false;
    if (!CreateFrameConstantBuffer(error)) return false;
    if (!CreateStudioEnvironment(error)) return false;

    if (!overlay.Initialize(device, directQueue, swapChain, error)) {
        return false;
    }

    return true;
}

bool D3D12ViewerPath::CreateDepthBuffer(UINT width, UINT height, std::wstring& error)
{
    if (!dsvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.NumDescriptors = 1;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (FAILED(device.Device()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&dsvHeap)))) {
            error = L"The depth buffer descriptor heap could not be created.";
            return false;
        }
    }

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Width = width;
    resourceDesc.Height = height;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_D32_FLOAT;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;

    depthBuffer.Reset();
    if (FAILED(device.Device()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc,
                                                          D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
                                                          IID_PPV_ARGS(&depthBuffer)))) {
        error = L"The depth buffer could not be created.";
        return false;
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device.Device()->CreateDepthStencilView(depthBuffer.Get(), &dsvDesc, dsvHeap->GetCPUDescriptorHandleForHeapStart());
    return CreatePickTarget(width, height, error);
}

bool D3D12ViewerPath::CreatePickTarget(UINT width, UINT height, std::wstring& error)
{
    // One stable uint id per viewport pixel, plus one 256-byte readback slot. No CPU
    // geometry catalog; picking uses exactly the depth-tested displayed pixels.
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = width; desc.Height = height;
    desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.Format = DXGI_FORMAT_R32_UINT;
    desc.SampleDesc.Count = 1; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES properties{}; properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE clear{}; clear.Format = desc.Format;
    pickTarget.Reset(); pickRtvHeap.Reset();
    if (uint64_t(width) * height > 16ull * 1024 * 1024
        || FAILED(device.Device()->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&pickRtvHeap)))
        || FAILED(device.Device()->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&pickTarget)))) {
        error = L"The selection surface could not be created."; return false;
    }
    device.Device()->CreateRenderTargetView(pickTarget.Get(), nullptr, pickRtvHeap->GetCPUDescriptorHandleForHeapStart());
    if (!pickReadback) pickReadback = CreateBuffer(device.Device(), 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!pickReadback) { error = L"The selection readback could not be created."; return false; }
    // T21: one bounded 64x64 RGBA16F sample slot. 64 texels * 8 bytes = 512,
    // already a 256-byte multiple, so every row obeys D3D12's placed-footprint
    // pitch requirement.
    if (!debugReadback) debugReadback = CreateBuffer(device.Device(),
        uint64_t(kDebugSampleMaxExtent) * kDebugSampleMaxExtent * 8,
        D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!debugReadback) { error = L"The diagnostic readback could not be created."; return false; }
    if (!CreateSceneColorTarget(width, height, error)) return false;
    return true;
}

bool D3D12ViewerPath::CreateSceneColorTarget(UINT width, UINT height, std::wstring& error)
{
    // The HDR target doubles color memory versus the old RGBA8 back buffer.
    // Keep the same bounded extent guard as the pick target and fail cleanly;
    // the caller treats that as a graphics/startup failure, never as a reason
    // to keep drawing into a stale target.
    if (width == 0 || height == 0 || uint64_t(width) * height > 16ull * 1024 * 1024) {
        error = L"The HDR scene target extent is not supported.";
        return false;
    }

    sceneColor.Reset();
    sceneColorRtvHeap.Reset();
    sceneColorSrvHeap.Reset();
    backdropColor.Reset();
    backdropRtvHeap.Reset();

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    // Scene colour plus the T32 indirect HDR share.
    D3D12_DESCRIPTOR_HEAP_DESC sceneRtvHeapDesc = rtvHeapDesc;
    sceneRtvHeapDesc.NumDescriptors = 2;
    D3D12_DESCRIPTOR_HEAP_DESC singleRtvHeapDesc = rtvHeapDesc;
    singleRtvHeapDesc.NumDescriptors = 1;
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    // t0 = HDR scene, t1 = display-space backdrop, t2/t3 = OIT accumulation
    // and revealage, t4 = T32 indirect share, t5 = T32 filtered AO mask,
    // t6 = T33 depth. Every slot always receives a valid descriptor
    // (duplicates of the scene in fallback mode) so the resolve table is never
    // partially uninitialized.
    srvHeapDesc.NumDescriptors = 7;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device.Device()->CreateDescriptorHeap(&sceneRtvHeapDesc, IID_PPV_ARGS(&sceneColorRtvHeap)))
        || FAILED(device.Device()->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&sceneColorSrvHeap)))
        || FAILED(device.Device()->CreateDescriptorHeap(&singleRtvHeapDesc, IID_PPV_ARGS(&backdropRtvHeap)))) {
        error = L"The HDR scene descriptor heaps could not be created.";
        return false;
    }

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = kSceneColorFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = kSceneColorFormat;
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    const HRESULT allocationResult = device.Device()->CreateCommittedResource(
        &properties, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
        &clear, IID_PPV_ARGS(&sceneColor));
    if (FAILED(allocationResult)) {
        error = L"The HDR scene target could not be created (HRESULT "
            + std::to_wstring(static_cast<long>(allocationResult)) + L").";
        return false;
    }
    sceneColorAllocationBytes = device.Device()->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;

    device.Device()->CreateRenderTargetView(sceneColor.Get(), nullptr,
        sceneColorRtvHeap->GetCPUDescriptorHandleForHeapStart());
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = kSceneColorFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device.Device()->CreateShaderResourceView(sceneColor.Get(), &srv,
        sceneColorSrvHeap->GetCPUDescriptorHandleForHeapStart());

    // T33: the resolve's edge filter reads the existing D32 depth buffer. It is
    // an SRV, never a second allocation; the pick target is unaffected.
    depthSrvAvailable = false;
    if (depthBuffer) {
        const UINT depthDescriptorSize =
            device.Device()->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE depthSrv =
            sceneColorSrvHeap->GetCPUDescriptorHandleForHeapStart();
        depthSrv.ptr += SIZE_T(6) * depthDescriptorSize;
        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc{};
        depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Texture2D.MipLevels = 1;
        device.Device()->CreateShaderResourceView(depthBuffer.Get(), &depthSrvDesc, depthSrv);
        depthSrvAvailable = true;
    }

    // T21a display-space backdrop target. RGBA8 is enough: it stores the
    // already-sRGB-encoded display color and the local ground treatment.
    D3D12_RESOURCE_DESC backdropDesc{};
    backdropDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    backdropDesc.Width = width;
    backdropDesc.Height = height;
    backdropDesc.DepthOrArraySize = 1;
    backdropDesc.MipLevels = 1;
    backdropDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    backdropDesc.SampleDesc.Count = 1;
    backdropDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE backdropClear{};
    backdropClear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (FAILED(device.Device()->CreateCommittedResource(
            &properties, D3D12_HEAP_FLAG_NONE, &backdropDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &backdropClear, IID_PPV_ARGS(&backdropColor)))) {
        error = L"The display-space backdrop target could not be created.";
        return false;
    }
    backdropAllocationBytes =
        device.Device()->GetResourceAllocationInfo(0, 1, &backdropDesc).SizeInBytes;
    device.Device()->CreateRenderTargetView(backdropColor.Get(), nullptr,
        backdropRtvHeap->GetCPUDescriptorHandleForHeapStart());
    const UINT srvDescriptorSize =
        device.Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE backdropSrv =
        sceneColorSrvHeap->GetCPUDescriptorHandleForHeapStart();
    backdropSrv.ptr += srvDescriptorSize;
    D3D12_SHADER_RESOURCE_VIEW_DESC backdropSrvDesc{};
    backdropSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    backdropSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    backdropSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    backdropSrvDesc.Texture2D.MipLevels = 1;
    device.Device()->CreateShaderResourceView(backdropColor.Get(), &backdropSrvDesc, backdropSrv);

    // T32 AO resources. Failure is a recorded AO-off fallback, never a startup
    // failure; the indirect target is also the opaque pass's second MRT.
    CreateAoTargets(width, height);
    WriteAoDescriptors();
    // T26 OIT targets. Failure is a recorded fallback, never a startup failure.
    CreateOitTargets(width, height);
    // T27 transmitted-radiance snapshot and mip pyramid. Failure degrades
    // transmissive materials to their authored dielectric response and is
    // reported through the transmission diagnostics fields.
    CreateTransmissionTargets(width, height);
    // T31 shadow targets are viewport-independent; create them once per device
    // instead of reallocating on every resize.
    CreateShadowTargets();
    RefreshTransmissionDescriptors();
    transmissionTargetBytes = transmission.allocationBytes;
    D3D12_CPU_DESCRIPTOR_HANDLE oitAccumSrv = sceneColorSrvHeap->GetCPUDescriptorHandleForHeapStart();
    oitAccumSrv.ptr += srvDescriptorSize * 2;
    D3D12_CPU_DESCRIPTOR_HANDLE oitRevealSrv = sceneColorSrvHeap->GetCPUDescriptorHandleForHeapStart();
    oitRevealSrv.ptr += srvDescriptorSize * 3;
    if (oit.available) {
        D3D12_SHADER_RESOURCE_VIEW_DESC accumSrvDesc{};
        accumSrvDesc.Format = kSceneColorFormat;
        accumSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        accumSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        accumSrvDesc.Texture2D.MipLevels = 1;
        device.Device()->CreateShaderResourceView(oit.accumulation.Get(), &accumSrvDesc, oitAccumSrv);
        D3D12_SHADER_RESOURCE_VIEW_DESC revealSrvDesc{};
        revealSrvDesc.Format = kSceneColorFormat;
        revealSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        revealSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        revealSrvDesc.Texture2D.MipLevels = 1;
        device.Device()->CreateShaderResourceView(oit.revealage.Get(), &revealSrvDesc, oitRevealSrv);
    } else {
        // Fallback: point the unused table entries at already-valid resources.
        // The resolve shader branches on the OIT flag and never samples them.
        device.Device()->CreateShaderResourceView(sceneColor.Get(), &srv, oitAccumSrv);
        device.Device()->CreateShaderResourceView(backdropColor.Get(), &backdropSrvDesc, oitRevealSrv);
    }
    oitTargetBytes = oit.accumulationAllocationBytes + oit.revealageAllocationBytes;
    return true;
}

bool D3D12ViewerPath::CreateOitTargets(UINT width, UINT height)
{
    oit.accumulation.Reset();
    oit.revealage.Reset();
    oit.rtvHeap.Reset();
    oit.accumulationAllocationBytes = 0;
    oit.revealageAllocationBytes = 0;
    oit.available = false;
    oit.fallbackReason = OitFallbackUnsupportedExtent;
    if (width == 0 || height == 0 || uint64_t(width) * height > 16ull * 1024 * 1024) {
        return false;
    }
    if (oitAllocationForcedFailure) {
        oit.fallbackReason = OitFallbackAllocationFailed;
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 2;
    if (FAILED(device.Device()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&oit.rtvHeap)))) {
        oit.fallbackReason = OitFallbackAllocationFailed;
        return false;
    }

    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC accumDesc{};
    accumDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    accumDesc.Width = width;
    accumDesc.Height = height;
    accumDesc.DepthOrArraySize = 1;
    accumDesc.MipLevels = 1;
    accumDesc.Format = kSceneColorFormat;
    accumDesc.SampleDesc.Count = 1;
    accumDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE accumClear{};
    accumClear.Format = kSceneColorFormat;
    const HRESULT accumResult = device.Device()->CreateCommittedResource(
        &properties, D3D12_HEAP_FLAG_NONE, &accumDesc, D3D12_RESOURCE_STATE_RENDER_TARGET,
        &accumClear, IID_PPV_ARGS(&oit.accumulation));
    if (FAILED(accumResult)) {
        oit.fallbackReason = OitFallbackAllocationFailed;
        oit.accumulation.Reset();
        oit.rtvHeap.Reset();
        return false;
    }
    oit.accumulationAllocationBytes =
        device.Device()->GetResourceAllocationInfo(0, 1, &accumDesc).SizeInBytes;

    D3D12_RESOURCE_DESC revealDesc = accumDesc;
    revealDesc.Format = kSceneColorFormat;
    D3D12_CLEAR_VALUE revealClear{};
    revealClear.Format = kSceneColorFormat;
    revealClear.Color[0] = 1.0f;
    const HRESULT revealResult = device.Device()->CreateCommittedResource(
        &properties, D3D12_HEAP_FLAG_NONE, &revealDesc, D3D12_RESOURCE_STATE_RENDER_TARGET,
        &revealClear, IID_PPV_ARGS(&oit.revealage));
    if (FAILED(revealResult)) {
        oit.fallbackReason = OitFallbackAllocationFailed;
        oit.accumulation.Reset();
        oit.revealage.Reset();
        oit.rtvHeap.Reset();
        oit.accumulationAllocationBytes = 0;
        return false;
    }
    oit.revealageAllocationBytes =
        device.Device()->GetResourceAllocationInfo(0, 1, &revealDesc).SizeInBytes;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = oit.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    device.Device()->CreateRenderTargetView(oit.accumulation.Get(), nullptr, rtv);
    rtv.ptr += device.Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    device.Device()->CreateRenderTargetView(oit.revealage.Get(), nullptr, rtv);
    oit.available = true;
    oit.fallbackReason = OitFallbackNone;
    return true;
}

bool D3D12ViewerPath::CreateTransmissionTargets(UINT width, UINT height)
{
    transmission.sceneCopy.Reset();
    transmission.rtvHeap.Reset();
    transmission.mipSrvHeap.Reset();
    transmission.allocationBytes = 0;
    transmission.mipCount = 0;
    transmission.available = false;
    transmission.fallbackReason = TransmissionFallbackUnsupportedExtent;
    if (width == 0 || height == 0 || uint64_t(width) * height > 16ull * 1024 * 1024) {
        return false;
    }
    if (transmissionAllocationForcedFailure) {
        transmission.fallbackReason = TransmissionFallbackForced;
        return false;
    }

    const UINT mipCount = kTransmissionMipCount;
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = static_cast<UINT16>(mipCount);
    desc.Format = kSceneColorFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = kSceneColorFormat;
    if (FAILED(device.Device()->CreateCommittedResource(
            &properties, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
            IID_PPV_ARGS(&transmission.sceneCopy)))) {
        transmission.fallbackReason = TransmissionFallbackAllocationFailed;
        return false;
    }
    transmission.allocationBytes =
        device.Device()->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = mipCount - 1;
    if (FAILED(device.Device()->CreateDescriptorHeap(&rtvHeapDesc,
            IID_PPV_ARGS(&transmission.rtvHeap)))) {
        transmission.sceneCopy.Reset();
        transmission.allocationBytes = 0;
        transmission.fallbackReason = TransmissionFallbackAllocationFailed;
        return false;
    }
    const UINT rtvSize = device.Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT mip = 1; mip < mipCount; ++mip) {
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = kSceneColorFormat;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Texture2D.MipSlice = mip;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = transmission.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(mip - 1) * rtvSize;
        device.Device()->CreateRenderTargetView(transmission.sceneCopy.Get(), &rtvDesc, rtv);
    }

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = mipCount;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device.Device()->CreateDescriptorHeap(&srvHeapDesc,
            IID_PPV_ARGS(&transmission.mipSrvHeap)))) {
        transmission.sceneCopy.Reset();
        transmission.rtvHeap.Reset();
        transmission.allocationBytes = 0;
        transmission.fallbackReason = TransmissionFallbackAllocationFailed;
        return false;
    }
    const UINT srvSize = device.Device()->GetDescriptorHandleIncrementSize(srvHeapDesc.Type);
    for (UINT mip = 0; mip < mipCount; ++mip) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = kSceneColorFormat;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MostDetailedMip = mip;
        srv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = transmission.mipSrvHeap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(mip) * srvSize;
        device.Device()->CreateShaderResourceView(transmission.sceneCopy.Get(), &srv, cpu);
    }
    transmission.mipCount = mipCount;
    transmission.available = true;
    transmission.fallbackReason = TransmissionFallbackNone;
    return true;
}

bool D3D12ViewerPath::CreateAoTargets(UINT width, UINT height)
{
    ao.indirect.Reset();
    ao.raw.Reset();
    ao.filtered.Reset();
    ao.rtvHeap.Reset();
    ao.srvHeap.Reset();
    ao.indirectBytes = 0;
    ao.rawBytes = 0;
    ao.filteredBytes = 0;
    ao.available = false;
    ao.fallbackReason = AoFallbackUnsupportedExtent;
    aoAllocationBytes = 0;
    if (!viewer_ao::TargetSupported(width, height)) return false;
    if (aoAllocationForcedFailure) {
        // The Debug switch stands in for a real low-memory allocation failure,
        // so it reports the same recorded reason as E_OUTOFMEMORY would.
        ao.fallbackReason = AoFallbackAllocationFailed;
        return false;
    }

    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC indirectDesc{};
    indirectDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    indirectDesc.Width = width;
    indirectDesc.Height = height;
    indirectDesc.DepthOrArraySize = 1;
    indirectDesc.MipLevels = 1;
    indirectDesc.Format = kSceneColorFormat;
    indirectDesc.SampleDesc.Count = 1;
    indirectDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE indirectClear{};
    indirectClear.Format = kSceneColorFormat;
    D3D12_RESOURCE_DESC maskDesc{};
    maskDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    maskDesc.Width = width;
    maskDesc.Height = height;
    maskDesc.DepthOrArraySize = 1;
    maskDesc.MipLevels = 1;
    maskDesc.Format = DXGI_FORMAT_R8_UNORM;
    maskDesc.SampleDesc.Count = 1;
    maskDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE maskClear{};
    maskClear.Format = DXGI_FORMAT_R8_UNORM;
    maskClear.Color[0] = 1.0f;
    if (FAILED(device.Device()->CreateCommittedResource(
            &properties, D3D12_HEAP_FLAG_NONE, &indirectDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &indirectClear, IID_PPV_ARGS(&ao.indirect)))
        || FAILED(device.Device()->CreateCommittedResource(
            &properties, D3D12_HEAP_FLAG_NONE, &maskDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &maskClear, IID_PPV_ARGS(&ao.raw)))
        || FAILED(device.Device()->CreateCommittedResource(
            &properties, D3D12_HEAP_FLAG_NONE, &maskDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &maskClear, IID_PPV_ARGS(&ao.filtered)))) {
        ao.indirect.Reset();
        ao.raw.Reset();
        ao.filtered.Reset();
        ao.fallbackReason = AoFallbackAllocationFailed;
        return false;
    }
    ao.indirectBytes = device.Device()->GetResourceAllocationInfo(0, 1, &indirectDesc).SizeInBytes;
    ao.rawBytes = device.Device()->GetResourceAllocationInfo(0, 1, &maskDesc).SizeInBytes;
    ao.filteredBytes = ao.rawBytes;

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 2;
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 3;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device.Device()->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&ao.rtvHeap)))
        || FAILED(device.Device()->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&ao.srvHeap)))) {
        ao.indirect.Reset();
        ao.raw.Reset();
        ao.filtered.Reset();
        ao.rtvHeap.Reset();
        ao.srvHeap.Reset();
        ao.indirectBytes = ao.rawBytes = ao.filteredBytes = 0;
        ao.fallbackReason = AoFallbackAllocationFailed;
        return false;
    }
    const UINT rtvSize = device.Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rawRtv = ao.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    device.Device()->CreateRenderTargetView(ao.raw.Get(), nullptr, rawRtv);
    rawRtv.ptr += rtvSize;
    device.Device()->CreateRenderTargetView(ao.filtered.Get(), nullptr, rawRtv);
    ao.available = true;
    ao.fallbackReason = AoFallbackNone;
    aoAllocationBytes = ao.indirectBytes + ao.rawBytes + ao.filteredBytes;
    return true;
}

void D3D12ViewerPath::WriteAoDescriptors()
{
    if (!sceneColorSrvHeap || !sceneColor) return;
    const UINT srvSize = device.Device()->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE indirectSrv = sceneColorSrvHeap->GetCPUDescriptorHandleForHeapStart();
    indirectSrv.ptr += SIZE_T(4) * srvSize;
    D3D12_CPU_DESCRIPTOR_HANDLE aoSrv = sceneColorSrvHeap->GetCPUDescriptorHandleForHeapStart();
    aoSrv.ptr += SIZE_T(5) * srvSize;
    D3D12_CPU_DESCRIPTOR_HANDLE filteredSrv = ao.available && ao.srvHeap
        ? ao.srvHeap->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{};
    if (ao.available && ao.srvHeap) filteredSrv.ptr += SIZE_T(2) * srvSize;
    if (ao.available && ao.indirect && ao.filtered) {
        srv.Format = kSceneColorFormat;
        device.Device()->CreateShaderResourceView(ao.indirect.Get(), &srv, indirectSrv);
        srv.Format = DXGI_FORMAT_R8_UNORM;
        device.Device()->CreateShaderResourceView(ao.filtered.Get(), &srv, aoSrv);
        device.Device()->CreateShaderResourceView(ao.filtered.Get(), &srv, filteredSrv);
        // The opaque pass's second MRT.
        const UINT rtvSize = device.Device()->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE indirectRtv =
            sceneColorRtvHeap->GetCPUDescriptorHandleForHeapStart();
        indirectRtv.ptr += rtvSize;
        device.Device()->CreateRenderTargetView(ao.indirect.Get(), nullptr, indirectRtv);

        // AO pass heap: raw (t0), scene depth (t1), filtered (t2).
        D3D12_CPU_DESCRIPTOR_HANDLE raw = ao.srvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE depth = raw; depth.ptr += srvSize;
        srv.Format = DXGI_FORMAT_R8_UNORM;
        device.Device()->CreateShaderResourceView(ao.raw.Get(), &srv, raw);
        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc{};
        depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Texture2D.MipLevels = 1;
        if (depthBuffer) {
            device.Device()->CreateShaderResourceView(depthBuffer.Get(), &depthSrvDesc, depth);
        }
    } else {
        // Fallback: resolve never samples these slots (gAo.x == 0), but the
        // descriptor table must remain fully initialized.
        srv.Format = kSceneColorFormat;
        device.Device()->CreateShaderResourceView(sceneColor.Get(), &srv, indirectSrv);
        device.Device()->CreateShaderResourceView(sceneColor.Get(), &srv, aoSrv);
    }
}

bool D3D12ViewerPath::CreateAoPipelines(std::wstring& error)
{
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 2;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = 32;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 2;
    rootDesc.pParameters = params;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers = &sampler;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> signatureError;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                            &signature, &signatureError))
        || FAILED(device.Device()->CreateRootSignature(0, signature->GetBufferPointer(),
                    signature->GetBufferSize(), IID_PPV_ARGS(&aoRootSignature)))) {
        error = L"The screen-space AO root signature could not be created.";
        return false;
    }

    ComPtr<ID3DBlob> vs;
    ComPtr<ID3DBlob> rawPs;
    ComPtr<ID3DBlob> blurPs;
    if (!CompileShader(kResolveVertexShaderSource, sizeof(kResolveVertexShaderSource) - 1,
                       "VSMain", "vs_5_1", vs, error)
        || !CompileShader(kAoRawPixelShaderSource, sizeof(kAoRawPixelShaderSource) - 1,
                          "PSMain", "ps_5_1", rawPs, error)
        || !CompileShader(kAoBlurPixelShaderSource, sizeof(kAoBlurPixelShaderSource) - 1,
                          "PSMain", "ps_5_1", blurPs, error)) {
        return false;
    }

    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.DepthClipEnable = TRUE;
    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = aoRootSignature.Get();
    psoDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    psoDesc.BlendState = blend;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState = rasterizer;
    psoDesc.DepthStencilState = depth;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    psoDesc.PS = { rawPs->GetBufferPointer(), rawPs->GetBufferSize() };
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&aoRawPipelineState)))) {
        error = L"The screen-space AO raw pipeline state could not be created.";
        return false;
    }
    psoDesc.PS = { blurPs->GetBufferPointer(), blurPs->GetBufferSize() };
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&aoBlurPipelineState)))) {
        error = L"The screen-space AO filter pipeline state could not be created.";
        return false;
    }
    return true;
}

bool D3D12ViewerPath::RenderScreenSpaceAo(const DirectX::XMFLOAT4X4& inverseViewProjection,
                                           const viewer_ao::Parameters& parameters,
                                           float sceneScale,
                                           const DirectX::XMFLOAT4& viewportRect,
                                           const DirectX::XMFLOAT4& eyePosition)
{
    if (!commandList || !ao.available || !ao.indirect || !ao.raw || !ao.filtered
        || !ao.rtvHeap || !ao.srvHeap || !aoRootSignature || !aoRawPipelineState
        || !aoBlurPipelineState || !depthBuffer) {
        return false;
    }
    const auto started = std::chrono::steady_clock::now();
    const bool recordGpu = aoQueryHeap && aoQueryReadback && !aoQueryInFlight;
    if (recordGpu) commandList->EndQuery(aoQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    const float worldRadius = viewer_ao::WorldRadius(parameters, sceneScale);
    lastAoStrength = std::clamp(parameters.strength, 0.0f, 1.0f);
    lastAoRadiusFraction = parameters.radiusFraction;
    lastAoWorldRadius = worldRadius;
    lastAoPixelRadius = parameters.pixelRadiusMax;
    lastAoSampleCount = parameters.sampleCount;

    const auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                 D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        commandList->ResourceBarrier(1, &barrier);
    };

    // Raw AO is a shader resource between frames; depth is DEPTH_WRITE after
    // the opaque pass. Filtered stays a render target (resolve returns it).
    transition(ao.raw.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(depthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_CPU_DESCRIPTOR_HANDLE rawRtv = ao.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE filteredRtv = rawRtv;
    filteredRtv.ptr += device.Device()->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    commandList->ClearRenderTargetView(rawRtv, white, 0, nullptr);
    commandList->ClearRenderTargetView(filteredRtv, white, 0, nullptr);
    D3D12_VIEWPORT viewport{ viewportRect.x, viewportRect.y, viewportRect.z,
                             viewportRect.w, 0.0f, 1.0f };
    D3D12_RECT scissor{ static_cast<LONG>(viewportRect.x), static_cast<LONG>(viewportRect.y),
                        static_cast<LONG>(viewportRect.x + viewportRect.z),
                        static_cast<LONG>(viewportRect.y + viewportRect.w) };
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    float constants[32]{};
    std::memcpy(constants, &inverseViewProjection, sizeof(inverseViewProjection));
    // The shader maps SV_POSITION to full-target UV and texel sizes, not the
    // smaller 3D viewport rect: the viewport can exclude the toolbar, info
    // panel and bottom bar while the depth texture spans the whole target.
    constants[16] = static_cast<float>((std::max)(1u, swapChain.Width()));
    constants[17] = static_cast<float>((std::max)(1u, swapChain.Height()));
    constants[18] = 0.0f;
    constants[19] = 0.0f;
    constants[20] = worldRadius;
    constants[21] = parameters.pixelRadiusMax;
    constants[22] = parameters.bias;
    constants[23] = lastAoStrength;
    constants[24] = static_cast<float>(parameters.sampleCount);
    constants[25] = static_cast<float>(viewer_ao::kFilterRadius);
    constants[26] = static_cast<float>(aoDebugView);
    constants[28] = eyePosition.x;
    constants[29] = eyePosition.y;
    constants[30] = eyePosition.z;

    ID3D12DescriptorHeap* heaps[] = { ao.srvHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootSignature(aoRootSignature.Get());
    commandList->SetGraphicsRoot32BitConstants(0, 32, constants, 0);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 0, nullptr);
    commandList->IASetIndexBuffer(nullptr);

    commandList->OMSetRenderTargets(1, &rawRtv, FALSE, nullptr);
    commandList->SetPipelineState(aoRawPipelineState.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE depthHandle = ao.srvHeap->GetGPUDescriptorHandleForHeapStart();
    depthHandle.ptr += device.Device()->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    commandList->SetGraphicsRootDescriptorTable(1, depthHandle);
    commandList->DrawInstanced(3, 1, 0, 0);

    transition(ao.raw.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->OMSetRenderTargets(1, &filteredRtv, FALSE, nullptr);
    commandList->SetPipelineState(aoBlurPipelineState.Get());
    commandList->SetGraphicsRootDescriptorTable(1,
        ao.srvHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->DrawInstanced(3, 1, 0, 0);

    transition(depthBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_DEPTH_WRITE);

    if (recordGpu) {
        commandList->EndQuery(aoQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        commandList->ResolveQueryData(aoQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                      0, 2, aoQueryReadback.Get(), 0);
        aoQueryRecorded = true;
        aoQueryInFlight = true;
    }

    lastAoPassMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    return true;
}

void D3D12ViewerPath::PollAoGpuTiming()
{
    if (!aoQueryInFlight || !aoQueryReadback || aoQueryFence == 0
        || directQueue.CompletedValue() < aoQueryFence) {
        return;
    }
    D3D12_RANGE range{ 0, sizeof(uint64_t) * 2 };
    void* bytes = nullptr;
    if (SUCCEEDED(aoQueryReadback->Map(0, &range, &bytes))) {
        uint64_t timestamps[2]{};
        std::memcpy(timestamps, bytes, sizeof(timestamps));
        D3D12_RANGE written{ 0, 0 };
        aoQueryReadback->Unmap(0, &written);
        uint64_t frequency = 0;
        if (SUCCEEDED(directQueue.Queue()->GetTimestampFrequency(&frequency))
            && frequency != 0 && timestamps[1] >= timestamps[0]) {
            lastAoGpuMilliseconds = static_cast<double>(timestamps[1] - timestamps[0])
                * 1000.0 / static_cast<double>(frequency);
        }
    }
    aoQueryInFlight = false;
    aoQueryFence = 0;
}

bool D3D12ViewerPath::CreateShadowTargets()
{
    // The fitted map is viewport-independent. Keep the current device's
    // allocation across resize; Initialize() releases it before a new device is
    // created, so device recovery still rebuilds it.
    if (shadow.available && shadow.depth && shadow.mask) {
        shadowAllocationBytes = shadow.depthBytes + shadow.maskBytes;
        return true;
    }
    shadow.depth.Reset();
    shadow.mask.Reset();
    shadow.dsvHeap.Reset();
    shadow.rtvHeap.Reset();
    shadow.srvHeap.Reset();
    shadow.depthBytes = 0;
    shadow.maskBytes = 0;
    shadow.size = 0;
    shadow.available = false;
    shadow.fallbackReason = ShadowFallbackAllocationFailed;
    shadowAllocationBytes = 0;
    if (shadowAllocationForcedFailure) {
        shadow.fallbackReason = ShadowFallbackForced;
        return false;
    }

    const uint32_t candidates[2] = {
        viewer_ground_shadow::kShadowMapResolution,
        viewer_ground_shadow::kShadowMapFallbackResolution };
    for (uint32_t size : candidates) {
        if (size == 0) continue;
        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvHeapDesc.NumDescriptors = 1;
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.NumDescriptors = 1;
        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.NumDescriptors = 2;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device.Device()->CreateDescriptorHeap(&dsvHeapDesc,
                IID_PPV_ARGS(&shadow.dsvHeap)))
            || FAILED(device.Device()->CreateDescriptorHeap(&rtvHeapDesc,
                IID_PPV_ARGS(&shadow.rtvHeap)))
            || FAILED(device.Device()->CreateDescriptorHeap(&srvHeapDesc,
                IID_PPV_ARGS(&shadow.srvHeap)))) {
            continue;
        }

        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC depthDesc{};
        depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthDesc.Width = size;
        depthDesc.Height = size;
        depthDesc.DepthOrArraySize = 1;
        depthDesc.MipLevels = 1;
        depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
        depthDesc.SampleDesc.Count = 1;
        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE depthClear{};
        depthClear.Format = DXGI_FORMAT_D32_FLOAT;
        depthClear.DepthStencil.Depth = 1.0f;
        D3D12_RESOURCE_DESC maskDesc{};
        maskDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        maskDesc.Width = size;
        maskDesc.Height = size;
        maskDesc.DepthOrArraySize = 1;
        maskDesc.MipLevels = 1;
        maskDesc.Format = DXGI_FORMAT_R8_UNORM;
        maskDesc.SampleDesc.Count = 1;
        maskDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE maskClear{};
        maskClear.Format = DXGI_FORMAT_R8_UNORM;
        // Both resources start as shader resources so every frame can run the
        // same PIXEL_SHADER_RESOURCE -> render/depth-write -> resource cycle.
        if (FAILED(device.Device()->CreateCommittedResource(
                &properties, D3D12_HEAP_FLAG_NONE, &depthDesc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &depthClear,
                IID_PPV_ARGS(&shadow.depth)))
            || FAILED(device.Device()->CreateCommittedResource(
                &properties, D3D12_HEAP_FLAG_NONE, &maskDesc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &maskClear,
                IID_PPV_ARGS(&shadow.mask)))) {
            shadow.depth.Reset();
            shadow.mask.Reset();
            continue;
        }
        shadow.depthBytes = device.Device()->GetResourceAllocationInfo(0, 1, &depthDesc).SizeInBytes;
        shadow.maskBytes = device.Device()->GetResourceAllocationInfo(0, 1, &maskDesc).SizeInBytes;

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device.Device()->CreateDepthStencilView(shadow.depth.Get(), &dsvDesc,
            shadow.dsvHeap->GetCPUDescriptorHandleForHeapStart());
        device.Device()->CreateRenderTargetView(shadow.mask.Get(), nullptr,
            shadow.rtvHeap->GetCPUDescriptorHandleForHeapStart());
        const UINT srvSize = device.Device()->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv{};
        depthSrv.Format = DXGI_FORMAT_R32_FLOAT;
        depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE depthHandle =
            shadow.srvHeap->GetCPUDescriptorHandleForHeapStart();
        device.Device()->CreateShaderResourceView(shadow.depth.Get(), &depthSrv, depthHandle);
        D3D12_SHADER_RESOURCE_VIEW_DESC maskSrv{};
        maskSrv.Format = DXGI_FORMAT_R8_UNORM;
        maskSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        maskSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        maskSrv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE maskHandle =
            shadow.srvHeap->GetCPUDescriptorHandleForHeapStart();
        maskHandle.ptr += srvSize;
        device.Device()->CreateShaderResourceView(shadow.mask.Get(), &maskSrv, maskHandle);

        shadow.size = size;
        shadow.available = true;
        shadow.fallbackReason = ShadowFallbackNone;
        shadowAllocationBytes = shadow.depthBytes + shadow.maskBytes;
        return true;
    }
    shadow.fallbackReason = ShadowFallbackAllocationFailed;
    return false;
}

void D3D12ViewerPath::WriteTransmissionSrv(ID3D12DescriptorHeap& heap, UINT index,
                                           UINT descriptorSize)
{    D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap.GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(index) * descriptorSize;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = kSceneColorFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    if (transmission.available) {
        srv.Texture2D.MipLevels = transmission.mipCount;
        device.Device()->CreateShaderResourceView(transmission.sceneCopy.Get(), &srv, cpu);
    } else if (sceneColor) {
        // The root table must always hold a valid descriptor. The shader gates
        // transmission on gTransmissionFrame.w and never samples this fallback.
        device.Device()->CreateShaderResourceView(sceneColor.Get(), &srv, cpu);
    }
}

void D3D12ViewerPath::RefreshTransmissionDescriptors()
{
    if (model.srvHeap) {
        WriteTransmissionSrv(*model.srvHeap.Get(), model.transmissionDescriptorIndex,
                             model.srvDescriptorSize);
    }
    if (pendingModel.srvHeap) {
        WriteTransmissionSrv(*pendingModel.srvHeap.Get(), pendingModel.transmissionDescriptorIndex,
                             pendingModel.srvDescriptorSize);
    }
}

void D3D12ViewerPath::GenerateTransmissionMips()
{
    if (!transmission.available || !transmission.sceneCopy || !transmission.mipSrvHeap
        || !transmissionPipelineState || !transmissionRootSignature || !commandList
        || transmission.mipCount < 2) {
        return;
    }
    const auto transitionSubresource=[&](ID3D12Resource* resource,UINT subresource,
                                          D3D12_RESOURCE_STATES before,
                                          D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = subresource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        commandList->ResourceBarrier(1, &barrier);
    };

    // Snapshot the finished opaque scene. This is the only writer of the
    // transmission copy; every transmissive pass below only reads it, so a
    // render target is never sampled while bound for writing.
    transitionSubresource(sceneColor.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transitionSubresource(transmission.sceneCopy.Get(), 0,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = sceneColor.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = transmission.sceneCopy.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;
    commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    transitionSubresource(sceneColor.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    transitionSubresource(transmission.sceneCopy.Get(), 0,
                          D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    const UINT descriptorSize=device.Device()->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT rtvSize=device.Device()->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ID3D12DescriptorHeap* heaps[] = { transmission.mipSrvHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootSignature(transmissionRootSignature.Get());
    commandList->SetPipelineState(transmissionPipelineState.Get());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 0, nullptr);
    commandList->IASetIndexBuffer(nullptr);
    UINT mipWidth = swapChain.Width();
    UINT mipHeight = swapChain.Height();
    for (UINT mip = 1; mip < transmission.mipCount; ++mip) {
        transitionSubresource(transmission.sceneCopy.Get(), mip,
                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                              D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = transmission.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(mip - 1) * rtvSize;
        commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        mipWidth = (std::max)(1u, mipWidth >> 1);
        mipHeight = (std::max)(1u, mipHeight >> 1);
        D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(mipWidth),
                                 static_cast<float>(mipHeight), 0.0f, 1.0f };
        D3D12_RECT scissor{ 0, 0, static_cast<LONG>(mipWidth), static_cast<LONG>(mipHeight) };
        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissor);
        D3D12_GPU_DESCRIPTOR_HANDLE srv =
            transmission.mipSrvHeap->GetGPUDescriptorHandleForHeapStart();
        srv.ptr += SIZE_T(mip - 1) * descriptorSize;
        commandList->SetGraphicsRootDescriptorTable(0, srv);
        commandList->DrawInstanced(3, 1, 0, 0);
        transitionSubresource(transmission.sceneCopy.Get(), mip,
                              D3D12_RESOURCE_STATE_RENDER_TARGET,
                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
}


bool D3D12ViewerPath::PollPick(bool& hit)
{
    if (!pickInFlight || directQueue.CompletedValue() < pickFence) return false;
    D3D12_RANGE range{0,sizeof(uint32_t)}; void* bytes = nullptr;
    hit = false; lastPickedId = 0;
    if (SUCCEEDED(pickReadback->Map(0, &range, &bytes))) {
        std::memcpy(&lastPickedId,bytes,sizeof(lastPickedId));
        hit = lastPickedId != 0;
        D3D12_RANGE written{0,0}; pickReadback->Unmap(0, &written);
    }
    pickInFlight = false; return true;
}

void D3D12ViewerPath::RequestDebugSample(int x0, int y0, int x1, int y1)
{
    const int width = static_cast<int>(swapChain.Width());
    const int height = static_cast<int>(swapChain.Height());
    x0 = std::clamp(x0, 0, std::max(0, width - 1));
    y0 = std::clamp(y0, 0, std::max(0, height - 1));
    x1 = std::clamp(x1, x0 + 1, std::min(width, x0 + static_cast<int>(kDebugSampleMaxExtent)));
    y1 = std::clamp(y1, y0 + 1, std::min(height, y0 + static_cast<int>(kDebugSampleMaxExtent)));
    debugSampleX0 = x0; debugSampleY0 = y0; debugSampleX1 = x1; debugSampleY1 = y1;
    debugSampleState = 1;
    debugSampleArmed = true;
    debugSampleRecorded = false;
    debugSampleFence = 0;
}

int D3D12ViewerPath::PollDebugSample(DebugSample& out)
{
    if (debugSampleState == 1 && debugSampleRecorded
        && debugSampleFence != 0 && directQueue.CompletedValue() >= debugSampleFence) {
        const UINT sampleWidth = static_cast<UINT>(debugSampleX1 - debugSampleX0);
        const UINT sampleHeight = static_cast<UINT>(debugSampleY1 - debugSampleY0);
        const UINT rowPitch = static_cast<UINT>(kDebugSampleMaxExtent) * 8;
        D3D12_RANGE range{0, rowPitch * sampleHeight}; void* bytes = nullptr;
        if (SUCCEEDED(debugReadback->Map(0, &range, &bytes))) {
            double sums[4]{};
            const auto* pixels = static_cast<const uint16_t*>(bytes);
            const UINT step = std::max<UINT>(1, std::max(sampleWidth, sampleHeight) / 17);
            UINT count = 0;
            for (UINT y = 0; y < sampleHeight; y += step) {
                for (UINT x = 0; x < sampleWidth; x += step) {
                    const uint16_t* texel = pixels + size_t(y) * (rowPitch / 2) + size_t(x) * 4;
                    for (int channel = 0; channel < 4; ++channel)
                        sums[channel] += DirectX::PackedVector::XMConvertHalfToFloat(texel[channel]);
                    ++count;
                }
            }
            D3D12_RANGE written{0,0}; debugReadback->Unmap(0, &written);
            if (count) {
                for (int channel = 0; channel < 4; ++channel)
                    out.rgba[channel] = static_cast<float>(sums[channel] / count);
                out.width = sampleWidth; out.height = sampleHeight;
                debugSample = out;
                debugSampleState = 2;
            } else debugSampleState = 3;
        } else debugSampleState = 3;
        debugSampleRecorded = false;
    }
    out = debugSample;
    return debugSampleState;
}

bool D3D12ViewerPath::CreatePipeline(std::wstring& error)
{
    D3D12_ROOT_PARAMETER rootParam{};
    rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParam.Descriptor.ShaderRegister = 0;
    rootParam.Descriptor.RegisterSpace = 0;
    rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc{};
    D3D12_ROOT_PARAMETER params[3] = {rootParam, {}, {}};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 1; params[1].Constants.Num32BitValues = 33;
    // The ground-shadow pixel shader reads its constants from b1 too; the
    // compact/render pipelines only read it from the vertex stage.
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // The compact STEP vertex format still carries an authored material per
    // draw. Keep its color in root constants rather than expanding millions
    // of vertices to the full textured layout.
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 2; params[2].Constants.Num32BitValues = 4;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootSigDesc.NumParameters = 3;
    rootSigDesc.pParameters = params;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signatureBlob;
    ComPtr<ID3DBlob> signatureErrorBlob;
    if (FAILED(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob,
                                            &signatureErrorBlob))) {
        error = L"The D3D12 root signature could not be serialized.";
        return false;
    }
    if (FAILED(device.Device()->CreateRootSignature(0, signatureBlob->GetBufferPointer(),
                                                      signatureBlob->GetBufferSize(),
                                                      IID_PPV_ARGS(&rootSignature)))) {
        error = L"The D3D12 root signature could not be created.";
        return false;
    }

    ComPtr<ID3DBlob> vsBlob;
    if (!CompileShader(kVertexShaderSource, sizeof(kVertexShaderSource) - 1, "VSMain", "vs_5_1", vsBlob, error))
        return false;
    ComPtr<ID3DBlob> psBlob;
    if (!CompileShader(kPixelShaderSource, sizeof(kPixelShaderSource) - 1, "PSMain", "ps_5_1", psBlob, error))
        return false;
    // T32 opaque variant: scene + indirect + pick. The legacy two-target blob
    // stays available for the AO-off low-memory fallback.
    D3D_SHADER_MACRO aoDefines[] = { {"AO_INDIRECT", "1"}, {nullptr, nullptr} };
    ComPtr<ID3DBlob> aoPsBlob;
    if (!CompileShader(kPixelShaderSource, sizeof(kPixelShaderSource) - 1, "PSMain", "ps_5_1",
                       aoPsBlob, error, aoDefines))
        return false;

    D3D12_INPUT_ELEMENT_DESC inputElements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    // No culling for this slice -- the wire format doesn't guarantee a
    // winding convention this pipeline has verified against the camera's
    // handedness yet, and an invisible-due-to-winding mesh is a worse
    // failure mode than an occasionally-visible backface. Revisit once a
    // real material/culling policy exists.
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.FrontCounterClockwise = FALSE;
    rasterizer.DepthClipEnable = TRUE;

    D3D12_BLEND_DESC blend{};
    // Color may blend, but the second MRT is an integer pick ID and must
    // never inherit target 0's blend state. Recent drivers reject that PSO
    // combination instead of deferring the mismatch until draw time.
    blend.IndependentBlendEnable = TRUE;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    blend.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_DEPTH_STENCIL_DESC depthStencil{};
    depthStencil.DepthEnable = TRUE;
    depthStencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencil.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    depthStencil.StencilEnable = FALSE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSignature.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.BlendState = blend;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState = rasterizer;
    psoDesc.DepthStencilState = depthStencil;
    psoDesc.InputLayout = { inputElements, static_cast<UINT>(std::size(inputElements)) };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 2;
    psoDesc.RTVFormats[0] = kSceneColorFormat;
    psoDesc.RTVFormats[1] = DXGI_FORMAT_R32_UINT;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;

    if (FAILED(device.Device()->CreateGraphicsPipelineState(&psoDesc,
            IID_PPV_ARGS(&pipelineStateLegacy)))) {
        error = L"The D3D12 pipeline state could not be created.";
        return false;
    }
    auto clayPsoDesc = psoDesc;
    clayPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    // Pull Clay's overridden surfaces a bounded step toward the camera so an
    // authored glass layer coincident with its trim/body wins deterministically
    // instead of speckling along shared edges.
    clayPsoDesc.RasterizerState.DepthBias = -1000;
    clayPsoDesc.RasterizerState.SlopeScaledDepthBias = -1.0f;
    clayPsoDesc.RasterizerState.DepthBiasClamp = 0.0f;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&clayPsoDesc,
                                                             IID_PPV_ARGS(&clayPipelineStateLegacy)))) {
        error = L"The compact Clay pipeline state could not be created.";
        return false;
    }
    // T32 AO target layout for opaque compact geometry.
    auto aoPsoDesc = psoDesc;
    aoPsoDesc.PS = { aoPsBlob->GetBufferPointer(), aoPsBlob->GetBufferSize() };
    aoPsoDesc.NumRenderTargets = 3;
    aoPsoDesc.RTVFormats[1] = kSceneColorFormat;
    aoPsoDesc.RTVFormats[2] = DXGI_FORMAT_R32_UINT;
    aoPsoDesc.BlendState.RenderTarget[2].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&aoPsoDesc,
            IID_PPV_ARGS(&pipelineState)))) {
        error = L"The compact AO pipeline state could not be created.";
        return false;
    }
    auto clayAoPsoDesc = aoPsoDesc;
    clayAoPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    clayAoPsoDesc.RasterizerState.DepthBias = -1000;
    clayAoPsoDesc.RasterizerState.SlopeScaledDepthBias = -1.0f;
    clayAoPsoDesc.RasterizerState.DepthBiasClamp = 0.0f;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&clayAoPsoDesc,
            IID_PPV_ARGS(&clayPipelineState)))) {
        error = L"The compact Clay AO pipeline state could not be created.";
        return false;
    }
    auto blendPsoDesc = psoDesc;
    auto& colorBlend = blendPsoDesc.BlendState.RenderTarget[0];
    colorBlend.BlendEnable = TRUE;
    colorBlend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    colorBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    colorBlend.BlendOp = D3D12_BLEND_OP_ADD;
    colorBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
    colorBlend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    colorBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&blendPsoDesc, IID_PPV_ARGS(&blendPipelineState)))) {
        error = L"The compact-material blend pipeline state could not be created.";
        return false;
    }
    // T26 compact-material OIT and nearest-visible pick variants.
    D3D_SHADER_MACRO oitDefines[] = { {"OIT_PASS", "1"}, {nullptr, nullptr} };
    ComPtr<ID3DBlob> oitPsBlob;
    if (!CompileShader(kPixelShaderSource, sizeof(kPixelShaderSource) - 1, "PSMain", "ps_5_1",
                       oitPsBlob, error, oitDefines))
        return false;
    auto oitPsoDesc = psoDesc;
    oitPsoDesc.PS = { oitPsBlob->GetBufferPointer(), oitPsBlob->GetBufferSize() };
    oitPsoDesc.RTVFormats[0] = kSceneColorFormat;
    oitPsoDesc.RTVFormats[1] = kSceneColorFormat;
    oitPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    oitPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    auto& oitAccum = oitPsoDesc.BlendState.RenderTarget[0];
    oitAccum.BlendEnable = TRUE;
    oitAccum.SrcBlend = D3D12_BLEND_ONE; oitAccum.DestBlend = D3D12_BLEND_ONE;
    oitAccum.BlendOp = D3D12_BLEND_OP_ADD;
    oitAccum.SrcBlendAlpha = D3D12_BLEND_ONE; oitAccum.DestBlendAlpha = D3D12_BLEND_ONE;
    oitAccum.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    auto& oitReveal = oitPsoDesc.BlendState.RenderTarget[1];
    oitReveal.BlendEnable = TRUE;
    oitReveal.SrcBlend = D3D12_BLEND_ZERO; oitReveal.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    oitReveal.BlendOp = D3D12_BLEND_OP_ADD;
    oitReveal.SrcBlendAlpha = D3D12_BLEND_ZERO; oitReveal.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    oitReveal.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&oitPsoDesc, IID_PPV_ARGS(&oitPipelineState)))) {
        error = L"The compact-material OIT pipeline state could not be created.";
        return false;
    }
    auto pickPsoDesc = psoDesc;
    pickPsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
    pickPsoDesc.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pickPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pickPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&pickPsoDesc, IID_PPV_ARGS(&pickPipelineState)))) {
        error = L"The compact-material nearest-visible pick pipeline state could not be created.";
        return false;
    }
    auto wirePsoDesc = psoDesc;
    wirePsoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    wirePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    wirePsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    wirePsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    wirePsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&wirePsoDesc, IID_PPV_ARGS(&wireframePipelineState)))) {
        error = L"The wireframe overlay pipeline state could not be created.";
        return false;
    }
    ComPtr<ID3DBlob> gridVsBlob;
    ComPtr<ID3DBlob> gridPsBlob;
    if (!CompileShader(kGridVertexShaderSource, sizeof(kGridVertexShaderSource) - 1,
                       "VSMain", "vs_5_1", gridVsBlob, error)
        || !CompileShader(kGridPixelShaderSource, sizeof(kGridPixelShaderSource) - 1,
                          "PSMain", "ps_5_1", gridPsBlob, error)) {
        return false;
    }
    auto gridPsoDesc = psoDesc;
    gridPsoDesc.VS = { gridVsBlob->GetBufferPointer(), gridVsBlob->GetBufferSize() };
    gridPsoDesc.PS = { gridPsBlob->GetBufferPointer(), gridPsBlob->GetBufferSize() };
    gridPsoDesc.InputLayout = {};
    gridPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    auto& gridBlend = gridPsoDesc.BlendState.RenderTarget[0];
    gridBlend.BlendEnable = TRUE;
    gridBlend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    gridBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    gridBlend.BlendOp = D3D12_BLEND_OP_ADD;
    gridBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
    gridBlend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    gridBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&gridPsoDesc, IID_PPV_ARGS(&gridPipelineState)))) {
        error = L"The ground-grid pipeline state could not be created.";
        return false;
    }
    ComPtr<ID3DBlob> groundVsBlob;
    ComPtr<ID3DBlob> groundPsBlob;
    if (!CompileShader(kGroundVertexShaderSource, sizeof(kGroundVertexShaderSource) - 1,
                       "VSMain", "vs_5_1", groundVsBlob, error)
        || !CompileShader(kGroundPixelShaderSource, sizeof(kGroundPixelShaderSource) - 1,
                          "PSMain", "ps_5_1", groundPsBlob, error)) {
        return false;
    }
    auto groundPsoDesc = psoDesc;
    groundPsoDesc.VS = { groundVsBlob->GetBufferPointer(), groundVsBlob->GetBufferSize() };
    groundPsoDesc.PS = { groundPsBlob->GetBufferPointer(), groundPsBlob->GetBufferSize() };
    groundPsoDesc.InputLayout = {};
    groundPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    // T21a: the highlight/shadow layer renders into the display-space backdrop,
    // not the linear HDR scene. Premultiplied source-over is exact there.
    groundPsoDesc.NumRenderTargets = 1;
    groundPsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    groundPsoDesc.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
    groundPsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    auto& groundBlend = groundPsoDesc.BlendState.RenderTarget[0];
    groundBlend.BlendEnable = TRUE;
    groundBlend.SrcBlend = D3D12_BLEND_ONE;
    groundBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    groundBlend.BlendOp = D3D12_BLEND_OP_ADD;
    groundBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
    groundBlend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    groundBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    groundPsoDesc.DepthStencilState.DepthEnable = FALSE;
    groundPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    groundPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&groundPsoDesc,
                                                             IID_PPV_ARGS(&groundPipelineState)))) {
        error = L"The ground-shadow pipeline state could not be created.";
        return false;
    }
    std::string positionOnlySource(kVertexShaderSource);
    auto positionOnlyBegin=positionOnlySource.find("    float3 normal : NORMAL;\n    float2 uv : TEXCOORD0;");
    positionOnlySource.erase(positionOnlyBegin,std::string("    float3 normal : NORMAL;\n    float2 uv : TEXCOORD0;").size());
    auto positionOnlyNormal=positionOnlySource.find("input.normal");
    positionOnlySource.replace(positionOnlyNormal,std::string("input.normal").size(),"float3(0,0,1)");
    ComPtr<ID3DBlob> positionOnlyVs, pointVs, coloredPointVs, pointGs, pointPs;
    if (!CompileShader(positionOnlySource.data(),positionOnlySource.size(),"VSMain","vs_5_1",positionOnlyVs,error)) return false;
    if (!CompileShader(kPointVertexShaderSource,sizeof(kPointVertexShaderSource)-1,"VSMain","vs_5_1",pointVs,error)
        || !CompileShader(kColoredPointVertexShaderSource,sizeof(kColoredPointVertexShaderSource)-1,"VSMain","vs_5_1",coloredPointVs,error)
        || !CompileShader(kPointGeometryShaderSource,sizeof(kPointGeometryShaderSource)-1,"GSMain","gs_5_1",pointGs,error)
        || !CompileShader(kPointPixelShaderSource,sizeof(kPointPixelShaderSource)-1,"PSMain","ps_5_1",pointPs,error)) return false;
    psoDesc.VS = {pointVs->GetBufferPointer(), pointVs->GetBufferSize()};
    psoDesc.GS = {pointGs->GetBufferPointer(), pointGs->GetBufferSize()};
    psoDesc.PS = {pointPs->GetBufferPointer(), pointPs->GetBufferSize()};
    psoDesc.InputLayout.NumElements = 1;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pointPipelineState)))) {
        error = L"The point pipeline could not be created."; return false;
    }
    D3D12_INPUT_ELEMENT_DESC completeElements[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TANGENT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,32,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,48,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
    };
    psoDesc.VS={coloredPointVs->GetBufferPointer(),coloredPointVs->GetBufferSize()};
    psoDesc.InputLayout={completeElements,static_cast<UINT>(std::size(completeElements))};
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&psoDesc,IID_PPV_ARGS(&coloredPointPipelineState)))) {
        error=L"The colored point pipeline could not be created.";return false;
    }
    // Position-only triangles retain the original neutral shader.
    psoDesc.GS={};psoDesc.PS={psBlob->GetBufferPointer(),psBlob->GetBufferSize()};
    psoDesc.VS={positionOnlyVs->GetBufferPointer(),positionOnlyVs->GetBufferSize()};
    psoDesc.InputLayout={inputElements,1};psoDesc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&psoDesc,IID_PPV_ARGS(&positionOnlyPipelineState)))) {
        error=L"The position-only mesh pipeline could not be created.";return false;
    }
    auto positionOnlyAoDesc=psoDesc;
    positionOnlyAoDesc.PS={aoPsBlob->GetBufferPointer(),aoPsBlob->GetBufferSize()};
    positionOnlyAoDesc.NumRenderTargets=3;
    positionOnlyAoDesc.RTVFormats[1]=kSceneColorFormat;
    positionOnlyAoDesc.RTVFormats[2]=DXGI_FORMAT_R32_UINT;
    positionOnlyAoDesc.BlendState.RenderTarget[2].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&positionOnlyAoDesc,
            IID_PPV_ARGS(&positionOnlyAoPipelineState)))) {
        error=L"The position-only AO mesh pipeline could not be created.";return false;
    }
    wirePsoDesc.VS={positionOnlyVs->GetBufferPointer(),positionOnlyVs->GetBufferSize()};
    wirePsoDesc.InputLayout={inputElements,1};
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&wirePsoDesc,IID_PPV_ARGS(&positionOnlyWireframePipelineState)))) {
        error=L"The position-only wireframe overlay pipeline state could not be created.";return false;
    }

    // T17 line pipeline. Position-only input layout keeps it usable for every
    // normalized vertex layout (the vertex-buffer stride comes from the VBV).
    // Negative depth bias plus LESS_EQUAL keeps a line visible where it is
    // coplanar with the surface it outlines without letting it write depth.
    ComPtr<ID3DBlob> lineVsBlob, linePsBlob;
    if (!CompileShader(kLineVertexShaderSource, sizeof(kLineVertexShaderSource)-1,
                       "VSMain","vs_5_1",lineVsBlob,error)
        || !CompileShader(kLinePixelShaderSource, sizeof(kLinePixelShaderSource)-1,
                          "PSMain","ps_5_1",linePsBlob,error)) return false;
    auto linePsoDesc = psoDesc;
    linePsoDesc.VS = {lineVsBlob->GetBufferPointer(),lineVsBlob->GetBufferSize()};
    linePsoDesc.PS = {linePsBlob->GetBufferPointer(),linePsBlob->GetBufferSize()};
    linePsoDesc.GS = {};
    linePsoDesc.InputLayout = {inputElements,1};
    linePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    linePsoDesc.RasterizerState.DepthBias = -1000;
    linePsoDesc.RasterizerState.SlopeScaledDepthBias = -1.0f;
    linePsoDesc.RasterizerState.DepthBiasClamp = -0.01f;
    linePsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    linePsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&linePsoDesc,IID_PPV_ARGS(&linePipelineState)))) {
        error=L"The line pipeline state could not be created.";return false;
    }

    return true;
}

bool D3D12ViewerPath::CreateShadowPipelines(std::wstring& error)
{
    // Depth-only caster pass. b0 = frame CBV (unused), b1 = world-from-model
    // draw constants, b3 = light view-projection + caster attenuation, b2 =
    // material record, t0/s0 = authored base colour for MASK cutouts.
    D3D12_ROOT_PARAMETER casterParams[5]{};
    casterParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    casterParams[0].Descriptor.ShaderRegister = 0;
    casterParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    casterParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    casterParams[1].Constants.ShaderRegister = 1;
    casterParams[1].Constants.Num32BitValues = 33;
    casterParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    casterParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    casterParams[2].Constants.ShaderRegister = 3;
    casterParams[2].Constants.Num32BitValues = 20;
    casterParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    casterParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    casterParams[3].Descriptor.ShaderRegister = 2;
    casterParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_DESCRIPTOR_RANGE casterRange{};
    casterRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    casterRange.NumDescriptors = 1;
    casterRange.BaseShaderRegister = 0;
    casterRange.RegisterSpace = 0;
    casterRange.OffsetInDescriptorsFromTableStart = 0;
    casterParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    casterParams[4].DescriptorTable.NumDescriptorRanges = 1;
    casterParams[4].DescriptorTable.pDescriptorRanges = &casterRange;
    casterParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC casterSampler{};
    casterSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    casterSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    casterSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    casterSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    casterSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    casterSampler.MaxLOD = D3D12_FLOAT32_MAX;
    casterSampler.ShaderRegister = 0;
    casterSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC casterRootDesc{};
    casterRootDesc.NumParameters = 5;
    casterRootDesc.pParameters = casterParams;
    casterRootDesc.NumStaticSamplers = 1;
    casterRootDesc.pStaticSamplers = &casterSampler;
    casterRootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> casterSignature;
    ComPtr<ID3DBlob> casterSignatureError;
    if (FAILED(D3D12SerializeRootSignature(&casterRootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                            &casterSignature, &casterSignatureError))) {
        error = L"The shadow caster root signature could not be serialized.";
        return false;
    }
    if (FAILED(device.Device()->CreateRootSignature(0, casterSignature->GetBufferPointer(),
            casterSignature->GetBufferSize(), IID_PPV_ARGS(&shadowRootSignature)))) {
        error = L"The shadow caster root signature could not be created.";
        return false;
    }

    ComPtr<ID3DBlob> shadowVs;
    ComPtr<ID3DBlob> shadowOpaqueVs;
    ComPtr<ID3DBlob> shadowPs;
    ComPtr<ID3DBlob> shadowMaskPs;
    if (!CompileShader(kShadowVertexShaderSource, sizeof(kShadowVertexShaderSource) - 1,
                       "VSMain", "vs_5_1", shadowVs, error)
        || !CompileShader(kShadowOpaqueVertexShaderSource,
                          sizeof(kShadowOpaqueVertexShaderSource) - 1,
                          "VSMain", "vs_5_1", shadowOpaqueVs, error)
        || !CompileShader(kShadowPixelShaderSource, sizeof(kShadowPixelShaderSource) - 1,
                          "PSMain", "ps_5_1", shadowPs, error)
        || !CompileShader(kShadowMaskPixelShaderSource, sizeof(kShadowMaskPixelShaderSource) - 1,
                          "PSMain", "ps_5_1", shadowMaskPs, error)) {
        return false;
    }

    D3D12_INPUT_ELEMENT_DESC shadowPositionOnly[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_INPUT_ELEMENT_DESC shadowMasked[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC casterPso{};
    casterPso.pRootSignature = shadowRootSignature.Get();
    casterPso.VS = { shadowOpaqueVs->GetBufferPointer(), shadowOpaqueVs->GetBufferSize() };
    casterPso.PS = { shadowPs->GetBufferPointer(), shadowPs->GetBufferSize() };
    casterPso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    casterPso.SampleMask = UINT_MAX;
    casterPso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    // Mirrored and double-sided casters must still block the sun; an authored
    // winding convention is not a shadow culling contract.
    casterPso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    casterPso.RasterizerState.FrontCounterClockwise = FALSE;
    casterPso.RasterizerState.DepthClipEnable = TRUE;
    // Push the recorded caster slightly away from the light; the pixel shader
    // adds the bound, scale-invariant normalized bias on top.
    casterPso.RasterizerState.DepthBias = 200;
    casterPso.RasterizerState.SlopeScaledDepthBias = 2.0f;
    casterPso.RasterizerState.DepthBiasClamp = 0.0f;
    casterPso.DepthStencilState.DepthEnable = TRUE;
    casterPso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    casterPso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    casterPso.DepthStencilState.StencilEnable = FALSE;
    casterPso.InputLayout = { shadowPositionOnly,
        static_cast<UINT>(std::size(shadowPositionOnly)) };
    casterPso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    casterPso.NumRenderTargets = 1;
    casterPso.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
    casterPso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    casterPso.SampleDesc.Count = 1;
    HRESULT shadowPsoResult = device.Device()->CreateGraphicsPipelineState(&casterPso,
        IID_PPV_ARGS(&shadowPipelineState));
    if (FAILED(shadowPsoResult)) {
        error = L"The shadow caster pipeline state could not be created (HRESULT "
            + std::to_wstring(static_cast<long>(shadowPsoResult)) + L").";
        return false;
    }
    casterPso.PS = { shadowMaskPs->GetBufferPointer(), shadowMaskPs->GetBufferSize() };
    casterPso.VS = { shadowVs->GetBufferPointer(), shadowVs->GetBufferSize() };
    casterPso.InputLayout = { shadowMasked, static_cast<UINT>(std::size(shadowMasked)) };
    shadowPsoResult = device.Device()->CreateGraphicsPipelineState(&casterPso,
        IID_PPV_ARGS(&shadowMaskPipelineState));
    if (FAILED(shadowPsoResult)) {
        error = L"The masked shadow caster pipeline state could not be created (HRESULT "
            + std::to_wstring(static_cast<long>(shadowPsoResult)) + L").";
        return false;
    }

    // Ground layer that samples the fitted map. b0 = frame VP (quad position),
    // b1 = ground/texel/matrix constants, t0/t1 = depth and caster attenuation.
    D3D12_ROOT_PARAMETER groundParams[3]{};
    groundParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    groundParams[0].Descriptor.ShaderRegister = 0;
    groundParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    groundParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    groundParams[1].Constants.ShaderRegister = 1;
    groundParams[1].Constants.Num32BitValues = 28;
    groundParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_DESCRIPTOR_RANGE groundRange{};
    groundRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    groundRange.NumDescriptors = 2;
    groundRange.BaseShaderRegister = 0;
    groundRange.RegisterSpace = 0;
    groundRange.OffsetInDescriptorsFromTableStart = 0;
    groundParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    groundParams[2].DescriptorTable.NumDescriptorRanges = 1;
    groundParams[2].DescriptorTable.pDescriptorRanges = &groundRange;
    groundParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC groundSamplers[2]{};
    groundSamplers[0].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    groundSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    groundSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    groundSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    groundSamplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    groundSamplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    groundSamplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    groundSamplers[0].ShaderRegister = 0;
    groundSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    groundSamplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    groundSamplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    groundSamplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    groundSamplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    groundSamplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    groundSamplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    groundSamplers[1].ShaderRegister = 1;
    groundSamplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC groundRootDesc{};
    groundRootDesc.NumParameters = 3;
    groundRootDesc.pParameters = groundParams;
    groundRootDesc.NumStaticSamplers = 2;
    groundRootDesc.pStaticSamplers = groundSamplers;
    groundRootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> groundSignature;
    ComPtr<ID3DBlob> groundSignatureError;
    if (FAILED(D3D12SerializeRootSignature(&groundRootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                            &groundSignature, &groundSignatureError))) {
        error = L"The shadow ground root signature could not be serialized.";
        return false;
    }
    if (FAILED(device.Device()->CreateRootSignature(0, groundSignature->GetBufferPointer(),
            groundSignature->GetBufferSize(), IID_PPV_ARGS(&shadowGroundRootSignature)))) {
        error = L"The shadow ground root signature could not be created.";
        return false;
    }
    ComPtr<ID3DBlob> groundVs;
    ComPtr<ID3DBlob> shadowGroundPs;
    if (!CompileShader(kGroundVertexShaderSource, sizeof(kGroundVertexShaderSource) - 1,
                       "VSMain", "vs_5_1", groundVs, error)
        || !CompileShader(kShadowGroundPixelShaderSource,
                          sizeof(kShadowGroundPixelShaderSource) - 1,
                          "PSMain", "ps_5_1", shadowGroundPs, error)) {
        return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC shadowGroundPso{};
    shadowGroundPso.pRootSignature = shadowGroundRootSignature.Get();
    shadowGroundPso.VS = { groundVs->GetBufferPointer(), groundVs->GetBufferSize() };
    shadowGroundPso.PS = { shadowGroundPs->GetBufferPointer(), shadowGroundPs->GetBufferSize() };
    shadowGroundPso.BlendState.IndependentBlendEnable = TRUE;
    auto& shadowBlend = shadowGroundPso.BlendState.RenderTarget[0];
    shadowBlend.BlendEnable = TRUE;
    shadowBlend.SrcBlend = D3D12_BLEND_ONE;
    shadowBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    shadowBlend.BlendOp = D3D12_BLEND_OP_ADD;
    shadowBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
    shadowBlend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    shadowBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    shadowBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    shadowGroundPso.BlendState.RenderTarget[1].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;
    shadowGroundPso.SampleMask = UINT_MAX;
    shadowGroundPso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    shadowGroundPso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    shadowGroundPso.RasterizerState.DepthClipEnable = TRUE;
    shadowGroundPso.DepthStencilState.DepthEnable = FALSE;
    shadowGroundPso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    shadowGroundPso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    shadowGroundPso.InputLayout = {};
    shadowGroundPso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    shadowGroundPso.NumRenderTargets = 2;
    shadowGroundPso.RTVFormats[0] = kSceneColorFormat;
    shadowGroundPso.RTVFormats[1] = DXGI_FORMAT_R32_UINT;
    shadowGroundPso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    shadowGroundPso.SampleDesc.Count = 1;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&shadowGroundPso,
            IID_PPV_ARGS(&shadowGroundPipelineState)))) {
        error = L"The geometry-derived ground shadow pipeline state could not be created.";
        return false;
    }
    return true;
}

bool D3D12ViewerPath::CreateTexturedPipeline(std::wstring& error)
{
    // T22/T24/T25/T27 root layout. Sixteen SRV tables (the thirteen material
    // slots, the studio cubemap, the split-sum BRDF LUT and the T27
    // transmitted-radiance pyramid) plus the 25-DWORD b1 draw block and two
    // CBVs (b0 frame, b2 material) total 45 DWORDs, well inside D3D12's
    // 64-DWORD limit. The material record itself stays a fixed b2 CBV: adding
    // map slots never appends root constants.
    D3D12_ROOT_PARAMETER rootParams[19]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE srvRanges[16]{};
    const UINT materialRootParameters[model_core::kMaterialTextureSlotCount] = {
        1, 2, 3, 4, 6, 10, 11, 12, 13, 14, 15, 16, 17};
    // The first five slots occupy their historical registers t0..t3/t5; the
    // environment cube (t4) and BRDF LUT (t6) sit between them, and the eight
    // T25 extension maps are t7..t14.
    const UINT materialShaderRegisters[model_core::kMaterialTextureSlotCount] = {
        0, 1, 2, 3, 5, 7, 8, 9, 10, 11, 12, 13, 14};
    for (UINT i=0;i<model_core::kMaterialTextureSlotCount;++i) {
        srvRanges[i].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[i].NumDescriptors=1;srvRanges[i].BaseShaderRegister=materialShaderRegisters[i];
        srvRanges[i].OffsetInDescriptorsFromTableStart=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        rootParams[materialRootParameters[i]].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[materialRootParameters[i]].DescriptorTable.NumDescriptorRanges=1;
        rootParams[materialRootParameters[i]].DescriptorTable.pDescriptorRanges=&srvRanges[i];
        rootParams[materialRootParameters[i]].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    }
    srvRanges[13].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRanges[13].NumDescriptors=1;srvRanges[13].BaseShaderRegister=4;
    srvRanges[13].OffsetInDescriptorsFromTableStart=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    rootParams[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[5].DescriptorTable.NumDescriptorRanges=1;
    rootParams[5].DescriptorTable.pDescriptorRanges=&srvRanges[13];
    rootParams[5].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    srvRanges[14].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRanges[14].NumDescriptors=1;srvRanges[14].BaseShaderRegister=6;
    srvRanges[14].OffsetInDescriptorsFromTableStart=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    rootParams[9].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[9].DescriptorTable.NumDescriptorRanges=1;
    rootParams[9].DescriptorTable.pDescriptorRanges=&srvRanges[14];
    rootParams[9].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    // T27: transmitted-radiance pyramid at t15.
    srvRanges[15].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRanges[15].NumDescriptors=1;srvRanges[15].BaseShaderRegister=15;
    srvRanges[15].OffsetInDescriptorsFromTableStart=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    rootParams[18].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[18].DescriptorTable.NumDescriptorRanges=1;
    rootParams[18].DescriptorTable.pDescriptorRanges=&srvRanges[15];
    rootParams[18].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    rootParams[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[7].Constants.ShaderRegister = 1; rootParams[7].Constants.Num32BitValues = 25;
    rootParams[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rootParams[8].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[8].Descriptor.ShaderRegister=2;
    rootParams[8].Descriptor.RegisterSpace=0;
    rootParams[8].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc{};
    rootSigDesc.NumParameters = static_cast<UINT>(std::size(rootParams));
    rootSigDesc.pParameters = rootParams;
    D3D12_STATIC_SAMPLER_DESC samplers[3]{sampler, sampler, sampler};
    samplers[0].ShaderRegister=0;
    // T33 bounded anisotropic filtering for authored linear/mipmap slots. It
    // stays at 4x so the cost is predictable on 11_0, and the shader still
    // routes authored nearest slots through samplers[1], which stays point.
    samplers[0].Filter=D3D12_FILTER_ANISOTROPIC;
    samplers[0].MaxAnisotropy=4;
    samplers[1].ShaderRegister=1;
    samplers[1].Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[1].MaxAnisotropy=1;
    // Cube faces meet at wrap seams; sampling the prefiltered mip chain with
    // wrap on all three axes avoids a visible seam line.
    samplers[2].ShaderRegister=2;
    samplers[2].AddressU=D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[2].AddressV=D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[2].AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    rootSigDesc.NumStaticSamplers = 3;
    rootSigDesc.pStaticSamplers = samplers;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signatureBlob;
    ComPtr<ID3DBlob> signatureErrorBlob;
    if (FAILED(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob,
                                            &signatureErrorBlob))) {
        error = L"The textured D3D12 root signature could not be serialized.";
        return false;
    }
    if (FAILED(device.Device()->CreateRootSignature(0, signatureBlob->GetBufferPointer(),
                                                      signatureBlob->GetBufferSize(),
                                                      IID_PPV_ARGS(&texturedRootSignature)))) {
        error = L"The textured D3D12 root signature could not be created.";
        return false;
    }

    ComPtr<ID3DBlob> vsBlob;
    if (!CompileShader(kTexturedVertexShaderSource, sizeof(kTexturedVertexShaderSource) - 1, "VSMain", "vs_5_1",
                        vsBlob, error))
        return false;
    ComPtr<ID3DBlob> psBlob;
    if (!CompileShader(kTexturedPixelShaderSource, sizeof(kTexturedPixelShaderSource) - 1, "PSMain", "ps_5_1",
                        psBlob, error))
        return false;
    D3D_SHADER_MACRO aoDefines[] = { {"AO_INDIRECT", "1"}, {nullptr, nullptr} };
    ComPtr<ID3DBlob> aoPsBlob;
    if (!CompileShader(kTexturedPixelShaderSource, sizeof(kTexturedPixelShaderSource) - 1,
                       "PSMain", "ps_5_1", aoPsBlob, error, aoDefines))
        return false;

    D3D12_INPUT_ELEMENT_DESC inputElements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_BACK;
    // glTF (and the STL/PLY/OBJ/FBX/USD/3MF/STEP importers) author front
    // faces counter-clockwise when viewed from outside in a right-handed
    // frame. The view/projection stay right-handed and the D3D viewport keeps
    // NDC +Y pointing at the render target's smaller y, so the authored
    // winding survives to the rasterizer unchanged: CCW must be front-facing.
    // (The mirrored variant below flips this for negative-determinant
    // instance transforms, which reverse the projected winding.)
    rasterizer.FrontCounterClockwise = TRUE;
    rasterizer.DepthClipEnable = TRUE;

    D3D12_BLEND_DESC blend{};
    // Keep color alpha blending independent from the integer pick-ID MRT.
    blend.IndependentBlendEnable = TRUE;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    blend.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_DEPTH_STENCIL_DESC depthStencil{};
    depthStencil.DepthEnable = TRUE;
    depthStencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencil.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    depthStencil.StencilEnable = FALSE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = texturedRootSignature.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.BlendState = blend;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState = rasterizer;
    psoDesc.DepthStencilState = depthStencil;
    psoDesc.InputLayout = { inputElements, static_cast<UINT>(std::size(inputElements)) };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 2;
    psoDesc.RTVFormats[0] = kSceneColorFormat;
    psoDesc.RTVFormats[1] = DXGI_FORMAT_R32_UINT;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // Historical two-target layout, retained for the AO-off low-memory path.
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&psoDesc,
            IID_PPV_ARGS(&texturedLegacyPipelineState)))) {
        error = L"The textured D3D12 pipeline state could not be created.";
        return false;
    }
    psoDesc.RasterizerState.FrontCounterClockwise=FALSE;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&psoDesc,
            IID_PPV_ARGS(&texturedLegacyMirroredPipelineState)))) {
        error=L"The mirrored material pipeline state could not be created.";return false;
    }
    psoDesc.RasterizerState.FrontCounterClockwise=TRUE;
    psoDesc.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&psoDesc,
            IID_PPV_ARGS(&texturedLegacyDoubleSidedPipelineState)))) {
        error=L"The double-sided material pipeline state could not be created.";return false;
    }
    psoDesc.RasterizerState.CullMode=D3D12_CULL_MODE_BACK;
    psoDesc.RasterizerState.FrontCounterClockwise=TRUE;

    // T32 opaque layout: scene + indirect + pick. The wireframe/blend/OIT/pick
    // PSOs keep the historical two-target layout below.
    auto aoPsoDesc = psoDesc;
    aoPsoDesc.PS = { aoPsBlob->GetBufferPointer(), aoPsBlob->GetBufferSize() };
    aoPsoDesc.NumRenderTargets = 3;
    aoPsoDesc.RTVFormats[1] = kSceneColorFormat;
    aoPsoDesc.RTVFormats[2] = DXGI_FORMAT_R32_UINT;
    aoPsoDesc.BlendState.RenderTarget[2].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&aoPsoDesc,
            IID_PPV_ARGS(&texturedPipelineState)))) {
        error = L"The textured AO material pipeline state could not be created.";
        return false;
    }
    auto wirePsoDesc = psoDesc;
    wirePsoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    wirePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    wirePsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    wirePsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    wirePsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    const HRESULT wireResult=device.Device()->CreateGraphicsPipelineState(
        &wirePsoDesc,IID_PPV_ARGS(&texturedWireframePipelineState));
    if (FAILED(wireResult)) {
        error=L"The textured wireframe overlay pipeline state could not be created (HRESULT "
            +std::to_wstring(static_cast<long>(wireResult))+L").";return false;
    }
    aoPsoDesc.RasterizerState.FrontCounterClockwise=FALSE;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&aoPsoDesc,
            IID_PPV_ARGS(&texturedMirroredPipelineState)))) {
        error=L"The mirrored AO material pipeline state could not be created.";return false;
    }
    aoPsoDesc.RasterizerState.FrontCounterClockwise=TRUE;
    aoPsoDesc.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&aoPsoDesc,
            IID_PPV_ARGS(&texturedDoubleSidedPipelineState)))) {
        error=L"The double-sided AO material pipeline state could not be created.";return false;
    }
    // T29 Clay overlaid triangles: opaque shader, depth test + write, LESS_EQUAL
    // so an authored glass surface coplanar with its trim/body resolves after
    // the solid prepass instead of z-fighting. Sorted far-to-near by the CPU.
    // Both target layouts get the same depth policy; only the MRT count and PS
    // variant differ.
    auto makeClayVariant = [&](D3D12_GRAPHICS_PIPELINE_STATE_DESC descriptor,
                               bool doubleSided, bool mirrored,
                               ID3D12PipelineState** output, const wchar_t* label) {
        descriptor.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        descriptor.RasterizerState.DepthBias = -1000;
        descriptor.RasterizerState.SlopeScaledDepthBias = -1.0f;
        descriptor.RasterizerState.DepthBiasClamp = 0.0f;
        descriptor.RasterizerState.CullMode = doubleSided ? D3D12_CULL_MODE_NONE
                                                          : D3D12_CULL_MODE_BACK;
        descriptor.RasterizerState.FrontCounterClockwise = mirrored ? FALSE : TRUE;
        const HRESULT result = device.Device()->CreateGraphicsPipelineState(
            &descriptor, IID_PPV_ARGS(output));
        if (FAILED(result)) error = label;
        return SUCCEEDED(result);
    };
    if (!makeClayVariant(psoDesc, true, false,
                         texturedLegacyClayDoubleSidedPipelineState.ReleaseAndGetAddressOf(),
                         L"The Clay double-sided material pipeline state could not be created.")
        || !makeClayVariant(psoDesc, false, false,
                            texturedLegacyClayPipelineState.ReleaseAndGetAddressOf(),
                            L"The Clay material pipeline state could not be created.")
        || !makeClayVariant(psoDesc, false, true,
                            texturedLegacyClayMirroredPipelineState.ReleaseAndGetAddressOf(),
                            L"The Clay mirrored material pipeline state could not be created.")
        || !makeClayVariant(aoPsoDesc, true, false,
                            texturedClayDoubleSidedPipelineState.ReleaseAndGetAddressOf(),
                            L"The Clay double-sided AO pipeline state could not be created.")
        || !makeClayVariant(aoPsoDesc, false, false,
                            texturedClayPipelineState.ReleaseAndGetAddressOf(),
                            L"The Clay AO pipeline state could not be created.")
        || !makeClayVariant(aoPsoDesc, false, true,
                            texturedClayMirroredPipelineState.ReleaseAndGetAddressOf(),
                            L"The Clay mirrored AO pipeline state could not be created.")) {
        return false;
    }
    psoDesc.BlendState.RenderTarget[0].BlendEnable=TRUE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend=D3D12_BLEND_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].DestBlend=D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOp=D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha=D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlendAlpha=D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOpAlpha=D3D12_BLEND_OP_ADD;
    psoDesc.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.FrontCounterClockwise=TRUE;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&psoDesc,IID_PPV_ARGS(&texturedBlendDoubleSidedPipelineState)))) {
        error=L"The blended double-sided material pipeline state could not be created.";return false;
    }
    psoDesc.RasterizerState.CullMode=D3D12_CULL_MODE_BACK;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&psoDesc,IID_PPV_ARGS(&texturedBlendPipelineState)))) {
        error=L"The blended material pipeline state could not be created.";return false;
    }
    psoDesc.RasterizerState.FrontCounterClockwise=FALSE;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&psoDesc,IID_PPV_ARGS(&texturedBlendMirroredPipelineState)))) {
        error=L"The mirrored blended material pipeline state could not be created.";return false;
    }

    // T26 weighted blended OIT pipeline. It writes two float targets and never
    // the integer pick target; a separate nearest-visible pick pass owns
    // selection. Depth tests against the opaque prepass remain on so hidden
    // geometry cannot contribute to the transparent accumulation.
    D3D_SHADER_MACRO oitDefines[] = { {"OIT_PASS", "1"}, {nullptr, nullptr} };
    ComPtr<ID3DBlob> oitPsBlob;
    if (!CompileShader(kTexturedPixelShaderSource, sizeof(kTexturedPixelShaderSource) - 1,
                       "PSMain", "ps_5_1", oitPsBlob, error, oitDefines)) return false;
    auto oitPsoDesc = psoDesc;
    oitPsoDesc.PS = { oitPsBlob->GetBufferPointer(), oitPsBlob->GetBufferSize() };
    oitPsoDesc.RTVFormats[0] = kSceneColorFormat;
    oitPsoDesc.RTVFormats[1] = kSceneColorFormat;
    oitPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    oitPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    auto& oitAccum = oitPsoDesc.BlendState.RenderTarget[0];
    oitAccum.BlendEnable = TRUE;
    oitAccum.SrcBlend = D3D12_BLEND_ONE; oitAccum.DestBlend = D3D12_BLEND_ONE;
    oitAccum.BlendOp = D3D12_BLEND_OP_ADD;
    oitAccum.SrcBlendAlpha = D3D12_BLEND_ONE; oitAccum.DestBlendAlpha = D3D12_BLEND_ONE;
    oitAccum.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    auto& oitReveal = oitPsoDesc.BlendState.RenderTarget[1];
    oitReveal.BlendEnable = TRUE;
    oitReveal.SrcBlend = D3D12_BLEND_ZERO; oitReveal.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    oitReveal.BlendOp = D3D12_BLEND_OP_ADD;
    oitReveal.SrcBlendAlpha = D3D12_BLEND_ZERO; oitReveal.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    oitReveal.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    oitPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    oitPsoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&oitPsoDesc,IID_PPV_ARGS(&texturedOitDoubleSidedPipelineState)))) {
        error=L"The double-sided OIT pipeline state could not be created.";return false;
    }
    oitPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&oitPsoDesc,IID_PPV_ARGS(&texturedOitPipelineState)))) {
        error=L"The OIT pipeline state could not be created.";return false;
    }
    oitPsoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&oitPsoDesc,IID_PPV_ARGS(&texturedOitMirroredPipelineState)))) {
        error=L"The mirrored OIT pipeline state could not be created.";return false;
    }

    // Nearest-visible transparent selection. Colour writes are masked off and
    // only the integer pick target is written; depth writes are enabled so the
    // surviving id is the nearest visible fragment, never an averaged id.
    auto pickPsoDesc = psoDesc;
    pickPsoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    pickPsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
    pickPsoDesc.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pickPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pickPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pickPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pickPsoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&pickPsoDesc,IID_PPV_ARGS(&texturedPickDoubleSidedPipelineState)))) {
        error=L"The double-sided transparent pick pipeline state could not be created.";return false;
    }
    pickPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&pickPsoDesc,IID_PPV_ARGS(&texturedPickPipelineState)))) {
        error=L"The transparent pick pipeline state could not be created.";return false;
    }
    pickPsoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&pickPsoDesc,IID_PPV_ARGS(&texturedPickMirroredPipelineState)))) {
        error=L"The mirrored transparent pick pipeline state could not be created.";return false;
    }
    return true;
}

bool D3D12ViewerPath::CreateResolvePipeline(std::wstring& error)
{
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    // t0 scene, t1 backdrop, t2/t3 OIT, t4 indirect, t5 AO mask, t6 depth.
    srvRange.NumDescriptors = 7;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = 16;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc{};
    rootSigDesc.NumParameters = static_cast<UINT>(std::size(params));
    rootSigDesc.pParameters = params;
    rootSigDesc.NumStaticSamplers = 1;
    rootSigDesc.pStaticSamplers = &sampler;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> signatureBlob;
    ComPtr<ID3DBlob> signatureErrorBlob;
    if (FAILED(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                            &signatureBlob, &signatureErrorBlob))) {
        error = L"The resolve root signature could not be serialized.";
        return false;
    }
    if (FAILED(device.Device()->CreateRootSignature(0, signatureBlob->GetBufferPointer(),
                                                      signatureBlob->GetBufferSize(),
                                                      IID_PPV_ARGS(&resolveRootSignature)))) {
        error = L"The resolve root signature could not be created.";
        return false;
    }

    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;
    if (!CompileShader(kResolveVertexShaderSource, sizeof(kResolveVertexShaderSource) - 1,
                       "VSMain", "vs_5_1", vsBlob, error)
        || !CompileShader(kResolvePixelShaderSource, sizeof(kResolvePixelShaderSource) - 1,
                          "PSMain", "ps_5_1", psBlob, error)) {
        return false;
    }

    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.DepthClipEnable = TRUE;
    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = resolveRootSignature.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.BlendState = blend;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState = rasterizer;
    psoDesc.DepthStencilState = depth;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&resolvePipelineState)))) {
        error = L"The full-screen resolve pipeline state could not be created.";
        return false;
    }

    // T27 transmission mip reduction: a second simple root signature and
    // full-screen pipeline. It shares the resolve vertex shader and reads one
    // source level per draw.
    D3D12_DESCRIPTOR_RANGE mipRange{};
    mipRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    mipRange.NumDescriptors = 1;
    mipRange.BaseShaderRegister = 0;
    mipRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER mipParam{};
    mipParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    mipParam.DescriptorTable.NumDescriptorRanges = 1;
    mipParam.DescriptorTable.pDescriptorRanges = &mipRange;
    mipParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC mipRootDesc{};
    mipRootDesc.NumParameters = 1;
    mipRootDesc.pParameters = &mipParam;
    D3D12_STATIC_SAMPLER_DESC mipSampler = sampler;
    mipSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    mipRootDesc.NumStaticSamplers = 1;
    mipRootDesc.pStaticSamplers = &mipSampler;
    mipRootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> mipSignature;
    ComPtr<ID3DBlob> mipSignatureError;
    if (FAILED(D3D12SerializeRootSignature(&mipRootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &mipSignature, &mipSignatureError))
        || FAILED(device.Device()->CreateRootSignature(0, mipSignature->GetBufferPointer(),
                    mipSignature->GetBufferSize(), IID_PPV_ARGS(&transmissionRootSignature)))) {
        error = L"The transmission root signature could not be created.";
        return false;
    }
    ComPtr<ID3DBlob> mipPsBlob;
    if (!CompileShader(kTransmissionMipPixelShaderSource, sizeof(kTransmissionMipPixelShaderSource) - 1,
                       "PSMain", "ps_5_1", mipPsBlob, error)) {
        return false;
    }
    auto mipPsoDesc = psoDesc;
    mipPsoDesc.pRootSignature = transmissionRootSignature.Get();
    mipPsoDesc.PS = { mipPsBlob->GetBufferPointer(), mipPsBlob->GetBufferSize() };
    mipPsoDesc.RTVFormats[0] = kSceneColorFormat;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&mipPsoDesc,
            IID_PPV_ARGS(&transmissionPipelineState)))) {
        error = L"The transmission mip pipeline state could not be created.";
        return false;
    }
    return true;
}

void D3D12ViewerPath::ResolveSceneToBackBuffer(D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
                                               const OverlayInfo& info, bool allowAa)
{
    const auto resolveStart = std::chrono::steady_clock::now();
    if (!sceneColor || !backdropColor || !sceneColorSrvHeap || !resolveRootSignature || !resolvePipelineState) return;

    const bool useOit = OitUsable();
    lastResolveUsedOit = useOit;
    const bool applyAo = lastAoMode == 1 && ao.available && !aoAllocationForcedFailure
        && ao.indirect && ao.filtered;
    // T33 edge AA state. The depth SRV is created with the scene target and
    // recreated on resize/device recovery; no AA-specific allocation exists.
    const bool aaRequested = allowAa && !aaForceDisabled;
    const viewer_aa::Mode aaMode = viewer_aa::EffectiveMode(
        aaRequested, depthSrvAvailable && depthBuffer != nullptr);
    lastAaMode = static_cast<uint32_t>(aaMode);
    lastAaFallbackReason = static_cast<uint32_t>(
        !allowAa ? viewer_aa::FallbackReason::None
        : aaForceDisabled ? viewer_aa::FallbackReason::ForcedOff
        : aaMode == viewer_aa::Mode::Unavailable
            ? viewer_aa::FallbackReason::Unsupported
            : viewer_aa::FallbackReason::None);
    lastAaTapCount = aaMode == viewer_aa::Mode::DepthAwareFxaa
        ? viewer_aa::kTapCount : 0;
    const bool applyAa = aaMode == viewer_aa::Mode::DepthAwareFxaa;
    D3D12_RESOURCE_BARRIER barriers[8]{};
    UINT barrierCount = 2;
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = sceneColor.Get();
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = backdropColor.Get();
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (useOit) {
        barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[2].Transition.pResource = oit.accumulation.Get();
        barriers[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[3].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[3].Transition.pResource = oit.revealage.Get();
        barriers[3].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[3].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[3].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrierCount = 4;
    }
    if (applyAo) {
        D3D12_RESOURCE_BARRIER& indirectBarrier = barriers[barrierCount++];
        indirectBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        indirectBarrier.Transition.pResource = ao.indirect.Get();
        indirectBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        indirectBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        indirectBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        D3D12_RESOURCE_BARRIER& aoBarrier = barriers[barrierCount++];
        aoBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        aoBarrier.Transition.pResource = ao.filtered.Get();
        aoBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        aoBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        aoBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    if (applyAa) {
        D3D12_RESOURCE_BARRIER& depthBarrier = barriers[barrierCount++];
        depthBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        depthBarrier.Transition.pResource = depthBuffer.Get();
        depthBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        depthBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        depthBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    commandList->ResourceBarrier(barrierCount, barriers);

    lastResolveExposure = viewer_presentation::ClampExposure(info.exposure);
    lastResolveToneMap = static_cast<uint32_t>(info.toneMap);
    const float constants[16] = {
        lastResolveExposure,
        static_cast<float>(info.toneMap),
        static_cast<float>(info.background),
        oitResolveDiagnostic != 0 ? static_cast<float>(oitResolveDiagnostic) : 12.0f,
        0.0f, 0.0f, 0.0f, useOit ? 1.0f : 0.0f,
        applyAo ? 1.0f : 0.0f,
        lastAoStrength,
        static_cast<float>(swapChain.Width()),
        static_cast<float>(swapChain.Height()),
        applyAa ? 1.0f : 0.0f,
        static_cast<float>(lastAaTapCount),
        1.0f / static_cast<float>((std::max)(1u, swapChain.Width())),
        1.0f / static_cast<float>((std::max)(1u, swapChain.Height())),
    };
    commandList->OMSetRenderTargets(1, &backBufferRtv, FALSE, nullptr);
    D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(swapChain.Width()),
                             static_cast<float>(swapChain.Height()), 0.0f, 1.0f };
    D3D12_RECT scissor{ 0, 0, static_cast<LONG>(swapChain.Width()),
                        static_cast<LONG>(swapChain.Height()) };
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    ID3D12DescriptorHeap* heaps[] = { sceneColorSrvHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootSignature(resolveRootSignature.Get());
    commandList->SetPipelineState(resolvePipelineState.Get());
    commandList->SetGraphicsRoot32BitConstants(0, 16, constants, 0);
    commandList->SetGraphicsRootDescriptorTable(1,
        sceneColorSrvHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 0, nullptr);
    commandList->IASetIndexBuffer(nullptr);
    commandList->DrawInstanced(3, 1, 0, 0);

    for (UINT index = 0; index < barrierCount; ++index)
        std::swap(barriers[index].Transition.StateBefore, barriers[index].Transition.StateAfter);
    commandList->ResourceBarrier(barrierCount, barriers);
    lastAaResolveMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - resolveStart).count();
}

bool D3D12ViewerPath::CreateFrameConstantBuffer(std::wstring& error)
{
    // One 256-byte slot per frame in flight. 256 is D3D12's CBV alignment
    // requirement and comfortably covers one 4x4 matrix (64 bytes).
    frameConstantBuffer = CreateBuffer(device.Device(), kConstantBufferSlotBytes * kFrameCount,
                                        D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!frameConstantBuffer) {
        error = L"The per-frame constant buffer could not be created.";
        return false;
    }
    D3D12_RANGE noRead{ 0, 0 };
    void* mapped = nullptr;
    if (FAILED(frameConstantBuffer->Map(0, &noRead, &mapped))) {
        error = L"The per-frame constant buffer could not be mapped.";
        return false;
    }
    frameConstantBufferMapped = static_cast<std::byte*>(mapped);
    return true;
}

bool D3D12ViewerPath::CreateStudioEnvironment(std::wstring& error)
{
    const auto bakeStart = std::chrono::steady_clock::now();
    const studio_ibl::EnvironmentBake bake = studio_ibl::BakeStudioEnvironment();
    environment.bakeMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - bakeStart).count();
    environment.version = bake.version;
    environment.variant = static_cast<uint32_t>(bake.variant);
    environment.specularSampleCount = bake.specularSampleCount;
    environment.identityHash = bake.identityHash;
    environment.residentBytes = bake.residentBytes;
    const UINT faceSize = bake.base.size;
    const UINT mipCount = bake.MipCount();
    if (faceSize == 0 || mipCount == 0) {
        error = L"The Studio environment bake produced no cubemap.";
        return false;
    }

    // RGBA32F is a feature-level-11_0 format; the environment is small
    // (128^2 x 6 plus mips, well under 4 MiB), so no format fallback is
    // needed. If a future device cannot sample it, T15 owns the fallback.
    constexpr DXGI_FORMAT kCubeFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
    constexpr UINT kBytesPerTexel = 16;

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = faceSize;
    textureDesc.Height = faceSize;
    textureDesc.DepthOrArraySize = static_cast<UINT16>(6);
    textureDesc.MipLevels = static_cast<UINT16>(mipCount);
    textureDesc.Format = kCubeFormat;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    environment.cube.Reset();
    if (FAILED(device.Device()->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&environment.cube)))) {
        error = L"The Studio environment cubemap could not be created.";
        return false;
    }

    struct SubresourceCopy
    {
        UINT mip = 0;
        UINT face = 0;
        UINT width = 0;
        UINT height = 0;
        UINT rowPitch = 0;
        UINT64 offset = 0;
    };
    std::vector<SubresourceCopy> copies;
    copies.reserve(static_cast<size_t>(mipCount) * 6);
    uint64_t uploadBytes = 0;
    for (UINT face = 0; face < 6; ++face) {
        for (UINT mip = 0; mip < mipCount; ++mip) {
            SubresourceCopy copy;
            copy.mip = mip;
            copy.face = face;
            copy.width = (std::max)(1u, faceSize >> mip);
            copy.height = copy.width;
            copy.rowPitch = (copy.width * kBytesPerTexel + 255u) & ~255u;
            uploadBytes = (uploadBytes + 511ull) & ~511ull;
            copy.offset = uploadBytes;
            uploadBytes += static_cast<uint64_t>(copy.rowPitch) * copy.height;
            copies.push_back(copy);
        }
    }
    uploadBytes = (uploadBytes + 511ull) & ~511ull;

    // T24 split-sum DFG LUT. One tiny R32G32_FLOAT texture alongside the cube;
    // the same bounded startup upload carries both.
    const UINT brdfLutSize = bake.brdfLut.Empty() ? 0u : bake.brdfLut.size;
    UINT brdfLutRowPitch = 0;
    UINT64 brdfLutOffset = 0;
    if (brdfLutSize != 0) {
        D3D12_RESOURCE_DESC lutDesc{};
        lutDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        lutDesc.Width = brdfLutSize;
        lutDesc.Height = brdfLutSize;
        lutDesc.DepthOrArraySize = 1;
        lutDesc.MipLevels = 1;
        lutDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
        lutDesc.SampleDesc.Count = 1;
        lutDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        environment.brdfLut.Reset();
        if (FAILED(device.Device()->CreateCommittedResource(
                &defaultHeap, D3D12_HEAP_FLAG_NONE, &lutDesc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&environment.brdfLut)))) {
            error = L"The Studio environment BRDF LUT could not be created.";
            return false;
        }
        brdfLutRowPitch = (brdfLutSize * 8u + 255u) & ~255u;
        uploadBytes = (uploadBytes + 511ull) & ~511ull;
        brdfLutOffset = uploadBytes;
        uploadBytes += static_cast<uint64_t>(brdfLutRowPitch) * brdfLutSize;
        uploadBytes = (uploadBytes + 511ull) & ~511ull;
        environment.brdfLutSize = brdfLutSize;
    }

    auto uploadBuffer = CreateBuffer(device.Device(), uploadBytes, D3D12_HEAP_TYPE_UPLOAD,
                                     D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!uploadBuffer) {
        error = L"The Studio environment upload buffer could not be created.";
        return false;
    }
    D3D12_RANGE noRead{0, 0};
    void* mapped = nullptr;
    if (FAILED(uploadBuffer->Map(0, &noRead, &mapped))) {
        error = L"The Studio environment upload buffer could not be mapped.";
        return false;
    }
    for (const auto& copy : copies) {
        const auto& source = bake.specularMips[copy.mip].faces[copy.face];
        auto* destination = reinterpret_cast<std::byte*>(mapped) + copy.offset;
        for (UINT y = 0; y < copy.height; ++y) {
            float* row = reinterpret_cast<float*>(destination + static_cast<size_t>(y) * copy.rowPitch);
            for (UINT x = 0; x < copy.width; ++x) {
                const studio_ibl::Rgb& texel = source[static_cast<size_t>(y) * copy.width + x];
                row[x * 4 + 0] = texel.r;
                row[x * 4 + 1] = texel.g;
                row[x * 4 + 2] = texel.b;
                row[x * 4 + 3] = 1.0f;
            }
        }
    }
    if (brdfLutSize != 0) {
        auto* destination = reinterpret_cast<std::byte*>(mapped) + brdfLutOffset;
        for (UINT y = 0; y < brdfLutSize; ++y) {
            float* row = reinterpret_cast<float*>(destination + static_cast<size_t>(y) * brdfLutRowPitch);
            const float* source = &bake.brdfLut.rg[static_cast<size_t>(y) * brdfLutSize * 2];
            std::memcpy(row, source, static_cast<size_t>(brdfLutSize) * 2 * sizeof(float));
        }
    }
    uploadBuffer->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device.Device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(&allocator)))
        || FAILED(device.Device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     allocator.Get(), nullptr, IID_PPV_ARGS(&list)))) {
        error = L"The Studio environment upload command list could not be created.";
        return false;
    }
    for (const auto& copy : copies) {
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = uploadBuffer.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint.Offset = copy.offset;
        source.PlacedFootprint.Footprint.Format = kCubeFormat;
        source.PlacedFootprint.Footprint.Width = copy.width;
        source.PlacedFootprint.Footprint.Height = copy.height;
        source.PlacedFootprint.Footprint.Depth = 1;
        source.PlacedFootprint.Footprint.RowPitch = copy.rowPitch;

        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = environment.cube.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = copy.mip + copy.face * mipCount;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
    if (environment.brdfLut) {
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = uploadBuffer.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint.Offset = brdfLutOffset;
        source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32_FLOAT;
        source.PlacedFootprint.Footprint.Width = brdfLutSize;
        source.PlacedFootprint.Footprint.Height = brdfLutSize;
        source.PlacedFootprint.Footprint.Depth = 1;
        source.PlacedFootprint.Footprint.RowPitch = brdfLutRowPitch;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = environment.brdfLut.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = environment.cube.Get();
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    UINT barrierCount = 1;
    if (environment.brdfLut) {
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition.pResource = environment.brdfLut.Get();
        barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrierCount = 2;
    }
    list->ResourceBarrier(barrierCount, barriers);
    if (FAILED(list->Close())) {
        error = L"The Studio environment upload command list could not be closed.";
        return false;
    }
    ID3D12CommandList* lists[] = {list.Get()};
    directQueue.Queue()->ExecuteCommandLists(1, lists);
    const uint64_t fence = directQueue.SignalNext();
    if (fence == 0 || directQueue.WaitForValue(fence, kIdleWaitTimeoutMs)
            != D3D12CommandQueue::WaitResult::Signaled) {
        error = L"The Studio environment upload did not complete.";
        return false;
    }

    for (size_t i = 0; i < 9; ++i) {
        const auto& coefficient = bake.irradiance.values[i];
        environment.sh[i] = DirectX::XMFLOAT4(coefficient.r, coefficient.g, coefficient.b, 0.0f);
    }
    environment.mipCount = mipCount;
    environment.uploadBytes = uploadBytes;
    environment.parameters = DirectX::XMFLOAT4(
        1.0f, 0.0f, 1.0f, static_cast<float>(mipCount - 1));
    return true;
}

void D3D12ViewerPath::WriteEnvironmentSrv(ID3D12DescriptorHeap& heap, UINT index,
                                          UINT descriptorSize)
{
    if (!environment.cube) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.TextureCube.MostDetailedMip = 0;
    srv.TextureCube.MipLevels = environment.mipCount;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap.GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(index) * descriptorSize;
    device.Device()->CreateShaderResourceView(environment.cube.Get(), &srv, cpu);
}

void D3D12ViewerPath::WriteBrdfLutSrv(ID3D12DescriptorHeap& heap, UINT index,
                                      UINT descriptorSize)
{
    if (!environment.brdfLut) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32G32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap.GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(index) * descriptorSize;
    device.Device()->CreateShaderResourceView(environment.brdfLut.Get(), &srv, cpu);
}

void D3D12ViewerPath::WaitForIdle()
{
    // Every outstanding slot, not one scalar: with the CPU running ahead,
    // more than one frame's work can be in flight.
    uint64_t highest = 0;
    for (const auto& frame : frames) {
        if (frame.fenceValue > highest) highest = frame.fenceValue;
    }
    if (highest != 0) {
        directQueue.WaitForValue(highest, kIdleWaitTimeoutMs);
    }

    // The copy queue is a separate timeline and has to be drained
    // separately. Signalling a fresh value and waiting on it covers
    // everything already submitted, without the ring having to expose its
    // own bookkeeping. This is a shutdown/resize drain -- the whole point of
    // the rest of this class is that no *ordinary* path waits here.
    if (uploadRing.CopyQueue().Queue() && (uploadInFlight || !retiredModels.empty() || hasModel)) {
        uploadRing.FlushBatch();
        uint64_t copyValue = uploadRing.CopyQueue().SignalNext();
        if (copyValue != 0) {
            uploadRing.CopyQueue().WaitForValue(copyValue, kIdleWaitTimeoutMs);
        }
    }
}

UINT D3D12ViewerPath::BeginFrame()
{
    // The frame-latency waitable object is what bounds how far ahead the CPU
    // may run (SetMaximumFrameLatency(2) in D3D12SwapChain::Initialize). This
    // is its first and only consumer -- it has been created and exposed since
    // Gate 2 with nothing ever waiting on it.
    if (HANDLE waitable = swapChain.FrameLatencyWaitableHandle()) {
        WaitForSingleObject(waitable, kIdleWaitTimeoutMs);
    }

    const UINT index = swapChain.CurrentBackBufferIndex();
    // This slot's own last submission must have completed before its
    // allocator may be reset. Normally already signaled -- WaitForValue
    // short-circuits on the fast path when the fence has passed.
    if (frames[index].fenceValue != 0) {
        directQueue.WaitForValue(frames[index].fenceValue, kIdleWaitTimeoutMs);
    }
    return index;
}

void D3D12ViewerPath::DrawChrome(UINT frameIndex, const DirectX::XMFLOAT4& orientation,
                                const OverlayFrame& chrome)
{
    const auto started = std::chrono::steady_clock::now();
    overlay.DrawChrome(frameIndex, orientation, chrome);
    const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - started;
    lastOverlayMs = elapsed.count();
    overlayTotalMs += lastOverlayMs;
    ++overlayPasses;
}

void D3D12ViewerPath::EndFrame(UINT frameIndex)
{
    const HRESULT presentResult = swapChain.Present();
    lastPresentResult = presentResult;
    frameStats.RecordPresent(presentResult);

    const uint64_t signaled = directQueue.SignalNext();
    if (signaled != 0) {
        frames[frameIndex].fenceValue = signaled;
    }
    // SignalNext returns 0 without incrementing on failure. Leaving the slot's
    // previous value in place is the safe reading: it keeps the allocator
    // gated on the newest value we know actually reached the queue, rather
    // than silently degrading the wait to a no-op.
}

bool D3D12ViewerPath::Resize(int width, int height, std::wstring& error)
{
    WaitForIdle();
    // The wrapped resources hold references to the back buffers, so
    // ResizeBuffers cannot succeed until they are dropped and flushed.
    overlay.ReleaseBackBufferReferences();
    if (!swapChain.Resize(static_cast<UINT>(width), static_cast<UINT>(height), error)) return false;
    if (!overlay.RecreateBackBufferReferences(swapChain, error)) return false;
    return CreateDepthBuffer(static_cast<UINT>(width), static_cast<UINT>(height), error);
}

void D3D12ViewerPath::RenderClearFrame(const DirectX::XMFLOAT4& orientation, const OverlayFrame& chrome)
{
    // An idle frame has no opaque geometry, so no AO mask is valid.
    lastAoMode = 0;
    lastAoFallbackReason = AoFallbackNone;
    lastAoPassMilliseconds = 0.0;
    PollAoGpuTiming();
    const UINT index = BeginFrame();

    if (FAILED(frames[index].allocator->Reset())) {
        return;
    }
    if (FAILED(commandList->Reset(frames[index].allocator.Get(), nullptr))) {
        return;
    }

    ID3D12Resource* backBuffer = swapChain.BackBuffer(index);

    D3D12_RESOURCE_BARRIER toRenderTarget{};
    toRenderTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRenderTarget.Transition.pResource = backBuffer;
    toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRenderTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commandList->ResourceBarrier(1, &toRenderTarget);

    // The idle frame still goes through the same resolve so background mode,
    // exposure and tone map behave identically before and after a model loads.
    if (sceneColor && sceneColorRtvHeap && backdropColor && backdropRtvHeap) {
        const float sceneClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        commandList->ClearRenderTargetView(
            sceneColorRtvHeap->GetCPUDescriptorHandleForHeapStart(), sceneClear, 0, nullptr);
        const auto display = viewer_presentation::BackgroundDisplayColor(
            chrome.info.background, chrome.info.backgroundColor);
        const float backdropClear[4] = { display[0], display[1], display[2], 1.0f };
        commandList->ClearRenderTargetView(
            backdropRtvHeap->GetCPUDescriptorHandleForHeapStart(), backdropClear, 0, nullptr);
        ResolveSceneToBackBuffer(swapChain.BackBufferRtv(index), chrome.info, false);
    } else {
        float clearColor[4]{};
        PresentationClearColor(chrome.info.presentationProfile, clearColor);
        commandList->ClearRenderTargetView(swapChain.BackBufferRtv(index), clearColor, 0, nullptr);
    }

    // With an overlay pass following, the back buffer is left in
    // RENDER_TARGET and the bridge's ReleaseWrappedResources moves it to
    // PRESENT instead (04-rendering-and-streaming.md:46).

    if (FAILED(commandList->Close())) {
        return;
    }
    ID3D12CommandList* lists[] = { commandList.Get() };
    directQueue.Queue()->ExecuteCommandLists(1, lists);

    DrawChrome(index, orientation, chrome);
    EndFrame(index);
}

void D3D12ViewerPath::RenderFrame(const DirectX::XMFLOAT4X4& viewProjection,
                                   const DirectX::XMFLOAT4& orientation, const OverlayFrame& chrome, const double cameraTarget[3], const DirectX::XMFLOAT4& eyeSelection)
{
    PollAoGpuTiming();
    const UINT index = BeginFrame();

    if (FAILED(frames[index].allocator->Reset())) return;
    if (FAILED(commandList->Reset(frames[index].allocator.Get(), pipelineState.Get()))) return;

    ID3D12Resource* backBuffer = swapChain.BackBuffer(index);

    D3D12_RESOURCE_BARRIER toRenderTarget{};
    toRenderTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRenderTarget.Transition.pResource = backBuffer;
    toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRenderTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commandList->ResourceBarrier(1, &toRenderTarget);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = sceneColorRtvHeap
        ? sceneColorRtvHeap->GetCPUDescriptorHandleForHeapStart()
        : swapChain.BackBufferRtv(index);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();

    const auto backgroundDisplay = viewer_presentation::BackgroundDisplayColor(
        chrome.info.background, chrome.info.backgroundColor);
    const float sceneClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    commandList->ClearRenderTargetView(rtv, sceneClear, 0, nullptr);
    D3D12_CPU_DESCRIPTOR_HANDLE indirectRtv{};
    const bool aoLayout = ao.available && !aoAllocationForcedFailure;
    if (aoLayout && ao.indirect && sceneColorRtvHeap) {
        indirectRtv = sceneColorRtvHeap->GetCPUDescriptorHandleForHeapStart();
        indirectRtv.ptr += device.Device()->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        commandList->ClearRenderTargetView(indirectRtv, sceneClear, 0, nullptr);
    }
    if (backdropRtvHeap && backdropColor) {
        const float backdropClear[4] = {
            backgroundDisplay[0], backgroundDisplay[1], backgroundDisplay[2], 1.0f };
        commandList->ClearRenderTargetView(
            backdropRtvHeap->GetCPUDescriptorHandleForHeapStart(), backdropClear, 0, nullptr);
    }
    commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    const auto pickRtv = pickRtvHeap->GetCPUDescriptorHandleForHeapStart();
    const float pickClear[4]{};
    commandList->ClearRenderTargetView(pickRtv, pickClear, 0, nullptr);
    D3D12_CPU_DESCRIPTOR_HANDLE targets[2] = {rtv, pickRtv};
    D3D12_CPU_DESCRIPTOR_HANDLE opaqueTargets[3] = {rtv, indirectRtv, pickRtv};
    commandList->OMSetRenderTargets(2, targets, FALSE, &dsv);

    const bool useOit = OitUsable();
    const bool transmissionUsable = TransmissionUsable();
    lastTransparencyAlgorithm = useOit ? 1u : 0u;
    lastTransparencyFallbackReason = useOit ? OitFallbackNone
        : (debugChannel != 0 ? OitFallbackDiagnostic
            : (oitForceFallback ? OitFallbackForced : oit.fallbackReason));
    lastTransmissionFallbackReason = transmissionUsable ? TransmissionFallbackNone
        : (transmissionAllocationForcedFailure ? TransmissionFallbackForced
            : (transmission.fallbackReason ? transmission.fallbackReason
                : TransmissionFallbackAllocationFailed));
    lastForwardTransmissionDrawCount = 0;
    lastTransparentDrawCount = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE oitTargets[2]{};
    if (useOit && oit.rtvHeap) {
        oitTargets[0] = oit.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        oitTargets[1] = oitTargets[0];
        oitTargets[1].ptr += device.Device()->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        const float accumClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        const float revealClear[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
        commandList->ClearRenderTargetView(oitTargets[0], accumClear, 0, nullptr);
        commandList->ClearRenderTargetView(oitTargets[1], revealClear, 0, nullptr);
    }

    const bool loading = chrome.info.state == ViewerState::Loading;
    const LONG top = loading ? 0 : chrome.info.toolbarHeight;
    const LONG right = std::max(1L, static_cast<LONG>(swapChain.Width()) - (loading ? 0 : chrome.info.infoPanelWidth));
    const LONG bottom = std::max(top + 1, static_cast<LONG>(swapChain.Height()) - (loading ? 0 : chrome.info.bottomBarHeight));
    D3D12_VIEWPORT viewport{ 0.0f, static_cast<float>(top), static_cast<float>(right),
                             static_cast<float>(bottom - top), 0.0f, 1.0f };
    D3D12_RECT scissor{ 0, top, right, bottom };
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    // row_major in the HLSL cbuffer declaration above + no transpose here --
    // matches Renderer.cpp's D3D11 shader convention exactly (its cbuffer
    // field is also declared row_major, fed directly from
    // XMStoreFloat4x4(view * projection) with no transpose). The caller
    // composed it that way under the camera lock.
    //
    // This frame's own slot -- writing the single shared slot would race the
    // GPU still reading the previous frame's matrix.
    const UINT64 constantBufferOffset = static_cast<UINT64>(index) * kConstantBufferSlotBytes;
    std::memcpy(frameConstantBufferMapped + constantBufferOffset, &viewProjection, sizeof(viewProjection));
    std::memcpy(frameConstantBufferMapped + constantBufferOffset + sizeof(viewProjection), &eyeSelection, sizeof(eyeSelection));
    // gViewport.z is the OIT depth reference. Using the eye distance keeps the
    // weight curve scene-relative instead of assuming a fixed world unit.
    const float oitDepthScale = (std::max)(1e-3f, std::sqrt(
        eyeSelection.x * eyeSelection.x + eyeSelection.y * eyeSelection.y
        + eyeSelection.z * eyeSelection.z));
    const DirectX::XMFLOAT4 viewportConstants(float(right),float(bottom-top),oitDepthScale,0);
    std::memcpy(frameConstantBufferMapped + constantBufferOffset + sizeof(viewProjection)+sizeof(eyeSelection),
                &viewportConstants,sizeof(viewportConstants));
    const DirectX::XMFLOAT4 lightingConstants(
        static_cast<float>(chrome.info.lightingMode),
        chrome.info.directionalLightAngle * DirectX::XM_2PI,
        chrome.info.directionalLightElevation, 0.0f);
    size_t frameOffset = constantBufferOffset + sizeof(viewProjection) + sizeof(eyeSelection)
        + sizeof(viewportConstants);
    std::memcpy(frameConstantBufferMapped + frameOffset, &lightingConstants, sizeof(lightingConstants));
    frameOffset += sizeof(lightingConstants);
    // World-from-camera quaternion (the camera is the only view rotation;
    // the model's ground-axis transform is already baked into the vertices).
    std::memcpy(frameConstantBufferMapped + frameOffset, &orientation, sizeof(orientation));
    frameOffset += sizeof(orientation);
    const DirectX::XMFLOAT4 studioParameters(
        chrome.info.studioIntensity, 0.0f,
        viewer_presentation::ClampExposure(chrome.info.exposure),
        static_cast<float>(environment.mipCount > 0 ? environment.mipCount - 1 : 0));
    std::memcpy(frameConstantBufferMapped + frameOffset, &studioParameters,
                sizeof(studioParameters));
    frameOffset += sizeof(studioParameters);
    std::memcpy(frameConstantBufferMapped + frameOffset, environment.sh,
                sizeof(environment.sh));
    frameOffset += sizeof(environment.sh);
    const DirectX::XMFLOAT4 presentationConstants(
        chrome.info.keyLightScale * studio_ibl::kStudioDirectKeyIntensity,
        chrome.info.fillLightScale * studio_ibl::kStudioDirectFillIntensity,
        chrome.info.rimLightScale * studio_ibl::kStudioDirectRimIntensity,
        static_cast<float>(debugChannel));
    std::memcpy(frameConstantBufferMapped + frameOffset, &presentationConstants,
                sizeof(presentationConstants));
    frameOffset += sizeof(presentationConstants);
    const DirectX::XMFLOAT4 transmissionConstants(
        1.0f / static_cast<float>((std::max)(1u, swapChain.Width())),
        1.0f / static_cast<float>((std::max)(1u, swapChain.Height())),
        static_cast<float>(transmission.mipCount > 0 ? transmission.mipCount - 1 : 0),
        transmissionUsable ? 1.0f : 0.0f);
    std::memcpy(frameConstantBufferMapped + frameOffset, &transmissionConstants,
                sizeof(transmissionConstants));
    frameOffset += sizeof(transmissionConstants);
    // T30: the single world-space Directional sun. The object shader, the
    // legacy compact shader, the nav gizmo and the ground projection all use
    // this helper. Shading directions already share the Z-up world frame;
    // the camera rotation is applied by the view matrix.
    float sunWorld[3]{};
    viewer_lighting::DirectionalSunDirection(
        chrome.info.directionalLightAngle, chrome.info.directionalLightElevation, sunWorld);
    const DirectX::XMFLOAT4 sunConstants(sunWorld[0], sunWorld[1], sunWorld[2], 0.0f);
    std::memcpy(frameConstantBufferMapped + frameOffset, &sunConstants,
                sizeof(sunConstants));
    const D3D12_GPU_VIRTUAL_ADDRESS constantBufferAddress
        = frameConstantBuffer->GetGPUVirtualAddress() + constantBufferOffset;

    const bool opaqueBackdrop = chrome.info.background != viewer_presentation::BackgroundMode::Transparent
        && chrome.info.background != viewer_presentation::BackgroundMode::Checkerboard;
    const float cameraTargetF[3] = {
        static_cast<float>(cameraTarget[0]),
        static_cast<float>(cameraTarget[1]),
        static_cast<float>(cameraTarget[2]),
    };
    // T30: Directional projects its contact shadow along the same world-space
    // sun the geometry shader and gizmo use. Other modes keep the stable Studio
    // key direction. T31 keeps consuming this same direction.
    const float* shadowLight =
        chrome.info.lightingMode == LightingMode::Directional
            ? sunWorld : viewer_ground_shadow::kKeyLightDirection;

    uint32_t shadowMode = 0;
    uint32_t shadowFallback = ShadowFallbackNone;
    uint32_t shadowCasterCount = 0;
    viewer_ground_shadow::Bounds worldBounds{};
    viewer_ground_shadow::GroundPlane plane{};
    viewer_ground_shadow::ShadowFootprint footprint{};
    viewer_ground_shadow::ShadowFrustum shadowFrustum{};
    viewer_ground_shadow::Constants groundConstants{};
    viewer_ground_shadow::ShadowConstants shadowConstants{};
    bool haveGroundLayer = false;
    float highlightTint[3]{};
    DirectX::XMFLOAT4X4 groundTransform{};
    if (haveModelBounds) {
        DirectX::XMStoreFloat4x4(&groundTransform,
            GroundAxisTransform(chrome.info.groundAxis, sourceUpAxis,
                chrome.info.showNativeOrientation, chrome.info.groundAxisInverted));
        viewer_ground_shadow::Bounds sourceBounds;
        sourceBounds.minimum[0] = modelBoundsMin.x;
        sourceBounds.minimum[1] = modelBoundsMin.y;
        sourceBounds.minimum[2] = modelBoundsMin.z;
        sourceBounds.maximum[0] = modelBoundsMax.x;
        sourceBounds.maximum[1] = modelBoundsMax.y;
        sourceBounds.maximum[2] = modelBoundsMax.z;
        worldBounds = viewer_ground_shadow::TransformBounds(sourceBounds, &groundTransform._11);
        plane = viewer_ground_shadow::PlaceGroundPlane(worldBounds);
        footprint = viewer_ground_shadow::ProjectShadow(worldBounds, plane, shadowLight);
        viewer_ground_shadow::HighlightColor(backgroundDisplay.data(), highlightTint);
        haveGroundLayer = plane.valid && footprint.valid;
        if (haveGroundLayer) {
            // Only triangle meshes become casters. Point/line-only content has
            // no silhouette, so it keeps empty ground instead of gaining a
            // bounds ellipse; MASK blobs must be able to sample their cutout.
            for (const auto& mesh : model.meshes) {
                if (!mesh.drawEnabled || mesh.points || mesh.lines) continue;
                const bool canMask = mesh.completeVertex && mesh.textureHeap
                    && mesh.materialConstants.mapped;
                const auto kind = viewer_ground_shadow::ClassifyShadowCaster(
                    mesh.hasMaterial ? mesh.material.alphaMode : 0u,
                    mesh.hasMaterial ? mesh.material.flags : 0u, canMask);
                if (kind != viewer_ground_shadow::ShadowCasterKind::None) ++shadowCasterCount;
            }
            if (chrome.info.shadowVisible && opaqueBackdrop && shadowCasterCount > 0) {
                if (shadow.available && !shadowFallbackForced) {
                    shadowFrustum = viewer_ground_shadow::FitShadowFrustum(
                        worldBounds, shadowLight, shadow.size);
                    if (shadowFrustum.valid) {
                        shadowMode = 1;
                    } else {
                        shadowMode = 2;
                        shadowFallback = ShadowFallbackAllocationFailed;
                    }
                } else {
                    shadowMode = 2;
                    shadowFallback = shadowFallbackForced
                        ? ShadowFallbackForced : shadow.fallbackReason;
                }
            } else if (chrome.info.shadowVisible && shadowCasterCount == 0) {
                shadowFallback = ShadowFallbackNoTriangleCasters;
            }
        }
    }
    lastShadowMode = shadowMode;
    lastShadowFallbackReason = shadowFallback;
    lastShadowCasterCount = shadowCasterCount;
    lastShadowPassMilliseconds = 0.0;

    // T32 AO state. `aoLayout` is a resource/layout decision (the opaque pass
    // writes the indirect MRT); `aoMode` is whether this frame actually runs
    // the depth passes and applies the mask. They are deliberately separate so
    // the user toggle never changes the draw layout mid-session.
    bool aoTriangleContent = false;
    for (const auto& mesh : model.meshes) {
        if (mesh.drawEnabled && !mesh.points && !mesh.lines) { aoTriangleContent = true; break; }
    }
    float sceneScale = 1.0f;
    if (haveModelBounds) {
        const float dx = modelBoundsMax.x - modelBoundsMin.x;
        const float dy = modelBoundsMax.y - modelBoundsMin.y;
        const float dz = modelBoundsMax.z - modelBoundsMin.z;
        sceneScale = std::max(std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f, 1e-4f);
    }
    uint32_t aoMode = 0;
    uint32_t aoFallback = AoFallbackNone;
    viewer_ao::Parameters aoParameters = viewer_ao::ParametersForMode(0);
    if (!aoLayout) {
        aoMode = 2;
        aoFallback = aoAllocationForcedFailure ? AoFallbackAllocationFailed
            : (aoForceFallback ? AoFallbackForced : ao.fallbackReason);
    } else if (!chrome.info.ambientOcclusionVisible) {
        aoMode = 0;
        aoFallback = AoFallbackDisabled;
    } else if (debugChannel != 0) {
        aoMode = 0;
        aoFallback = AoFallbackDiagnostic;
    } else if (aoForceFallback) {
        aoMode = 2;
        aoFallback = AoFallbackForced;
    } else if (!aoTriangleContent || !haveModelBounds) {
        aoMode = 2;
        aoFallback = AoFallbackNoTriangleGeometry;
    } else {
        aoMode = 1;
        const uint32_t reviewMode = chrome.info.lightingMode == LightingMode::Clay ? 1u
            : (chrome.info.lightingMode == LightingMode::Directional ? 2u : 0u);
        aoParameters = viewer_ao::ParametersForMode(reviewMode);
        if (chrome.info.ambientOcclusionStrengthOverride >= 0.0f) {
            aoParameters.strength = std::clamp(
                chrome.info.ambientOcclusionStrengthOverride, 0.0f, 1.0f);
        }
        if (chrome.info.ambientOcclusionRadiusScale > 0.0f) {
            aoParameters.radiusFraction = std::clamp(
                aoParameters.radiusFraction * chrome.info.ambientOcclusionRadiusScale,
                aoParameters.radiusMinFraction, aoParameters.radiusMaxFraction);
        }
    }
    lastAoMode = aoMode;
    lastAoFallbackReason = aoFallback;
    lastAoPassMilliseconds = 0.0;
    if (aoMode != 1) {
        lastAoStrength = 0.0f;
        lastAoRadiusFraction = 0.0f;
        lastAoWorldRadius = 0.0f;
        lastAoPixelRadius = 0.0f;
        lastAoSampleCount = 0;
    }

    // 1. Fit and render the geometry depth map before anything samples it.
    if (shadowMode == 1) {
        const auto shadowStart = std::chrono::steady_clock::now();
        D3D12_RESOURCE_BARRIER barriers[2]{};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = shadow.depth.Get();
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition.pResource = shadow.mask.Get();
        barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        commandList->ResourceBarrier(2, barriers);
        D3D12_CPU_DESCRIPTOR_HANDLE shadowDsv =
            shadow.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE shadowMaskRtv =
            shadow.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        commandList->OMSetRenderTargets(1, &shadowMaskRtv, FALSE, &shadowDsv);
        commandList->ClearDepthStencilView(shadowDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        const float maskClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        commandList->ClearRenderTargetView(shadowMaskRtv, maskClear, 0, nullptr);
        D3D12_VIEWPORT shadowViewport{ 0.0f, 0.0f,
            static_cast<float>(shadow.size), static_cast<float>(shadow.size), 0.0f, 1.0f };
        D3D12_RECT shadowScissor{ 0, 0, static_cast<LONG>(shadow.size),
            static_cast<LONG>(shadow.size) };
        commandList->RSSetViewports(1, &shadowViewport);
        commandList->RSSetScissorRects(1, &shadowScissor);
        if (model.srvHeap) {
            ID3D12DescriptorHeap* heaps[] = { model.srvHeap.Get() };
            commandList->SetDescriptorHeaps(1, heaps);
        }
        commandList->SetGraphicsRootSignature(shadowRootSignature.Get());
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const double zeroOrigin[3] = { 0.0, 0.0, 0.0 };
        const double zeroTarget[3] = { 0.0, 0.0, 0.0 };
        for (auto& mesh : model.meshes) {
            if (!mesh.drawEnabled || mesh.points || mesh.lines) continue;
            const bool canMask = mesh.completeVertex && mesh.textureHeap
                && mesh.materialConstants.mapped;
            const auto kind = viewer_ground_shadow::ClassifyShadowCaster(
                mesh.hasMaterial ? mesh.material.alphaMode : 0u,
                mesh.hasMaterial ? mesh.material.flags : 0u, canMask);
            if (kind == viewer_ground_shadow::ShadowCasterKind::None) continue;
            float shadowFrame[20]{};
            std::memcpy(shadowFrame, shadowFrustum.matrix, sizeof(shadowFrustum.matrix));
            shadowFrame[16] = kind == viewer_ground_shadow::ShadowCasterKind::Transmissive
                ? viewer_ground_shadow::TransmissionShadowAttenuation(
                    mesh.material.transmissionFactor)
                : 1.0f;
            const DrawConstants worldDraw = BuildDrawConstants(
                mesh.instanceTransform, mesh.origin, zeroOrigin, zeroTarget, groundTransform);
            commandList->SetGraphicsRoot32BitConstants(1, 33, &worldDraw, 0);
            commandList->SetGraphicsRoot32BitConstants(2, 20, shadowFrame, 0);
            if (kind == viewer_ground_shadow::ShadowCasterKind::Masked) {
                commandList->SetPipelineState(shadowMaskPipelineState.Get());
                uint32_t mapMask = 0;
                D3D12_GPU_DESCRIPTOR_HANDLE baseColor =
                    mesh.textureHeap->GetGPUDescriptorHandleForHeapStart();
                const int selected = mesh.textureIndices[0] >= 0
                    ? mesh.textureIndices[0] : int(mesh.neutralDescriptorBase);
                if (mesh.textureIndices[0] >= 0) mapMask |= 1u;
                baseColor.ptr += uint64_t(selected) * mesh.textureDescriptorSize;
                commandList->SetGraphicsRootDescriptorTable(4, baseColor);
                MaterialConstantData material{};
                const bool twoSided = !mesh.hasMaterial
                    || (mesh.material.flags & model_core::kMaterialFlagDoubleSided) != 0;
                FillMaterialConstantData(mesh.material, mapMask, twoSided, mesh.mirrored, material);
                const UINT64 materialOffset = UINT64(index) * kMaterialConstantStride;
                std::memcpy(mesh.materialConstants.mapped + materialOffset,
                            &material, sizeof(material));
                commandList->SetGraphicsRootConstantBufferView(3,
                    mesh.materialConstants.resource->GetGPUVirtualAddress() + materialOffset);
            } else {
                commandList->SetPipelineState(shadowPipelineState.Get());
            }
            commandList->IASetVertexBuffers(0, 1, &mesh.vbv);
            commandList->IASetIndexBuffer(&mesh.ibv);
            commandList->DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);
        }
        D3D12_RESOURCE_BARRIER sampleBarriers[2]{};
        sampleBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        sampleBarriers[0].Transition.pResource = shadow.depth.Get();
        sampleBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        sampleBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        sampleBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        sampleBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        sampleBarriers[1].Transition.pResource = shadow.mask.Get();
        sampleBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        sampleBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        sampleBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        commandList->ResourceBarrier(2, sampleBarriers);
        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissor);
        commandList->OMSetRenderTargets(2, targets, FALSE, &dsv);
        lastShadowPassMilliseconds =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - shadowStart).count();
    }

    // 2. The illuminated ground highlight remains a display-space backdrop
    // layer. The geometry shadow is drawn into the HDR scene below so exposure
    // and tone mapping stay in the normal linear pipeline.
    if (haveGroundLayer && opaqueBackdrop
        && (chrome.info.groundHighlightVisible || shadowMode == 2)
        && backdropRtvHeap && backdropColor) {
        groundConstants = viewer_ground_shadow::BuildConstants(
            plane, footprint, cameraTargetF,
            chrome.info.groundHighlightVisible, highlightTint,
            shadowMode == 2);
        D3D12_CPU_DESCRIPTOR_HANDLE backdropTarget =
            backdropRtvHeap->GetCPUDescriptorHandleForHeapStart();
        commandList->OMSetRenderTargets(1, &backdropTarget, FALSE, nullptr);
        commandList->SetGraphicsRootSignature(rootSignature.Get());
        commandList->SetPipelineState(groundPipelineState.Get());
        commandList->SetGraphicsRootConstantBufferView(0, constantBufferAddress);
        commandList->SetGraphicsRoot32BitConstants(1, 16, &groundConstants, 0);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->IASetVertexBuffers(0, 0, nullptr);
        commandList->IASetIndexBuffer(nullptr);
        commandList->DrawInstanced(6, 1, 0, 0);
        commandList->OMSetRenderTargets(2, targets, FALSE, &dsv);
    }

    // 3. Geometry-derived contact shadow as premultiplied black coverage in
    // the linear HDR target. Only the projected occluders darken the floor;
    // the rest of the ground quad remains transparent.
    if (shadowMode == 1 && haveGroundLayer && shadowFrustum.valid) {
        shadowConstants = viewer_ground_shadow::BuildShadowConstants(
            plane, footprint, shadowFrustum, cameraTargetF,
            viewer_ground_shadow::kDefaultOpacity);
        ID3D12DescriptorHeap* heaps[] = { shadow.srvHeap.Get() };
        commandList->SetDescriptorHeaps(1, heaps);
        commandList->SetGraphicsRootSignature(shadowGroundRootSignature.Get());
        commandList->SetPipelineState(shadowGroundPipelineState.Get());
        commandList->SetGraphicsRootConstantBufferView(0, constantBufferAddress);
        commandList->SetGraphicsRoot32BitConstants(1, 28, &shadowConstants, 0);
        commandList->SetGraphicsRootDescriptorTable(2,
            shadow.srvHeap->GetGPUDescriptorHandleForHeapStart());
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->IASetVertexBuffers(0, 0, nullptr);
        commandList->IASetIndexBuffer(nullptr);
        commandList->DrawInstanced(6, 1, 0, 0);
    }

    if (chrome.info.gridVisible && haveModelBounds) {
        DirectX::XMFLOAT3 boundsMin;
        DirectX::XMFLOAT3 boundsMax;
        TransformGridBounds(modelBoundsMin, modelBoundsMax,
            GroundAxisTransform(chrome.info.groundAxis, sourceUpAxis, chrome.info.showNativeOrientation,
                chrome.info.groundAxisInverted),
            boundsMin, boundsMax);
        const float extentX = boundsMax.x - boundsMin.x;
        const float extentY = boundsMax.y - boundsMin.y;
        const float extentZ = boundsMax.z - boundsMin.z;
        const float radius = std::max(0.001f,
            std::sqrt(extentX * extentX + extentY * extentY + extentZ * extentZ) * 0.5f);
        const float gridConstants[4] = {
            (boundsMin.x + boundsMax.x) * 0.5f - static_cast<float>(cameraTarget[0]),
            (boundsMin.y + boundsMax.y) * 0.5f - static_cast<float>(cameraTarget[1]),
            boundsMin.z - radius * 0.012f - static_cast<float>(cameraTarget[2]),
            radius * 2.2f,
        };
        commandList->SetGraphicsRootSignature(rootSignature.Get());
        commandList->SetPipelineState(gridPipelineState.Get());
        commandList->SetGraphicsRootConstantBufferView(0, constantBufferAddress);
        commandList->SetGraphicsRoot32BitConstants(1, 4, gridConstants, 0);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        commandList->IASetVertexBuffers(0, 0, nullptr);
        commandList->IASetIndexBuffer(nullptr);
        commandList->DrawInstanced(84, 1, 0, 0);
    }

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (model.srvHeap) {
        ID3D12DescriptorHeap* heaps[] = { model.srvHeap.Get() };
        commandList->SetDescriptorHeaps(1, heaps);
    }

    std::vector<GpuMesh*> draws;
    draws.reserve(model.meshes.size());
    DirectX::XMFLOAT4X4 modelTransform{};
    DirectX::XMStoreFloat4x4(&modelTransform,
        GroundAxisTransform(chrome.info.groundAxis, sourceUpAxis, chrome.info.showNativeOrientation,
            chrome.info.groundAxisInverted));
    const auto viewProjectionMatrix=DirectX::XMLoadFloat4x4(&viewProjection);
    for (auto& mesh:model.meshes) {
        if (!mesh.drawEnabled) continue;
        mesh.viewPriority = mesh.instanceId
            ? InstanceViewPriority(mesh.instanceBoundsMin, mesh.instanceBoundsMax, sceneOrigin,
                                   cameraTarget, modelTransform, viewProjection)
            : DetailViewPriority(mesh.sourceGeometry, sceneOrigin, cameraTarget, modelTransform, viewProjection);
        if (mesh.viewPriority == 0) continue;
        double center[3];
        for (unsigned axis=0;axis<3;++axis)
            center[axis]=(mesh.instanceId
                ? (mesh.instanceBoundsMin[axis]+mesh.instanceBoundsMax[axis])*0.5
                : mesh.sourceGeometry.origin[axis]
                    +(double(mesh.sourceGeometry.localMin[axis])+double(mesh.sourceGeometry.localMax[axis]))*0.5)
                - sceneOrigin[axis];
        const double transformedCenter[3] = {
            center[0]*modelTransform._11 + center[1]*modelTransform._21 + center[2]*modelTransform._31,
            center[0]*modelTransform._12 + center[1]*modelTransform._22 + center[2]*modelTransform._32,
            center[0]*modelTransform._13 + center[1]*modelTransform._23 + center[2]*modelTransform._33,
        };
        DirectX::XMFLOAT4 clip;
        DirectX::XMStoreFloat4(&clip,DirectX::XMVector4Transform(DirectX::XMVectorSet(
            float(transformedCenter[0]-cameraTarget[0]),float(transformedCenter[1]-cameraTarget[1]),
            float(transformedCenter[2]-cameraTarget[2]),1),
            viewProjectionMatrix));
        mesh.viewDepth=clip.w!=0 && std::isfinite(clip.z/clip.w) ? clip.z/clip.w : 1.0f;
        mesh.lastVisibleFrame = uint32_t(frameStats.PresentedFrames());
        draws.push_back(&mesh);
    }
    // T29: Clay overrides the effective draw bucket, not just the shader. Every
    // triangle is an opaque, depth-writing draw even when the authored
    // material was BLEND, transmissive or a decal; opaque surfaces therefore
    // sort and depth-test as solid geometry, with no blend-order seam or
    // see-through interior. MASK stays in the opaque bucket and keeps its
    // cutout discard. Lines and points keep their pipelines exactly as before.
    const bool clayMode=chrome.info.lightingMode==LightingMode::Clay;
    const auto flattenedForClay=[&](const GpuMesh* mesh) {
        return clayMode && !mesh->points && !mesh->lines;
    };
    const auto isBlendTriangle=[&](const GpuMesh* mesh) {
        return !flattenedForClay(mesh)
            && mesh->material.alphaMode==uint32_t(model_core::AlphaModeId::Blend);
    };
    // T27: an OPAQUE transmissive material also needs the transparent pass,
    // because transmission samples the finished opaque scene. It keeps its
    // authored alphaMode (OPAQUE) and opaque coverage (1); only its pass
    // changes. MASK transmission keeps the authored discard/cutout and stays in
    // the opaque bucket (the importer never marks it transmissive). Clay
    // suppresses the optical path entirely and draws the same surface opaque.
    const auto isForwardTransmission=[&](const GpuMesh* mesh) {
        return !flattenedForClay(mesh)
            && mesh->completeVertex && mesh->textureHeap && !mesh->points && !mesh->lines
            && mesh->hasMaterial
            && mesh->material.alphaMode==uint32_t(model_core::AlphaModeId::Opaque)
            && (mesh->material.flags&model_core::kMaterialFlagTransmissive)!=0;
    };
    const auto isTransparentTriangle=[&](const GpuMesh* mesh) {
        return isBlendTriangle(mesh)
            || (transmissionUsable && isForwardTransmission(mesh));
    };
    const auto isClayOverridden=[&](const GpuMesh* mesh) {
        return flattenedForClay(mesh)
            && (mesh->material.alphaMode==uint32_t(model_core::AlphaModeId::Blend)
                || (mesh->material.flags&model_core::kMaterialFlagTransmissive)!=0);
    };
    // OIT deliberately stops using mesh-centre depth as the transparent draw
    // authority. Transparent draws get a stable material/chunk order so any
    // permutation is reproducible; the accumulation itself is commutative.
    // The sorted fallback keeps the historical view-depth order. Clay's
    // overridden surfaces form a third, opaque-overlay bucket after the solid
    // prepass and before the (unused) transparent bucket; within it, far-to-near
    // ordering plus LESS_EQUAL makes the nearest authored surface win without
    // blend order or coplanar z-fighting.
    const auto drawRank=[&](const GpuMesh* mesh) {
        if (isTransparentTriangle(mesh)) return 2;
        if (isClayOverridden(mesh)) return 1;
        return 0;
    };
    std::stable_sort(draws.begin(),draws.end(),[&](const GpuMesh* a,const GpuMesh* b) {
        const int arank=drawRank(a), brank=drawRank(b);
        if (arank!=brank) return arank<brank;
        if (arank==1 && a->viewDepth!=b->viewDepth) return a->viewDepth>b->viewDepth;
        if (arank==2 && !useOit && a->viewDepth!=b->viewDepth) return a->viewDepth>b->viewDepth;
        if (a->hasMaterial!=b->hasMaterial) return a->hasMaterial>b->hasMaterial;
        if (a->materialChunkId!=b->materialChunkId) return a->materialChunkId<b->materialChunkId;
        return a->chunkId<b->chunkId;
    });
    const auto firstClayOverlay=std::find_if(draws.begin(),draws.end(),
        [&](const GpuMesh* mesh) { return isClayOverridden(mesh); });
    const auto firstTransparent=std::find_if(draws.begin(),draws.end(),
        [&](const GpuMesh* mesh) { return isTransparentTriangle(mesh); });
    if (reverseTransparentDrawOrderForTesting)
        std::reverse(firstTransparent,draws.end());
    lastTransparentDrawCount=static_cast<uint32_t>(std::distance(firstTransparent,draws.end()));
    lastForwardTransmissionDrawCount=static_cast<uint32_t>(std::count_if(
        firstTransparent,draws.end(),[&](const GpuMesh* mesh) {
            return isForwardTransmission(mesh);
        }));

    auto bindTextured=[&](GpuMesh& mesh,const DrawConstants& drawConstants,
                          ID3D12PipelineState* pso) {
        ID3D12DescriptorHeap* heaps[] = { mesh.textureHeap.Get() };
        commandList->SetDescriptorHeaps(1, heaps);
        commandList->SetGraphicsRootSignature(texturedRootSignature.Get());
        commandList->SetPipelineState(pso);
        D3D12_GPU_DESCRIPTOR_HANDLE environmentHandle =
            mesh.textureHeap->GetGPUDescriptorHandleForHeapStart();
        environmentHandle.ptr += uint64_t(model.environmentDescriptorIndex)
            * mesh.textureDescriptorSize;
        commandList->SetGraphicsRootDescriptorTable(5, environmentHandle);
        D3D12_GPU_DESCRIPTOR_HANDLE brdfHandle =
            mesh.textureHeap->GetGPUDescriptorHandleForHeapStart();
        brdfHandle.ptr += uint64_t(model.brdfLutDescriptorIndex)
            * mesh.textureDescriptorSize;
        commandList->SetGraphicsRootDescriptorTable(9, brdfHandle);
        D3D12_GPU_DESCRIPTOR_HANDLE transmissionHandle =
            mesh.textureHeap->GetGPUDescriptorHandleForHeapStart();
        transmissionHandle.ptr += uint64_t(model.transmissionDescriptorIndex)
            * mesh.textureDescriptorSize;
        commandList->SetGraphicsRootDescriptorTable(18, transmissionHandle);
        commandList->SetGraphicsRootConstantBufferView(0, constantBufferAddress);
        uint32_t mapMask=0;
        for (UINT slot=0;slot<model_core::kMaterialTextureSlotCount;++slot) {
            D3D12_GPU_DESCRIPTOR_HANDLE handle=mesh.textureHeap->GetGPUDescriptorHandleForHeapStart();
            const int selected=mesh.textureIndices[slot]>=0 ? mesh.textureIndices[slot]
                : int(mesh.neutralDescriptorBase+slot);
            if (mesh.textureIndices[slot]>=0) mapMask|=1u<<slot;
            handle.ptr+=uint64_t(selected)*mesh.textureDescriptorSize;
            commandList->SetGraphicsRootDescriptorTable(kMaterialSlotRootParameter[slot],handle);
        }
        const auto texturedDraw = BuildTexturedDrawConstants(drawConstants);
        commandList->SetGraphicsRoot32BitConstants(7, 25, &texturedDraw, 0);
        MaterialConstantData material{};
        const bool twoSided=!mesh.hasMaterial
            || (mesh.material.flags&model_core::kMaterialFlagDoubleSided)!=0;
        FillMaterialConstantData(mesh.material,mapMask,twoSided,mesh.mirrored,material);
        if (mesh.materialConstants.mapped && mesh.materialConstants.resource) {
            const UINT64 materialOffset=UINT64(index)*kMaterialConstantStride;
            std::memcpy(mesh.materialConstants.mapped+materialOffset,&material,sizeof(material));
            commandList->SetGraphicsRootConstantBufferView(8,
                mesh.materialConstants.resource->GetGPUVirtualAddress()+materialOffset);
        }
    };
    auto bindCompact=[&](GpuMesh& mesh,const DrawConstants& drawConstants,
                         ID3D12PipelineState* pso,bool blend) {
        commandList->SetGraphicsRootSignature(rootSignature.Get());
        commandList->SetPipelineState(pso);
        commandList->SetGraphicsRootConstantBufferView(0, constantBufferAddress);
        commandList->SetGraphicsRoot32BitConstants(1, 33, &drawConstants, 0);
        float color[4];
        std::memcpy(color,mesh.material.baseColorFactor,sizeof(color));
        if (!blend) color[3]=1.0f;
        commandList->SetGraphicsRoot32BitConstants(2, 4, color, 0);
    };
    auto drawMesh=[&](GpuMesh& mesh) {
        commandList->IASetPrimitiveTopology(mesh.points ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST
            : mesh.lines ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->IASetVertexBuffers(0, 1, &mesh.vbv);
        commandList->IASetIndexBuffer(mesh.points ? nullptr : &mesh.ibv);
        if (mesh.points) commandList->DrawInstanced(mesh.vertexCount, 1, 0, 0);
        else commandList->DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);
    };

    if (chrome.info.lightingMode!=LightingMode::Wireframe) {
      // Opaque and masked geometry first, with depth writes, into scene+pick
      // (plus the T32 indirect HDR target when the AO layout is available).
      commandList->OMSetRenderTargets(aoLayout ? 3 : 2,
          aoLayout ? opaqueTargets : targets, FALSE, &dsv);
      const auto opaqueEnd = (firstClayOverlay < firstTransparent)
          ? firstClayOverlay : firstTransparent;
      for (auto it=draws.begin();it!=opaqueEnd;++it) {
        auto& mesh=**it;
        DrawConstants drawConstants=BuildDrawConstants(mesh.instanceTransform,mesh.origin,
            sceneOrigin,cameraTarget,modelTransform);
        drawConstants.pickId=mesh.instanceId ? mesh.instanceId : mesh.chunkId;
        const bool triangle=!mesh.points && !mesh.lines;
        // Points and lines keep the two-target layout; only opaque triangle
        // surfaces contribute an indirect share.
        commandList->OMSetRenderTargets(triangle && aoLayout ? 3 : 2,
            triangle && aoLayout ? opaqueTargets : targets, FALSE, &dsv);
        if (mesh.completeVertex && triangle && mesh.textureHeap) {
            const bool twoSided=!mesh.hasMaterial
                || (mesh.material.flags&model_core::kMaterialFlagDoubleSided)!=0;
            bindTextured(mesh,drawConstants, aoLayout
                ? (twoSided?texturedDoubleSidedPipelineState.Get()
                   :(mesh.mirrored?texturedMirroredPipelineState.Get()
                     :texturedPipelineState.Get()))
                : (twoSided?texturedLegacyDoubleSidedPipelineState.Get()
                   :(mesh.mirrored?texturedLegacyMirroredPipelineState.Get()
                     :texturedLegacyPipelineState.Get())));
        } else {
            ID3D12PipelineState* compactState = mesh.lines ? linePipelineState.Get()
                : mesh.points ? (mesh.completeVertex?coloredPointPipelineState.Get()
                                 :pointPipelineState.Get())
                : mesh.positionOnly ? (aoLayout?positionOnlyAoPipelineState.Get()
                                       :positionOnlyPipelineState.Get())
                : (aoLayout?pipelineState.Get():pipelineStateLegacy.Get());
            bindCompact(mesh,drawConstants,compactState,false);
        }
        drawMesh(mesh);
      }

      // T29 Clay overridden surfaces (authored BLEND/transmission) draw after
      // the solid prepass as opaque, depth-writing geometry. LESS_EQUAL lets a
      // coplanar window/trim pair resolve to the surface authored in front
      // without z-fighting; far-to-near order gives the nearest of two
      // overridden surfaces. No blend bucket is entered.
      for (auto it=firstClayOverlay;it!=firstTransparent &&
           firstClayOverlay<firstTransparent;++it) {
        auto& mesh=**it;
        DrawConstants drawConstants=BuildDrawConstants(mesh.instanceTransform,mesh.origin,
            sceneOrigin,cameraTarget,modelTransform);
        drawConstants.pickId=mesh.instanceId ? mesh.instanceId : mesh.chunkId;
        commandList->OMSetRenderTargets(aoLayout ? 3 : 2,
            aoLayout ? opaqueTargets : targets, FALSE, &dsv);
        if (mesh.completeVertex && mesh.textureHeap && !mesh.points && !mesh.lines) {
            const bool twoSided=!mesh.hasMaterial
                || (mesh.material.flags&model_core::kMaterialFlagDoubleSided)!=0;
            bindTextured(mesh,drawConstants, aoLayout
                ? (twoSided?texturedClayDoubleSidedPipelineState.Get()
                   :(mesh.mirrored?texturedClayMirroredPipelineState.Get()
                     :texturedClayPipelineState.Get()))
                : (twoSided?texturedLegacyClayDoubleSidedPipelineState.Get()
                   :(mesh.mirrored?texturedLegacyClayMirroredPipelineState.Get()
                     :texturedLegacyClayPipelineState.Get())));
        } else {
            bindCompact(mesh,drawConstants,
                aoLayout?clayPipelineState.Get():clayPipelineStateLegacy.Get(),false);
        }
        drawMesh(mesh);
      }

      // T32: AO now sees the finished opaque/clay depth and writes only the two
      // bounded masks. Depth is returned to DEPTH_WRITE for the transparent and
      // pick passes; transparent surfaces never write depth before this point,
      // so a window cannot become an opaque AO sheet.
      if (aoMode == 1 && aoLayout) {
          DirectX::XMFLOAT4X4 inverseViewProjection{};
          DirectX::XMStoreFloat4x4(&inverseViewProjection,
              DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&viewProjection)));
          const DirectX::XMFLOAT4 aoViewport(viewport.TopLeftX, viewport.TopLeftY,
                                             viewport.Width, viewport.Height);
          if (!RenderScreenSpaceAo(inverseViewProjection, aoParameters, sceneScale,
                                   aoViewport, eyeSelection)) {
              lastAoMode = 2;
              lastAoFallbackReason = ao.fallbackReason ? ao.fallbackReason
                                                       : AoFallbackAllocationFailed;
          }
          commandList->RSSetViewports(1, &viewport);
          commandList->RSSetScissorRects(1, &scissor);
          commandList->OMSetRenderTargets(2, targets, FALSE, &dsv);
      }

      const bool needsTransmissionSnapshot=debugChannel==0
          && std::any_of(firstTransparent,draws.end(),
          [&](const GpuMesh* mesh) {
              return mesh->completeVertex && mesh->textureHeap && mesh->hasMaterial
                  && (mesh->material.flags&model_core::kMaterialFlagTransmissive)!=0;
          });
      if (needsTransmissionSnapshot) {
          // T27: snapshot and filter the finished opaque scene before any
          // transmissive draw. Nothing below samples a target it is writing.
          GenerateTransmissionMips();
          commandList->RSSetViewports(1,&viewport);
          commandList->RSSetScissorRects(1,&scissor);
      }

      if (useOit) {
        // One bounded transparent pass into the two OIT targets. Depth tests
        // against the opaque prepass reject hidden geometry; no depth writes
        // and no per-pixel list, so memory/work do not grow with layer count.
        commandList->OMSetRenderTargets(2, oitTargets, FALSE, &dsv);
        for (auto it=firstTransparent;it!=draws.end();++it) {
            auto& mesh=**it;
            DrawConstants drawConstants=BuildDrawConstants(mesh.instanceTransform,mesh.origin,
                sceneOrigin,cameraTarget,modelTransform);
            drawConstants.pickId=mesh.instanceId ? mesh.instanceId : mesh.chunkId;
            if (mesh.completeVertex && mesh.textureHeap) {
                const bool twoSided=!mesh.hasMaterial
                    || (mesh.material.flags&model_core::kMaterialFlagDoubleSided)!=0;
                bindTextured(mesh,drawConstants,
                    twoSided?texturedOitDoubleSidedPipelineState.Get()
                    :(mesh.mirrored?texturedOitMirroredPipelineState.Get():texturedOitPipelineState.Get()));
            } else {
                bindCompact(mesh,drawConstants,oitPipelineState.Get(),true);
            }
            drawMesh(mesh);
        }
      } else {
        // Recorded fallback: the historical stable-sorted straight-alpha path.
        // Restore the scene+pick targets because the transmission snapshot pass
        // rebound the render targets to the mip levels.
        commandList->OMSetRenderTargets(2, targets, FALSE, &dsv);
        for (auto it=firstTransparent;it!=draws.end();++it) {
            auto& mesh=**it;
            DrawConstants drawConstants=BuildDrawConstants(mesh.instanceTransform,mesh.origin,
                sceneOrigin,cameraTarget,modelTransform);
            drawConstants.pickId=mesh.instanceId ? mesh.instanceId : mesh.chunkId;
            if (mesh.completeVertex && mesh.textureHeap) {
                const bool twoSided=!mesh.hasMaterial
                    || (mesh.material.flags&model_core::kMaterialFlagDoubleSided)!=0;
                bindTextured(mesh,drawConstants,
                    twoSided?texturedBlendDoubleSidedPipelineState.Get()
                    :(mesh.mirrored?texturedBlendMirroredPipelineState.Get():texturedBlendPipelineState.Get()));
            } else {
                bindCompact(mesh,drawConstants,blendPipelineState.Get(),true);
            }
            drawMesh(mesh);
        }
      }

      if (firstTransparent!=draws.end()) {
        // Deterministic nearest-visible transparent selection. This pass runs
        // for both OIT and the fallback, writes only the integer pick target,
        // and writes depth so neither draw order nor an averaged id decides the
        // answer. Opaque ids already in the target stay for hidden geometry.
        commandList->OMSetRenderTargets(2, targets, FALSE, &dsv);
        for (auto it=firstTransparent;it!=draws.end();++it) {
            auto& mesh=**it;
            DrawConstants drawConstants=BuildDrawConstants(mesh.instanceTransform,mesh.origin,
                sceneOrigin,cameraTarget,modelTransform);
            drawConstants.pickId=mesh.instanceId ? mesh.instanceId : mesh.chunkId;
            if (mesh.completeVertex && mesh.textureHeap) {
                const bool twoSided=!mesh.hasMaterial
                    || (mesh.material.flags&model_core::kMaterialFlagDoubleSided)!=0;
                bindTextured(mesh,drawConstants,
                    twoSided?texturedPickDoubleSidedPipelineState.Get()
                    :(mesh.mirrored?texturedPickMirroredPipelineState.Get():texturedPickPipelineState.Get()));
            } else {
                bindCompact(mesh,drawConstants,pickPipelineState.Get(),true);
            }
            drawMesh(mesh);
        }
      }
    }

    // Wireframe is its own fill-free inspection mode. The material pass above
    // is skipped completely, so textures and triangle surfaces are invisible.
    if (chrome.info.lightingMode==LightingMode::Wireframe) {
        for (GpuMesh* meshPtr:draws) {
            auto& mesh=*meshPtr;
            if (mesh.points) continue;
            if (mesh.lines) {
                DrawConstants lineConstants=BuildDrawConstants(mesh.instanceTransform,mesh.origin,
                    sceneOrigin,cameraTarget,modelTransform);
                lineConstants.pickId=(mesh.instanceId ? mesh.instanceId : mesh.chunkId)|0x80000000u;
                commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
                commandList->SetGraphicsRootSignature(rootSignature.Get());
                commandList->SetPipelineState(linePipelineState.Get());
                commandList->SetGraphicsRootConstantBufferView(0,constantBufferAddress);
                commandList->SetGraphicsRoot32BitConstants(1,33,&lineConstants,0);
                commandList->IASetVertexBuffers(0,1,&mesh.vbv);
                commandList->IASetIndexBuffer(&mesh.ibv);
                commandList->DrawIndexedInstanced(mesh.indexCount,1,0,0,0);
                continue;
            }
            DrawConstants drawConstants=BuildDrawConstants(mesh.instanceTransform,mesh.origin,
                sceneOrigin,cameraTarget,modelTransform);
            drawConstants.pickId=(mesh.instanceId ? mesh.instanceId : mesh.chunkId)|0x80000000u;
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            if (mesh.completeVertex && mesh.textureHeap) {
                ID3D12DescriptorHeap* heaps[] = { mesh.textureHeap.Get() };
                commandList->SetDescriptorHeaps(1, heaps);
                commandList->SetGraphicsRootSignature(texturedRootSignature.Get());
                commandList->SetPipelineState(texturedWireframePipelineState.Get());
                D3D12_GPU_DESCRIPTOR_HANDLE environmentHandle =
                    mesh.textureHeap->GetGPUDescriptorHandleForHeapStart();
                environmentHandle.ptr += uint64_t(model.environmentDescriptorIndex)
                    * mesh.textureDescriptorSize;
                commandList->SetGraphicsRootDescriptorTable(5, environmentHandle);
                D3D12_GPU_DESCRIPTOR_HANDLE brdfHandle =
                    mesh.textureHeap->GetGPUDescriptorHandleForHeapStart();
                brdfHandle.ptr += uint64_t(model.brdfLutDescriptorIndex)
                    * mesh.textureDescriptorSize;
                commandList->SetGraphicsRootDescriptorTable(9, brdfHandle);
                D3D12_GPU_DESCRIPTOR_HANDLE transmissionHandle =
                    mesh.textureHeap->GetGPUDescriptorHandleForHeapStart();
                transmissionHandle.ptr += uint64_t(model.transmissionDescriptorIndex)
                    * mesh.textureDescriptorSize;
                commandList->SetGraphicsRootDescriptorTable(18, transmissionHandle);
                commandList->SetGraphicsRootConstantBufferView(0,constantBufferAddress);
                uint32_t mapMask=0;
                for (UINT slot=0;slot<model_core::kMaterialTextureSlotCount;++slot) {
                    D3D12_GPU_DESCRIPTOR_HANDLE handle=mesh.textureHeap->GetGPUDescriptorHandleForHeapStart();
                    const int selected=mesh.textureIndices[slot]>=0 ? mesh.textureIndices[slot]
                        : int(mesh.neutralDescriptorBase+slot);
                    if (mesh.textureIndices[slot]>=0) mapMask|=1u<<slot;
                    handle.ptr+=uint64_t(selected)*mesh.textureDescriptorSize;
                    commandList->SetGraphicsRootDescriptorTable(kMaterialSlotRootParameter[slot],handle);
                }
                const auto texturedDraw = BuildTexturedDrawConstants(drawConstants);
                commandList->SetGraphicsRoot32BitConstants(7,25,&texturedDraw,0);
                const bool twoSided=(mesh.material.flags&model_core::kMaterialFlagDoubleSided)!=0;
                MaterialConstantData material{};
                FillMaterialConstantData(mesh.material,mapMask,twoSided,mesh.mirrored,material);
                if (mesh.materialConstants.mapped && mesh.materialConstants.resource) {
                    const UINT64 materialOffset=UINT64(index)*kMaterialConstantStride;
                    std::memcpy(mesh.materialConstants.mapped+materialOffset,&material,sizeof(material));
                    commandList->SetGraphicsRootConstantBufferView(8,
                        mesh.materialConstants.resource->GetGPUVirtualAddress()+materialOffset);
                }
            } else {
                commandList->SetGraphicsRootSignature(rootSignature.Get());
                commandList->SetPipelineState(mesh.positionOnly
                    ? positionOnlyWireframePipelineState.Get() : wireframePipelineState.Get());
                commandList->SetGraphicsRootConstantBufferView(0,constantBufferAddress);
                commandList->SetGraphicsRoot32BitConstants(1,33,&drawConstants,0);
            }
            commandList->IASetVertexBuffers(0,1,&mesh.vbv);
            commandList->IASetIndexBuffer(&mesh.ibv);
            commandList->DrawIndexedInstanced(mesh.indexCount,1,0,0,0);
        }
    }

    // T21 numeric diagnostic readback. The HDR scene target is still a render
    // target here; copy the bounded sample region before the resolve applies
    // exposure/tone mapping. An unarmed request is a no-op.
    if (debugSampleArmed) {
        debugSampleArmed = false;
        const UINT sampleWidth = static_cast<UINT>(debugSampleX1 - debugSampleX0);
        const UINT sampleHeight = static_cast<UINT>(debugSampleY1 - debugSampleY0);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = sceneColor.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = debugReadback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint.Footprint = {
            kSceneColorFormat, sampleWidth, sampleHeight, 1,
            static_cast<UINT>(kDebugSampleMaxExtent) * 8};
        const D3D12_BOX box{static_cast<UINT>(debugSampleX0), static_cast<UINT>(debugSampleY0), 0,
                            static_cast<UINT>(debugSampleX1), static_cast<UINT>(debugSampleY1), 1};
        commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
        debugSampleRecorded = true;
        debugSampleFence = UINT64_MAX; // resolved to the slot fence after EndFrame
    }
    const bool picking = !pickInFlight && pickX >= 0 && pickY >= 0
        && UINT(pickX) < swapChain.Width() && UINT(pickY) < swapChain.Height();
    if (picking) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = pickTarget.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        commandList->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = pickTarget.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dest{}; dest.pResource = pickReadback.Get();
        dest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dest.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_UINT, 1, 1, 1, 256};
        D3D12_BOX box{UINT(pickX),UINT(pickY),0,UINT(pickX+1),UINT(pickY+1),1};
        commandList->CopyTextureRegion(&dest,0,0,0,&source,&box);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
        commandList->ResourceBarrier(1, &barrier);
    }

    // Full-screen resolve: linear HDR -> background composite -> exposure ->
    // tone map -> sRGB back buffer. The overlay still draws afterwards.
    ResolveSceneToBackBuffer(swapChain.BackBufferRtv(index), chrome.info);

    if (FAILED(commandList->Close())) return;
    ID3D12CommandList* lists[] = { commandList.Get() };
    directQueue.Queue()->ExecuteCommandLists(1, lists);

    DrawChrome(index, orientation, chrome);
    EndFrame(index);
    if (aoQueryRecorded) {
        aoQueryFence = frames[index].fenceValue;
        aoQueryRecorded = false;
        if (aoQueryFence == 0) aoQueryInFlight = false;
    }
    if (debugSampleRecorded && debugSampleFence == UINT64_MAX) {
        debugSampleFence = frames[index].fenceValue;
    }    if (picking) { pickFence = frames[index].fenceValue; pickInFlight = true; pickX = pickY = -1; }
}

bool D3D12ViewerPath::CreateAndQueueBuffer(const void* data, uint64_t sizeBytes, uint32_t clusterId,
                                           ComPtr<ID3D12Resource>& outBuffer, std::wstring& error, uint64_t destinationOffset)
{
    HRESULT allocationResult = S_OK;
    auto destination = outBuffer ? outBuffer : CreateBuffer(device.Device(), sizeBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, &allocationResult);
    if (!destination) {
        if (allocationResult == E_OUTOFMEMORY) uploadErrorCode = model_core::ImportErrorCode::OutOfMemory;
        error = L"A GPU buffer could not be created.";
        return false;
    }

    D3D12UploadRing::UploadRequest request;
    request.sourceBytes = std::span<const std::byte>(static_cast<const std::byte*>(data),
                                                      static_cast<size_t>(sizeBytes));
    request.destination = destination.Get();
    request.destinationOffset = destinationOffset;
    request.generation = pendingToken;
    request.clusterId = clusterId;

    switch (uploadRing.Upload(request)) {
    case D3D12UploadRing::UploadResult::Uploaded:
        break;
    case D3D12UploadRing::UploadResult::Backpressured:
        error = L"The upload staging ring could not make room for this model.";
        return false;
    case D3D12UploadRing::UploadResult::Failed:
    default:
        error = L"A mesh buffer could not be queued for upload.";
        return false;
    }

    outBuffer = destination;
    return true;
}

bool D3D12ViewerPath::CreateAndQueueTexture(const d3d12_import_bridge::ImportedImage& image, UINT heapIndex,
                                             ID3D12DescriptorHeap& heap, UINT descriptorSize,
                                             uint32_t clusterId, ComPtr<ID3D12Resource>& outTexture,
                                             std::wstring& error)
{
    if (uploadIsCancelled && uploadIsCancelled()) return false;
    auto dxgiFormat = DxgiFormatFor(image.pixelFormat, image.colorSpace);
    if (!dxgiFormat || image.width == 0 || image.height == 0 || image.mipLevels == 0 || image.mipLevels > model_core::FullImageMipCount(image.width,image.height)) {
        error = L"An imported texture had an unsupported format or mip count.";
        return false;
    }

    const auto expected=model_core::ComputeImagePixelBytes(image.pixelFormat,image.width,image.height,image.mipLevels);
    if (!expected || *expected!=image.pixelBytes.size() || image.width>model_core::kMaxTextureDimension
        || image.height>model_core::kMaxTextureDimension) { error=L"An imported image has an invalid byte size or dimension.";return false; }
    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = image.width;
    texDesc.Height = image.height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = static_cast<UINT16>(image.mipLevels);
    texDesc.Format = *dxgiFormat;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> texture;
    // COMMON, not COPY_DEST: the copy now happens on a copy queue, which
    // cannot record the COPY_DEST -> PIXEL_SHADER_RESOURCE barrier the old
    // direct-queue path used. COMMON lets the resource promote implicitly
    // in both directions instead. Proven, including a debug-layer message
    // count, in UploadRingTests.cpp.
    const HRESULT allocationResult = device.Device()->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
                                                          D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                          IID_PPV_ARGS(&texture));
    if (FAILED(allocationResult)) {
        if (allocationResult == E_OUTOFMEMORY) uploadErrorCode = model_core::ImportErrorCode::OutOfMemory;
        error = L"A GPU texture could not be created.";
        return false;
    }

    D3D12UploadRing::UploadTextureRequest request;
    request.sourceBytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(image.pixelBytes.data()), image.pixelBytes.size());
    request.destination = texture.Get();
    request.width = image.width;
    request.height = image.height;
    request.format = *dxgiFormat;
    request.generation = pendingToken;
    request.clusterId = clusterId;

    // Record smallest levels first. Publish only once the complete immutable
    // resource has crossed the copy fence; a low-chain import was published earlier.
    for (uint32_t level=image.mipLevels;level-->0;) {
        if (uploadIsCancelled && uploadIsCancelled()) { outTexture=texture;return false; }
        const uint32_t w=std::max(1u,image.width>>level),h=std::max(1u,image.height>>level);
        const auto offset=level ? model_core::ComputeImagePixelBytes(image.pixelFormat,image.width,image.height,level)
                                : std::optional<uint64_t>(0);
        const auto size=model_core::ComputeImagePixelBytes(image.pixelFormat,w,h,1);
        if (!offset || !size || *offset>image.pixelBytes.size() || *size>image.pixelBytes.size()-*offset) {
            outTexture=texture; error=L"An imported mip has an invalid size."; return false;
        }
        request.sourceBytes=std::span(image.pixelBytes).subspan(static_cast<size_t>(*offset),static_cast<size_t>(*size));
        request.width=w;request.height=h;request.destinationSubresource=level;
        request.publishResource=level==0;
        switch (uploadRing.UploadTexture(request)) {
    case D3D12UploadRing::UploadResult::Uploaded:
        break;
    case D3D12UploadRing::UploadResult::Backpressured:
        error = L"The upload staging ring could not make room for this model's textures.";
        outTexture=texture; return false;
    case D3D12UploadRing::UploadResult::Failed:
    default:
        // UploadTexture also rejects a source smaller than the footprint it
        // computed, which is the "declared size" check this used to make
        // itself.
        error = L"An imported texture could not be queued for upload.";
        outTexture=texture; return false;
    }

    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = *dxgiFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = image.mipLevels;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = heap.GetCPUDescriptorHandleForHeapStart();
    cpuHandle.ptr += static_cast<SIZE_T>(heapIndex) * descriptorSize;
    device.Device()->CreateShaderResourceView(texture.Get(), &srvDesc, cpuHandle);

    outTexture = texture;
    return true;
}

bool D3D12ViewerPath::EnsureMaterialConstants(GpuMesh& mesh, std::wstring& error)
{
    if (mesh.materialConstants.resource && mesh.materialConstants.mapped
        && mesh.materialConstants.materialChunkId == mesh.materialChunkId
        && mesh.materialConstants.mirrored == mesh.mirrored)
        return true;
    GpuMesh::MaterialConstantBuffer constants;
    HRESULT allocationResult = S_OK;
    const uint64_t bytes = uint64_t(kMaterialConstantStride) * kFrameCount;
    constants.resource = CreateBuffer(device.Device(), bytes, D3D12_HEAP_TYPE_UPLOAD,
                                      D3D12_RESOURCE_STATE_GENERIC_READ, &allocationResult);
    if (!constants.resource) {
        if (allocationResult == E_OUTOFMEMORY) uploadErrorCode = model_core::ImportErrorCode::OutOfMemory;
        error = L"A material constant buffer could not be created.";
        return false;
    }
    D3D12_RANGE noRead{0, 0};
    void* mapped = nullptr;
    if (FAILED(constants.resource->Map(0, &noRead, &mapped)) || !mapped) {
        error = L"A material constant buffer could not be mapped.";
        return false;
    }
    constants.mapped = static_cast<std::byte*>(mapped);
    constants.materialChunkId = mesh.materialChunkId;
    constants.mirrored = mesh.mirrored;
    const auto desc = constants.resource->GetDesc();
    constants.allocationBytes = device.Device()->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    mesh.materialConstants = constants;
    return true;
}

bool D3D12ViewerPath::BeginUploadModel(const std::vector<d3d12_import_bridge::ImportedMesh>& importedMeshes,
                                         const std::vector<d3d12_import_bridge::ImportedMaterial>& importedMaterials,
                                         const std::vector<d3d12_import_bridge::ImportedImage>& importedImages,
                                         std::wstring& error,
                                         std::span<const d3d12_import_bridge::ImportedNode> importedNodes,
                                         std::span<const d3d12_import_bridge::ImportedInstance> importedInstances)
{
    uploadErrorCode = model_core::ImportErrorCode::UploadFailure;
    // Supersede anything already in flight. Advancing IS the cancellation
    // signal: the abandoned batch's copies still complete, but
    // DrainCompletedPublications drops their publications as stale instead
    // of promoting them. Deliberately does NOT touch `model` -- the
    // currently drawn one keeps drawing until this batch is fully ready.
    pendingToken = platform::GenerationToken(uploadGeneration.Advance());
    RetireInFlightUpload();
    pendingResourceCount = 0;
    uploadInFlight = false;

    ModelResources staged;
    staged.meshes.reserve((std::max)(importedMeshes.size(), importedInstances.size()));
    retiredModels.reserve(retiredModels.size() + 2);
    const bool needsNeutralTextures=std::any_of(importedMeshes.begin(),importedMeshes.end(),[](const auto& mesh) {
        return mesh.geometry.lodLevel!=model_core::kScanLod
            && mesh.topology==model_core::ChunkTopology::TriangleList
            && mesh.vertexLayoutId==model_core::VertexLayoutId::PositionNormalUv0TangentColor_F32;
    });

    // Queue each unique image at most once, into a fresh shader-visible
    // heap sized to importedImages.size() -- created up front so each SRV
    // can be written straight to its slot.
    if (!importedImages.empty() || needsNeutralTextures) {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        // Thirteen neutral slots, one for every fixed material texture slot.
        // Unbound slots sample these and are skipped by the shader's map mask;
        // the descriptor table still needs a valid view for each root table.
        const UINT materialDescriptors = static_cast<UINT>(
            importedImages.size() + (needsNeutralTextures
                ? model_core::kMaterialTextureSlotCount : 0));
        staged.environmentDescriptorIndex = materialDescriptors;
        staged.brdfLutDescriptorIndex = materialDescriptors + 1;
        staged.transmissionDescriptorIndex = materialDescriptors + 2;
        heapDesc.NumDescriptors = materialDescriptors + 3; // + cubemap + LUT + transmission
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        const HRESULT heapResult = device.Device()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&staged.srvHeap));
        if (FAILED(heapResult)) {
            if (heapResult == E_OUTOFMEMORY) uploadErrorCode = model_core::ImportErrorCode::OutOfMemory;
            error = L"The texture descriptor heap could not be created.";
            return false;
        }
        staged.srvDescriptorSize
            = device.Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        WriteEnvironmentSrv(*staged.srvHeap.Get(), staged.environmentDescriptorIndex,
                            staged.srvDescriptorSize);
        WriteBrdfLutSrv(*staged.srvHeap.Get(), staged.brdfLutDescriptorIndex,
                        staged.srvDescriptorSize);
        WriteTransmissionSrv(*staged.srvHeap.Get(), staged.transmissionDescriptorIndex,
                             staged.srvDescriptorSize);

        staged.textures.resize(importedImages.size());
        for (size_t i = 0; i < importedImages.size(); ++i) {
            GpuTexture texture;
            texture.chunkId = importedImages[i].logicalChunkId ? importedImages[i].logicalChunkId : importedImages[i].chunkId;
            texture.heap = staged.srvHeap;
            texture.descriptorSize = staged.srvDescriptorSize;
            texture.srvHeapIndex = static_cast<UINT>(i);
            if (!CreateAndQueueTexture(importedImages[i], texture.srvHeapIndex, *staged.srvHeap.Get(),
                                        staged.srvDescriptorSize, static_cast<uint32_t>(kTextureClusterIdBase + i),
                                        texture.resource, error)) {
                staged.textures[i] = std::move(texture);
                RetireStagedResources(std::move(staged));
                pendingResourceCount = 0;
                return false;
            }
            staged.textures[i] = std::move(texture);
            const auto textureDesc = staged.textures[i].resource->GetDesc();
            staged.textures[i].allocationBytes = device.Device()->GetResourceAllocationInfo(0,1,&textureDesc).SizeInBytes;
            ++pendingResourceCount;
        }
        const uint8_t fallbackPixels[model_core::kMaterialTextureSlotCount][4]={
            {255,255,255,255},{255,255,255,255},{128,128,255,255},{0,0,0,255},
            {255,255,255,255},{255,255,255,255},{255,255,255,255},{128,128,255,255},
            {255,255,255,255},{255,255,255,255},{255,255,255,255},{255,255,255,255},
            {255,255,255,255}};
        staged.neutralTextures.resize(needsNeutralTextures
            ? model_core::kMaterialTextureSlotCount : 0);
        for (size_t i=0;i<staged.neutralTextures.size();++i) {
            d3d12_import_bridge::ImportedImage fallback;
            fallback.pixelFormat=model_core::PixelFormatId::RGBA8_UNORM;
            fallback.width=fallback.height=fallback.mipLevels=1;
            const bool srgbColor=i==0||i==3||i==9||i==10;
            fallback.colorSpace=srgbColor ? model_core::ColorSpaceId::Srgb
                                          : model_core::ColorSpaceId::Linear;
            fallback.pixelBytes.resize(4);
            std::memcpy(fallback.pixelBytes.data(),fallbackPixels[i],4);
            auto& texture=staged.neutralTextures[i];
            texture.heap=staged.srvHeap;texture.descriptorSize=staged.srvDescriptorSize;
            texture.srvHeapIndex=static_cast<UINT>(importedImages.size()+i);
            if (!CreateAndQueueTexture(fallback,texture.srvHeapIndex,*staged.srvHeap.Get(),staged.srvDescriptorSize,
                    static_cast<uint32_t>(kTextureClusterIdBase+importedImages.size()+i),texture.resource,error)) {
                RetireStagedResources(std::move(staged));pendingResourceCount=0;return false;
            }
            const auto desc=texture.resource->GetDesc();
            texture.allocationBytes=device.Device()->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;
            ++pendingResourceCount;
        }
    }

    auto findImageIndex = [&importedImages](uint32_t chunkId) -> int {
        if (chunkId == 0) return -1;
        for (size_t i = 0; i < importedImages.size(); ++i) {
            if (importedImages[i].chunkId == chunkId) return static_cast<int>(i);
        }
        return -1;
    };
    auto findMaterial = [&importedMaterials](uint32_t chunkId) -> const d3d12_import_bridge::ImportedMaterial* {
        if (chunkId == 0) return nullptr;
        for (const auto& m : importedMaterials) {
            if (m.chunkId == chunkId) return &m;
        }
        return nullptr;
    };
    // T22 material cbuffers. One CPU-writable region per material chunk (and a
    // shared default for meshes without one), each with kFrameCount slots so a
    // per-frame write never races a frame still sampling the previous copy.
    constexpr uint32_t kDefaultMaterialBufferKey = 0xFFFFFFFFu;
    std::unordered_map<uint64_t, GpuMesh::MaterialConstantBuffer> materialConstantBuffers;
    bool materialBufferFailed = false;
    const auto materialConstantsFor = [&](uint32_t materialChunkId, bool mirrored) {
        // The mirrored sign is the only per-draw value that reaches the
        // material cbuffer, so two instances of one material can share it only
        // when they agree on handedness.
        const uint64_t key = (uint64_t(materialChunkId ? materialChunkId : kDefaultMaterialBufferKey) << 1)
            | (mirrored ? 1u : 0u);
        if (const auto found = materialConstantBuffers.find(key); found != materialConstantBuffers.end())
            return found->second;
        GpuMesh::MaterialConstantBuffer constants;
        HRESULT allocationResult = S_OK;
        const uint64_t bytes = uint64_t(kMaterialConstantStride) * kFrameCount;
        constants.resource = CreateBuffer(device.Device(), bytes, D3D12_HEAP_TYPE_UPLOAD,
                                          D3D12_RESOURCE_STATE_GENERIC_READ, &allocationResult);
        if (!constants.resource) {
            if (allocationResult == E_OUTOFMEMORY) uploadErrorCode = model_core::ImportErrorCode::OutOfMemory;
            error = L"A material constant buffer could not be created.";
            materialBufferFailed = true;
            return constants;
        }
        D3D12_RANGE noRead{0, 0};
        void* mapped = nullptr;
        if (FAILED(constants.resource->Map(0, &noRead, &mapped)) || !mapped) {
            error = L"A material constant buffer could not be mapped.";
            materialBufferFailed = true;
            return constants;
        }
        constants.mapped = static_cast<std::byte*>(mapped);
        constants.materialChunkId = materialChunkId;
        constants.mirrored = mirrored;
        const auto desc = constants.resource->GetDesc();
        constants.allocationBytes = device.Device()->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        materialConstantBuffers.emplace(key, constants);
        return constants;
    };
    struct ResolvedNode { double world[16]{}; bool visible = false; bool resolving = false; };
    std::unordered_map<uint32_t, const model_core::NodePayload*> sourceNodes;
    std::unordered_map<uint32_t, ResolvedNode> resolvedNodes;
    for (const auto& node : importedNodes) sourceNodes.emplace(node.data.nodeId, &node.data);
    std::function<bool(uint32_t)> resolveNode = [&](uint32_t id) {
        if (auto found = resolvedNodes.find(id); found != resolvedNodes.end() && !found->second.resolving)
            return true;
        const auto source = sourceNodes.find(id);
        if (source == sourceNodes.end()) return false;
        auto& resolved = resolvedNodes[id];
        if (resolved.resolving) return false;
        resolved.resolving = true;
        const auto& payload = *source->second;
        std::memcpy(resolved.world, payload.localTransform, sizeof(resolved.world));
        resolved.visible = (payload.flags & model_core::kSceneRecordVisible) != 0;
        if (payload.parentNodeId) {
            if (!resolveNode(payload.parentNodeId)) return false;
            double product[16]{};
            const auto& parent = resolvedNodes.at(payload.parentNodeId);
            for (uint32_t row=0; row<4; ++row) for (uint32_t column=0; column<4; ++column)
                for (uint32_t k=0; k<4; ++k)
                    product[row*4+column] += payload.localTransform[row*4+k] * parent.world[k*4+column];
            std::memcpy(resolved.world, product, sizeof(product));
            resolved.visible = resolved.visible && parent.visible;
        }
        resolved.resolving = false;
        return true;
    };
    // Node records are allowed to span progressive publications. The import
    // bridge keeps the generation-wide catalog and stamps resolved world
    // transforms onto instances; eagerly resolving this publication's node
    // fragment would reject a child whose parent arrived in an earlier batch.
    std::unordered_map<uint32_t, std::vector<const model_core::MeshInstancePayload*>> instancesByGeometry;
    std::unordered_map<uint32_t, const d3d12_import_bridge::ImportedInstance*> importedInstanceById;
    for (const auto& instance : importedInstances) {
        const bool preResolved=instance.worldTransform[15]==1.0;
        if (!preResolved && !resolveNode(instance.data.nodeId)) {
            error = L"An imported instance references an unavailable node.";
            return false;
        }
        instancesByGeometry[instance.data.geometryChunkId].push_back(&instance.data);
        importedInstanceById.emplace(instance.data.instanceId,&instance);
    }

    // Coarse regions share immutable buffers within a bounded publication.
    // Individual 108-byte samples must not each consume two 64-KiB heaps.
    uint64_t coarseVertexBytes=0,coarseIndexBytes=0,coarseVertexOffset=0,coarseIndexOffset=0;
    for (const auto& mesh:importedMeshes) if (mesh.geometry.lodLevel==model_core::kCoarseLod || mesh.geometry.lodLevel==model_core::kPreviewLod) {
        coarseVertexBytes+=uint64_t(mesh.vertexCount)*model_core::VertexStrideForLayout(mesh.vertexLayoutId);
        coarseIndexBytes+=uint64_t(mesh.indexCount)*4;
    }
    ComPtr<ID3D12Resource> coarseVertices,coarseIndices;
    HRESULT coarseVertexResult=S_OK,coarseIndexResult=S_OK;
    if (coarseVertexBytes) coarseVertices=CreateBuffer(device.Device(),coarseVertexBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,&coarseVertexResult);
    if (coarseIndexBytes) coarseIndices=CreateBuffer(device.Device(),coarseIndexBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,&coarseIndexResult);
    if ((coarseVertexBytes && !coarseVertices) || (coarseIndexBytes && !coarseIndices)) {
        if (coarseVertexResult==E_OUTOFMEMORY || coarseIndexResult==E_OUTOFMEMORY) uploadErrorCode=model_core::ImportErrorCode::OutOfMemory;
        error=L"The reserved coarse buffers could not be created.";
        RetireStagedResources(std::move(staged)); return false;
    }
    if (std::any_of(importedMeshes.begin(),importedMeshes.end(),[](const auto& mesh) { return mesh.geometry.lodLevel==model_core::kCoarseLod; })) {
        for (const auto& buffer:{coarseVertices,coarseIndices}) if (buffer) {
            const auto desc=buffer->GetDesc(); staged.coarseAllocationBytes+=device.Device()->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;
        }
    }
    uint32_t clusterId = 0;
    for (const auto& mesh : importedMeshes) {
        if (mesh.geometry.lodLevel == model_core::kScanLod) continue;
        const bool points = mesh.topology == model_core::ChunkTopology::PointList;
        const bool lines = mesh.topology == model_core::ChunkTopology::LineList;
        const bool positionOnly = mesh.vertexLayoutId == model_core::VertexLayoutId::PositionOnly_F32;
        const bool completeVertex=mesh.vertexLayoutId==model_core::VertexLayoutId::PositionNormalUv0TangentColor_F32;
        if (!points && !lines && (mesh.topology != model_core::ChunkTopology::TriangleList
            || (!positionOnly && !completeVertex && mesh.vertexLayoutId != model_core::VertexLayoutId::PositionNormalUv0_F32))) continue;
        if (points && !positionOnly && !completeVertex) continue;
        if (lines && !positionOnly && !completeVertex
            && mesh.vertexLayoutId != model_core::VertexLayoutId::PositionNormalUv0_F32) continue;
        if (mesh.vertexCount == 0 || (!points && mesh.indexCount == 0)) continue;
        if (lines && (mesh.indexCount & 1u)) { error = L"An imported line list had a partial segment."; RetireStagedResources(std::move(staged)); pendingResourceCount = 0; return false; }

        const uint64_t vertexBytes
            = static_cast<uint64_t>(mesh.vertexCount) * model_core::VertexStrideForLayout(mesh.vertexLayoutId);
        const uint64_t indexBytes = static_cast<uint64_t>(mesh.indexCount) * sizeof(uint32_t);
        if (mesh.payload.size() < vertexBytes + indexBytes) {
            error = L"An imported mesh's data was smaller than its declared size.";
            RetireStagedResources(std::move(staged));
            pendingResourceCount = 0;
            return false;
        }

        // Both buffers go onto `staged` before any early return, so a
        // failure retires every resource the ring has already recorded a
        // copy into rather than destructing it under an unexecuted copy.
        GpuMesh gpuMesh;
        gpuMesh.chunkId = mesh.chunkId;
        gpuMesh.materialChunkId = mesh.materialChunkId;
        gpuMesh.sourceMeshId = mesh.geometry.meshId;
        gpuMesh.sourceNodeId = mesh.geometry.nodeId;
        gpuMesh.sourceGeometry = mesh.geometry;
        gpuMesh.points = points; gpuMesh.lines = lines; gpuMesh.vertexCount = mesh.vertexCount;
        gpuMesh.positionOnly = positionOnly;
        gpuMesh.completeVertex=completeVertex;
        gpuMesh.drawEnabled=(mesh.geometry.geometryFlags&model_core::kGeometryReusableInstanceSource)==0;
        std::memcpy(gpuMesh.origin, mesh.geometry.origin, sizeof(gpuMesh.origin));
        gpuMesh.textureHeap = staged.srvHeap;
        gpuMesh.textureDescriptorSize = staged.srvDescriptorSize;
        gpuMesh.neutralDescriptorBase=static_cast<UINT>(importedImages.size());
        if (completeVertex && !points)
            for (UINT i=0;i<model_core::kMaterialTextureSlotCount;++i)
                gpuMesh.neutralResources[i]=staged.neutralTextures[i].resource;
        const bool coarse=mesh.geometry.lodLevel==model_core::kCoarseLod || mesh.geometry.lodLevel==model_core::kPreviewLod;
        const uint64_t vertexOffset=coarse ? coarseVertexOffset : 0, indexOffset=coarse ? coarseIndexOffset : 0;
        if (coarse) { gpuMesh.vertexBuffer=coarseVertices; gpuMesh.indexBuffer=coarseIndices; }
        const bool vertexOk
            = CreateAndQueueBuffer(mesh.payload.data(), vertexBytes, clusterId++, gpuMesh.vertexBuffer, error, vertexOffset);
        const bool indexOk = vertexOk
            && (points || CreateAndQueueBuffer(mesh.payload.data() + vertexBytes, indexBytes, clusterId++,
                                     gpuMesh.indexBuffer, error, indexOffset));
        if (!vertexOk || !indexOk) {
            staged.meshes.push_back(std::move(gpuMesh));
            RetireStagedResources(std::move(staged));
            pendingResourceCount = 0;
            return false;
        }
        pendingResourceCount += points ? 1 : 2;

        gpuMesh.vbv.BufferLocation = gpuMesh.vertexBuffer->GetGPUVirtualAddress()+vertexOffset;
        const auto vertexDesc = gpuMesh.vertexBuffer->GetDesc();
        gpuMesh.vertexAllocationBytes = device.Device()->GetResourceAllocationInfo(0,1,&vertexDesc).SizeInBytes;
        if (gpuMesh.indexBuffer) {
            const auto indexDesc = gpuMesh.indexBuffer->GetDesc();
            gpuMesh.indexAllocationBytes = device.Device()->GetResourceAllocationInfo(0,1,&indexDesc).SizeInBytes;
        }
        gpuMesh.vbv.SizeInBytes = static_cast<UINT>(vertexBytes);
        gpuMesh.vbv.StrideInBytes = static_cast<UINT>(model_core::VertexStrideForLayout(mesh.vertexLayoutId));
        gpuMesh.ibv.BufferLocation = points ? 0 : gpuMesh.indexBuffer->GetGPUVirtualAddress()+indexOffset;
        gpuMesh.ibv.SizeInBytes = static_cast<UINT>(indexBytes);
        gpuMesh.ibv.Format = DXGI_FORMAT_R32_UINT;
        gpuMesh.indexCount = mesh.indexCount;
        if (coarse) { coarseVertexOffset+=vertexBytes; coarseIndexOffset+=indexBytes; }

        const auto bindMaterial = [&](GpuMesh& draw, uint32_t materialId) {
            draw.materialChunkId = materialId;
            draw.textureIndex = -1;
            std::fill(std::begin(draw.textureIndices), std::end(draw.textureIndices), -1);
            draw.material = {};
            draw.hasMaterial = false;
            if (const auto* material = findMaterial(materialId)) {
                draw.textureIndex = findImageIndex(material->baseColorImageChunkId);
                for (UINT slot=0; slot<model_core::kMaterialTextureSlotCount; ++slot) {
                    draw.textureIndices[slot]=findImageIndex(material->ImageChunkId(slot));
                    const int index=draw.textureIndices[slot];
                    GpuMesh::TextureInfo info{};
                    info.chunkId=material->ImageChunkId(slot);
                    info.bound=index>=0;
                    if (info.bound && index<static_cast<int>(importedImages.size())) {
                        info.pixelFormat=importedImages[index].pixelFormat;
                        info.colorSpace=importedImages[index].colorSpace;
                    }
                    draw.textureInfo[slot]=info;
                }
                draw.material=material->data;draw.hasMaterial=true;
            } else {
                // Match the viewer's established untextured solid color. Pure
                // white clips under studio lighting and makes fine form read as
                // flat gray; this slightly cool neutral remains colorless in
                // intent while preserving useful value separation.
                draw.material.baseColorFactor[0]=0.72f;
                draw.material.baseColorFactor[1]=0.72f;
                draw.material.baseColorFactor[2]=0.76f;
                draw.material.baseColorFactor[3]=1.0f;
                draw.material.roughnessFactor=1.0f;
            }
            draw.materialConstants=materialConstantsFor(materialId,draw.mirrored);
        };
        const auto occurrences = instancesByGeometry.find(mesh.chunkId);
        if (occurrences == instancesByGeometry.end()) {
            bindMaterial(gpuMesh, mesh.materialChunkId);
            staged.meshes.push_back(std::move(gpuMesh));
        } else {
            for (const auto* instance : occurrences->second) {
                const auto importedInstance=importedInstanceById.at(instance->instanceId);
                const bool preResolved=importedInstance->worldTransform[15]==1.0;
                const auto* world=preResolved?importedInstance->worldTransform:resolvedNodes.at(instance->nodeId).world;
                const bool visible=preResolved?importedInstance->resolvedVisible
                    : ((instance->flags&model_core::kSceneRecordVisible)&&resolvedNodes.at(instance->nodeId).visible);
                if (!visible) continue;
                GpuMesh draw = gpuMesh; // ComPtr copies intentionally share the one geometry allocation.
                draw.drawEnabled=true;
                draw.instanceId = instance->instanceId;
                draw.sourceNodeId = instance->nodeId;
                std::memcpy(draw.instanceTransform, world, sizeof(draw.instanceTransform));
                std::memcpy(draw.instanceBoundsMin, instance->worldMin, sizeof(draw.instanceBoundsMin));
                std::memcpy(draw.instanceBoundsMax, instance->worldMax, sizeof(draw.instanceBoundsMax));
                draw.mirrored = preResolved?importedInstance->mirrored:[&]{const auto* m=draw.instanceTransform;
                    return m[0]*(m[5]*m[10]-m[6]*m[9])-m[1]*(m[4]*m[10]-m[6]*m[8])
                        +m[2]*(m[4]*m[9]-m[5]*m[8])<0;}();
                bindMaterial(draw, instance->materialChunkId);
                staged.meshes.push_back(std::move(draw));
            }
        }
    }

    if (materialBufferFailed) {
        RetireStagedResources(std::move(staged));
        pendingResourceCount = 0;
        return false;
    }

    for (const auto& [geometryId, occurrences] : instancesByGeometry) {
        if (std::none_of(importedMeshes.begin(), importedMeshes.end(),
                         [&](const auto& mesh) { return mesh.chunkId == geometryId; })) {
            error = L"An imported instance references geometry that is not available for upload.";
            RetireStagedResources(std::move(staged)); pendingResourceCount = 0; return false;
        }
    }

    // Submit whatever is still under the batch threshold, so the copies
    // actually start rather than waiting for a later caller to flush.
    uploadRing.FlushBatch();

    pendingModel = std::move(staged);
    uploadInFlight = true;
    return true;
}

bool D3D12ViewerPath::RebuildMaterialDescriptors(ModelResources& resources, std::wstring& error)
{
    const bool needsMaterialHeap=std::any_of(resources.meshes.begin(),resources.meshes.end(),[](const auto& mesh) {
        return mesh.completeVertex && !mesh.points;
    });
    if (!needsMaterialHeap) return true;
    if (resources.neutralTextures.size()<model_core::kMaterialTextureSlotCount) {
        error=L"The material fallback catalog is incomplete.";
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    // + one neutral per material slot, + 1 global Studio cubemap + 1 BRDF LUT
    // + 1 T27 transmitted-radiance pyramid.
    heapDesc.NumDescriptors=static_cast<UINT>(resources.textures.size()
        +model_core::kMaterialTextureSlotCount+3);
    heapDesc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> rebuilt;
    if (FAILED(device.Device()->CreateDescriptorHeap(&heapDesc,IID_PPV_ARGS(&rebuilt)))) {
        error=L"The progressive material descriptor catalog could not be rebuilt.";
        return false;
    }
    const UINT descriptorSize=device.Device()->GetDescriptorHandleIncrementSize(heapDesc.Type);
    const auto writeSrv=[&](ID3D12Resource* resource,UINT index) {
        const auto resourceDesc=resource->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format=resourceDesc.Format;
        srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels=resourceDesc.MipLevels;
        auto cpu=rebuilt->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr+=uint64_t(index)*descriptorSize;
        device.Device()->CreateShaderResourceView(resource,&srv,cpu);
    };
    for (UINT i=0;i<resources.textures.size();++i) writeSrv(resources.textures[i].resource.Get(),i);
    const UINT neutralBase=static_cast<UINT>(resources.textures.size());
    for (UINT i=0;i<model_core::kMaterialTextureSlotCount;++i)
        writeSrv(resources.neutralTextures[i].resource.Get(),neutralBase+i);
    const UINT environmentIndex=neutralBase+model_core::kMaterialTextureSlotCount;
    WriteEnvironmentSrv(*rebuilt.Get(),environmentIndex,descriptorSize);
    const UINT brdfLutIndex=environmentIndex+1;
    WriteBrdfLutSrv(*rebuilt.Get(),brdfLutIndex,descriptorSize);
    const UINT transmissionIndex=brdfLutIndex+1;
    WriteTransmissionSrv(*rebuilt.Get(),transmissionIndex,descriptorSize);

    ModelResources displaced;
    std::unordered_set<ID3D12DescriptorHeap*> oldHeaps;
    const auto retain=[&](ComPtr<ID3D12DescriptorHeap>& heap) {
        if (heap && heap.Get()!=rebuilt.Get() && oldHeaps.insert(heap.Get()).second)
            displaced.retainedDescriptorHeaps.push_back(heap);
    };
    retain(resources.srvHeap);
    for (auto& texture:resources.textures) retain(texture.heap);
    for (auto& texture:resources.neutralTextures) retain(texture.heap);
    for (auto& mesh:resources.meshes) retain(mesh.textureHeap);

    resources.srvHeap=rebuilt;
    resources.srvDescriptorSize=descriptorSize;
    resources.environmentDescriptorIndex=environmentIndex;
    resources.brdfLutDescriptorIndex=brdfLutIndex;
    resources.transmissionDescriptorIndex=transmissionIndex;
    for (UINT i=0;i<resources.textures.size();++i) {
        resources.textures[i].heap=rebuilt;
        resources.textures[i].descriptorSize=descriptorSize;
        resources.textures[i].srvHeapIndex=i;
    }
    for (UINT i=0;i<model_core::kMaterialTextureSlotCount;++i) {
        resources.neutralTextures[i].heap=rebuilt;
        resources.neutralTextures[i].descriptorSize=descriptorSize;
        resources.neutralTextures[i].srvHeapIndex=neutralBase+i;
    }
    for (auto& mesh:resources.meshes) if (mesh.completeVertex && !mesh.points) {
        mesh.textureHeap=rebuilt;
        mesh.textureDescriptorSize=descriptorSize;
        mesh.neutralDescriptorBase=neutralBase;
        for (UINT i=0;i<model_core::kMaterialTextureSlotCount;++i)
            mesh.neutralResources[i]=resources.neutralTextures[i].resource;
    }
    if (!displaced.retainedDescriptorHeaps.empty()) {
        uint64_t fence=0;for (const auto& frame:frames) fence=std::max(fence,frame.fenceValue);
        retiredModels.push_back({std::move(displaced),fence,0});
    }
    return true;
}

void D3D12ViewerPath::RetireStagedResources(ModelResources&& resources)
{
    if (resources.meshes.empty() && resources.textures.empty()
        && resources.neutralTextures.empty() && resources.fallbackTextures.empty()
        && resources.retainedDescriptorHeaps.empty()) {
        return;
    }
    // Close any still-open batch first, so LastSubmittedFenceValue()
    // genuinely covers the copies recorded into these resources. This is
    // the same hazard D3D12UploadRing::Grow() already handles for the ring
    // resource itself: a fence value captured before the batch is submitted
    // does not cover work in it.
    uploadRing.FlushBatch();
    retiredModels.push_back(
        RetiredModel{ std::move(resources), /*directFenceValue=*/0, uploadRing.LastSubmittedFenceValue() });
}

void D3D12ViewerPath::RetireInFlightUpload()
{
    if (uploadInFlight) {
        RetireStagedResources(std::move(pendingModel));
    }
    pendingModel = ModelResources{};
    uploadInFlight = false;
}

bool D3D12ViewerPath::PollUploads()
{
    SceneSnapshotPtr snapshot = uploadRing.DrainCompletedPublications(uploadGeneration);
    if (!uploadInFlight) {
        return false;
    }

    // Every resource of this generation must be fence-complete before any
    // of it is drawn -- a partially-copied vertex buffer renders garbage,
    // and this slice publishes a whole model rather than progressive
    // proxies (that is the separate non-terminal-delivery chunk).
    size_t readyForThisGeneration = 0;
    for (const ReadyResourceInfo& info : snapshot->ReadyResources()) {
        if (info.generationValue == pendingToken.Value()) {
            ++readyForThisGeneration;
        }
    }
    if (readyForThisGeneration < pendingResourceCount) {
        return false;
    }

    // Retire the displaced model behind the newest direct fence that could
    // still reference it, rather than releasing it here.
    if (hasModel || !model.meshes.empty()) {
        uint64_t highest = 0;
        for (const auto& frame : frames) {
            if (frame.fenceValue > highest) highest = frame.fenceValue;
        }
        // Copies into this set are long finished -- it has been drawing --
        // so only the direct timeline constrains it.
        retiredModels.push_back(RetiredModel{ std::move(model), highest, /*copyFenceValue=*/0 });
    }

    model = std::move(pendingModel);
    pendingModel = ModelResources{};
    uploadInFlight = false;
    pendingResourceCount = 0;
    hasModel = true;
    return true;
}

void D3D12ViewerPath::UpdateCoarseVisibility(ModelResources& resources)
{
    std::unordered_set<uint32_t> fine;
    bool preview=false;
    for (const auto& mesh:resources.meshes)
        if (mesh.sourceGeometry.lodLevel==model_core::kFineLod) fine.insert(mesh.chunkId);
        else if (mesh.sourceGeometry.lodLevel==model_core::kPreviewLod) preview=true;
    for (auto& mesh:resources.meshes)
        mesh.drawEnabled = (mesh.instanceId != 0
            || (mesh.sourceGeometry.geometryFlags
                & model_core::kGeometryReusableInstanceSource)==0)
            && !(mesh.sourceGeometry.lodLevel==model_core::kPreviewLod && resources.coarseComplete)
            && !(mesh.sourceGeometry.lodLevel==model_core::kCoarseLod && preview && !resources.coarseComplete)
            && !(mesh.sourceGeometry.lodLevel==model_core::kCoarseLod
            && (mesh.chunkId & model_core::kCoarseIdentity)
            && fine.contains(mesh.chunkId & ~model_core::kCoarseIdentity));
}

void D3D12ViewerPath::EvictFineChunks(std::span<const uint32_t> identities)
{
    std::unordered_set<uint32_t> requested(identities.begin(),identities.end());
    std::unordered_set<uint32_t> parents;
    for (const auto& mesh:model.meshes)
        if (mesh.sourceGeometry.lodLevel==model_core::kCoarseLod && (mesh.chunkId&model_core::kCoarseIdentity))
            parents.insert(mesh.chunkId&~model_core::kCoarseIdentity);
    ModelResources retired;
    auto end=std::remove_if(model.meshes.begin(),model.meshes.end(),[&](auto& mesh) {
        if (mesh.sourceGeometry.lodLevel!=model_core::kFineLod || !requested.contains(mesh.chunkId)) return false;
        // Never evict a region whose always-resident coarse parent is absent.
        if (!parents.contains(mesh.chunkId)) return false;
        retired.meshes.push_back(std::move(mesh)); return true;
    });
    model.meshes.erase(end,model.meshes.end());
    uint64_t fence=0; for (const auto& frame:frames) fence=(std::max)(fence,frame.fenceValue);
    if (!retired.meshes.empty()) retiredModels.push_back({std::move(retired),fence,0});
    UpdateCoarseVisibility(model);
}

void D3D12ViewerPath::RetirePreviewChunks(ModelResources& resources)
{
    ModelResources retired;
    auto end=std::remove_if(resources.meshes.begin(),resources.meshes.end(),[&](auto& mesh) {
        if (mesh.sourceGeometry.lodLevel!=model_core::kPreviewLod) return false;
        retired.meshes.push_back(std::move(mesh)); return true;
    });
    resources.meshes.erase(end,resources.meshes.end());
    uint64_t fence=0; for (const auto& frame:frames) fence=(std::max)(fence,frame.fenceValue);
    if (!retired.meshes.empty()) retiredModels.push_back({std::move(retired),fence,0});
}

void D3D12ViewerPath::ReclaimRetired()
{
    if (uploadRing.CopyQueue().Queue()) uploadRing.ReclaimCompleted();

    const uint64_t directCompleted = directQueue.Queue() ? directQueue.CompletedValue() : UINT64_MAX;
    const uint64_t copyCompleted = uploadRing.CopyQueue().Queue() ? uploadRing.CopyQueue().CompletedValue() : UINT64_MAX;
    for (auto it = retiredModels.begin(); it != retiredModels.end();) {
        // Both timelines, not either: a superseded upload is constrained by
        // the copy fence and a displaced model by the direct one, and a set
        // can in principle be waiting on both.
        if (it->directFenceValue <= directCompleted && it->copyFenceValue <= copyCompleted) {
            it = retiredModels.erase(it);
        } else {
            ++it;
        }
    }
}

void D3D12ViewerPath::ClearModel()
{
    // Unconditional, unlike the retire path: callers are shutdown and
    // explicit close, where waiting is correct and there is no later frame
    // to reclaim behind.
    if (hasModel || uploadInFlight || !retiredModels.empty()) WaitForIdle();
    uploadGeneration.Advance(); // strand any still-in-flight publications
    // WaitForIdle above drained both timelines, so everything here is
    // provably unreferenced and can be released outright rather than
    // retired.
    model = ModelResources{};
    pendingModel = ModelResources{};
    retiredModels.clear();
    uploadInFlight = false;
    pendingResourceCount = 0;
    hasModel = false;
    haveModelBounds = false;
}


uint64_t D3D12ViewerPath::EstimateUploadBytes(ID3D12Device* device,
    std::span<const d3d12_import_bridge::ImportedMesh> meshes,
    std::span<const d3d12_import_bridge::ImportedImage> images,
    bool includeMaterialFallbacks)
{
    const auto align = [](uint64_t n) { return (n+65535)/65536*65536; };
    uint64_t bytes=0, packedVertices=0, packedIndices=0;
    for (const auto& mesh:meshes) {
        if (mesh.geometry.lodLevel == model_core::kScanLod) continue;
        const uint64_t vertices = uint64_t(mesh.vertexCount)*model_core::VertexStrideForLayout(mesh.vertexLayoutId);
        const uint64_t indices = uint64_t(mesh.indexCount)*4;
        if (mesh.geometry.lodLevel >= model_core::kCoarseLod) { packedVertices+=vertices; packedIndices+=indices; }
        else bytes+=align(vertices)+align(indices);
    }
    bytes+=align(packedVertices)+align(packedIndices);
    for (const auto& image:images) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width=image.width; desc.Height=image.height;
        desc.DepthOrArraySize=1; desc.MipLevels=UINT16(image.mipLevels); desc.SampleDesc.Count=1;
        auto format=DxgiFormatFor(image.pixelFormat,image.colorSpace);
        if (!format) return UINT64_MAX;
        desc.Format=*format;
        const auto allocation=device->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;
        if (allocation == UINT64_MAX || allocation > UINT64_MAX-bytes) return UINT64_MAX;
        bytes+=allocation;
    }
    const bool needsNeutral=includeMaterialFallbacks && std::any_of(meshes.begin(),meshes.end(),[](const auto& mesh) {
        return mesh.geometry.lodLevel!=model_core::kScanLod
            && mesh.topology==model_core::ChunkTopology::TriangleList
            && mesh.vertexLayoutId==model_core::VertexLayoutId::PositionNormalUv0TangentColor_F32;
    });
    if (needsNeutral) {
        D3D12_RESOURCE_DESC neutral{};neutral.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        neutral.Width=1;neutral.Height=1;neutral.DepthOrArraySize=1;neutral.MipLevels=1;neutral.SampleDesc.Count=1;
        neutral.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        const uint64_t allocation=device->GetResourceAllocationInfo(0,1,&neutral).SizeInBytes;
        if (allocation==UINT64_MAX || allocation>(UINT64_MAX-bytes)/5) return UINT64_MAX;
        bytes+=allocation*5;
    }
    if (!images.empty() || needsNeutral)
        bytes+=align(uint64_t(images.size()+(needsNeutral?5:0))*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
    // T22 per-material constant buffers (one 256-byte-aligned region per frame
    // per draw, and instance expansion can only increase the count).
    if (needsNeutral) {
        uint64_t draws=0;
        for (const auto& mesh:meshes) {
            if (mesh.geometry.lodLevel==model_core::kScanLod) continue;
            ++draws;
        }
        const uint64_t materialBytes=draws*uint64_t(D3D12ViewerPath::kMaterialConstantStride)
            *D3D12ViewerPath::kFrameCount;
        if (materialBytes>UINT64_MAX-bytes) return UINT64_MAX;
        bytes+=align(materialBytes);
    }
    return bytes;
}

uint64_t D3D12ViewerPath::AccountedAllocationBytes(const ModelResources* extra) const
{
    std::unordered_set<const void*> accounted;
    uint64_t total=4ull*1024*1024; // permanent pipeline/overlay/descriptor headroom
    const auto resource=[&](ID3D12Resource* value, uint64_t size=0) {
        if (!value || !accounted.insert(value).second) return;
        if (!size) { const auto desc=value->GetDesc(); size=device.Device()->GetResourceAllocationInfo(0,1,&desc).SizeInBytes; }
        total+=size;
    };
    const auto heap=[&](ID3D12DescriptorHeap* value) {
        if (!value || !accounted.insert(value).second) return;
        const auto desc=value->GetDesc();
        total+=(uint64_t(desc.NumDescriptors)*device.Device()->GetDescriptorHandleIncrementSize(desc.Type)+65535)/65536*65536;
    };
    const auto modelBytes=[&](const ModelResources& resources) {
        for (const auto& mesh:resources.meshes) {
            resource(mesh.vertexBuffer.Get(),mesh.vertexAllocationBytes); resource(mesh.indexBuffer.Get(),mesh.indexAllocationBytes); heap(mesh.textureHeap.Get());
            for (const auto& neutral:mesh.neutralResources) resource(neutral.Get());
            resource(mesh.materialConstants.resource.Get(),mesh.materialConstants.allocationBytes);
        }
        for (const auto* textures:{&resources.textures,&resources.neutralTextures,&resources.fallbackTextures})
            for (const auto& texture:*textures) { resource(texture.resource.Get(),texture.allocationBytes); heap(texture.heap.Get()); }
        heap(resources.srvHeap.Get());
        for (const auto& retained:resources.retainedDescriptorHeaps) heap(retained.Get());
    };
    for (UINT frame=0; frame<kFrameCount; ++frame) resource(swapChain.BackBuffer(frame));
    resource(depthBuffer.Get()); resource(pickTarget.Get()); resource(pickReadback.Get()); resource(frameConstantBuffer.Get());
    resource(sceneColor.Get(),sceneColorAllocationBytes);
    resource(backdropColor.Get(),backdropAllocationBytes);
    resource(ao.indirect.Get(),ao.indirectBytes);
    resource(ao.raw.Get(),ao.rawBytes);
    resource(ao.filtered.Get(),ao.filteredBytes);
    resource(aoQueryReadback.Get());
    resource(oit.accumulation.Get(),oit.accumulationAllocationBytes);
    resource(oit.revealage.Get(),oit.revealageAllocationBytes);
    resource(transmission.sceneCopy.Get(),transmission.allocationBytes);
    resource(shadow.depth.Get(),shadow.depthBytes);
    resource(shadow.mask.Get(),shadow.maskBytes);
    heap(dsvHeap.Get()); heap(pickRtvHeap.Get()); heap(sceneColorRtvHeap.Get()); heap(sceneColorSrvHeap.Get());
    heap(backdropRtvHeap.Get()); heap(oit.rtvHeap.Get());
    heap(ao.rtvHeap.Get()); heap(ao.srvHeap.Get());
    heap(transmission.rtvHeap.Get()); heap(transmission.mipSrvHeap.Get());
    heap(shadow.dsvHeap.Get()); heap(shadow.rtvHeap.Get()); heap(shadow.srvHeap.Get());
    modelBytes(model); modelBytes(pendingModel);
    for (const auto& retired:retiredModels) modelBytes(retired.resources);
    if (extra) modelBytes(*extra);
    return total;
}

void D3D12ViewerPath::ShedTextureDetail()
{
    ModelResources retired;
    for (auto& texture:model.textures) {
        auto fallback=std::find_if(model.fallbackTextures.begin(),model.fallbackTextures.end(),
            [&](const auto& low) { return low.chunkId==texture.chunkId; });
        if (fallback != model.fallbackTextures.end() && fallback->resource.Get()!=texture.resource.Get()) {
            retired.textures.push_back(std::move(texture)); texture=*fallback;
        }
    }
    if (retired.textures.empty()) return;
    std::wstring error;
    if (!RebuildMaterialDescriptors(model,error)) {
        // Descriptor allocation failure must not leave live descriptors
        // pointing at resources moved into the retirement list.
        for (auto& old:retired.textures) {
            auto current=std::find_if(model.textures.begin(),model.textures.end(),
                [&](const auto& texture) { return texture.chunkId==old.chunkId; });
            if (current!=model.textures.end()) *current=std::move(old);
        }
        return;
    }
    uint64_t fence=0; for (const auto& frame:frames) fence=std::max(fence,frame.fenceValue);
    retiredModels.push_back({std::move(retired),fence,0});
}
