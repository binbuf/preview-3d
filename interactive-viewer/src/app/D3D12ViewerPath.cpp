#include "D3D12ViewerPath.h"
#include "DetailView.h"
#include "GroundShadow.h"
#include "StudioEnvironment.h"

#include <d3dcompiler.h>
#include <windows.h>
#include <DirectXPackedVector.h>

#include <algorithm>
#include <chrono>
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
        | ((material.flags & model_core::kMaterialFlagTransmissive) ? 4096u : 0u);
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
    float4 gLighting; // mode, directional azimuth, reserved, reserved
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
cbuffer FrameConstants : register(b0) { row_major float4x4 gViewProjection; float4 gEyeSelection; float4 gViewport; float4 gLighting; };
cbuffer MaterialConstants : register(b2) { float4 gBaseColorFactor; };
struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD1;
    float3 normal : NORMAL;
    nointerpolation uint pickId : TEXCOORD7;
};

struct PixelOutput { float4 color : SV_TARGET0; uint pick : SV_TARGET1; };
PixelOutput PSMain(PSInput input)
{
    if ((input.pickId&0x80000000u)!=0) {
        PixelOutput wire;wire.color=float4(.72f,.76f,.82f,1);wire.pick=input.pickId&0x7fffffffu;return wire;
    }
    float3 n = normalize(input.normal);
    float3 albedo = gLighting.x>0.5f && gLighting.x<1.5f
        ? float3(.58f,.58f,.58f) : gBaseColorFactor.rgb;
    float3 color;
    if (gLighting.x > 1.5f) {
        float az=gLighting.y, el=clamp(gLighting.z,0.0f,1.4835f);
        float3 lightDir=normalize(float3(cos(az)*cos(el),sin(az)*cos(el),sin(el)));
        color=albedo*(0.08f+1.05f*saturate(dot(n,lightDir)));
    } else {
        float key=saturate(dot(n,normalize(float3(.45f,-.55f,.70f))));
        float fill=saturate(dot(n,normalize(float3(-.70f,.25f,.38f))));
        float rim=saturate(dot(n,normalize(float3(.18f,.76f,.28f))));
        float hemi=saturate(n.z*.5f+.5f);
        color=albedo*(0.10f+0.62f*key+0.30f*fill+0.16f*rim+0.18f*hemi);
    }
    float3 viewDir = normalize(gEyeSelection.xyz - input.worldPosition);
    float outline = pow(1.0f - saturate(dot(n,viewDir)), 2.0f);
    color += gEyeSelection.w * (outline * 0.45f * float3(0.36f,0.62f,1.0f) + 0.03f);
    PixelOutput output; output.color = float4(saturate(color),
        gLighting.x>0.5f && gLighting.x<1.5f ? 1.0f : gBaseColorFactor.a);
    output.pick = input.pickId; return output;
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
    float4 gMaterialExtras;         // normal scale, mirrored sign, reserved
    float4 gSlotUv[5];              // per-slot offset.xy, scale.xy
    float4 gSlotParams[5];          // per-slot cos, sin, packed sampler/uv-set
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

struct PixelOutput { float4 color : SV_TARGET0; uint pick : SV_TARGET1; };

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
// The vertex shader still produces camera-space positions/normals (the
// camera-relative precision policy). The pixel shader rotates those directions
// back into the model/world frame before touching the environment, which keeps
// reflections attached to the model while the camera orbits. The camera-
// relative key/fill rig deliberately stays in camera space.
float3 RotateWorldFromCamera(float3 v)
{
    float3 q=gWorldFromCamera.xyz;
    return v+2.0f*cross(q,cross(q,v)+gWorldFromCamera.w*v);
}
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
PixelOutput PSMain(PSInput input)
{
    if ((input.pickId&0x80000000u)!=0) {
        PixelOutput wire;wire.color=float4(.72f,.76f,.82f,1);wire.pick=input.pickId&0x7fffffffu;return wire;
    }
    uint flags=(uint)gMaterialFactors.w;
    uint maps=(uint)gUvLegacy2.z;
    bool textureLayer=(flags&512u)!=0;
    bool textureMix=(flags&1024u)!=0;
    bool clay=gLighting.x>0.5f && gLighting.x<1.5f;
    bool transmissive=!clay && (flags&4096u)!=0;
    bool doubleSided=(flags&1u)!=0;
    // Every map slot resolves its own uv set, transform, address mode and
    // sampler. Slots 0..4 mirror the material chunk's image dependency order.
    SlotUv baseUv=ResolveSlotUv(0,input.uv);
    float4 vertexColor=input.color;
    if ((flags&2048u)!=0) vertexColor.rgb=SrgbToLinear(vertexColor.rgb);
    float4 base=clay ? float4(.58f,.58f,.58f,1.0f) : gBaseColorFactor*vertexColor;
    if (!clay && (maps&1) && baseUv.valid) {
        float4 tex=SampleSlot(gBaseColor,baseUv);
        if (!textureLayer) tex.a=1.0f;
        if (textureMix) {
            base.rgb=tex.rgb*tex.a+base.rgb*(1.0f-tex.a);
            base.a=tex.a+base.a*(1.0f-tex.a);
        } else base*=tex;
    } else if (!clay && (maps&1) && textureLayer) {
        // An authored address mode of None outside [0,1] means the layer
        // contributes no coverage; it must not fall back to UV0 sampling.
        base.a=0.0f;
    }
    if ((flags&4)!=0 && base.a<gMaterialFactors.z) discard;
    float3 n=normalize(input.normal);
    // glTF double-sided back faces: reverse the geometric normal before the
    // tangent basis is formed. Flipping the perturbed normal toward the eye
    // instead would invert the normal map's slope on back faces.
    if (doubleSided && !input.frontFace) n=-n;
    if (!clay && (maps&4)) {
        SlotUv normalUv=ResolveSlotUv(2,input.uv);
        if (normalUv.valid) {
            float3 t=input.tangent.xyz;
            float tangentLength=dot(t,t);
            if (tangentLength<1e-10f) {
                // PositionNormalUv0 layouts carry no tangent; recover one from
                // the screen-space derivatives of the (transformed) normal-map
                // uv so the map still perturbs in the right frame.
                float2 uv=TransformSlotUvUnwrapped(2,input.uv);
                float3 dpdx=ddx(input.worldPosition),dpdy=ddy(input.worldPosition);
                float2 duvdx=ddx(uv),duvdy=ddy(uv);
                float det=duvdx.x*duvdy.y-duvdx.y*duvdy.x;
                if (abs(det)>1e-8f) {
                    t=(dpdx*duvdy.y-dpdy*duvdx.y)/det;
                } else {
                    t=cross(n,float3(0.0f,0.0f,1.0f));
                    if (dot(t,t)<1e-8f) t=cross(n,float3(0.0f,1.0f,0.0f));
                }
            }
            t=normalize(t);
            // A mirrored instance reverses tangent handedness; multiply by the
            // instance determinant sign so the reconstructed bitangent matches
            // the transformed authored basis.
            float tangentSign=input.tangent.w*gMaterialExtras.y;
            float3 b=normalize(cross(n,t))*tangentSign;
            float3 sampled=SampleSlot(gNormal,normalUv).xyz*2-1;
            // normalTexture.scale multiplies the tangent-space XY only.
            sampled.xy*=gMaterialExtras.x;
            n=normalize(sampled.x*t+sampled.y*b+sampled.z*n);
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
    float clearcoatRoughness=clamp(gMaterialLobes0.y,.045f,1.0f);
    float3 sheenColor=clay?float3(0.0f,0.0f,0.0f):saturate(gMaterialLobes1.rgb);
    float sheenRoughness=clamp(gMaterialLobes0.w,0.0f,1.0f);
    // KHR_materials_ior/specular. specularColor (with IOR) sets F0 at normal
    // incidence; specularFactor sets F90 at grazing. They stay independent at
    // normal and grazing angles, as the extension defines. Metals keep base
    // color as F0 and F90 = 1.
    float iorF0=IorToF0(gMaterialLobes2.x);
    float specularWeight=saturate(gMaterialLobes1.w);
    float3 dielectricF0=saturate(iorF0*gMaterialLobes2.yzw);
    float3 f0=lerp(dielectricF0,base.rgb,metallic);
    float3 f90=lerp(specularWeight.xxx,float3(1.0f,1.0f,1.0f),metallic);
    // KHR_materials_transmission interim compositing. The blend alpha carries
    // the view-dependent interface reflectance, so the environment reflection
    // is kept un-Fresneled for transmissive materials to avoid counting
    // Fresnel twice. T27 owns the real transmitted-scene path; diagnostic
    // channel 22 exposes (alpha, glassReflectance, transmission) so the
    // approximation is visible rather than silent.
    float transmission=transmissive?clamp(gUvLegacy2.w,0.0f,1.0f):0.0f;
    float nv=saturate(dot(n,viewDir));
    float diffuseScale=1.0f-transmission;
    float glassF0=dot(dielectricF0,float3(0.3333333f,0.3333333f,0.3333333f));
    float glassReflectance=lerp(glassF0,1.0f,pow(1.0f-nv,5.0f));
    if (transmissive) base.a=saturate(base.a*diffuseScale+glassReflectance*transmission);
    float3 worldN=RotateWorldFromCamera(n);
    float3 worldView=RotateWorldFromCamera(viewDir);
    float envIntensity=gEnvironment.x;
    // One Fresnel-weighted IBL path shared by Studio, Clay and Directional.
    // Split-sum DFG weight, then the Khronos dielectric/metal mix. Occlusion
    // darkens the environment response only; direct light stays unoccluded.
    float specularEnvironmentScale=clay?.25f:1.0f;
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
    if (transmissive) { iblDiffuse=diffuseEnvironment; iblSpecular=specularEnvironment; }
    // Coat IBL is the same prefiltered environment at the coat roughness. It
    // is weighted once by the layer mix below; it must not carry a second
    // Fresnel factor.
    float coatFresnel=FresnelSchlick(clamp(iorF0,0.0f,1.0f),1.0f,nv);
    float coatWeight=clearcoat*coatFresnel;
    float3 coatEnvironment=SampleStudioSpecular(worldN,worldView,clearcoatRoughness)
        *envIntensity*occlusion;
    // T21/T24 diagnostic contributions. Every split term is accumulated with
    // exactly the arithmetic that reaches `color`, so channel 20 remains the
    // sum of channels 14..19 while exposing the individual lobes.
    float3 directDiffuse=float3(0,0,0),directSpecular=float3(0,0,0);
    float3 envDiffuse=float3(0,0,0),envSpecular=float3(0,0,0);
    float3 coatContribution=float3(0,0,0),emission=float3(0,0,0);
    float3 baseDirect=float3(0,0,0),coatDirect=float3(0,0,0);
    float3 dd=float3(0,0,0),ds=float3(0,0,0);
    float3 sheen=float3(0,0,0);
    float sheenWeight=0.0f;
    if (!clay && any(sheenColor>0.0f)) {
        sheenWeight=pow(1.0f-nv,3.0f)*(0.35f+0.65f*sheenRoughness);
        sheen=sheenColor*sheenWeight*(0.35f+0.65f*envIntensity);
    }
    if (!clay && (flags&2)) {
        // glTF unlit: authored base color only, no environment or coat.
        iblDiffuse=base.rgb*diffuseScale;
        iblSpecular=float3(0,0,0);
        sheen=float3(0,0,0);
        coatEnvironment=float3(0,0,0);
        coatWeight=0.0f;
    } else if (gLighting.x>1.5f) {
        // Directional: one coherent sun for base and coat. The IBL above is
        // identical to Studio, so both modes share one material BRDF.
        float az=gLighting.y, el=clamp(gLighting.z,0.0f,1.4835f);
        float3 lightDir=normalize(float3(cos(az)*cos(el),sin(az)*cos(el),sin(el)));
        float3 d,s;
        baseDirect=EvaluateLightSplit(n,viewDir,lightDir,base.rgb*diffuseScale,metallic,
            roughness,f0,f90,3.4f*envIntensity*gPresentation.x,d,s);
        dd+=d;ds+=s;
        coatDirect=EvaluateClearcoatLight(n,viewDir,lightDir,clearcoatRoughness,
            3.4f*envIntensity*gPresentation.x);
    } else {
        // World-stable Studio IBL plus the camera-relative key/fill/rim rig.
        float3 d0,s0,d1,s1,d2,s2;
        float3 key=EvaluateLightSplit(n,viewDir,normalize(float3(.45f,-.55f,.70f)),
            base.rgb*diffuseScale,metallic,roughness,f0,f90,1.15f*envIntensity*gPresentation.x,d0,s0);
        float3 fill=EvaluateLightSplit(n,viewDir,normalize(float3(-.70f,.25f,.38f)),
            base.rgb*diffuseScale,metallic,roughness,f0,f90,.35f*envIntensity*gPresentation.y,d1,s1);
        float3 rim=EvaluateLightSplit(n,viewDir,normalize(float3(.18f,.76f,.28f)),
            base.rgb*diffuseScale,metallic,roughness,f0,f90,.22f*envIntensity*gPresentation.z,d2,s2);
        baseDirect=key+fill+rim;
        dd=d0+d1+d2;ds=s0+s1+s2;
        // Coat direct lights use the same Studio directions as the base rig.
        coatDirect=EvaluateClearcoatLight(n,viewDir,normalize(float3(.45f,-.55f,.70f)),
                clearcoatRoughness,1.15f*envIntensity*gPresentation.x)
            +EvaluateClearcoatLight(n,viewDir,normalize(float3(-.70f,.25f,.38f)),
                clearcoatRoughness,.35f*envIntensity*gPresentation.y)
            +EvaluateClearcoatLight(n,viewDir,normalize(float3(.18f,.76f,.28f)),
                clearcoatRoughness,.22f*envIntensity*gPresentation.z);
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
    float3 color=directDiffuse+directSpecular+envDiffuse+envSpecular+coatContribution+emission;
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
        else if (debugChannel==9u) dbg=base.a.xxx;
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
        else if (debugChannel==22u) dbg=float3(base.a,glassReflectance,transmission);
        // Fixed linear patch used to check the resolve's tone-map/display
        // contract without depending on scene lighting or sampling.
        else if (debugChannel==23u) dbg=float3(0.3f,0.3f,0.3f);
        else dbg=float3(0.0f,1.0f,1.0f);
        PixelOutput debugOutput; debugOutput.color=float4(max(dbg,0.0f),1.0f);
        debugOutput.pick=input.pickId; return debugOutput;
    }
    // Linear HDR output. T15's resolve pass owns exposure, tone mapping and the
    // sRGB transfer so transparency, transmission and emission all blend in the
    // same color space before the display transform.
    PixelOutput output; output.color = float4(max(color,0.0f), base.a); output.pick = input.pickId; return output;
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
};
Texture2D<float4> gSceneColor : register(t0);
Texture2D<float4> gBackdrop : register(t1);
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
float4 PSMain(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
    float4 scene=gSceneColor.Sample(gLinearSampler,uv);
    uint backgroundMode=(uint)gResolve.z;
    // The HDR target stores straight-alpha scene color premultiplied over a
    // black clear. Recover the straight color before the display transform so
    // glass does not get darker just because its coverage is low.
    float alpha=saturate(scene.a);
    float3 sceneStraight=alpha>1e-5f ? scene.rgb/alpha : float3(0.0f,0.0f,0.0f);
    float3 mapped=SrgbEncode(ApplyToneMap(sceneStraight,gResolve.x,(uint)gResolve.y));
    float3 backdrop=gBackdrop.Sample(gLinearSampler,uv).rgb;
    if (backgroundMode==3u) {
        // Explicit checkerboard choice, in display space so it is stable.
        float2 cell=floor(position.xy/max(4.0f,gResolve.w));
        float parity=fmod(cell.x+cell.y,2.0f);
        backdrop=lerp(float3(0.10f,0.10f,0.11f),float3(0.18f,0.18f,0.19f),parity);
    } else if (backgroundMode==2u) {
        backdrop=float3(0.0f,0.0f,0.0f);
    }
    float4 output;
    if (backgroundMode==2u) {
        // Transparent capture: premultiplied display color and preserved
        // coverage alpha. The backdrop/ground contributes no alpha.
        output.rgb=mapped*alpha;
        output.a=alpha;
    } else {
        output.rgb=lerp(backdrop,mapped,alpha);
        output.a=1.0f;
    }
    return output;
}
)";

bool CompileShader(const char* source, size_t sourceLength, const char* entryPoint, const char* target,
                    ComPtr<ID3DBlob>& outBlob, std::wstring& error)
{
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompile(source, sourceLength, nullptr, nullptr, nullptr, entryPoint, target, flags, 0,
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

bool D3D12ViewerPath::Initialize(HWND window, std::wstring& error)
{
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
    rtvHeapDesc.NumDescriptors = 1;
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    // t0 = HDR scene, t1 = display-space backdrop.
    srvHeapDesc.NumDescriptors = 2;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device.Device()->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&sceneColorRtvHeap)))
        || FAILED(device.Device()->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&sceneColorSrvHeap)))
        || FAILED(device.Device()->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&backdropRtvHeap)))) {
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
    return true;
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

    if (FAILED(device.Device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState)))) {
        error = L"The D3D12 pipeline state could not be created.";
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

bool D3D12ViewerPath::CreateTexturedPipeline(std::wstring& error)
{
    // T22/T24 root layout. Seven SRV tables (baseColor, MR, normal, emissive,
    // the studio cubemap, the independent occlusion map and the split-sum
    // BRDF LUT) plus the 25-DWORD b1 draw block and two CBVs (b0 frame, b2
    // material) total 36 DWORDs, well inside D3D12's 64-DWORD limit.
    D3D12_ROOT_PARAMETER rootParams[10]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE srvRanges[7]{};
    for (UINT i=0;i<6;++i) {
        srvRanges[i].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[i].NumDescriptors=1;srvRanges[i].BaseShaderRegister=i;
        srvRanges[i].OffsetInDescriptorsFromTableStart=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        rootParams[1+i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[1+i].DescriptorTable.NumDescriptorRanges=1;
        rootParams[1+i].DescriptorTable.pDescriptorRanges=&srvRanges[i];
        rootParams[1+i].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    }
    srvRanges[6].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRanges[6].NumDescriptors=1;srvRanges[6].BaseShaderRegister=6;
    srvRanges[6].OffsetInDescriptorsFromTableStart=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    rootParams[9].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[9].DescriptorTable.NumDescriptorRanges=1;
    rootParams[9].DescriptorTable.pDescriptorRanges=&srvRanges[6];
    rootParams[9].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
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
    samplers[1].ShaderRegister=1;
    samplers[1].Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;
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

    if (FAILED(device.Device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&texturedPipelineState)))) {
        error = L"The textured D3D12 pipeline state could not be created.";
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
    psoDesc.RasterizerState.FrontCounterClockwise=FALSE;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&psoDesc,IID_PPV_ARGS(&texturedMirroredPipelineState)))) {
        error=L"The mirrored material pipeline state could not be created.";return false;
    }
    psoDesc.RasterizerState.FrontCounterClockwise=TRUE;
    psoDesc.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&psoDesc,IID_PPV_ARGS(&texturedDoubleSidedPipelineState)))) {
        error=L"The double-sided material pipeline state could not be created.";return false;
    }
    psoDesc.BlendState.RenderTarget[0].BlendEnable=TRUE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend=D3D12_BLEND_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].DestBlend=D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOp=D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha=D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlendAlpha=D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOpAlpha=D3D12_BLEND_OP_ADD;
    psoDesc.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
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
    return true;
}

