#include "D3D12ViewerPath.h"

#include <d3dcompiler.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>
#include <utility>
#include <vector>

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
    float3 normal : NORMAL;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = mul(float4(input.position, 1.0f), gViewProjection);
    output.normal = input.normal;
    return output;
}
)";

constexpr char kPixelShaderSource[] = R"(
struct PSInput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
};

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 n = normalize(input.normal);
    float3 lightDir = normalize(float3(0.4f, 0.7f, -0.5f));
    float ndotl = saturate(dot(n, lightDir));
    float hemi = 0.5f + 0.5f * n.y;
    float3 albedo = float3(0.72f, 0.72f, 0.76f);
    float3 color = albedo * (0.25f + 0.5f * hemi + 0.4f * ndotl);
    return float4(saturate(color), 1.0f);
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
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = mul(float4(input.position, 1.0f), gViewProjection);
    output.normal = input.normal;
    output.uv = input.uv;
    return output;
}
)";

constexpr char kTexturedPixelShaderSource[] = R"(
Texture2D gBaseColor : register(t0);
SamplerState gSampler : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 n = normalize(input.normal);
    float3 lightDir = normalize(float3(0.4f, 0.7f, -0.5f));
    float ndotl = saturate(dot(n, lightDir));
    float hemi = 0.5f + 0.5f * n.y;
    float3 texColor = gBaseColor.Sample(gSampler, input.uv).rgb;
    float3 color = texColor * (0.25f + 0.5f * hemi + 0.4f * ndotl);
    return float4(saturate(color), 1.0f);
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
                                     D3D12_RESOURCE_STATES initialState)
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
    device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
                                     IID_PPV_ARGS(&resource));
    return resource;
}

} // namespace

bool D3D12ViewerPath::Initialize(HWND window, std::wstring& error)
{
    auto deviceResult = device.Initialize();
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

    // The upload lane: its own copy-typed queue, allocator, list and fence,
    // all owned by the ring. Nothing about a load is submitted to the direct
    // queue any more.
    if (!uploadRing.Initialize(device)) {
        error = L"The D3D12 upload ring could not be created.";
        return false;
    }

    if (!CreateDepthBuffer(swapChainOptions.width, swapChainOptions.height, error)) return false;
    if (!CreatePipeline(error)) return false;
    if (!CreateTexturedPipeline(error)) return false;
    if (!CreateFrameConstantBuffer(error)) return false;

    if (overlayEnabled && !overlay.Initialize(device, directQueue, swapChain, error)) {
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
    return true;
}

bool D3D12ViewerPath::CreatePipeline(std::wstring& error)
{
    D3D12_ROOT_PARAMETER rootParam{};
    rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParam.Descriptor.ShaderRegister = 0;
    rootParam.Descriptor.RegisterSpace = 0;
    rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc{};
    rootSigDesc.NumParameters = 1;
    rootSigDesc.pParameters = &rootParam;
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
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;

    if (FAILED(device.Device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState)))) {
        error = L"The D3D12 pipeline state could not be created.";
        return false;
    }
    return true;
}

