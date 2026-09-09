#include "D3D12ViewerPath.h"

#include <d3dcompiler.h>
#include <windows.h>

#include <cstring>
#include <iterator>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {
constexpr DWORD kIdleWaitTimeoutMs = 5000;

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

    if (FAILED(device.Device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                          IID_PPV_ARGS(&commandAllocator)))) {
        error = L"The D3D12 command allocator could not be created.";
        return false;
    }
    if (FAILED(device.Device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator.Get(),
                                                    nullptr, IID_PPV_ARGS(&commandList)))) {
        error = L"The D3D12 command list could not be created.";
        return false;
    }
    // CreateCommandList returns it open; close before the first Reset(),
    // matching the FrameRecorder convention SwapChainTests.cpp established.
    commandList->Close();

    if (!CreateDepthBuffer(swapChainOptions.width, swapChainOptions.height, error)) return false;
    if (!CreatePipeline(error)) return false;
    if (!CreateFrameConstantBuffer(error)) return false;

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

bool D3D12ViewerPath::CreateFrameConstantBuffer(std::wstring& error)
{
    // 256-byte aligned, per D3D12's CBV alignment requirement -- comfortably
    // covers one 4x4 matrix (64 bytes).
    frameConstantBuffer = CreateBuffer(device.Device(), 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!frameConstantBuffer) {
        error = L"The per-frame constant buffer could not be created.";
        return false;
    }
    D3D12_RANGE noRead{ 0, 0 };
    if (FAILED(frameConstantBuffer->Map(0, &noRead, &frameConstantBufferMapped))) {
        error = L"The per-frame constant buffer could not be mapped.";
        return false;
    }
    return true;
}

void D3D12ViewerPath::WaitForIdle()
{
    if (lastFrameFence != 0) {
        directQueue.WaitForValue(lastFrameFence, kIdleWaitTimeoutMs);
    }
}

bool D3D12ViewerPath::Resize(int width, int height, std::wstring& error)
{
    WaitForIdle();
    if (!swapChain.Resize(static_cast<UINT>(width), static_cast<UINT>(height), error)) return false;
    return CreateDepthBuffer(static_cast<UINT>(width), static_cast<UINT>(height), error);
}

void D3D12ViewerPath::RenderClearFrame()
{
    WaitForIdle();

    if (FAILED(commandAllocator->Reset())) {
        return;
    }
    if (FAILED(commandList->Reset(commandAllocator.Get(), nullptr))) {
        return;
    }

    UINT index = swapChain.CurrentBackBufferIndex();
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

    D3D12_RESOURCE_BARRIER toPresent = toRenderTarget;
    std::swap(toPresent.Transition.StateBefore, toPresent.Transition.StateAfter);
    commandList->ResourceBarrier(1, &toPresent);

    if (FAILED(commandList->Close())) {
        return;
    }
    ID3D12CommandList* lists[] = { commandList.Get() };
    directQueue.Queue()->ExecuteCommandLists(1, lists);

    swapChain.Present();
    lastFrameFence = directQueue.SignalNext();
}

void D3D12ViewerPath::RenderFrame(const Camera& camera, float aspect)
{
    WaitForIdle();

    if (FAILED(commandAllocator->Reset())) return;
    if (FAILED(commandList->Reset(commandAllocator.Get(), pipelineState.Get()))) return;

    UINT index = swapChain.CurrentBackBufferIndex();
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
    // XMStoreFloat4x4(view * projection) with no transpose).
    DirectX::XMFLOAT4X4 viewProjection{};
    DirectX::XMStoreFloat4x4(&viewProjection, camera.ViewMatrix() * camera.ProjectionMatrix(aspect));
    std::memcpy(frameConstantBufferMapped, &viewProjection, sizeof(viewProjection));

    commandList->SetGraphicsRootSignature(rootSignature.Get());
    commandList->SetPipelineState(pipelineState.Get());
    commandList->SetGraphicsRootConstantBufferView(0, frameConstantBuffer->GetGPUVirtualAddress());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (const auto& mesh : meshes) {
        commandList->IASetVertexBuffers(0, 1, &mesh.vbv);
        commandList->IASetIndexBuffer(&mesh.ibv);
        commandList->DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);
    }

    D3D12_RESOURCE_BARRIER toPresent = toRenderTarget;
    std::swap(toPresent.Transition.StateBefore, toPresent.Transition.StateAfter);
    commandList->ResourceBarrier(1, &toPresent);

    if (FAILED(commandList->Close())) return;
    ID3D12CommandList* lists[] = { commandList.Get() };
    directQueue.Queue()->ExecuteCommandLists(1, lists);

    swapChain.Present();
    lastFrameFence = directQueue.SignalNext();
}

