#include "D3D12ViewerPath.h"
#include "DetailView.h"

#include <d3dcompiler.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>
#include <utility>
#include <vector>
#include <unordered_set>

using Microsoft::WRL::ComPtr;

namespace {
constexpr DWORD kIdleWaitTimeoutMs = 5000;

// SceneSnapshotPublisher keys a published resource by (clusterId, lodLevel)
// and replaces an entry that repeats the pair, so mesh buffers and textures
// must not share an id space. Meshes count up from 0; textures start here.
constexpr uint32_t kTextureClusterIdBase = 1u << 20;

// Embedded HLSL, compiled at runtime via D3DCompile -- same convention
// Renderer.cpp's D3D11 shader strings already use; no .hlsl files on disk.
// Deliberately minimal: no materials/textures (the wire format doesn't
// carry any yet), a single hardcoded albedo shaded from the vertex normal
// only. TEXCOORD0 is declared in the input layout/VSInput for future
// texturing but not otherwise used.
constexpr char kVertexShaderSource[] = R"(
cbuffer FrameConstants : register(b0)
{
    row_major float4x4 gViewProjection;
    float4 gEyeSelection;
};

cbuffer DrawConstants : register(b1)
{
    row_major float4x4 gLocalToCamera;
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
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    float4 relativePosition = mul(float4(input.position, 1.0f), gLocalToCamera);
    output.position = mul(relativePosition, gViewProjection);
    output.worldPosition = relativePosition.xyz;
    output.normal = mul(input.normal, (float3x3)gLocalToCamera);
    return output;
}
)";

constexpr char kPixelShaderSource[] = R"(
cbuffer FrameConstants : register(b0) { row_major float4x4 gViewProjection; float4 gEyeSelection; };
struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD1;
    float3 normal : NORMAL;
};

struct PixelOutput { float4 color : SV_TARGET0; float pick : SV_TARGET1; };
PixelOutput PSMain(PSInput input)
{
    float3 n = normalize(input.normal);
    float3 lightDir = normalize(float3(0.4f, 0.7f, -0.5f));
    float ndotl = saturate(dot(n, lightDir));
    float hemi = 0.5f + 0.5f * n.y;
    float3 albedo = float3(0.72f, 0.72f, 0.76f);
    float3 color = albedo * (0.25f + 0.5f * hemi + 0.4f * ndotl);
    float3 viewDir = normalize(gEyeSelection.xyz - input.worldPosition);
    float outline = pow(1.0f - saturate(dot(n,viewDir)), 2.0f);
    color += gEyeSelection.w * (outline * 0.45f * float3(0.36f,0.62f,1.0f) + 0.03f);
    PixelOutput output; output.color = float4(saturate(color), 1.0f); output.pick = 1.0f; return output;
}
)";

// Textured variant: same vertex shader shape but passes uv through; pixel
// shader samples a base-color texture and multiplies it into the same
// hemisphere/Lambertian shade the untextured PSInstMain uses -- deliberately
// not consuming MaterialPayload's baseColorFactor/metallic/roughness/
// emissive fields yet (a one-line follow-up, scoped out of this slice).
constexpr char kTexturedVertexShaderSource[] = R"(
cbuffer FrameConstants : register(b0)
{
    row_major float4x4 gViewProjection;
    float4 gEyeSelection;
};

cbuffer DrawConstants : register(b1)
{
    row_major float4x4 gLocalToCamera;
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
    float2 uv : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    float4 relativePosition = mul(float4(input.position, 1.0f), gLocalToCamera);
    output.position = mul(relativePosition, gViewProjection);
    output.worldPosition = relativePosition.xyz;
    output.normal = mul(input.normal, (float3x3)gLocalToCamera);
    output.uv = input.uv;
    return output;
}
)";