bool D3D12ViewerPath::CreateTexturedPipeline(std::wstring& error)
{
    D3D12_ROOT_PARAMETER rootParams[2]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

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
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
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
    if (uploadInFlight || !retiredModels.empty() || hasModel) {
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

double D3D12ViewerPath::DrawSpikeOverlay(UINT frameIndex)
{
    const auto started = std::chrono::steady_clock::now();

    ID2D1DeviceContext* target = overlay.BeginDraw(frameIndex);
    if (target == nullptr) return 0.0;

    // A lost target takes every device resource created from it with it.
    if (overlay.ConsumeTargetsWereRecreated()) overlayBrush.Reset();

    // Brush and text format are created once and reused, exactly as
    // Renderer.cpp does (one brush recolored per primitive; text formats
    // rebuilt only on DPI change). Creating either per frame would measure
    // this code's own inefficiency rather than the bridge's cost.
    if (!overlayBrush) {
        if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0xF5F5F7), &overlayBrush))) {
            overlay.EndDraw(frameIndex);
            return 0.0;
        }
    }
    if (!overlayTextFormat && overlay.WriteFactory() != nullptr) {
        overlay.WriteFactory()->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f,
                                                  L"en-us", &overlayTextFormat);
    }
    ID2D1SolidColorBrush* brush = overlayBrush.Get();

    const float width = static_cast<float>(swapChain.Width());
    const float height = static_cast<float>(swapChain.Height());

    if (overlayPrimitives > 0) {
        // Bars and panel.
        brush->SetColor(D2D1::ColorF(0x2C2C2E));
        target->FillRectangle(D2D1::RectF(0.0f, 0.0f, width, 52.0f), brush);
        target->FillRectangle(D2D1::RectF(0.0f, height - 44.0f, width, height), brush);
        target->FillRectangle(D2D1::RectF(width - 300.0f, 52.0f, width, height - 44.0f), brush);
    }

    // Sized to the real chrome rather than to something convenient: the
    // title bar, bottom bar, info panel and gizmo in Renderer.cpp add up to
    // roughly 250 filled/stroked primitives plus about 40 short text runs
    // once the hand-drawn vector icon atlas is counted.
    for (int i = 0; i < overlayPrimitives; ++i) {
        const float x = 8.0f + static_cast<float>((i * 37) % 1200);
        const float y = 8.0f + static_cast<float>((i * 53) % 600);
        brush->SetColor(D2D1::ColorF(0x3A3A3C + static_cast<UINT32>(i)));
        if ((i % 3) == 0) {
            const D2D1_ROUNDED_RECT rounded{ D2D1::RectF(x, y, x + 32.0f, y + 32.0f), 6.0f, 6.0f };
            target->FillRoundedRectangle(rounded, brush);
        } else if ((i % 3) == 1) {
            target->DrawLine(D2D1::Point2F(x, y), D2D1::Point2F(x + 24.0f, y + 18.0f), brush, 1.5f);
        } else {
            target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), 9.0f, 9.0f), brush, 1.5f);
        }
    }

    if (overlayTextFormat) {
        brush->SetColor(D2D1::ColorF(0xF5F5F7));
        for (int i = 0; i < overlayTextRuns; ++i) {
            const wchar_t* text = L"Stats & Shading";
            const float y = 60.0f + static_cast<float>(i) * 18.0f;
            target->DrawTextW(text, 15, overlayTextFormat.Get(),
                               D2D1::RectF(width - 292.0f, y, width - 8.0f, y + 18.0f), brush,
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    overlay.EndDraw(frameIndex);

    const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - started;
    lastOverlayMs = elapsed.count();
    overlayTotalMs += lastOverlayMs;
    ++overlayPasses;
    return lastOverlayMs;
}

void D3D12ViewerPath::EndFrame(UINT frameIndex)
{
    const HRESULT presentResult = swapChain.Present();
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
    if (overlayEnabled) {
        // The brush belongs to the D2D context's targets, which are about
        // to be destroyed and rebuilt against the resized back buffers.
        overlayBrush.Reset();
        overlay.ReleaseBackBufferReferences();
    }
    if (!swapChain.Resize(static_cast<UINT>(width), static_cast<UINT>(height), error)) return false;
    if (overlayEnabled && !overlay.RecreateBackBufferReferences(swapChain, error)) return false;
    return CreateDepthBuffer(static_cast<UINT>(width), static_cast<UINT>(height), error);
}

void D3D12ViewerPath::RenderClearFrame()
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
    if (!overlayEnabled) {
        D3D12_RESOURCE_BARRIER toPresent = toRenderTarget;
        std::swap(toPresent.Transition.StateBefore, toPresent.Transition.StateAfter);
        commandList->ResourceBarrier(1, &toPresent);
    }

    if (FAILED(commandList->Close())) {
        return;
    }
    ID3D12CommandList* lists[] = { commandList.Get() };
    directQueue.Queue()->ExecuteCommandLists(1, lists);

    if (overlayEnabled) DrawSpikeOverlay(index);
    EndFrame(index);
}

void D3D12ViewerPath::RenderFrame(const DirectX::XMFLOAT4X4& viewProjection)
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
    commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(swapChain.Width()), static_cast<float>(swapChain.Height()),
                             0.0f, 1.0f };
    D3D12_RECT scissor{ 0, 0, static_cast<LONG>(swapChain.Width()), static_cast<LONG>(swapChain.Height()) };
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
    for (const auto& mesh : model.meshes) {
        if (mesh.textureIndex >= 0 && model.srvHeap) {
            commandList->SetGraphicsRootSignature(texturedRootSignature.Get());
            commandList->SetPipelineState(texturedPipelineState.Get());
            commandList->SetGraphicsRootConstantBufferView(0, constantBufferAddress);
            D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = model.srvHeap->GetGPUDescriptorHandleForHeapStart();
            gpuHandle.ptr += static_cast<UINT64>(mesh.textureIndex) * model.srvDescriptorSize;
            commandList->SetGraphicsRootDescriptorTable(1, gpuHandle);
        } else {
            commandList->SetGraphicsRootSignature(rootSignature.Get());
            commandList->SetPipelineState(pipelineState.Get());
            commandList->SetGraphicsRootConstantBufferView(0, constantBufferAddress);
        }
        commandList->IASetVertexBuffers(0, 1, &mesh.vbv);
        commandList->IASetIndexBuffer(&mesh.ibv);
        commandList->DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);
    }

    if (!overlayEnabled) {
        D3D12_RESOURCE_BARRIER toPresent = toRenderTarget;
        std::swap(toPresent.Transition.StateBefore, toPresent.Transition.StateAfter);
        commandList->ResourceBarrier(1, &toPresent);
    }

    if (FAILED(commandList->Close())) return;
    ID3D12CommandList* lists[] = { commandList.Get() };
    directQueue.Queue()->ExecuteCommandLists(1, lists);

    if (overlayEnabled) DrawSpikeOverlay(index);
    EndFrame(index);
}

