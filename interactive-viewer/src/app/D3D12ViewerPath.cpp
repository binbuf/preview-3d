#include "D3D12ViewerPath.h"
#include "DetailView.h"

#include <d3dcompiler.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
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
        float3 lightDir=normalize(float3(cos(gLighting.y),sin(gLighting.y),0.55f));
        color=albedo*(0.08f+1.05f*saturate(dot(n,lightDir)));
    } else {
        float key=saturate(dot(n,normalize(float3(.45f,-.55f,.70f))));
        float fill=saturate(dot(n,normalize(float3(-.70f,.25f,.38f))));
        float rim=saturate(dot(n,normalize(float3(.18f,.76f,.28f))));
        color=albedo*(0.22f+0.78f*key+0.34f*fill+0.18f*rim);
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
    row_major float4x4 gLocalToCamera;
    row_major float4x4 gNormalToCamera;
    uint gPickId;
};
cbuffer MaterialConstants : register(b2)
{
    float4 gBaseColorFactor;
    float4 gMaterialFactors; // metallic, roughness, alpha cutoff, flags-as-float
    float4 gEmissive;
    float4 gUvTransform; // offset.xy, scale.xy
    float4 gUvRotationAndMaps; // cos, sin, map mask, unused
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
    float4 relativePosition = mul(float4(input.position, 1.0f), gLocalToCamera);
    output.position = mul(relativePosition, gViewProjection);
    output.worldPosition = relativePosition.xyz;
    output.normal = mul(float4(input.normal, 0.0f), gNormalToCamera).xyz;
    float2 scaled=input.uv*gUvTransform.zw;
    output.uv=float2(scaled.x*gUvRotationAndMaps.x-scaled.y*gUvRotationAndMaps.y,
                     scaled.x*gUvRotationAndMaps.y+scaled.y*gUvRotationAndMaps.x)+gUvTransform.xy;
    if (((uint)gMaterialFactors.w&8u)!=0) output.uv.y=1.0f-output.uv.y;
    output.tangent=float4(normalize(mul(float4(input.tangent.xyz,0.0f),gNormalToCamera).xyz),input.tangent.w);
    output.color=input.color;
    output.pickId=gPickId;
    return output;
}
)";

