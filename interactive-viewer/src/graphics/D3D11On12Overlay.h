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
// The spike settled two things empirically rather than by assumption, both
// of which this implementation now depends on:
//
//  - Direct2D attaches to the DXGI_FORMAT_R8G8B8A8_UNORM back buffer the
//    design specifies (`04-...:17`) even though the D3D11 renderer uses BGRA.
//    D3D11_CREATE_DEVICE_BGRA_SUPPORT is a device capability flag, not a
//    constraint on the swap-chain format.
//  - the resource-state handshake. Per `04-...:46`, when an overlay pass
//    follows, the scene "leaves the wrapped back buffer in the bridge's
//    declared input state; otherwise it transitions the buffer to PRESENT
//    itself." So the caller's command list must leave the back buffer in
//    RENDER_TARGET and skip its own transition to PRESENT -- this class
//    declares RENDER_TARGET in and PRESENT out, and the Release inside
//    EndDraw performs that transition.
//
// One ID2D1Device plus a single ID2D1DeviceContext, retargeted at a bitmap
// per back buffer -- not the D2D 1.0 CreateDxgiSurfaceRenderTarget shape the
// spike used. That shape gives three *independent* render targets, and a D2D
// resource belongs to the target that created it, so every cached brush would
// have to be duplicated per back buffer. Device-level resources are shared
// here instead. Drawing code is unaffected either way: ID2D1DeviceContext
// *is* an ID2D1RenderTarget, which is what the ~750 lines being ported out of
// Renderer.cpp are written against.
//
// Ownership: one thread owns every interop context (ADR-010) -- the render
// thread, which owns D3D12ViewerPath and therefore this.

#include "D3D12CommandQueue.h"
#include "D3D12Device.h"
#include "D3D12SwapChain.h"

#include <d2d1_1.h>
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

    // Creates the 11on12 device over `directQueue`, the D2D device and its
    // single context, and a target bitmap per back buffer. `error` carries
    // the failing step and HRESULT.
    bool Initialize(D3D12Device& device, D3D12CommandQueue& directQueue, D3D12SwapChain& swapChain,
                     std::wstring& error);

    // The wrapped resources hold references to the swap chain's buffers, so
    // they must be dropped before ResizeBuffers and rebuilt after it.
    void ReleaseBackBufferReferences();
    bool RecreateBackBufferReferences(D3D12SwapChain& swapChain, std::wstring& error);

    // Acquires the wrapped back buffer, points the shared context at that
    // buffer's bitmap, and returns it already inside BeginDraw. Returns
    // nullptr if the bridge is not ready, so a caller can skip the pass.
    //
    // The returned pointer is the SAME context every frame, so brushes and
    // other device resources created from it stay valid across back buffers
    // and across resize.
    ID2D1DeviceContext* BeginDraw(UINT backBufferIndex);
    // EndDraw + ReleaseWrappedResources (which transitions the back buffer to
    // PRESENT) + Flush. On D2DERR_RECREATE_TARGET the bitmaps are dropped so
    // the next frame rebuilds them -- Renderer.cpp resets its target on that
    // HRESULT but never recreates it except via a later Resize, so the whole
    // overlay silently disappears until the user resizes the window. That bug
    // is deliberately not reproduced here.
    HRESULT EndDraw(UINT backBufferIndex);

    IDWriteFactory* WriteFactory() const noexcept { return writeFactory_.Get(); }
    // The one context every caller draws into. Valid for the bridge's whole
    // lifetime, so cached device resources need no per-frame revalidation.
    ID2D1DeviceContext* Context() const noexcept { return d2dContext_.Get(); }
    bool IsReady() const noexcept { return d3d11On12Device_ != nullptr; }
    // True when EndDraw saw D2DERR_RECREATE_TARGET and the caller's cached
    // device resources must be dropped and rebuilt.
    bool ConsumeTargetsWereRecreated() noexcept;

    // Releases D2D and D3D11On12 before the owner releases its D3D12
    // objects, which `04-rendering-and-streaming.md:190` requires explicitly.
    void Shutdown();

private:
    bool CreateTargetsForBackBuffers(D3D12SwapChain& swapChain, std::wstring& error);

    Microsoft::WRL::ComPtr<ID3D11Device> d3d11Device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d11Context_;
    Microsoft::WRL::ComPtr<ID3D11On12Device> d3d11On12Device_;
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2dFactory_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> d2dContext_;
    Microsoft::WRL::ComPtr<IDWriteFactory> writeFactory_;
    Microsoft::WRL::ComPtr<ID3D11Resource> wrappedBackBuffers_[D3D12SwapChain::kBufferCount];
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> targetBitmaps_[D3D12SwapChain::kBufferCount];
    // Set when the targets were lost and rebuilt, cleared by
    // ConsumeTargetsWereRecreated.
    bool targetsRecreated_ = false;
    // Kept so a lost target can be rebuilt without the caller handing the
    // swap chain back in.
    D3D12SwapChain* swapChain_ = nullptr;
};