constexpr char kTexturedPixelShaderSource[] = R"(
cbuffer FrameConstants : register(b0) { row_major float4x4 gViewProjection; float4 gEyeSelection; };
Texture2D gBaseColor : register(t0);
SamplerState gSampler : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD1;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct PixelOutput { float4 color : SV_TARGET0; float pick : SV_TARGET1; };
PixelOutput PSMain(PSInput input)
{
    float3 n = normalize(input.normal);
    float3 lightDir = normalize(float3(0.4f, 0.7f, -0.5f));
    float ndotl = saturate(dot(n, lightDir));
    float hemi = 0.5f + 0.5f * n.y;
    float3 texColor = gBaseColor.Sample(gSampler, input.uv).rgb;
    float3 color = texColor * (0.25f + 0.5f * hemi + 0.4f * ndotl);
    float3 viewDir = normalize(gEyeSelection.xyz - input.worldPosition);
    float outline = pow(1.0f - saturate(dot(n,viewDir)), 2.0f);
    color += gEyeSelection.w * (outline * 0.45f * float3(0.36f,0.62f,1.0f) + 0.03f);
    PixelOutput output; output.color = float4(saturate(color), 1.0f); output.pick = 1.0f; return output;
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
    // One byte per viewport pixel, plus one 256-byte readback slot. No CPU
    // geometry catalog; picking uses exactly the depth-tested displayed pixels.
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = width; desc.Height = height;
    desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES properties{}; properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE clear{}; clear.Format = desc.Format;
    pickTarget.Reset(); pickRtvHeap.Reset();
    if (uint64_t(width) * height > 64ull * 1024 * 1024
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
    D3D12_RANGE range{0,1}; void* bytes = nullptr;
    hit = false;
    if (SUCCEEDED(pickReadback->Map(0, &range, &bytes))) {
        hit = *static_cast<unsigned char*>(bytes) != 0;
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
    D3D12_ROOT_PARAMETER params[2] = {rootParam, {}};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 1; params[1].Constants.Num32BitValues = 16;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rootSigDesc.NumParameters = 2;
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
    psoDesc.RTVFormats[1] = DXGI_FORMAT_R8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;

    if (FAILED(device.Device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState)))) {
        error = L"The D3D12 pipeline state could not be created.";
        return false;
    }
    std::string pointSource(kVertexShaderSource);
    auto begin = pointSource.find("    float3 normal : NORMAL;\n    float2 uv : TEXCOORD0;");
    pointSource.erase(begin, std::string("    float3 normal : NORMAL;\n    float2 uv : TEXCOORD0;").size());
    auto normal = pointSource.find("input.normal");
    pointSource.replace(normal, std::string("input.normal").size(), "float3(0,0,1)");
    ComPtr<ID3DBlob> pointVs;
    if (!CompileShader(pointSource.data(), pointSource.size(), "VSMain", "vs_5_1", pointVs, error)) return false;
    psoDesc.VS = {pointVs->GetBufferPointer(), pointVs->GetBufferSize()};
    psoDesc.InputLayout.NumElements = 1;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&positionOnlyPipelineState)))) {
        error = L"The position-only mesh pipeline could not be created."; return false;
    }
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    if (FAILED(device.Device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pointPipelineState)))) {
        error = L"The point pipeline could not be created."; return false;
    }

    return true;
}