bool D3D12ViewerPath::UploadOneBuffer(const void* data, uint64_t sizeBytes, D3D12_RESOURCE_STATES finalState,
                                       ComPtr<ID3D12Resource>& outBuffer, std::wstring& error)
{
    WaitForIdle();

    auto staging = CreateBuffer(device.Device(), sizeBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!staging) {
        error = L"A staging buffer could not be created.";
        return false;
    }
    void* mapped = nullptr;
    D3D12_RANGE noRead{ 0, 0 };
    if (FAILED(staging->Map(0, &noRead, &mapped))) {
        error = L"A staging buffer could not be mapped.";
        return false;
    }
    std::memcpy(mapped, data, static_cast<size_t>(sizeBytes));
    staging->Unmap(0, nullptr);

    auto destination = CreateBuffer(device.Device(), sizeBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    if (!destination) {
        error = L"A GPU buffer could not be created.";
        return false;
    }

    if (FAILED(commandAllocator->Reset()) || FAILED(commandList->Reset(commandAllocator.Get(), nullptr))) {
        error = L"The upload command list could not be reset.";
        return false;
    }
    commandList->CopyBufferRegion(destination.Get(), 0, staging.Get(), 0, sizeBytes);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = destination.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = finalState;
    commandList->ResourceBarrier(1, &barrier);

    if (FAILED(commandList->Close())) {
        error = L"The upload command list could not be closed.";
        return false;
    }
    ID3D12CommandList* lists[] = { commandList.Get() };
    directQueue.Queue()->ExecuteCommandLists(1, lists);
    lastFrameFence = directQueue.SignalNext();
    WaitForIdle(); // synchronous: this buffer must be fully uploaded before returning

    outBuffer = destination;
    return true;
}

bool D3D12ViewerPath::UploadModel(const std::vector<d3d12_import_bridge::ImportedMesh>& importedMeshes,
                                   std::wstring& error)
{
    ClearModel();

    std::vector<GpuMesh> newMeshes;
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
            return false;
        }

        GpuMesh gpuMesh;
        if (!UploadOneBuffer(mesh.payload.data(), vertexBytes, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                              gpuMesh.vertexBuffer, error))
            return false;
        if (!UploadOneBuffer(mesh.payload.data() + vertexBytes, indexBytes, D3D12_RESOURCE_STATE_INDEX_BUFFER,
                              gpuMesh.indexBuffer, error))
            return false;

        gpuMesh.vbv.BufferLocation = gpuMesh.vertexBuffer->GetGPUVirtualAddress();
        gpuMesh.vbv.SizeInBytes = static_cast<UINT>(vertexBytes);
        gpuMesh.vbv.StrideInBytes = sizeof(model_core::VertexPositionNormalUv0F32);
        gpuMesh.ibv.BufferLocation = gpuMesh.indexBuffer->GetGPUVirtualAddress();
        gpuMesh.ibv.SizeInBytes = static_cast<UINT>(indexBytes);
        gpuMesh.ibv.Format = DXGI_FORMAT_R32_UINT;
        gpuMesh.indexCount = mesh.indexCount;
        newMeshes.push_back(std::move(gpuMesh));
    }

    if (newMeshes.empty()) {
        error = L"This file did not contain a mesh this preview slice can display yet.";
        return false;
    }

    meshes = std::move(newMeshes);
    hasModel = true;
    return true;
}

void D3D12ViewerPath::ClearModel()
{
    if (hasModel) WaitForIdle();
    meshes.clear();
    hasModel = false;
}