constexpr char kTexturedPixelShaderSource[] = R"(
cbuffer FrameConstants : register(b0) { row_major float4x4 gViewProjection; float4 gEyeSelection; float4 gViewport; float4 gLighting; };
Texture2D gBaseColor : register(t0);
Texture2D gMetallicRoughness : register(t1);
Texture2D gNormal : register(t2);
Texture2D gEmissive : register(t3);
SamplerState gLinearSampler : register(s0);
SamplerState gNearestSampler : register(s1);
cbuffer MaterialConstants : register(b2)
{
    float4 gBaseColorFactor;
    float4 gMaterialFactors;
    float4 gEmissiveFactor;
    float4 gUvTransform;
    float4 gUvRotationAndMaps;
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

struct PixelOutput { float4 color : SV_TARGET0; uint pick : SV_TARGET1; };

float DistributionGGX(float3 n,float3 h,float roughness)
{
    float a=roughness*roughness, a2=a*a;
    float nh=saturate(dot(n,h));
    float d=nh*nh*(a2-1.0f)+1.0f;
    return a2/max(0.001f,3.14159265f*d*d);
}
float GeometrySchlickGGX(float nv,float roughness)
{
    float r=roughness+1.0f, k=(r*r)/8.0f;
    return nv/(nv*(1.0f-k)+k);
}
float3 EvaluateLight(float3 n,float3 v,float3 l,float3 base,float metallic,float roughness,float intensity)
{
    float3 h=normalize(v+l);
    float nl=saturate(dot(n,l)), nv=saturate(dot(n,v));
    float3 f0=lerp(float3(.04f,.04f,.04f),base,metallic);
    float3 f=f0+(1.0f-f0)*pow(1.0f-saturate(dot(h,v)),5.0f);
    float d=DistributionGGX(n,h,roughness);
    float g=GeometrySchlickGGX(nv,roughness)*GeometrySchlickGGX(nl,roughness);
    float3 specular=d*g*f/max(0.004f,4.0f*nv*nl);
    float3 diffuse=(1.0f-f)*(1.0f-metallic)*base/3.14159265f;
    return (diffuse+specular)*nl*intensity;
}
float3 DisplayMap(float3 color)
{
    // Compress only luminance and scale the chroma with it. A per-channel
    // roll-off lifts dark channels faster than bright ones, which washes
    // saturated base color toward grey; a luminance roll-off keeps hue and
    // saturation while still taming specular highlights.
    float peak=max(color.r,max(color.g,color.b));
    float mapped=peak/(peak+0.82f);
    color=peak>1e-5f ? color*(mapped/peak) : color;
    return pow(saturate(color),1.0f/2.2f);
}
float3 SrgbToLinear(float3 color)
{
    float3 low=color/12.92f;
    float3 high=pow((color+0.055f)/1.055f,2.4f);
    return lerp(high,low,step(color,0.04045f));
}
PixelOutput PSMain(PSInput input)
{
    if ((input.pickId&0x80000000u)!=0) {
        PixelOutput wire;wire.color=float4(.72f,.76f,.82f,1);wire.pick=input.pickId&0x7fffffffu;return wire;
    }
    uint flags=(uint)gMaterialFactors.w;
    uint maps=(uint)gUvRotationAndMaps.z;
    uint addressU=(flags>>4)&3u, addressV=(flags>>6)&3u;
    bool uvValid=(addressU!=3u || (input.uv.x>=0.0f && input.uv.x<=1.0f))
        && (addressV!=3u || (input.uv.y>=0.0f && input.uv.y<=1.0f));
    float2 sampleUv=input.uv;
    sampleUv.x=addressU==0u?frac(sampleUv.x):addressU==1u?1.0f-abs(frac(sampleUv.x*.5f)*2.0f-1.0f):saturate(sampleUv.x);
    sampleUv.y=addressV==0u?frac(sampleUv.y):addressV==1u?1.0f-abs(frac(sampleUv.y*.5f)*2.0f-1.0f):saturate(sampleUv.y);
    bool nearest=(flags&256u)!=0;
    bool textureLayer=(flags&512u)!=0;
    bool textureMix=(flags&1024u)!=0;
    bool clay=gLighting.x>0.5f && gLighting.x<1.5f;
    float4 vertexColor=input.color;
    if ((flags&2048u)!=0) vertexColor.rgb=SrgbToLinear(vertexColor.rgb);
    float4 base=clay ? float4(.58f,.58f,.58f,1.0f) : gBaseColorFactor*vertexColor;
    if (!clay && (maps&1)) {
        float4 tex=nearest?gBaseColor.Sample(gNearestSampler,sampleUv):gBaseColor.Sample(gLinearSampler,sampleUv);
        if (!textureLayer) tex.a=1.0f;
        else if (!uvValid) tex.a=0.0f;
        if (textureMix) {
            base.rgb=tex.rgb*tex.a+base.rgb*(1.0f-tex.a);
            base.a=tex.a+base.a*(1.0f-tex.a);
        } else base*=tex;
    }
    if ((flags&4)!=0 && base.a<gMaterialFactors.z) discard;
    float3 n=normalize(input.normal);
    if (!clay && (maps&4)) {
        float3 t=normalize(input.tangent.xyz);
        float3 b=normalize(cross(n,t))*input.tangent.w;
        float3 sampled=(nearest?gNormal.Sample(gNearestSampler,sampleUv):gNormal.Sample(gLinearSampler,sampleUv)).xyz*2-1;
        n=normalize(sampled.x*t+sampled.y*b+sampled.z*n);
    }
    float3 viewDir = normalize(gEyeSelection.xyz - input.worldPosition);
    // Double-sided materials can show their back faces. The authored normal
    // then points away from the eye (a surface authored "inside out" lights as
    // if it were in shadow), so light the side actually being viewed by
    // flipping the shading normal toward the camera. This matches glTF's
    // requirement to reverse the normal on back-facing double-sided triangles.
    if ((flags&1u)!=0 && dot(n,viewDir)<0.0f) n=-n;
    float metallic=clay?0.0f:gMaterialFactors.x, roughness=clay?.82f:gMaterialFactors.y;
    if (!clay && (maps&2) && uvValid) { float4 mr=nearest?gMetallicRoughness.Sample(gNearestSampler,sampleUv):gMetallicRoughness.Sample(gLinearSampler,sampleUv); metallic*=mr.b; roughness*=mr.g; }
    roughness=clamp(roughness,.045f,1.0f);
    float3 color;
    if (!clay && (flags&2)) {
        color=base.rgb;
    } else if (gLighting.x>1.5f) {
        // A single hard key at a mid elevation: high enough that upward-facing
        // surfaces read as daylight rather than dusk, while still raking
        // across slopes so the rotatable azimuth exposes surface relief.
        float3 lightDir=normalize(float3(cos(gLighting.y),sin(gLighting.y),0.55f));
        color=EvaluateLight(n,viewDir,lightDir,base.rgb,metallic,roughness,3.4f)
            + base.rgb*(1.0f-metallic)*.08f;
    } else {
        // Neutral high-dynamic-range studio environment. Three broad white
        // sources and a colorless diffuse/specular floor preserve authored
        // base color while making metallic and roughness easy to evaluate.
        color=EvaluateLight(n,viewDir,normalize(float3(.45f,-.55f,.70f)),base.rgb,metallic,roughness,2.45f);
        color+=EvaluateLight(n,viewDir,normalize(float3(-.70f,.25f,.38f)),base.rgb,metallic,roughness,1.15f);
        color+=EvaluateLight(n,viewDir,normalize(float3(.18f,.76f,.28f)),base.rgb,metallic,roughness,.72f);
        float3 f0=lerp(float3(.04f,.04f,.04f),base.rgb,metallic);
        float nv=saturate(dot(n,viewDir));
        float3 fresnel=f0+(1.0f-f0)*pow(1.0f-nv,5.0f);
        color+=base.rgb*(1.0f-metallic)*(.13f+.07f*saturate(n.z));
        color+=fresnel*(.12f+.22f*(1.0f-roughness)*(1.0f-roughness));
    }
    if (!clay) {
        float3 emissive=gEmissiveFactor.rgb;
        if ((maps&8) && uvValid) emissive*=(nearest?gEmissive.Sample(gNearestSampler,sampleUv):gEmissive.Sample(gLinearSampler,sampleUv)).rgb;
        color+=emissive;
    }
    float outline = pow(1.0f - saturate(dot(n,viewDir)), 2.0f);
    color += gEyeSelection.w * (outline * 0.45f * float3(0.36f,0.62f,1.0f) + 0.03f);
    PixelOutput output; output.color = float4(DisplayMap(color), base.a); output.pick = input.pickId; return output;
}
)";