bool D3D12ViewerPath::CreateAndQueueBuffer(const void* data, uint64_t sizeBytes, uint32_t clusterId,
                                            ComPtr<ID3D12Resource>& outBuffer, std::wstring& error)
{
    auto destination = CreateBuffer(device.Device(), sizeBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    if (!destination) {
        error = L"A GPU buffer could not be created.";
        return false;
    }

    D3D12UploadRing::UploadRequest request;
    request.sourceBytes = std::span<const std::byte>(static_cast<const std::byte*>(data),
                                                      static_cast<size_t>(sizeBytes));
    request.destination = destination.Get();
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
    auto dxgiFormat = DxgiFormatFor(image.pixelFormat, image.colorSpace);
    if (!dxgiFormat || image.width == 0 || image.height == 0 || image.mipLevels != 1) {
        error = L"An imported texture had an unsupported format or mip count.";
        return false;
    }

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = image.width;
    texDesc.Height = image.height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
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
    if (FAILED(device.Device()->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
                                                          D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                          IID_PPV_ARGS(&texture)))) {
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

    switch (uploadRing.UploadTexture(request)) {
    case D3D12UploadRing::UploadResult::Uploaded:
        break;
    case D3D12UploadRing::UploadResult::Backpressured:
        error = L"The upload staging ring could not make room for this model's textures.";
        return false;
    case D3D12UploadRing::UploadResult::Failed:
    default:
        // UploadTexture also rejects a source smaller than the footprint it
        // computed, which is the "declared size" check this used to make
        // itself.
        error = L"An imported texture could not be queued for upload.";
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = *dxgiFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

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

    // Queue each unique image at most once, into a fresh shader-visible
    // heap sized to importedImages.size() -- created up front so each SRV
    // can be written straight to its slot.
    if (!importedImages.empty()) {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.NumDescriptors = static_cast<UINT>(importedImages.size());
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device.Device()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&staged.srvHeap)))) {
            error = L"The texture descriptor heap could not be created.";
            return false;
        }
        staged.srvDescriptorSize
            = device.Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        staged.textures.resize(importedImages.size());
        for (size_t i = 0; i < importedImages.size(); ++i) {
            GpuTexture texture;
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

    uint32_t clusterId = 0;
    for (const auto& mesh : importedMeshes) {
        if (mesh.topology != model_core::ChunkTopology::TriangleList
            || mesh.vertexLayoutId != model_core::VertexLayoutId::PositionNormalUv0_F32) {
            continue; // point clouds / unrecognized layouts: a later slice
        }
        if (mesh.vertexCount == 0 || mesh.indexCount == 0) continue;

        const uint64_t vertexBytes
            = static_cast<uint64_t>(mesh.vertexCount) * sizeof(model_core::VertexPositionNormalUv0F32);
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
        const bool vertexOk
            = CreateAndQueueBuffer(mesh.payload.data(), vertexBytes, clusterId++, gpuMesh.vertexBuffer, error);
        const bool indexOk = vertexOk
            && CreateAndQueueBuffer(mesh.payload.data() + vertexBytes, indexBytes, clusterId++,
                                     gpuMesh.indexBuffer, error);
        if (!vertexOk || !indexOk) {
            staged.meshes.push_back(std::move(gpuMesh));
            RetireStagedResources(std::move(staged));
            pendingResourceCount = 0;
            return false;
        }
        pendingResourceCount += 2;

        gpuMesh.vbv.BufferLocation = gpuMesh.vertexBuffer->GetGPUVirtualAddress();
        gpuMesh.vbv.SizeInBytes = static_cast<UINT>(vertexBytes);
        gpuMesh.vbv.StrideInBytes = sizeof(model_core::VertexPositionNormalUv0F32);
        gpuMesh.ibv.BufferLocation = gpuMesh.indexBuffer->GetGPUVirtualAddress();
        gpuMesh.ibv.SizeInBytes = static_cast<UINT>(indexBytes);
        gpuMesh.ibv.Format = DXGI_FORMAT_R32_UINT;
        gpuMesh.indexCount = mesh.indexCount;

        if (const auto* material = findMaterial(mesh.materialChunkId)) {
            gpuMesh.textureIndex = findImageIndex(material->baseColorImageChunkId);
        }

        staged.meshes.push_back(std::move(gpuMesh));
    }

    if (staged.meshes.empty()) {
        error = L"This file did not contain a mesh this preview slice can display yet.";
        // Textures may already have been queued even though no mesh was.
        RetireStagedResources(std::move(staged));
        pendingResourceCount = 0;
        return false;
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

void D3D12ViewerPath::ReclaimRetired()
{
    uploadRing.ReclaimCompleted();

    const uint64_t directCompleted = directQueue.CompletedValue();
    const uint64_t copyCompleted = uploadRing.CopyQueue().CompletedValue();
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