bool D3D12ViewerPath::CreateTexturedPipeline(std::wstring& error)
{
    D3D12_ROOT_PARAMETER rootParams[3]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[2].Constants.ShaderRegister = 1; rootParams[2].Constants.Num32BitValues = 16;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc{};
    rootSigDesc.NumParameters = static_cast<UINT>(std::size(rootParams));
    rootSigDesc.pParameters = rootParams;
    rootSigDesc.NumStaticSamplers = 1;
    rootSigDesc.pStaticSamplers = &sampler;
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
    };

    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE; // see CreatePipeline's identical comment
    rasterizer.FrontCounterClockwise = FALSE;
    rasterizer.DepthClipEnable = TRUE;

    D3D12_BLEND_DESC blend{};
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
    psoDesc.RTVFormats[1] = DXGI_FORMAT_R8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;

    if (FAILED(device.Device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&texturedPipelineState)))) {
        error = L"The textured D3D12 pipeline state could not be created.";
        return false;
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
    const D3D12_GPU_VIRTUAL_ADDRESS constantBufferAddress
        = frameConstantBuffer->GetGPUVirtualAddress() + constantBufferOffset;

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (model.srvHeap) {
        ID3D12DescriptorHeap* heaps[] = { model.srvHeap.Get() };
        commandList->SetDescriptorHeaps(1, heaps);
    }

    // Untextured and textured meshes each need their own root
    // signature/PSO bound before their draw calls; state changes are
    // per-draw at this scale, no batching/sorting needed.
    for (auto& mesh : model.meshes) {
        if (!mesh.drawEnabled) continue;
        mesh.viewPriority = DetailViewPriority(mesh.sourceGeometry, sceneOrigin, cameraTarget,
            !chrome.info.showNativeOrientation && sourceUpAxis == model_core::UpAxisId::Y, viewProjection);
        if (mesh.viewPriority == 0) continue;
        mesh.lastVisibleFrame = uint32_t(frameStats.PresentedFrames());
        DirectX::XMFLOAT4X4 local;
        DirectX::XMStoreFloat4x4(&local, !chrome.info.showNativeOrientation && sourceUpAxis == model_core::UpAxisId::Y
            ? DirectX::XMMatrixSet(1,0,0,0, 0,0,1,0, 0,-1,0,0, 0,0,0,1) : DirectX::XMMatrixIdentity());
        const double native[3] = {mesh.origin[0]-sceneOrigin[0],mesh.origin[1]-sceneOrigin[1],mesh.origin[2]-sceneOrigin[2]};
        // Rotation precedes subtraction in double; never cast the absolute
        // source position or camera pivot to a shader float.
        local._41 = float(native[0]*local._11 + native[1]*local._21 + native[2]*local._31 - cameraTarget[0]);
        local._42 = float(native[0]*local._12 + native[1]*local._22 + native[2]*local._32 - cameraTarget[1]);
        local._43 = float(native[0]*local._13 + native[1]*local._23 + native[2]*local._33 - cameraTarget[2]);
        commandList->IASetPrimitiveTopology(mesh.points ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (mesh.textureIndex >= 0 && mesh.textureHeap) {
            ID3D12DescriptorHeap* heaps[] = { mesh.textureHeap.Get() };
            commandList->SetDescriptorHeaps(1, heaps);
            commandList->SetGraphicsRootSignature(texturedRootSignature.Get());
            commandList->SetPipelineState(texturedPipelineState.Get());
            commandList->SetGraphicsRootConstantBufferView(0, constantBufferAddress);
            D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = mesh.textureHeap->GetGPUDescriptorHandleForHeapStart();
            gpuHandle.ptr += static_cast<UINT64>(mesh.textureIndex) * mesh.textureDescriptorSize;
            commandList->SetGraphicsRootDescriptorTable(1, gpuHandle);
            commandList->SetGraphicsRoot32BitConstants(2, 16, &local, 0);
        } else {
            commandList->SetGraphicsRootSignature(rootSignature.Get());
            commandList->SetPipelineState(mesh.points ? pointPipelineState.Get()
                : mesh.positionOnly ? positionOnlyPipelineState.Get() : pipelineState.Get());
            commandList->SetGraphicsRootConstantBufferView(0, constantBufferAddress);
            commandList->SetGraphicsRoot32BitConstants(1, 16, &local, 0);
        }
        commandList->IASetVertexBuffers(0, 1, &mesh.vbv);
        commandList->IASetIndexBuffer(mesh.points ? nullptr : &mesh.ibv);
        if (mesh.points) commandList->DrawInstanced(mesh.vertexCount, 1, 0, 0);
        else commandList->DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);
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
        dest.PlacedFootprint.Footprint = {DXGI_FORMAT_R8_UNORM, 1, 1, 1, 256};
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
                                        std::wstring& error)
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
    staged.meshes.reserve(importedMeshes.size());
    retiredModels.reserve(retiredModels.size() + 2);

    // Queue each unique image at most once, into a fresh shader-visible
    // heap sized to importedImages.size() -- created up front so each SRV
    // can be written straight to its slot.
    if (!importedImages.empty()) {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.NumDescriptors = static_cast<UINT>(importedImages.size());
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
        const bool points = mesh.topology == model_core::ChunkTopology::PointList
            && mesh.vertexLayoutId == model_core::VertexLayoutId::PositionOnly_F32;
        const bool positionOnly = mesh.vertexLayoutId == model_core::VertexLayoutId::PositionOnly_F32;
        if (!points && (mesh.topology != model_core::ChunkTopology::TriangleList
            || (!positionOnly && mesh.vertexLayoutId != model_core::VertexLayoutId::PositionNormalUv0_F32))) continue;
        if (mesh.vertexCount == 0 || (!points && mesh.indexCount == 0)) continue;

        const uint64_t vertexBytes
            = static_cast<uint64_t>(mesh.vertexCount) * (positionOnly ? sizeof(model_core::VertexPositionOnlyF32) : sizeof(model_core::VertexPositionNormalUv0F32));
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
        std::memcpy(gpuMesh.origin, mesh.geometry.origin, sizeof(gpuMesh.origin));
        gpuMesh.textureHeap = staged.srvHeap;
        gpuMesh.textureDescriptorSize = staged.srvDescriptorSize;
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
        gpuMesh.vbv.StrideInBytes = positionOnly ? sizeof(model_core::VertexPositionOnlyF32) : sizeof(model_core::VertexPositionNormalUv0F32);
        gpuMesh.ibv.BufferLocation = points ? 0 : gpuMesh.indexBuffer->GetGPUVirtualAddress()+indexOffset;
        gpuMesh.ibv.SizeInBytes = static_cast<UINT>(indexBytes);
        gpuMesh.ibv.Format = DXGI_FORMAT_R32_UINT;
        gpuMesh.indexCount = mesh.indexCount;
        if (coarse) { coarseVertexOffset+=vertexBytes; coarseIndexOffset+=indexBytes; }

        if (const auto* material = findMaterial(mesh.materialChunkId)) {
            gpuMesh.textureIndex = findImageIndex(material->baseColorImageChunkId);
        }

        staged.meshes.push_back(std::move(gpuMesh));
    }

    // Submit whatever is still under the batch threshold, so the copies
    // actually start rather than waiting for a later caller to flush.
    uploadRing.FlushBatch();

    pendingModel = std::move(staged);
    uploadInFlight = true;
    return true;
}