// The grid is generated from SV_VertexID, so it needs no persistent vertex
// buffer or upload. Draw constants carry its camera-relative center, ground
// height, and span; this preserves the large-coordinate precision policy used
// by model draws while keeping the grid in the viewer's fixed Z-up world.
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
    if (!CreateFrameConstantBuffer(error)) return false;

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
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
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
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
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

    return true;
}

bool D3D12ViewerPath::CreateTexturedPipeline(std::wstring& error)
{
    D3D12_ROOT_PARAMETER rootParams[7]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE srvRanges[4]{};
    for (UINT i=0;i<4;++i) {
        srvRanges[i].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[i].NumDescriptors=1;srvRanges[i].BaseShaderRegister=i;
        srvRanges[i].OffsetInDescriptorsFromTableStart=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        rootParams[1+i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[1+i].DescriptorTable.NumDescriptorRanges=1;
        rootParams[1+i].DescriptorTable.pDescriptorRanges=&srvRanges[i];
        rootParams[1+i].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    }
    rootParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[5].Constants.ShaderRegister = 1; rootParams[5].Constants.Num32BitValues = 33;
    rootParams[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rootParams[6].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[6].Constants.ShaderRegister=2;rootParams[6].Constants.Num32BitValues=20;
    rootParams[6].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
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
    D3D12_STATIC_SAMPLER_DESC samplers[2]{sampler, sampler};
    samplers[0].ShaderRegister=0;
    samplers[1].ShaderRegister=1;
    samplers[1].Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;
    rootSigDesc.NumStaticSamplers = 2;
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
    rasterizer.FrontCounterClockwise = FALSE;
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
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
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
    psoDesc.RasterizerState.FrontCounterClockwise=TRUE;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&psoDesc,IID_PPV_ARGS(&texturedMirroredPipelineState)))) {
        error=L"The mirrored material pipeline state could not be created.";return false;
    }
    psoDesc.RasterizerState.FrontCounterClockwise=FALSE;
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
    psoDesc.RasterizerState.FrontCounterClockwise=TRUE;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&psoDesc,IID_PPV_ARGS(&texturedBlendMirroredPipelineState)))) {
        error=L"The mirrored blended material pipeline state could not be created.";return false;
    }
    return true;
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

    // .docs/design/10-delivery-plan.md's Gate 1 "immediate #1C1C1E background".
    const float clearColor[4] = { 0x1C / 255.0f, 0x1C / 255.0f, 0x1E / 255.0f, 1.0f };
    commandList->ClearRenderTargetView(swapChain.BackBufferRtv(index), clearColor, 0, nullptr);

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

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = swapChain.BackBufferRtv(index);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();

    const float clearColor[4] = { 0x1C / 255.0f, 0x1C / 255.0f, 0x1E / 255.0f, 1.0f };
    commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
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
        chrome.info.directionalLightAngle * DirectX::XM_2PI, 0.0f, 0.0f);
    std::memcpy(frameConstantBufferMapped + constantBufferOffset + sizeof(viewProjection)
                    + sizeof(eyeSelection) + sizeof(viewportConstants),
                &lightingConstants, sizeof(lightingConstants));
    const D3D12_GPU_VIRTUAL_ADDRESS constantBufferAddress
        = frameConstantBuffer->GetGPUVirtualAddress() + constantBufferOffset;

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
        commandList->IASetPrimitiveTopology(mesh.points ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (mesh.completeVertex && !mesh.points && mesh.textureHeap) {
            ID3D12DescriptorHeap* heaps[] = { mesh.textureHeap.Get() };
            commandList->SetDescriptorHeaps(1, heaps);
            commandList->SetGraphicsRootSignature(texturedRootSignature.Get());
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
            commandList->SetGraphicsRoot32BitConstants(5, 33, &drawConstants, 0);
            float material[20]{};
            std::memcpy(material,mesh.material.baseColorFactor,4*sizeof(float));
            material[4]=mesh.material.metallicFactor;material[5]=mesh.material.roughnessFactor;
            material[6]=mesh.material.alphaCutoff;
            material[7]=float((twoSided?1u:0u)
                | ((mesh.material.flags&model_core::kMaterialFlagUnlit)?2u:0u)
                | (mesh.material.alphaMode==uint32_t(model_core::AlphaModeId::Mask)?4u:0u)
                | ((mesh.material.flags&model_core::kMaterialFlagFlipV)?8u:0u)
                | ((mesh.material.flags&model_core::kMaterialSamplerFlags)<<1)
                | ((mesh.material.flags&model_core::kMaterialFlagTextureLayer)?512u:0u)
                | ((mesh.material.flags&model_core::kMaterialFlagTextureMix)?1024u:0u)
                | ((mesh.material.flags&model_core::kMaterialFlagVertexSrgb)?2048u:0u));
            std::memcpy(material+8,mesh.material.emissiveFactor,3*sizeof(float));
            material[12]=mesh.material.uvOffset[0];material[13]=mesh.material.uvOffset[1];
            material[14]=mesh.material.uvScale[0];material[15]=mesh.material.uvScale[1];
            material[16]=std::cos(mesh.material.uvRotation);material[17]=std::sin(mesh.material.uvRotation);
            material[18]=float(mapMask);
            commandList->SetGraphicsRoot32BitConstants(6,20,material,0);
        } else {
            commandList->SetGraphicsRootSignature(rootSignature.Get());
            commandList->SetPipelineState(mesh.points ? (mesh.completeVertex?coloredPointPipelineState.Get():pointPipelineState.Get())
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
            DrawConstants drawConstants=BuildDrawConstants(mesh.instanceTransform,mesh.origin,
                sceneOrigin,cameraTarget,modelTransform);
            drawConstants.pickId=(mesh.instanceId ? mesh.instanceId : mesh.chunkId)|0x80000000u;
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            if (mesh.completeVertex && mesh.textureHeap) {
                ID3D12DescriptorHeap* heaps[] = { mesh.textureHeap.Get() };
                commandList->SetDescriptorHeaps(1, heaps);
                commandList->SetGraphicsRootSignature(texturedRootSignature.Get());
                commandList->SetPipelineState(texturedWireframePipelineState.Get());
                commandList->SetGraphicsRootConstantBufferView(0,constantBufferAddress);
                uint32_t mapMask=0;
                for (UINT slot=0;slot<4;++slot) {
                    D3D12_GPU_DESCRIPTOR_HANDLE handle=mesh.textureHeap->GetGPUDescriptorHandleForHeapStart();
                    const int selected=mesh.textureIndices[slot]>=0 ? mesh.textureIndices[slot] : int(mesh.neutralDescriptorBase+slot);
                    if (mesh.textureIndices[slot]>=0) mapMask|=1u<<slot;
                    handle.ptr+=uint64_t(selected)*mesh.textureDescriptorSize;
                    commandList->SetGraphicsRootDescriptorTable(1+slot,handle);
                }
                commandList->SetGraphicsRoot32BitConstants(5,33,&drawConstants,0);
                float material[20]{};
                std::memcpy(material,mesh.material.baseColorFactor,4*sizeof(float));
                material[4]=mesh.material.metallicFactor;material[5]=mesh.material.roughnessFactor;
                material[6]=mesh.material.alphaCutoff;
                material[7]=float(((mesh.material.flags&model_core::kMaterialFlagDoubleSided)?1u:0u)
                    | ((mesh.material.flags&model_core::kMaterialFlagUnlit)?2u:0u)
                    | (mesh.material.alphaMode==uint32_t(model_core::AlphaModeId::Mask)?4u:0u)
                    | ((mesh.material.flags&model_core::kMaterialFlagFlipV)?8u:0u)
                    | ((mesh.material.flags&model_core::kMaterialSamplerFlags)<<1)
                    | ((mesh.material.flags&model_core::kMaterialFlagTextureLayer)?512u:0u)
                    | ((mesh.material.flags&model_core::kMaterialFlagTextureMix)?1024u:0u)
                    | ((mesh.material.flags&model_core::kMaterialFlagVertexSrgb)?2048u:0u));
                std::memcpy(material+8,mesh.material.emissiveFactor,3*sizeof(float));
                material[12]=mesh.material.uvOffset[0];material[13]=mesh.material.uvOffset[1];
                material[14]=mesh.material.uvScale[0];material[15]=mesh.material.uvScale[1];
                material[16]=std::cos(mesh.material.uvRotation);material[17]=std::sin(mesh.material.uvRotation);
                material[18]=float(mapMask);
                commandList->SetGraphicsRoot32BitConstants(6,20,material,0);
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

    if (FAILED(commandList->Close())) return;
    ID3D12CommandList* lists[] = { commandList.Get() };
    directQueue.Queue()->ExecuteCommandLists(1, lists);

    DrawChrome(index, orientation, chrome);
    EndFrame(index);
    if (picking) { pickFence = frames[index].fenceValue; pickInFlight = true; pickX = pickY = -1; }
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
        heapDesc.NumDescriptors = static_cast<UINT>(importedImages.size()+(needsNeutralTextures?4:0));
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
        const uint8_t fallbackPixels[4][4]={{255,255,255,255},{255,255,255,255},{128,128,255,255},{0,0,0,255}};
        staged.neutralTextures.resize(needsNeutralTextures?4:0);
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
        const bool positionOnly = mesh.vertexLayoutId == model_core::VertexLayoutId::PositionOnly_F32;
        const bool completeVertex=mesh.vertexLayoutId==model_core::VertexLayoutId::PositionNormalUv0TangentColor_F32;
        if (!points && (mesh.topology != model_core::ChunkTopology::TriangleList
            || (!positionOnly && !completeVertex && mesh.vertexLayoutId != model_core::VertexLayoutId::PositionNormalUv0_F32))) continue;
        if (points && !positionOnly && !completeVertex) continue;
        if (mesh.vertexCount == 0 || (!points && mesh.indexCount == 0)) continue;

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
        gpuMesh.points = points; gpuMesh.vertexCount = mesh.vertexCount;
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
    if (resources.neutralTextures.size()<4) {
        error=L"The material fallback catalog is incomplete.";
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors=static_cast<UINT>(resources.textures.size()+4);
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
    for (UINT i=0;i<4;++i) writeSrv(resources.neutralTextures[i].resource.Get(),neutralBase+i);

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
    for (UINT i=0;i<resources.textures.size();++i) {
        resources.textures[i].heap=rebuilt;
        resources.textures[i].descriptorSize=descriptorSize;
        resources.textures[i].srvHeapIndex=i;
    }
    for (UINT i=0;i<4;++i) {
        resources.neutralTextures[i].heap=rebuilt;
        resources.neutralTextures[i].descriptorSize=descriptorSize;
        resources.neutralTextures[i].srvHeapIndex=neutralBase+i;
    }
    for (auto& mesh:resources.meshes) if (mesh.completeVertex && !mesh.points) {
        mesh.textureHeap=rebuilt;
        mesh.textureDescriptorSize=descriptorSize;
        mesh.neutralDescriptorBase=neutralBase;
        for (UINT i=0;i<4;++i) mesh.neutralResources[i]=resources.neutralTextures[i].resource;
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
        if (allocation==UINT64_MAX || allocation>(UINT64_MAX-bytes)/4) return UINT64_MAX;
        bytes+=allocation*4;
    }
    if (!images.empty() || needsNeutral)
        bytes+=align(uint64_t(images.size()+(needsNeutral?4:0))*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
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
        }
        for (const auto* textures:{&resources.textures,&resources.neutralTextures,&resources.fallbackTextures})
            for (const auto& texture:*textures) { resource(texture.resource.Get(),texture.allocationBytes); heap(texture.heap.Get()); }
        heap(resources.srvHeap.Get());
        for (const auto& retained:resources.retainedDescriptorHeaps) heap(retained.Get());
    };
    for (UINT frame=0; frame<kFrameCount; ++frame) resource(swapChain.BackBuffer(frame));
    resource(depthBuffer.Get()); resource(pickTarget.Get()); resource(pickReadback.Get()); resource(frameConstantBuffer.Get());
    heap(dsvHeap.Get()); heap(pickRtvHeap.Get());
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
