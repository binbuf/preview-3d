#include "D3D12SwapChain.h"

#include "D3D12CommandQueue.h"
#include "D3D12Device.h"

using Microsoft::WRL::ComPtr;

D3D12SwapChain::CreateResult D3D12SwapChain::Initialize(D3D12Device& device,
                                                          D3D12CommandQueue& directQueue,
                                                          HWND window, const CreateOptions& options)
{
    CreateResult result;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = options.width;
    desc.Height = options.height;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1; // flip model forbids MSAA on the swap chain itself
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kBufferCount;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    ComPtr<IDXGISwapChain1> swapChain1;
    result.hr = device.Factory()->CreateSwapChainForHwnd(directQueue.Queue(), window, &desc,
                                                           nullptr, nullptr, &swapChain1);
    if (FAILED(result.hr)) {
        return result;
    }

    result.hr = swapChain1.As(&swapChain_);
    if (FAILED(result.hr)) {
        return result;
    }

    result.hr = device.Factory()->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(result.hr)) {
        return result;
    }

    result.hr = swapChain_->SetMaximumFrameLatency(2);
    if (FAILED(result.hr)) {
        return result;
    }

    frameLatencyWaitable_ = swapChain_->GetFrameLatencyWaitableObject();
    if (frameLatencyWaitable_ == nullptr) {
        result.hr = E_FAIL;
        return result;
    }

    creationFlags_ = desc.Flags;
    device_ = device.Device();
    width_ = options.width;
    height_ = options.height;

    std::wstring error;
    if (!CreateBackBufferViews(error)) {
        result.hr = E_FAIL;
        return result;
    }

    result.success = true;
    return result;
}

bool D3D12SwapChain::CreateBackBufferViews(std::wstring& error)
{
    if (!rtvHeap_) {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = kBufferCount;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap_)))) {
            error = L"The swap chain's render target descriptor heap could not be created.";
            return false;
        }
        rtvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kBufferCount; ++i) {
        if (FAILED(swapChain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i])))) {
            error = L"A swap chain back buffer could not be retrieved.";
            return false;
        }
        device_->CreateRenderTargetView(backBuffers_[i].Get(), nullptr, handle);
        handle.ptr += rtvDescriptorSize_;
    }
    return true;
}

void D3D12SwapChain::ReleaseBackBufferReferences()
{
    for (auto& backBuffer : backBuffers_) {
        backBuffer.Reset();
    }
}

UINT D3D12SwapChain::CurrentBackBufferIndex() const noexcept
{
    return swapChain_->GetCurrentBackBufferIndex();
}

ID3D12Resource* D3D12SwapChain::BackBuffer(UINT index) const noexcept
{
    return backBuffers_[index].Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12SwapChain::BackBufferRtv(UINT index) const noexcept
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * rtvDescriptorSize_;
    return handle;
}

HRESULT D3D12SwapChain::Present()
{
    return swapChain_->Present(1, 0);
}

bool D3D12SwapChain::Resize(UINT width, UINT height, std::wstring& error)
{
    if (!swapChain_ || width == 0 || height == 0) {
        return false;
    }
    if (width == width_ && height == height_) {
        return true;
    }

    ReleaseBackBufferReferences();

    // Must pass the ORIGINAL creation flags, not 0, or the frame-latency-
    // waitable behavior is silently dropped.
    HRESULT hr = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, creationFlags_);
    if (FAILED(hr)) {
        error = L"The swap chain could not be resized.";
        return false;
    }

    width_ = width;
    height_ = height;
    return CreateBackBufferViews(error);
}
