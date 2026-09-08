#pragma once

// Headless D3D12 adapter/device foundation. Implements
// .docs/design/04-rendering-and-streaming.md "Device and presentation"
// steps 1-4 only (debug layer, adapter enumeration/selection, device
// creation -- queue creation lives in D3D12CommandQueue). No swap chain,
// no window, no shaders: step 5 ("presentation to the window") and
// everything after is out of scope here and deferred to a later slice.

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <string>

class D3D12Device
{
public:
    struct CreateOptions
    {
        // Developer-only, explicit opt-in. Never automatic: ADR-009
        // (.docs/design/11-decisions-and-risks.md) rejects "automatic
        // WARP user fallback," and the design doc says "A developer-only
        // command line can request WARP; WARP is not an automatic
        // performance fallback." This must be threaded from an explicit
        // caller/command-line flag, never inferred.
        bool forceWarp = false;
    };

    struct CreateResult
    {
        bool success = false;
        HRESULT hr = S_OK;
        std::wstring adapterDescription;
        bool isWarpAdapter = false;
        bool debugLayerEnabled = false;
    };

    D3D12Device() = default;
    D3D12Device(const D3D12Device&) = delete;
    D3D12Device& operator=(const D3D12Device&) = delete;
    D3D12Device(D3D12Device&&) = default;
    D3D12Device& operator=(D3D12Device&&) = default;

    CreateResult Initialize(const CreateOptions& options = {});

    ID3D12Device* Device() const noexcept { return device_.Get(); }
    IDXGIFactory6* Factory() const noexcept { return factory_.Get(); }

private:
    static bool TryEnableDebugLayerIfDeveloperBuild();
    static Microsoft::WRL::ComPtr<IDXGIAdapter1> SelectHardwareAdapter(IDXGIFactory6& factory);
    static Microsoft::WRL::ComPtr<IDXGIAdapter1> SelectWarpAdapter(IDXGIFactory6& factory);

    Microsoft::WRL::ComPtr<IDXGIFactory6> factory_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
};