bool D3D12ViewerPath::CreateResolvePipeline(std::wstring& error)
{
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 2;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = 8;
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
    return true;
}

void D3D12ViewerPath::ResolveSceneToBackBuffer(D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
                                               const OverlayInfo& info)
{
    if (!sceneColor || !backdropColor || !sceneColorSrvHeap || !resolveRootSignature || !resolvePipelineState) return;

    D3D12_RESOURCE_BARRIER barriers[2]{};
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
    commandList->ResourceBarrier(2, barriers);

    lastResolveExposure = viewer_presentation::ClampExposure(info.exposure);
    lastResolveToneMap = static_cast<uint32_t>(info.toneMap);
    const float constants[8] = {
        lastResolveExposure,
        static_cast<float>(info.toneMap),
        static_cast<float>(info.background),
        12.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
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
    commandList->SetGraphicsRoot32BitConstants(0, 8, constants, 0);
    commandList->SetGraphicsRootDescriptorTable(1,
        sceneColorSrvHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 0, nullptr);
    commandList->IASetIndexBuffer(nullptr);
    commandList->DrawInstanced(3, 1, 0, 0);

    std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
    std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
    commandList->ResourceBarrier(2, barriers);
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
    const studio_ibl::EnvironmentBake bake = studio_ibl::BakeStudioEnvironment();
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
        ResolveSceneToBackBuffer(swapChain.BackBufferRtv(index), chrome.info);
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
    commandList->OMSetRenderTargets(2, targets, FALSE, &dsv);

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
    const DirectX::XMFLOAT4 viewportConstants(float(right),float(bottom-top),0,0);
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
        chrome.info.keyLightScale, chrome.info.fillLightScale,
        chrome.info.rimLightScale, static_cast<float>(debugChannel));
    std::memcpy(frameConstantBufferMapped + frameOffset, &presentationConstants,
                sizeof(presentationConstants));
    const D3D12_GPU_VIRTUAL_ADDRESS constantBufferAddress
        = frameConstantBuffer->GetGPUVirtualAddress() + constantBufferOffset;

    const bool opaqueBackdrop = chrome.info.background != viewer_presentation::BackgroundMode::Transparent
        && chrome.info.background != viewer_presentation::BackgroundMode::Checkerboard;
    if (opaqueBackdrop && (chrome.info.shadowVisible || chrome.info.groundHighlightVisible)
        && haveModelBounds && backdropRtvHeap && backdropColor) {
        DirectX::XMFLOAT4X4 groundTransform{};
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
        const auto worldBounds = viewer_ground_shadow::TransformBounds(
            sourceBounds, &groundTransform._11);
        const auto plane = viewer_ground_shadow::PlaceGroundPlane(worldBounds);
        const float cameraTargetF[3] = {
            static_cast<float>(cameraTarget[0]),
            static_cast<float>(cameraTarget[1]),
            static_cast<float>(cameraTarget[2]),
        };
        const auto footprint = viewer_ground_shadow::ProjectShadow(
            worldBounds, plane, viewer_ground_shadow::kKeyLightDirection);
        if (plane.valid && footprint.valid) {
            float highlightTint[3]{};
            viewer_ground_shadow::HighlightColor(backgroundDisplay.data(), highlightTint);
            const auto constants = viewer_ground_shadow::BuildConstants(
                plane, footprint, cameraTargetF,
                chrome.info.groundHighlightVisible, highlightTint,
                chrome.info.shadowVisible);
            // Draw the local highlight/contact shadow into the display-space
            // backdrop (not the HDR scene), so exposure and tone mapping cannot
            // alter it. The layer's two flags are independent.
            D3D12_CPU_DESCRIPTOR_HANDLE backdropTarget =
                backdropRtvHeap->GetCPUDescriptorHandleForHeapStart();
            commandList->OMSetRenderTargets(1, &backdropTarget, FALSE, nullptr);
            commandList->SetGraphicsRootSignature(rootSignature.Get());
            commandList->SetPipelineState(groundPipelineState.Get());
            commandList->SetGraphicsRootConstantBufferView(0, constantBufferAddress);
            commandList->SetGraphicsRoot32BitConstants(1, 16, &constants, 0);
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commandList->IASetVertexBuffers(0, 0, nullptr);
            commandList->IASetIndexBuffer(nullptr);
            commandList->DrawInstanced(6, 1, 0, 0);
            commandList->OMSetRenderTargets(2, targets, FALSE, &dsv);
        }
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
    std::stable_sort(draws.begin(),draws.end(),[](const GpuMesh* a,const GpuMesh* b) {
        const bool ablend=a->material.alphaMode==uint32_t(model_core::AlphaModeId::Blend);
        const bool bblend=b->material.alphaMode==uint32_t(model_core::AlphaModeId::Blend);
        if (ablend!=bblend) return !ablend;
        if (ablend && a->viewDepth!=b->viewDepth) return a->viewDepth>b->viewDepth;
        if (a->hasMaterial!=b->hasMaterial) return a->hasMaterial>b->hasMaterial;
        if (a->materialChunkId!=b->materialChunkId) return a->materialChunkId<b->materialChunkId;
        return a->chunkId<b->chunkId;
    });
    if (chrome.info.lightingMode!=LightingMode::Wireframe) {
      for (GpuMesh* meshPtr:draws) {
        auto& mesh=*meshPtr;
        DrawConstants drawConstants=BuildDrawConstants(mesh.instanceTransform,mesh.origin,
            sceneOrigin,cameraTarget,modelTransform);
        drawConstants.pickId=mesh.instanceId ? mesh.instanceId : mesh.chunkId;
        commandList->IASetPrimitiveTopology(mesh.points ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST
            : mesh.lines ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (mesh.completeVertex && !mesh.points && !mesh.lines && mesh.textureHeap) {
            ID3D12DescriptorHeap* heaps[] = { mesh.textureHeap.Get() };
            commandList->SetDescriptorHeaps(1, heaps);
            commandList->SetGraphicsRootSignature(texturedRootSignature.Get());
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
            const bool blend=mesh.material.alphaMode==uint32_t(model_core::AlphaModeId::Blend);
            // A mesh with no authored material has no culling policy either.
            // Treat it as double-sided: CAD/Rhino OBJ exports commonly contain
            // independently oriented surface patches, and back-face culling
            // otherwise punches apparent holes through details such as faces.
            const bool twoSided=!mesh.hasMaterial
                || (mesh.material.flags&model_core::kMaterialFlagDoubleSided)!=0;
            commandList->SetPipelineState(blend
                ? (twoSided?texturedBlendDoubleSidedPipelineState.Get()
                    :(mesh.mirrored?texturedBlendMirroredPipelineState.Get():texturedBlendPipelineState.Get()))
                : (twoSided?texturedDoubleSidedPipelineState.Get()
                    :(mesh.mirrored?texturedMirroredPipelineState.Get():texturedPipelineState.Get())));
            commandList->SetGraphicsRootConstantBufferView(0, constantBufferAddress);
            uint32_t mapMask=0;
            for (UINT slot=0;slot<4;++slot) {
                D3D12_GPU_DESCRIPTOR_HANDLE handle=mesh.textureHeap->GetGPUDescriptorHandleForHeapStart();
                const int selected=mesh.textureIndices[slot]>=0 ? mesh.textureIndices[slot] : int(mesh.neutralDescriptorBase+slot);
                if (mesh.textureIndices[slot]>=0) mapMask|=1u<<slot;
                handle.ptr+=uint64_t(selected)*mesh.textureDescriptorSize;
                commandList->SetGraphicsRootDescriptorTable(1+slot,handle);
            }
            // T22 independent occlusion map: its own descriptor table at t5,
            // mapped to root parameter 6 so parameter 5 can stay the Studio
            // environment cubemap.
            {
                D3D12_GPU_DESCRIPTOR_HANDLE handle=mesh.textureHeap->GetGPUDescriptorHandleForHeapStart();
                const int selected=mesh.textureIndices[4]>=0 ? mesh.textureIndices[4]
                    : int(mesh.neutralDescriptorBase+4);
                if (mesh.textureIndices[4]>=0) mapMask|=1u<<4;
                handle.ptr+=uint64_t(selected)*mesh.textureDescriptorSize;
                commandList->SetGraphicsRootDescriptorTable(6,handle);
            }
            const auto texturedDraw = BuildTexturedDrawConstants(drawConstants);
            commandList->SetGraphicsRoot32BitConstants(7, 25, &texturedDraw, 0);
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
            commandList->SetPipelineState(mesh.lines ? linePipelineState.Get()
                : mesh.points ? (mesh.completeVertex?coloredPointPipelineState.Get():pointPipelineState.Get())
                : mesh.positionOnly ? positionOnlyPipelineState.Get()
                : mesh.material.alphaMode == uint32_t(model_core::AlphaModeId::Blend)
                    ? blendPipelineState.Get() : pipelineState.Get());
            commandList->SetGraphicsRootConstantBufferView(0, constantBufferAddress);
            commandList->SetGraphicsRoot32BitConstants(1, 33, &drawConstants, 0);
            commandList->SetGraphicsRoot32BitConstants(2, 4, mesh.material.baseColorFactor, 0);
        }
        commandList->IASetVertexBuffers(0, 1, &mesh.vbv);
        commandList->IASetIndexBuffer(mesh.points ? nullptr : &mesh.ibv);
        if (mesh.points) commandList->DrawInstanced(mesh.vertexCount, 1, 0, 0);
        else commandList->DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);
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
                commandList->SetGraphicsRootConstantBufferView(0,constantBufferAddress);
                uint32_t mapMask=0;
                for (UINT slot=0;slot<4;++slot) {
                    D3D12_GPU_DESCRIPTOR_HANDLE handle=mesh.textureHeap->GetGPUDescriptorHandleForHeapStart();
                    const int selected=mesh.textureIndices[slot]>=0 ? mesh.textureIndices[slot] : int(mesh.neutralDescriptorBase+slot);
                    if (mesh.textureIndices[slot]>=0) mapMask|=1u<<slot;
                    handle.ptr+=uint64_t(selected)*mesh.textureDescriptorSize;
                    commandList->SetGraphicsRootDescriptorTable(1+slot,handle);
                }
                {
                    D3D12_GPU_DESCRIPTOR_HANDLE handle=mesh.textureHeap->GetGPUDescriptorHandleForHeapStart();
                    const int selected=mesh.textureIndices[4]>=0 ? mesh.textureIndices[4]
                        : int(mesh.neutralDescriptorBase+4);
                    if (mesh.textureIndices[4]>=0) mapMask|=1u<<4;
                    handle.ptr+=uint64_t(selected)*mesh.textureDescriptorSize;
                    commandList->SetGraphicsRootDescriptorTable(6,handle);
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
        // Five neutral slots: baseColor (white), MR (white), normal (flat),
        // emissive (black) and the T22 independent occlusion slot (white).
        const UINT materialDescriptors = static_cast<UINT>(
            importedImages.size() + (needsNeutralTextures ? 5 : 0));
        staged.environmentDescriptorIndex = materialDescriptors;
        staged.brdfLutDescriptorIndex = materialDescriptors + 1;
        heapDesc.NumDescriptors = materialDescriptors + 2; // + cubemap + BRDF LUT
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
        const uint8_t fallbackPixels[5][4]={{255,255,255,255},{255,255,255,255},{128,128,255,255},{0,0,0,255},{255,255,255,255}};
        staged.neutralTextures.resize(needsNeutralTextures?5:0);
        for (size_t i=0;i<staged.neutralTextures.size();++i) {
            d3d12_import_bridge::ImportedImage fallback;
            fallback.pixelFormat=model_core::PixelFormatId::RGBA8_UNORM;
            fallback.width=fallback.height=fallback.mipLevels=1;
            fallback.colorSpace=i==0 ? model_core::ColorSpaceId::Srgb : model_core::ColorSpaceId::Linear;
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
        if (completeVertex && !points) for (UINT i=0;i<4;++i) gpuMesh.neutralResources[i]=staged.neutralTextures[i].resource;
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
                draw.textureIndices[0]=draw.textureIndex;
                draw.textureIndices[1]=findImageIndex(material->metallicRoughnessImageChunkId);
                draw.textureIndices[2]=findImageIndex(material->normalImageChunkId);
                draw.textureIndices[3]=findImageIndex(material->emissiveImageChunkId);
                draw.textureIndices[4]=findImageIndex(material->occlusionImageChunkId);
                const uint32_t imageIds[5] = {material->baseColorImageChunkId,
                    material->metallicRoughnessImageChunkId, material->normalImageChunkId,
                    material->emissiveImageChunkId, material->occlusionImageChunkId};
                for (UINT slot=0; slot<5; ++slot) {
                    const int index=draw.textureIndices[slot];
                    GpuMesh::TextureInfo info{};
                    info.chunkId=imageIds[slot];
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
    if (resources.neutralTextures.size()<5) {
        error=L"The material fallback catalog is incomplete.";
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    // + 5 neutral material slots, + 1 global Studio cubemap + 1 BRDF LUT.
    heapDesc.NumDescriptors=static_cast<UINT>(resources.textures.size()+5+2);
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
    for (UINT i=0;i<5;++i) writeSrv(resources.neutralTextures[i].resource.Get(),neutralBase+i);
    const UINT environmentIndex=neutralBase+5;
    WriteEnvironmentSrv(*rebuilt.Get(),environmentIndex,descriptorSize);
    const UINT brdfLutIndex=environmentIndex+1;
    WriteBrdfLutSrv(*rebuilt.Get(),brdfLutIndex,descriptorSize);

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
    for (UINT i=0;i<resources.textures.size();++i) {
        resources.textures[i].heap=rebuilt;
        resources.textures[i].descriptorSize=descriptorSize;
        resources.textures[i].srvHeapIndex=i;
    }
    for (UINT i=0;i<5;++i) {
        resources.neutralTextures[i].heap=rebuilt;
        resources.neutralTextures[i].descriptorSize=descriptorSize;
        resources.neutralTextures[i].srvHeapIndex=neutralBase+i;
    }
    for (auto& mesh:resources.meshes) if (mesh.completeVertex && !mesh.points) {
        mesh.textureHeap=rebuilt;
        mesh.textureDescriptorSize=descriptorSize;
        mesh.neutralDescriptorBase=neutralBase;
        for (UINT i=0;i<5;++i) mesh.neutralResources[i]=resources.neutralTextures[i].resource;
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
    heap(dsvHeap.Get()); heap(pickRtvHeap.Get()); heap(sceneColorRtvHeap.Get()); heap(sceneColorSrvHeap.Get());
    heap(backdropRtvHeap.Get());
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