void D3D12ViewerPath::RetireStagedResources(ModelResources&& resources)
{
    if (resources.meshes.empty() && resources.textures.empty()) {
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
        mesh.drawEnabled = !(mesh.sourceGeometry.lodLevel==model_core::kPreviewLod && resources.coarseComplete)
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
}


uint64_t D3D12ViewerPath::EstimateUploadBytes(ID3D12Device* device,
    std::span<const d3d12_import_bridge::ImportedMesh> meshes,
    std::span<const d3d12_import_bridge::ImportedImage> images)
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
    if (!images.empty()) bytes+=align(uint64_t(images.size())*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
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
        }
        for (const auto* textures:{&resources.textures,&resources.fallbackTextures})
            for (const auto& texture:*textures) { resource(texture.resource.Get(),texture.allocationBytes); heap(texture.heap.Get()); }
        heap(resources.srvHeap.Get());
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
    // Drop all descriptor references from the old frame-boundary material state.
    for (auto& mesh:model.meshes) {
        if (mesh.textureIndex<0) continue;
        for (const auto& old:retired.textures) if (mesh.textureHeap.Get()==old.heap.Get() && UINT(mesh.textureIndex)==old.srvHeapIndex) {
            auto low=std::find_if(model.textures.begin(),model.textures.end(),[&](const auto& texture) { return texture.chunkId==old.chunkId; });
            mesh.textureHeap=low->heap; mesh.textureIndex=int(low->srvHeapIndex); mesh.textureDescriptorSize=low->descriptorSize;
            break;
        }
    }
    model.srvHeap.Reset(); // texture records own their current/fallback heaps
    uint64_t fence=0; for (const auto& frame:frames) fence=std::max(fence,frame.fenceValue);
    if (!retired.textures.empty()) retiredModels.push_back({std::move(retired),fence,0});
}
