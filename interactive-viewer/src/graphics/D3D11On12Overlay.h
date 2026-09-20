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
// *is* an ID2D1RenderTarget, which is what the chrome ported from Renderer.cpp
// was written against.
//
// Ownership: one thread owns every interop context (ADR-010) -- the render
// thread, which owns D3D12ViewerPath and therefore this.

#include "../render/Renderer.h" // OverlayInfo and the pure layout types
#include "D3D12CommandQueue.h"
#include "D3D12Device.h"
#include "D3D12SwapChain.h"

#include <d2d1_1.h>
#include <d3d11on12.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>

// Immutable UI snapshot, published to the render thread. No HWND calls or
// UI-owned objects are consulted while painting.
struct OverlayFrame
{
    OverlayInfo info;
    NavGizmo gizmo;
    Chrome chrome;
};

enum class OverlayIconKind;

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
    // the next frame rebuilds them. The device context and its brush survive
    // bitmap recreation; a full device loss requires owner-level recovery.
    HRESULT EndDraw(UINT backBufferIndex);

    // Draws the actual application chrome between Acquire and Release.
    HRESULT DrawChrome(UINT backBufferIndex, const DirectX::XMFLOAT4& orientation, const OverlayFrame& frame);

    IDWriteFactory* WriteFactory() const noexcept { return writeFactory_.Get(); }
    // The one context every caller draws into. Valid for the bridge's whole
    // lifetime, so cached device resources need no per-frame revalidation.
    ID2D1DeviceContext* Context() const noexcept { return d2dContext_.Get(); }
    bool IsReady() const noexcept { return d3d11On12Device_ != nullptr; }
    // True when BeginDraw rebuilt back-buffer bitmaps after target loss.
    bool ConsumeTargetsWereRecreated() noexcept;

    // Releases D2D and D3D11On12 before the owner releases its D3D12
    // objects, which `04-rendering-and-streaming.md:190` requires explicitly.
    void Shutdown();

private:
    bool CreateTextFormats(float scale);
    // Builds the cached, device-independent geometry the shaded-sphere
    // lighting glyphs (Studio) need. Best-effort: DrawIcon falls back to a
    // plain elliptical fill if this fails, so a missing geometry never takes
    // the whole overlay down.
    bool CreateIconGeometries();
    void SetBrush(D2D1_COLOR_F color);
    void DrawText(const std::wstring& text, IDWriteTextFormat* format, D2D1_RECT_F rectangle,
        D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING);
    void DrawSpinner(D2D1_POINT_2F center, float animationPhase, float innerRadius,
        float outerRadius, float strokeWidth);
    void DrawGizmo(const DirectX::XMFLOAT4& orientation, const NavGizmo& gizmo, const OverlayInfo& overlay, float scale);
    void DrawBottomBar(const OverlayInfo& overlay, float clientWidth, float clientHeight, float scale);
    void DrawIconButton(const RECT& rectI, OverlayIconKind icon, bool visible, bool enabled, bool active,
        bool hovered, bool pressedNow, float scale);
    void DrawInfoPanel(const OverlayInfo& overlay, float clientWidth, float clientHeight, float scale);
    static D2D1_RECT_F ToRectF(RECT rect);
    void DrawTitleBar(const OverlayInfo& overlay, const Chrome& chrome, float clientWidth, float scale);
    void DrawSpeedFlyout(const OverlayInfo& overlay, float scale);
    void DrawToggleRow(const D2D1_RECT_F& rowRect, const D2D1_RECT_F& switchRect,
        const std::wstring& label, bool on, float scale);
    void DrawSettingsPanel(const OverlayInfo& overlay, float scale);
    void DrawTooltip(const OverlayInfo& overlay, float clientWidth, float scale);
    void DrawOverlay(const DirectX::XMFLOAT4& orientation, const OverlayInfo& overlay, const NavGizmo& gizmo, const Chrome& chrome);

    bool CreateTargetsForBackBuffers(D3D12SwapChain& swapChain, std::wstring& error);

    // A single device brush is recolored for every primitive, shared by all
    // back-buffer bitmaps and retained across ResizeBuffers.
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> overlayBrush;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> dashedStroke, spinnerStroke;
    // Unit-space (centre at the origin, radius 1) intersection of the sphere
    // disc and the offset "lit" disc used by the Studio glyph, so the shaded
    // region is transformed per draw instead of rebuilt every frame.
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> studioLitGeometry;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> headingFormat, bodyFormat, smallFormat, filenameFormat, gizmoFormat;
    float textScale = 0.0f;

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
    bool highContrastFrame_ = false;
    // Kept so a lost target can be rebuilt without the caller handing the
    // swap chain back in.
    D3D12SwapChain* swapChain_ = nullptr;
};
