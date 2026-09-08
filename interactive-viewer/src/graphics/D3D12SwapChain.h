#pragma once

// Presentation-surface management only: create/resize/present/expose back
// buffers. Does not own window lifetime, command allocators/lists, or a
// frame fence -- those belong to whichever caller eventually drives a real
// frame loop (per .docs/design/04-rendering-and-streaming.md's ownership
// model, "render allocators/lists" and the render fence belong to the
// render thread, not the presentation surface). No Direct2D/overlay
// integration here -- that question (ADR-010) is explicitly out of scope
// for this slice.

#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <string>

class D3D12Device;
class D3D12CommandQueue;

class D3D12SwapChain
{
public:
    static constexpr UINT kBufferCount = 3;

    struct CreateOptions
    {
        UINT width = 0;
        UINT height = 0;
    };

    struct CreateResult
    {
        bool success = false;
        HRESULT hr = S_OK;
    };

    D3D12SwapChain() = default;
    D3D12SwapChain(const D3D12SwapChain&) = delete;
    D3D12SwapChain& operator=(const D3D12SwapChain&) = delete;
    D3D12SwapChain(D3D12SwapChain&&) = default;
    D3D12SwapChain& operator=(D3D12SwapChain&&) = default;

    // directQueue must have been Initialize()'d with
    // D3D12_COMMAND_LIST_TYPE_DIRECT -- D3D12's CreateSwapChainForHwnd
    // takes the command QUEUE, not the device, as its "device" argument
    // (an easy D3D11-habit mistake to make).
    CreateResult Initialize(D3D12Device& device, D3D12CommandQueue& directQueue, HWND window,
                             const CreateOptions& options);

    // Precondition (caller's responsibility -- this class owns no frame
    // fence): every command list referencing the current back buffers must
    // have already retired (e.g. via directQueue.WaitForValue(...)) before
    // calling this.
    bool Resize(UINT width, UINT height, std::wstring& error);

    UINT CurrentBackBufferIndex() const noexcept;
    ID3D12Resource* BackBuffer(UINT index) const noexcept;
    D3D12_CPU_DESCRIPTOR_HANDLE BackBufferRtv(UINT index) const noexcept;
    UINT Width() const noexcept { return width_; }
    UINT Height() const noexcept { return height_; }

    IDXGISwapChain3* SwapChain() const noexcept { return swapChain_.Get(); }

    // Forwards IDXGISwapChain3::Present(1, 0). Returns the raw HRESULT,
    // including DXGI_STATUS_OCCLUDED (a SUCCEEDED() success code, not
    // treated as an error here) -- the caller decides what to do with it.
    HRESULT Present();

    // Owned by the swap chain itself -- do NOT CloseHandle this.
    HANDLE FrameLatencyWaitableHandle() const noexcept { return frameLatencyWaitable_; }

private:
    bool CreateBackBufferViews(std::wstring& error);
    void ReleaseBackBufferReferences();

    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> backBuffers_[kBufferCount];
    ID3D12Device* device_ = nullptr; // non-owning, kept for CreateRenderTargetView in Resize
    UINT rtvDescriptorSize_ = 0;
    UINT width_ = 0;
    UINT height_ = 0;
    UINT creationFlags_ = 0; // remembered for ResizeBuffers' SwapChainFlags argument
    HANDLE frameLatencyWaitable_ = nullptr;
};
