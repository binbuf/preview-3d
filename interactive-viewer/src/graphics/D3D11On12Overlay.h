#pragma once

// The ADR-010 bridge: D3D11On12 plus Direct2D/DirectWrite over the same
// direct queue the D3D12 scene uses, drawn after scene commands and before
// the frame fence signal and present
// (.docs/design/11-decisions-and-risks.md:129).
//
// Built for required validation spike 1 (`11-...:243`, "Before Gate 1
// completion, measure D3D11On12 overlay ordering and cost at 144 Hz,
// including resize and GPU validation"). It has now been run and passed;
// ADR-010 is Accepted, with the measurements recorded there.
//
// Two things this deliberately settles empirically rather than by assumption:
//
//  - whether Direct2D will attach to a DXGI_FORMAT_R8G8B8A8_UNORM back
//    buffer at all. The design specifies that format (`04-...:17`) while the
//    existing D3D11 renderer uses BGRA, and D3D11_CREATE_DEVICE_BGRA_SUPPORT
//    is about the device supporting BGRA resources, not about the swap-chain
//    format. Initialize() reports the real HRESULT either way.
//  - the resource-state handshake. Per `04-...:46`, when an overlay pass
//    follows, the scene "leaves the wrapped back buffer in the bridge's
//    declared input state; otherwise it transitions the buffer to PRESENT
//    itself." So the caller's command list must leave the back buffer in
//    RENDER_TARGET and skip its own transition to PRESENT -- this class
//    declares RENDER_TARGET in and PRESENT out, and the Release inside
//    EndDraw performs that transition.
//
// Ownership: one thread owns every interop context (ADR-010). There is no
// render thread yet, so today that is the UI thread; the bridge moves with
// the rest of the direct-queue work when one lands.

#include "D3D12CommandQueue.h"
#include "D3D12Device.h"
#include "D3D12SwapChain.h"

#include <d2d1.h>
#include <d3d11on12.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>

class D3D11On12Overlay
{
public:
    D3D11On12Overlay() = default;
    ~D3D11On12Overlay();

    D3D11On12Overlay(const D3D11On12Overlay&) = delete;
    D3D11On12Overlay& operator=(const D3D11On12Overlay&) = delete;

    // Creates the 11on12 device over `directQueue`, wraps every back buffer,
    // and builds a D2D render target per buffer. `error` carries the failing
    // step and HRESULT -- the D2D target creation in particular is the
    // format question above, so its failure must be legible, not silent.
    bool Initialize(D3D12Device& device, D3D12CommandQueue& directQueue, D3D12SwapChain& swapChain,
                     std::wstring& error);

    // The wrapped resources hold references to the swap chain's buffers, so
    // they must be dropped before ResizeBuffers and rebuilt after it.
    void ReleaseBackBufferReferences();
    bool RecreateBackBufferReferences(D3D12SwapChain& swapChain, std::wstring& error);

    // Acquires the wrapped back buffer and returns the D2D target to draw
    // into, already inside BeginDraw. Returns nullptr if the bridge is not
    // ready, so a caller can simply skip the overlay pass.
    ID2D1RenderTarget* BeginDraw(UINT backBufferIndex);
    // EndDraw + ReleaseWrappedResources (which transitions the back buffer to
    // PRESENT) + Flush. Returns D2D's EndDraw HRESULT so a caller can detect
    // D2DERR_RECREATE_TARGET.
    HRESULT EndDraw(UINT backBufferIndex);

    IDWriteFactory* WriteFactory() const noexcept { return writeFactory_.Get(); }
    bool IsReady() const noexcept { return d3d11On12Device_ != nullptr; }

    // Releases D2D and D3D11On12 before the owner releases its D3D12
    // objects, which `04-rendering-and-streaming.md:190` requires explicitly.
    void Shutdown();

private:
    bool CreateTargetsForBackBuffers(D3D12SwapChain& swapChain, std::wstring& error);

    Microsoft::WRL::ComPtr<ID3D11Device> d3d11Device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d11Context_;
    Microsoft::WRL::ComPtr<ID3D11On12Device> d3d11On12Device_;
    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> writeFactory_;
    Microsoft::WRL::ComPtr<ID3D11Resource> wrappedBackBuffers_[D3D12SwapChain::kBufferCount];
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> targets_[D3D12SwapChain::kBufferCount];
};
