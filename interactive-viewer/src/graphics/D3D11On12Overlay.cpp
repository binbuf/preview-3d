#include "D3D11On12Overlay.h"

#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

enum class OverlayIconKind
{
    Grid,
    GroundAxis,
    GroundDirectionPositive,
    GroundDirectionNegative,
    AxisSnap,
    Speed,
    Fit,
    Reset,
    Share,
    Overflow,
    OpenWith,
    Info,
    Close,
    FullscreenEnter,
    FullscreenExit,
    Wireframe,
    Clay,
    Studio,
    Directional,
    XRay,
};

namespace {

float Scale(float value, float dpiScale)
{
    return std::round(value * dpiScale);
}

// Vector glyphs for the toolbar/status-bar icon buttons — hand-drawn with
// plain D2D primitives (lines/ellipses/rectangles) rather than a bitmap
// asset, so they stay pixel-crisp at any DPI and need no image-loading
// pipeline, matching how the system caption glyphs (DrawTitleBar) and the
// NavGizmo axis balls are already drawn.
// Four L-shaped corner brackets around (cx, cy). `vertexRadius` is where each
// bracket's corner sits and `armEndRadius` is how far its two arms reach;
// vertexRadius > armEndRadius points the brackets inward (Fit / enter
// fullscreen, brackets at the outer edge closing toward the middle) while
// vertexRadius < armEndRadius points them outward (exit fullscreen, brackets
// near the middle opening toward the edge).
void DrawCornerBrackets(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, float cx, float cy,
    float vertexRadius, float armEndRadius, float strokeWidth)
{
    for (float sx : { -1.0f, 1.0f })
    {
        for (float sy : { -1.0f, 1.0f })
        {
            const D2D1_POINT_2F vertex{ cx + sx * vertexRadius, cy + sy * vertexRadius };
            target->DrawLine(vertex, D2D1::Point2F(cx + sx * armEndRadius, cy + sy * vertexRadius), brush, strokeWidth);
            target->DrawLine(vertex, D2D1::Point2F(cx + sx * vertexRadius, cy + sy * armEndRadius), brush, strokeWidth);
        }
    }
}

// Parallel 45-degree hatch chords clipped analytically to the circle of
// radius `r` at (cx, cy) — the "shadow" convention the Blender-style lighting
// glyphs use. Each line is x - y = k, so the perpendicular distance from the
// centre is (cx - cy - k)/sqrt(2); lines further than r away are skipped and
// the rest are trimmed to their chord. The caller owns the brush colour and
// sets the alpha it wants afterwards.
void DrawHatch(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
    float cx, float cy, float r, float alpha, float scale)
{
    D2D1_COLOR_F color = brush->GetColor();
    color.a = alpha;
    brush->SetColor(color);
    constexpr float invSqrt2 = 0.70710678f;
    const float step = Scale(3.6f, scale) * 1.41421356f;
    for (float k = -2.0f * r; k <= 2.0f * r + step; k += step)
    {
        const float distance = (cx - cy - k) * invSqrt2;
        if (distance <= -r || distance >= r) continue;
        const float halfChord = std::sqrt(r * r - distance * distance);
        const float footX = cx - distance * invSqrt2;
        const float footY = cy + distance * invSqrt2;
        target->DrawLine(
            D2D1::Point2F(footX - halfChord * invSqrt2, footY - halfChord * invSqrt2),
            D2D1::Point2F(footX + halfChord * invSqrt2, footY + halfChord * invSqrt2),
            brush, Scale(1.15f, scale));
    }
}

void DrawIcon(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
    ID2D1PathGeometry* studioLitGeometry, ID2D1StrokeStyle* dashedStroke,
    OverlayIconKind kind, D2D1_RECT_F rect, float scale)
{
    const float cx = (rect.left + rect.right) * 0.5f;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float stroke = Scale(1.4f, scale);
    switch (kind)
    {
    case OverlayIconKind::GroundAxis:
        // The current X/Y/Z letter is drawn with DirectWrite by DrawTitleBar.
        // A small ground line and upward tick keep it recognizable as an
        // orientation control rather than an arbitrary text button.
        target->DrawLine(D2D1::Point2F(cx - Scale(7.0f, scale), cy + Scale(7.0f, scale)),
            D2D1::Point2F(cx + Scale(7.0f, scale), cy + Scale(7.0f, scale)), brush, stroke);
        target->DrawLine(D2D1::Point2F(cx - Scale(7.0f, scale), cy + Scale(7.0f, scale)),
            D2D1::Point2F(cx - Scale(7.0f, scale), cy + Scale(2.0f, scale)), brush, stroke);
        break;
    case OverlayIconKind::GroundDirectionPositive:
    case OverlayIconKind::GroundDirectionNegative:
    {
        const bool negative = kind == OverlayIconKind::GroundDirectionNegative;
        const float groundY = cy + (negative ? Scale(-5.0f, scale) : Scale(5.0f, scale));
        const float tipY = cy + (negative ? Scale(6.0f, scale) : Scale(-6.0f, scale));
        const float arrowBaseY = cy + (negative ? Scale(2.0f, scale) : Scale(-2.0f, scale));
        target->DrawLine(D2D1::Point2F(cx - Scale(7.0f, scale), groundY),
            D2D1::Point2F(cx + Scale(7.0f, scale), groundY), brush, stroke);
        target->DrawLine(D2D1::Point2F(cx, groundY), D2D1::Point2F(cx, tipY), brush, stroke);
        target->DrawLine(D2D1::Point2F(cx, tipY),
            D2D1::Point2F(cx - Scale(3.5f, scale), arrowBaseY), brush, stroke);
        target->DrawLine(D2D1::Point2F(cx, tipY),
            D2D1::Point2F(cx + Scale(3.5f, scale), arrowBaseY), brush, stroke);
        break;
    }
    case OverlayIconKind::Grid:
    {
        const float half = Scale(7.0f, scale);
        target->DrawRectangle(D2D1::RectF(cx - half, cy - half, cx + half, cy + half), brush, stroke);
        target->DrawLine(D2D1::Point2F(cx, cy - half), D2D1::Point2F(cx, cy + half), brush, stroke);
        target->DrawLine(D2D1::Point2F(cx - half, cy), D2D1::Point2F(cx + half, cy), brush, stroke);
        break;
    }
    case OverlayIconKind::Wireframe:
    {
        // Wire-globe: a meridian and an equator ellipse plus two latitude
        // chords read as a wire sphere rather than a flat grid.
        const float radius = Scale(8.0f, scale);
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush, stroke);
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius * 0.45f, radius), brush, stroke);
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius * 0.45f), brush, stroke);
        for (float latitude : { -0.55f, 0.55f })
        {
            const float y = cy + latitude * radius;
            const float halfWidth = radius * std::sqrt(std::max(0.0f, 1.0f - latitude * latitude));
            target->DrawLine(D2D1::Point2F(cx - halfWidth, y), D2D1::Point2F(cx + halfWidth, y), brush, stroke);
        }
        break;
    }
    case OverlayIconKind::Clay:
    {
        // Untextured matte ball: a single solid disc.
        const float radius = Scale(8.0f, scale);
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush);
        break;
    }
    case OverlayIconKind::Studio:
    {
        // Studio environment: a ball lit from the upper left, with the shaded
        // lower-right crescent hatched. The lit lens is the cached
        // intersection of the disc with an equal disc offset up-left.
        const float radius = Scale(8.0f, scale);
        DrawHatch(target, brush, cx, cy, radius, 0.48f, scale);
        D2D1_COLOR_F color = brush->GetColor();
        color.a = 0.92f;
        brush->SetColor(color);
        if (studioLitGeometry != nullptr)
        {
            D2D1_MATRIX_3X2_F previous{};
            target->GetTransform(&previous);
            target->SetTransform(D2D1::Matrix3x2F::Scale(radius, radius) * D2D1::Matrix3x2F::Translation(cx, cy));
            target->FillGeometry(studioLitGeometry, brush);
            target->SetTransform(previous);
        }
        else
        {
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx - 0.22f * radius, cy - 0.22f * radius),
                radius * 0.72f, radius * 0.72f), brush);
        }
        color.a = 1.0f;
        brush->SetColor(color);
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush, stroke);
        break;
    }
    case OverlayIconKind::Directional:
    {
        // Single raking light: the same ball, but only a specular highlight
        // is solid while the rest of the surface stays hatched.
        const float radius = Scale(8.0f, scale);
        DrawHatch(target, brush, cx, cy, radius, 0.48f, scale);
        D2D1_COLOR_F color = brush->GetColor();
        color.a = 0.95f;
        brush->SetColor(color);
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx - 0.30f * radius, cy - 0.30f * radius),
            radius * 0.52f, radius * 0.52f), brush);
        color.a = 1.0f;
        brush->SetColor(color);
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush, stroke);
        break;
    }
    case OverlayIconKind::XRay:
    {
        // Outlined frame with a dashed inner frame: "see through the shell".
        const float half = Scale(7.5f, scale);
        target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - half, cy - half, cx + half, cy + half),
            Scale(1.5f, scale), Scale(1.5f, scale)), brush, stroke);
        const float inner = Scale(4.6f, scale);
        target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - inner, cy - inner, cx + inner, cy + inner),
            Scale(1.0f, scale), Scale(1.0f, scale)), brush, stroke, dashedStroke);
        break;
    }
    case OverlayIconKind::AxisSnap:
    {
        // Target/crosshair with four tick marks poking outside the ring.
        const float radius = Scale(5.5f, scale);
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush, stroke);
        const float tickOuter = radius + Scale(3.0f, scale);
        const float tickInner = radius - Scale(1.5f, scale);
        target->DrawLine(D2D1::Point2F(cx, cy - tickOuter), D2D1::Point2F(cx, cy - tickInner), brush, stroke);
        target->DrawLine(D2D1::Point2F(cx, cy + tickInner), D2D1::Point2F(cx, cy + tickOuter), brush, stroke);
        target->DrawLine(D2D1::Point2F(cx - tickOuter, cy), D2D1::Point2F(cx - tickInner, cy), brush, stroke);
        target->DrawLine(D2D1::Point2F(cx + tickInner, cy), D2D1::Point2F(cx + tickOuter, cy), brush, stroke);
        break;
    }
    case OverlayIconKind::Speed:
    {
        // Speedometer: an upper-half-circle gauge face with a needle.
        const float radius = Scale(6.5f, scale);
        const float baseY = cy + Scale(1.5f, scale);
        constexpr int segments = 10;
        D2D1_POINT_2F previous{};
        for (int i = 0; i <= segments; ++i)
        {
            const float t = static_cast<float>(i) / segments;
            const float angle = XM_PI + t * XM_PI;
            const D2D1_POINT_2F point{ cx + std::cos(angle) * radius, baseY + std::sin(angle) * radius };
            if (i > 0) target->DrawLine(previous, point, brush, stroke);
            previous = point;
        }
        constexpr float needleAngle = -XM_PIDIV2 + 0.75f;
        target->DrawLine(D2D1::Point2F(cx, baseY),
            D2D1::Point2F(cx + std::cos(needleAngle) * radius * 0.8f, baseY + std::sin(needleAngle) * radius * 0.8f),
            brush, stroke);
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, baseY), Scale(1.3f, scale), Scale(1.3f, scale)), brush);
        break;
    }
    case OverlayIconKind::Fit:
    {
        // A picture-frame with a small mountain silhouette inside, reading as
        // "frame the subject" — deliberately distinct from the corner-bracket
        // expand glyph used for Fullscreen (below) so the two read as separate
        // actions instead of duplicates.
        const float halfW = Scale(7.5f, scale);
        const float halfH = Scale(6.0f, scale);
        target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - halfW, cy - halfH, cx + halfW, cy + halfH),
            Scale(2.0f, scale), Scale(2.0f, scale)), brush, stroke);
        const float baseY = cy + halfH - Scale(1.5f, scale);
        const D2D1_POINT_2F peak{ cx - Scale(1.0f, scale), cy - Scale(1.0f, scale) };
        const D2D1_POINT_2F left{ cx - halfW + Scale(2.0f, scale), baseY };
        const D2D1_POINT_2F right{ cx + halfW - Scale(2.0f, scale), baseY };
        target->DrawLine(left, peak, brush, stroke);
        target->DrawLine(peak, right, brush, stroke);
        target->DrawLine(left, right, brush, stroke);
        break;
    }
    case OverlayIconKind::FullscreenEnter:
        DrawCornerBrackets(target, brush, cx, cy, Scale(6.5f, scale), Scale(3.2f, scale), stroke);
        break;
    case OverlayIconKind::FullscreenExit:
        DrawCornerBrackets(target, brush, cx, cy, Scale(2.6f, scale), Scale(6.5f, scale), stroke);
        break;
    case OverlayIconKind::Reset:
    {
        // Circular refresh arrow: a ~280-degree arc with an arrowhead at the
        // trailing end, tangent to the direction of travel.
        const float radius = Scale(6.5f, scale);
        constexpr int segments = 12;
        constexpr float startAngle = -XM_PIDIV2 - 0.35f;
        constexpr float sweep = XM_2PI * 0.78f;
        D2D1_POINT_2F previous{};
        D2D1_POINT_2F tip{};
        float tipAngle = 0.0f;
        for (int i = 0; i <= segments; ++i)
        {
            const float t = static_cast<float>(i) / segments;
            const float angle = startAngle + t * sweep;
            const D2D1_POINT_2F point{ cx + std::cos(angle) * radius, cy + std::sin(angle) * radius };
            if (i > 0) target->DrawLine(previous, point, brush, stroke);
            previous = point;
            tip = point;
            tipAngle = angle;
        }
        const float tangent = tipAngle + XM_PIDIV2;
        const float headSize = Scale(3.2f, scale);
        for (float sign : { -1.0f, 1.0f })
        {
            const float headAngle = tangent + XM_PI + sign * 0.55f;
            target->DrawLine(tip, D2D1::Point2F(tip.x + std::cos(headAngle) * headSize, tip.y + std::sin(headAngle) * headSize),
                brush, stroke);
        }
        break;
    }
    case OverlayIconKind::Info:
    {
        const float radius = Scale(7.0f, scale);
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush, stroke);
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy - radius * 0.42f), Scale(1.15f, scale), Scale(1.15f, scale)), brush);
        target->DrawLine(D2D1::Point2F(cx, cy - radius * 0.02f), D2D1::Point2F(cx, cy + radius * 0.5f), brush, Scale(1.7f, scale));
        break;
    }
    case OverlayIconKind::Close:
    {
        const float radius = Scale(5.0f, scale);
        target->DrawLine(D2D1::Point2F(cx - radius, cy - radius), D2D1::Point2F(cx + radius, cy + radius), brush, stroke);
        target->DrawLine(D2D1::Point2F(cx + radius, cy - radius), D2D1::Point2F(cx - radius, cy + radius), brush, stroke);
        break;
    }
    case OverlayIconKind::Share:
    {
        const float halfW = Scale(6.0f, scale);
        const float boxTop = cy + Scale(1.0f, scale);
        const float boxBottom = cy + Scale(6.5f, scale);
        target->DrawLine(D2D1::Point2F(cx - halfW, boxTop), D2D1::Point2F(cx - halfW, boxBottom), brush, stroke);
        target->DrawLine(D2D1::Point2F(cx + halfW, boxTop), D2D1::Point2F(cx + halfW, boxBottom), brush, stroke);
        target->DrawLine(D2D1::Point2F(cx - halfW, boxBottom), D2D1::Point2F(cx + halfW, boxBottom), brush, stroke);
        const float arrowTop = cy - Scale(7.0f, scale);
        target->DrawLine(D2D1::Point2F(cx, boxTop + Scale(0.5f, scale)), D2D1::Point2F(cx, arrowTop), brush, stroke);
        const float headSize = Scale(3.0f, scale);
        target->DrawLine(D2D1::Point2F(cx, arrowTop), D2D1::Point2F(cx - headSize, arrowTop + headSize), brush, stroke);
        target->DrawLine(D2D1::Point2F(cx, arrowTop), D2D1::Point2F(cx + headSize, arrowTop + headSize), brush, stroke);
        break;
    }
    case OverlayIconKind::Overflow:
    {
        const float dotRadius = Scale(1.6f, scale);
        const float gapX = Scale(5.0f, scale);
        for (float i : { -1.0f, 0.0f, 1.0f })
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + i * gapX, cy), dotRadius, dotRadius), brush);
        break;
    }
    case OverlayIconKind::OpenWith:
    {
        const float half = Scale(4.6f, scale);
        const float boxCx = cx - Scale(6.0f, scale);
        const float boxCy = cy + Scale(1.0f, scale);
        target->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(boxCx - half, boxCy - half, boxCx + half, boxCy + half), Scale(1.5f, scale), Scale(1.5f, scale)),
            brush, stroke);
        const D2D1_POINT_2F arrowTip{ boxCx + half + Scale(2.2f, scale), boxCy - half - Scale(1.2f, scale) };
        target->DrawLine(D2D1::Point2F(boxCx + Scale(1.0f, scale), boxCy - Scale(1.0f, scale)), arrowTip, brush, stroke);
        const float headSize = Scale(2.6f, scale);
        target->DrawLine(arrowTip, D2D1::Point2F(arrowTip.x - headSize, arrowTip.y), brush, stroke);
        target->DrawLine(arrowTip, D2D1::Point2F(arrowTip.x, arrowTip.y + headSize), brush, stroke);
        const float chevCx = cx + Scale(7.0f, scale);
        const float chevHalf = Scale(2.4f, scale);
        target->DrawLine(D2D1::Point2F(chevCx - chevHalf, cy - Scale(1.5f, scale)), D2D1::Point2F(chevCx, cy + Scale(1.5f, scale)), brush, stroke);
        target->DrawLine(D2D1::Point2F(chevCx, cy + Scale(1.5f, scale)), D2D1::Point2F(chevCx + chevHalf, cy - Scale(1.5f, scale)), brush, stroke);
        break;
    }
    }
}


std::wstring WithHr(const wchar_t* what, HRESULT hr)
{
    return std::wstring(what) + L" (HRESULT 0x" + std::to_wstring(static_cast<unsigned long>(hr)) + L").";
}

} // namespace

D3D11On12Overlay::~D3D11On12Overlay()
{
    Shutdown();
}

bool D3D11On12Overlay::Initialize(D3D12Device& device, D3D12CommandQueue& directQueue, D3D12SwapChain& swapChain,
                                   std::wstring& error)
{
    IUnknown* queues[] = { directQueue.Queue() };
    // BGRA support is required for D2D interop regardless of the swap chain's
    // own format -- it is a device capability flag, not a surface format.
    const HRESULT deviceHr = D3D11On12CreateDevice(device.Device(), D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                                     queues, 1, 0, &d3d11Device_, &d3d11Context_, nullptr);
    if (FAILED(deviceHr)) {
        error = WithHr(L"The D3D11On12 device could not be created", deviceHr);
        return false;
    }
    const HRESULT queryHr = d3d11Device_.As(&d3d11On12Device_);
    if (FAILED(queryHr)) {
        error = WithHr(L"The D3D11On12 device interface could not be queried", queryHr);
        return false;
    }

    D2D1_FACTORY_OPTIONS factoryOptions{};
#ifdef _DEBUG
    factoryOptions.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    const HRESULT factoryHr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                                 &factoryOptions, &d2dFactory_);
    if (FAILED(factoryHr)) {
        error = WithHr(L"The Direct2D factory could not be created", factoryHr);
        return false;
    }

    // Best-effort: the shaded-sphere glyphs fall back to a plain fill if the
    // cached geometry cannot be built, so this never fails initialization.
    CreateIconGeometries();

    // The D2D device rides on the same underlying DXGI device as the 11on12
    // device, which is what lets one context target every back buffer.
    ComPtr<IDXGIDevice> dxgiDevice;
    const HRESULT dxgiHr = d3d11Device_.As(&dxgiDevice);
    if (FAILED(dxgiHr)) {
        error = WithHr(L"The D3D11On12 device is not a DXGI device", dxgiHr);
        return false;
    }
    const HRESULT d2dDeviceHr = d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_);
    if (FAILED(d2dDeviceHr)) {
        error = WithHr(L"The Direct2D device could not be created", d2dDeviceHr);
        return false;
    }
    const HRESULT contextHr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext_);
    if (FAILED(contextHr)) {
        error = WithHr(L"The Direct2D device context could not be created", contextHr);
        return false;
    }

    const HRESULT writeHr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                 reinterpret_cast<IUnknown**>(writeFactory_.GetAddressOf()));
    if (FAILED(writeHr)) {
        error = WithHr(L"The DirectWrite factory could not be created", writeHr);
        return false;
    }

    if (FAILED(d2dContext_->CreateSolidColorBrush(D2D1::ColorF(0xF5F5F7), &overlayBrush))) {
        error = L"The chrome brush could not be created.";
        return false;
    }
    auto dash = D2D1::StrokeStyleProperties();
    dash.dashStyle = D2D1_DASH_STYLE_DASH;
    auto spinner = D2D1::StrokeStyleProperties();
    spinner.startCap = D2D1_CAP_STYLE_ROUND;
    spinner.endCap = D2D1_CAP_STYLE_ROUND;
    if (FAILED(d2dFactory_->CreateStrokeStyle(dash, nullptr, 0, &dashedStroke)) ||
        FAILED(d2dFactory_->CreateStrokeStyle(spinner, nullptr, 0, &spinnerStroke)) ||
        !CreateTextFormats(1.0f)) {
        error = L"The chrome text and strokes could not be created.";
        return false;
    }
    swapChain_ = &swapChain;
    return CreateTargetsForBackBuffers(swapChain, error);
}

bool D3D11On12Overlay::CreateTargetsForBackBuffers(D3D12SwapChain& swapChain, std::wstring& error)
{
    swapChain_ = &swapChain;

    for (UINT i = 0; i < D3D12SwapChain::kBufferCount; ++i) {
        ID3D12Resource* backBuffer = swapChain.BackBuffer(i);
        if (backBuffer == nullptr) {
            error = L"A swap-chain back buffer was missing when wrapping it for the overlay.";
            return false;
        }

        // In RENDER_TARGET, out PRESENT: the scene pass leaves the buffer as
        // a render target and the Release inside EndDraw is what moves it to
        // PRESENT. See this class's header comment and 04-...:46.
        D3D11_RESOURCE_FLAGS flags{};
        flags.BindFlags = D3D11_BIND_RENDER_TARGET;
        const HRESULT wrapHr = d3d11On12Device_->CreateWrappedResource(
            backBuffer, &flags, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT,
            IID_PPV_ARGS(&wrappedBackBuffers_[i]));
        if (FAILED(wrapHr)) {
            error = WithHr(L"A swap-chain back buffer could not be wrapped for D3D11On12", wrapHr);
            return false;
        }

        ComPtr<IDXGISurface> surface;
        const HRESULT surfaceHr = wrappedBackBuffers_[i].As(&surface);
        if (FAILED(surfaceHr)) {
            error = WithHr(L"A wrapped back buffer is not a DXGI surface", surfaceHr);
            return false;
        }

        // The format must match the swap chain's. The spike confirmed D2D
        // accepts R8G8B8A8_UNORM here -- the design's format (04-...:17) --
        // so no BGRA swap chain is needed.
        DXGI_SURFACE_DESC surfaceDesc{};
        surface->GetDesc(&surfaceDesc);
        const D2D1_BITMAP_PROPERTIES1 bitmapProperties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(surfaceDesc.Format, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        const HRESULT bitmapHr
            = d2dContext_->CreateBitmapFromDxgiSurface(surface.Get(), &bitmapProperties, &targetBitmaps_[i]);
        if (FAILED(bitmapHr)) {
            error = WithHr(L"Direct2D could not create a target bitmap over the wrapped back buffer", bitmapHr);
            return false;
        }
    }
    return true;
}

void D3D11On12Overlay::ReleaseBackBufferReferences()
{
    if (d2dContext_) d2dContext_->SetTarget(nullptr);
    for (UINT i = 0; i < D3D12SwapChain::kBufferCount; ++i) {
        targetBitmaps_[i].Reset();
        wrappedBackBuffers_[i].Reset();
    }
    // The wrapped resources hold D3D11-side references; without a flush they
    // can outlive the Reset above and keep ResizeBuffers from succeeding.
    if (d3d11Context_) {
        d3d11Context_->ClearState();
        d3d11Context_->Flush();
    }
}

bool D3D11On12Overlay::RecreateBackBufferReferences(D3D12SwapChain& swapChain, std::wstring& error)
{
    if (!IsReady()) {
        error = L"The overlay bridge is not initialized.";
        return false;
    }
    return CreateTargetsForBackBuffers(swapChain, error);
}

bool D3D11On12Overlay::ConsumeTargetsWereRecreated() noexcept
{
    const bool was = targetsRecreated_;
    targetsRecreated_ = false;
    return was;
}

ID2D1DeviceContext* D3D11On12Overlay::BeginDraw(UINT backBufferIndex)
{
    if (!IsReady() || backBufferIndex >= D3D12SwapChain::kBufferCount) return nullptr;

    // Rebuild after a lost target, rather than going dark until the next
    // resize the way the D3D11 renderer does.
    if (!targetBitmaps_[backBufferIndex] && swapChain_ != nullptr) {
        std::wstring ignored;
        if (!CreateTargetsForBackBuffers(*swapChain_, ignored)) return nullptr;
        targetsRecreated_ = true;
    }
    if (!targetBitmaps_[backBufferIndex]) return nullptr;

    ID3D11Resource* resources[] = { wrappedBackBuffers_[backBufferIndex].Get() };
    d3d11On12Device_->AcquireWrappedResources(resources, 1);
    d2dContext_->SetTarget(targetBitmaps_[backBufferIndex].Get());
    d2dContext_->BeginDraw();
    return d2dContext_.Get();
}

HRESULT D3D11On12Overlay::EndDraw(UINT backBufferIndex)
{
    if (!IsReady() || backBufferIndex >= D3D12SwapChain::kBufferCount) return E_FAIL;

    const HRESULT hr = d2dContext_->EndDraw();
    d2dContext_->SetTarget(nullptr);

    ID3D11Resource* resources[] = { wrappedBackBuffers_[backBufferIndex].Get() };
    d3d11On12Device_->ReleaseWrappedResources(resources, 1);
    // Flush submits the 11on12 work onto the shared direct queue. Without it
    // the D2D commands are not on the queue when the caller signals its frame
    // fence and presents.
    d3d11Context_->Flush();

    if (hr == D2DERR_RECREATE_TARGET) {
        // Drop the back-buffer bitmaps so the next BeginDraw rebuilds them.
        // Device resources, including the single chrome brush, stay valid.
        ReleaseBackBufferReferences();
    }
    return hr;
}

void D3D11On12Overlay::Shutdown()
{
    ReleaseBackBufferReferences();
    overlayBrush.Reset();
    dashedStroke.Reset();
    spinnerStroke.Reset();
    studioLitGeometry.Reset();
    headingFormat.Reset(); bodyFormat.Reset(); smallFormat.Reset(); filenameFormat.Reset(); gizmoFormat.Reset();
    textScale = 0.0f;
    targetsRecreated_ = false;
    writeFactory_.Reset();
    d2dContext_.Reset();
    d2dDevice_.Reset();
    d2dFactory_.Reset();
    d3d11On12Device_.Reset();
    d3d11Context_.Reset();
    d3d11Device_.Reset();
    swapChain_ = nullptr;
}

RECT CalculateErrorCardRect(int width, int height, int toolbarHeight, float dpiScale)
{
    const int cardWidth = std::min(static_cast<int>(Scale(560.0f, dpiScale)), std::max(280, width - static_cast<int>(Scale(32.0f, dpiScale))));
    const int cardHeight = std::min(static_cast<int>(Scale(258.0f, dpiScale)), std::max(220, height - toolbarHeight - static_cast<int>(Scale(32.0f, dpiScale))));
    const int left = (width - cardWidth) / 2;
    const int top = toolbarHeight + std::max(0, (height - toolbarHeight - cardHeight) / 2);
    return RECT{ left, top, left + cardWidth, top + cardHeight };
}

bool D3D11On12Overlay::CreateIconGeometries()
{
    if (!d2dFactory_) return false;
    ComPtr<ID2D1EllipseGeometry> sphere;
    ComPtr<ID2D1EllipseGeometry> terminator;
    if (FAILED(d2dFactory_->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(0.0f, 0.0f), 1.0f, 1.0f), &sphere)) ||
        FAILED(d2dFactory_->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(-0.38f, -0.38f), 1.0f, 1.0f), &terminator)))
    {
        return false;
    }
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(d2dFactory_->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return false;
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    const HRESULT combine = sphere->CombineWithGeometry(terminator.Get(), D2D1_COMBINE_MODE_INTERSECT, nullptr, sink.Get());
    if (FAILED(combine) || FAILED(sink->Close())) return false;
    studioLitGeometry = path;
    return true;
}

bool D3D11On12Overlay::CreateTextFormats(float scale)
{
    if (std::abs(textScale - scale) < 0.01f && headingFormat && bodyFormat && smallFormat && filenameFormat && gizmoFormat) return true;
    headingFormat.Reset(); bodyFormat.Reset(); smallFormat.Reset(); filenameFormat.Reset();
    gizmoFormat.Reset();
    textScale = scale;
    auto create = [&](float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& output)
    {
        return SUCCEEDED(writeFactory_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, Scale(size, scale), L"en-us", &output));
    };
    if (!create(22, DWRITE_FONT_WEIGHT_SEMI_BOLD, headingFormat) ||
        !create(14, DWRITE_FONT_WEIGHT_NORMAL, bodyFormat) ||
        !create(12, DWRITE_FONT_WEIGHT_NORMAL, smallFormat) ||
        !create(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, filenameFormat) ||
        !create(10, DWRITE_FONT_WEIGHT_SEMI_BOLD, gizmoFormat)) return false;
    headingFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    headingFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    filenameFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    smallFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    gizmoFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    gizmoFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    return true;
}

void D3D11On12Overlay::SetBrush(D2D1_COLOR_F color)
{
    if (highContrastFrame_)
    {
        const float luminance = color.r * 0.2126f + color.g * 0.7152f + color.b * 0.0722f;
        const COLORREF system = luminance < 0.35f ? GetSysColor(COLOR_WINDOW)
            : luminance > 0.72f ? GetSysColor(COLOR_WINDOWTEXT) : GetSysColor(COLOR_HIGHLIGHT);
        color = D2D1::ColorF(GetRValue(system) / 255.0f, GetGValue(system) / 255.0f,
            GetBValue(system) / 255.0f, std::max(color.a, 0.82f));
    }
    overlayBrush->SetColor(color);
}

void D3D11On12Overlay::DrawText(const std::wstring& text, IDWriteTextFormat* format, D2D1_RECT_F rectangle,
    D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT alignment)
{
    if (text.empty()) return;
    format->SetTextAlignment(alignment);
    SetBrush(color);
    d2dContext_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rectangle, overlayBrush.Get(),
        D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void D3D11On12Overlay::DrawSpinner(D2D1_POINT_2F center, float animationPhase, float innerRadius,
    float outerRadius, float strokeWidth)
{
    constexpr int spokeCount = 12;
    const int leadingSpoke = static_cast<int>(animationPhase * spokeCount) % spokeCount;
    for (int spoke = 0; spoke < spokeCount; ++spoke)
    {
        const float angle = static_cast<float>(spoke) / spokeCount * XM_2PI - XM_PIDIV2;
        const int age = (spoke - leadingSpoke + spokeCount) % spokeCount;
        const float alpha = 0.14f + 0.78f * (1.0f - static_cast<float>(age) / spokeCount);
        SetBrush(D2D1::ColorF(0xF5F5F7, alpha));
        d2dContext_->DrawLine(
            D2D1::Point2F(center.x + std::cos(angle) * innerRadius, center.y + std::sin(angle) * innerRadius),
            D2D1::Point2F(center.x + std::cos(angle) * outerRadius, center.y + std::sin(angle) * outerRadius),
            overlayBrush.Get(), strokeWidth, spinnerStroke.Get());
    }
}

void D3D11On12Overlay::DrawGizmo(const DirectX::XMFLOAT4& orientation, const NavGizmo& gizmo, const OverlayInfo& overlay, float scale)
{
    if (!gizmoFormat) return;
    const NavGizmo::DrawGeometry g = gizmo.ComputeDraw(XMLoadFloat4(&orientation));
    const D2D1_POINT_2F center{ g.centerX, g.centerY };
    const D2D1_COLOR_F axisColors[3] = {
        D2D1::ColorF(0.98f, 0.28f, 0.32f, 1.0f),   // X red
        D2D1::ColorF(0.30f, 0.84f, 0.18f, 1.0f),   // Y green
        D2D1::ColorF(0.24f, 0.57f, 1.00f, 1.0f) }; // Z blue
    const wchar_t axisLetters[3] = { L'X', L'Y', L'Z' };

    // Ball: a quiet disc that brightens when the orbit-drag target. It is
    // sized independently of the white ring so growing the ring leaves the
    // interior untouched. The ring brightens when it is the light-rotation
    // target.
    const bool ballHover = g.hover == NavGizmo::Part::Ball;
    const bool lightHover = g.hover == NavGizmo::Part::Light;
    SetBrush(D2D1::ColorF(0x11141A, ballHover ? 0.32f : 0.16f));
    d2dContext_->FillEllipse(D2D1::Ellipse(center, g.ballRadius, g.ballRadius), overlayBrush.Get());
    SetBrush(D2D1::ColorF(lightHover ? 0xFFFFFF : 0xB9B9C2,
        lightHover ? 1.0f : (ballHover ? 0.95f : 0.40f)));
    d2dContext_->DrawEllipse(D2D1::Ellipse(center, g.outerRadius, g.outerRadius), overlayBrush.Get(),
        Scale(lightHover ? 2.0f : 1.1f, scale));

    // Stems, dimmed when their axis points away from the viewer.
    for (int axis = 0; axis < 3; ++axis)
    {
        const float depth = (g.positive[axis].depth + g.negative[axis].depth) * 0.5f;
        const bool hovered = g.hover == static_cast<NavGizmo::Part>(
            static_cast<int>(NavGizmo::Part::PosX) + axis);
        const float alpha = depth >= 0.0f ? 0.95f : (hovered ? 0.75f : 0.40f);
        SetBrush(D2D1::ColorF(axisColors[axis].r, axisColors[axis].g, axisColors[axis].b, alpha));
        d2dContext_->DrawLine(center,
            D2D1::Point2F(center.x + g.positive[axis].x, center.y + g.positive[axis].y),
            overlayBrush.Get(), g.stemWidth);
    }

    // Nodes and dots, painted back to front by view-space depth so the
    // gizmo reads as a small 3D object rather than a flat diagram.
    struct NodePaint
    {
        const NavGizmo::NodeGeometry* node;
        int axis;
        bool positive;
    };
    NodePaint nodes[6]{};
    for (int axis = 0; axis < 3; ++axis)
    {
        nodes[axis] = { &g.positive[axis], axis, true };
        nodes[axis + 3] = { &g.negative[axis], axis, false };
    }
    std::sort(nodes, nodes + 6, [](const NodePaint& a, const NodePaint& b)
        { return a.node->depth < b.node->depth; });
    for (const NodePaint& node : nodes)
    {
        const float radius = node.positive ? g.nodeRadius : g.dotRadius;
        const D2D1_POINT_2F position{ center.x + node.node->x, center.y + node.node->y };
        const NavGizmo::Part part = static_cast<NavGizmo::Part>(
            static_cast<int>(NavGizmo::Part::PosX) + node.axis + (node.positive ? 0 : 3));
        const bool hovered = g.hover == part;
        const bool behind = node.node->depth < 0.0f;
        const D2D1_COLOR_F base = axisColors[node.axis];
        const float dim = behind && !hovered ? 0.55f : 1.0f;
        const float lift = hovered ? 0.35f : 0.0f;
        SetBrush(D2D1::ColorF(
            std::min(1.0f, base.r * dim + lift),
            std::min(1.0f, base.g * dim + lift),
            std::min(1.0f, base.b * dim + lift),
            behind && !hovered ? 0.60f : 1.0f));
        d2dContext_->FillEllipse(D2D1::Ellipse(position, radius, radius), overlayBrush.Get());
        if (hovered)
        {
            SetBrush(D2D1::ColorF(0xFFFFFF, 0.95f));
            d2dContext_->DrawEllipse(D2D1::Ellipse(position, radius, radius), overlayBrush.Get(), Scale(1.6f, scale));
        }
        if (node.positive)
        {
            const std::wstring letter(1, axisLetters[node.axis]);
            SetBrush(D2D1::ColorF(0xFFFFFF, behind ? 0.80f : 1.0f));
            d2dContext_->DrawTextW(letter.c_str(), static_cast<UINT32>(letter.size()), gizmoFormat.Get(),
                D2D1::RectF(position.x - radius, position.y - radius, position.x + radius, position.y + radius),
                overlayBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    // Directional-light sun marker: a small sun riding the white outer ring at
    // the compass bearing of the key light. Always full opacity so it stays
    // easy to see and grab.
    if (overlay.lightingMode == LightingMode::Directional)
    {
        const NavGizmo::SunGeometry sun = gizmo.ComputeSun(XMLoadFloat4(&orientation),
            overlay.directionalLightAngle, overlay.directionalLightElevation);
        if (sun.visible)
        {
            const D2D1_POINT_2F position{ center.x + sun.x, center.y + sun.y };
            const float core = Scale(3.0f, scale);
            const float rayInner = Scale(4.5f, scale);
            const float rayOuter = Scale(7.3f, scale);
            const float sunStroke = Scale(1.5f, scale);

            // While the sun is held, an opaque disc larger than an axis node
            // makes it read as its own control rather than another axis button,
            // so a drag is never mistaken for an orbit/axis hit.
            if (overlay.lightDragging)
            {
                const float backdrop = g.nodeRadius * 1.45f;
                SetBrush(D2D1::ColorF(1.0f, 0.80f, 0.10f, 1.0f)); // golden yellow
                d2dContext_->FillEllipse(D2D1::Ellipse(position, backdrop, backdrop), overlayBrush.Get());
                SetBrush(D2D1::ColorF(0.82f, 0.58f, 0.06f, 1.0f)); // deeper gold edge
                d2dContext_->DrawEllipse(D2D1::Ellipse(position, backdrop, backdrop), overlayBrush.Get(),
                    Scale(1.4f, scale));
            }

            // White while the sun is held, lighter yellow on hover, amber
            // otherwise, so the grabbed control is unmistakable.
            SetBrush(overlay.lightDragging ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)
                    : lightHover ? D2D1::ColorF(1.0f, 0.95f, 0.62f, 1.0f)
                                 : D2D1::ColorF(1.0f, 0.80f, 0.26f, 1.0f));
            d2dContext_->DrawEllipse(D2D1::Ellipse(position, core, core), overlayBrush.Get(), sunStroke);
            for (int ray = 0; ray < 8; ++ray)
            {
                const float angle = static_cast<float>(ray) * XM_PIDIV4;
                const float ca = std::cos(angle);
                const float sa = std::sin(angle);
                d2dContext_->DrawLine(
                    D2D1::Point2F(position.x + ca * rayInner, position.y + sa * rayInner),
                    D2D1::Point2F(position.x + ca * rayOuter, position.y + sa * rayOuter),
                    overlayBrush.Get(), sunStroke);
            }
        }
    }
}

// Photos-style bottom bar: a bar strip matching the title bar's fill,
// with the Info button docked at the far left, a D2D-drawn zoom slider
// (ZoomTrackRect, Preview3D.cpp — same split as the Speed flyout track)
// docked bottom-right next to the zoom-percent readout, and the
// Fullscreen toggle at the very right. Only drawn while a model is loaded
// — see OverlayInfo::barBottomBarHeight, which the app zeroes out
// otherwise. All three button rects are computed by Preview3D.cpp (same
// split as the zoom track) so hit-testing and drawing never drift apart.
// Drawn (as a floating toolbar over the viewport, slightly translucent)
// even in Fullscreen, where OverlayInfo::bottomBarHeight — the *reserved*
// viewport inset — has already collapsed to 0; barBottomBarHeight is the
// bar's own always-real height, so it keeps showing (and the Fullscreen
// toggle stays reachable) instead of vanishing along with that space.
void D3D11On12Overlay::DrawBottomBar(const OverlayInfo& overlay, float clientWidth, float clientHeight, float scale)
{
    if (overlay.barBottomBarHeight <= 0) return;
    const float barTop = clientHeight - static_cast<float>(overlay.barBottomBarHeight);
    const float fillAlpha = overlay.isFullscreen ? 0.88f : 1.0f;
    SetBrush(D2D1::ColorF(0x2C2C2E, fillAlpha));
    d2dContext_->FillRectangle(D2D1::RectF(0, barTop, clientWidth, clientHeight), overlayBrush.Get());
    SetBrush(D2D1::ColorF(0x3A3A3C));
    d2dContext_->FillRectangle(D2D1::RectF(0, barTop, clientWidth, barTop + 1.0f), overlayBrush.Get());

    DrawIconButton(overlay.infoButtonRect, OverlayIconKind::Info, /*visible*/ true, /*enabled*/ true,
        overlay.infoPanelVisible, overlay.infoButtonHover, overlay.infoButtonPressed, scale);

    const float durationLeft = static_cast<float>(overlay.infoButtonRect.right) + Scale(12, scale);
    if (overlay.renderTimerRunning)
    {
        DrawSpinner(D2D1::Point2F(durationLeft + Scale(7, scale),
            (barTop + clientHeight) * 0.5f), overlay.animationPhase,
            Scale(3, scale), Scale(7, scale), Scale(1.6f, scale));
    }
    else if (!overlay.renderDurationText.empty())
    {
        DrawText(overlay.renderDurationText, smallFormat.Get(),
            D2D1::RectF(durationLeft, barTop, durationLeft + Scale(96, scale), clientHeight),
            D2D1::ColorF(0xA1A1A6));
    }

    if (overlay.lightingToolbarRect.right>overlay.lightingToolbarRect.left
        && overlay.lightingToolbarRect.bottom>overlay.lightingToolbarRect.top) {
        const D2D1_RECT_F lightingBounds=ToRectF(overlay.lightingToolbarRect);
        SetBrush(D2D1::ColorF(0x1C1C1E,overlay.isFullscreen?0.94f:0.82f));
        d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(lightingBounds,Scale(9,scale),Scale(9,scale)),overlayBrush.Get());
        SetBrush(D2D1::ColorF(0x48484C));
        d2dContext_->DrawRoundedRectangle(D2D1::RoundedRect(lightingBounds,Scale(9,scale),Scale(9,scale)),overlayBrush.Get(),1.0f);

    auto drawModeButton=[&](const RECT& rect,OverlayIconKind icon,bool active,bool hovered,bool pressed) {
        const D2D1_RECT_F button=ToRectF(rect);
        D2D1_COLOR_F fill=D2D1::ColorF(0x000000,0.0f);
        if (active) fill=D2D1::ColorF(0x0A84FF,0.34f);
        if (hovered) fill=active?D2D1::ColorF(0x0A84FF,0.48f):D2D1::ColorF(0x4A4A4F,0.78f);
        if (pressed) fill=active?D2D1::ColorF(0x0A84FF,0.62f):D2D1::ColorF(0x5A5A60,0.72f);
        SetBrush(fill);
        d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(button,Scale(6,scale),Scale(6,scale)),overlayBrush.Get());
        SetBrush(active?D2D1::ColorF(0xFFFFFF):D2D1::ColorF(0xD1D1D6));
        DrawIcon(d2dContext_.Get(),overlayBrush.Get(),studioLitGeometry.Get(),dashedStroke.Get(),icon,button,scale);
    };
    drawModeButton(overlay.studioButtonRect,OverlayIconKind::Studio,overlay.lightingMode==LightingMode::Studio,
        overlay.studioButtonHover,overlay.studioButtonPressed);
    drawModeButton(overlay.clayButtonRect,OverlayIconKind::Clay,overlay.lightingMode==LightingMode::Clay,
        overlay.clayButtonHover,overlay.clayButtonPressed);
    drawModeButton(overlay.directionalButtonRect,OverlayIconKind::Directional,overlay.lightingMode==LightingMode::Directional,
        overlay.directionalButtonHover,overlay.directionalButtonPressed);
    drawModeButton(overlay.wireframeButtonRect,OverlayIconKind::Wireframe,overlay.lightingMode==LightingMode::Wireframe,
        overlay.wireframeButtonHover,overlay.wireframeButtonPressed);
    }

    const std::wstring percentText = std::to_wstring(static_cast<int>(std::lround(overlay.zoomPercent))) + L"%";
    const float labelWidth = Scale(56, scale);
    const float gap = Scale(10, scale);
    const D2D1_RECT_F fullscreenRect = ToRectF(overlay.fullscreenButtonRect);
    DrawText(percentText, smallFormat.Get(),
        D2D1::RectF(fullscreenRect.left - gap - labelWidth, barTop, fullscreenRect.left - gap, clientHeight),
        D2D1::ColorF(0xA1A1A6), DWRITE_TEXT_ALIGNMENT_TRAILING);

    const D2D1_RECT_F track = ToRectF(overlay.zoomTrackRect);
    const float trackY = (track.top + track.bottom) * 0.5f;
    SetBrush(D2D1::ColorF(0x48484C));
    d2dContext_->DrawLine(D2D1::Point2F(track.left, trackY), D2D1::Point2F(track.right, trackY), overlayBrush.Get(), Scale(3, scale));
    const float thumbX = track.left + (track.right - track.left) * std::clamp(overlay.zoomSliderT, 0.0f, 1.0f);
    SetBrush(D2D1::ColorF(0x0A84FF));
    d2dContext_->DrawLine(D2D1::Point2F(track.left, trackY), D2D1::Point2F(thumbX, trackY), overlayBrush.Get(), Scale(3, scale));
    SetBrush(D2D1::ColorF(0xF5F5F7));
    d2dContext_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, trackY), Scale(7, scale), Scale(7, scale)), overlayBrush.Get());

    DrawIconButton(overlay.fullscreenButtonRect, overlay.isFullscreen ? OverlayIconKind::FullscreenExit : OverlayIconKind::FullscreenEnter,
        /*visible*/ true, /*enabled*/ true, overlay.isFullscreen, overlay.fullscreenButtonHover, overlay.fullscreenButtonPressed, scale);
}

// Shared icon-button chrome (fill/hover/press/active/disabled) for both
// the title-bar action buttons and the bottom-bar Info/Fullscreen
// buttons, painting one of the vector glyphs from OverlayIconKind on top.
void D3D11On12Overlay::DrawIconButton(const RECT& rectI, OverlayIconKind icon, bool visible, bool enabled, bool active,
    bool hovered, bool pressedNow, float scale)
{
    if (!visible) return;
    const D2D1_RECT_F rect = ToRectF(rectI);
    D2D1_COLOR_F fill = D2D1::ColorF(0x3A3A3C);
    D2D1_COLOR_F iconColor = D2D1::ColorF(0xF5F5F7);
    if (active) fill = D2D1::ColorF(0x0A84FF, 0.28f);
    if (hovered) fill = active ? D2D1::ColorF(0x0A84FF, 0.40f) : D2D1::ColorF(0x46464A);
    if (pressedNow) fill = active ? D2D1::ColorF(0x0A84FF, 0.55f) : D2D1::ColorF(0x2C2C2E);
    if (!enabled) { fill = D2D1::ColorF(0x2C2C2E); iconColor = D2D1::ColorF(0x707075); }
    SetBrush(fill);
    d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(rect, Scale(6, scale), Scale(6, scale)), overlayBrush.Get());
    SetBrush(iconColor);
    DrawIcon(d2dContext_.Get(), overlayBrush.Get(), studioLitGeometry.Get(), dashedStroke.Get(), icon, rect, scale);
}

// Right-docked, read-only "Stats & Shading" panel — see InfoPanel.h for
// the section/row content, built by the app from the loaded model's
// scanned ModelStats.
void D3D11On12Overlay::DrawInfoPanel(const OverlayInfo& overlay, float clientWidth, float clientHeight, float scale)
{
    if (overlay.infoPanelWidth <= 0) return;
    const float panelWidth = static_cast<float>(overlay.infoPanelWidth);
    const float left = clientWidth - panelWidth;
    const float top = static_cast<float>(overlay.barToolbarHeight);
    const float bottom = clientHeight - static_cast<float>(overlay.barBottomBarHeight);

    SetBrush(D2D1::ColorF(0x242426));
    d2dContext_->FillRectangle(D2D1::RectF(left, top, clientWidth, bottom), overlayBrush.Get());
    SetBrush(D2D1::ColorF(0x3A3A3C));
    d2dContext_->FillRectangle(D2D1::RectF(left, top, left + 1.0f, bottom), overlayBrush.Get());

    const float margin = Scale(16, scale);
    const float rowHeight = Scale(24, scale);
    const float sectionGap = Scale(18, scale);
    const float textLeft = left + margin;
    const D2D1_RECT_F closeButton = ToRectF(overlay.infoPanelCloseButtonRect);
    const float textRight = closeButton.left > textLeft
        ? closeButton.left - Scale(8, scale)
        : clientWidth - margin;
    float y = top + Scale(18, scale);

    DrawText(L"Stats & Shading", filenameFormat.Get(),
        D2D1::RectF(textLeft, y, textRight, y + Scale(22, scale)), D2D1::ColorF(0xF5F5F7));
    if (overlay.infoPanelCloseButtonRect.right > overlay.infoPanelCloseButtonRect.left &&
        overlay.infoPanelCloseButtonRect.bottom > overlay.infoPanelCloseButtonRect.top)
    {
        DrawIconButton(overlay.infoPanelCloseButtonRect, OverlayIconKind::Close, /*visible*/ true, /*enabled*/ true,
            /*active*/ false, overlay.infoPanelCloseButtonHover, overlay.infoPanelCloseButtonPressed, scale);
    }
    y += Scale(34, scale);

    // Everything below the fixed header scrolls as one block
    // (overlay.infoPanelScrollOffset, driven by the mouse wheel over the
    // panel — see WM_MOUSEWHEEL in Preview3D.cpp), clipped to the panel's
    // body so scrolled rows never bleed into the bars above/below it or
    // the viewport to its left.
    const float contentTop = y;
    const float visibleHeight = std::max(0.0f, bottom - contentTop);
    const InfoPanelScrollMetrics metrics = ComputeInfoPanelScrollMetrics(overlay.infoPanelSections, scale);
    const float maxScroll = std::max(0.0f, metrics.contentHeight - visibleHeight);
    const float scrollOffset = std::clamp(overlay.infoPanelScrollOffset, 0.0f, maxScroll);

    d2dContext_->PushAxisAlignedClip(D2D1::RectF(left, contentTop, clientWidth, bottom), D2D1_ANTIALIAS_MODE_ALIASED);
    y = contentTop - scrollOffset;
    for (const InfoPanelSection& section : overlay.infoPanelSections)
    {
        if (y > bottom) break;
        DrawText(section.title, smallFormat.Get(), D2D1::RectF(textLeft, y, textRight, y + rowHeight),
            D2D1::ColorF(0x0A84FF));
        y += rowHeight;
        for (const InfoPanelRow& row : section.rows)
        {
            if (y > bottom) break;
            const D2D1_RECT_F rowRect = D2D1::RectF(textLeft, y, textRight, y + rowHeight);
            DrawText(row.label, smallFormat.Get(), rowRect, D2D1::ColorF(0xA1A1A6));
            DrawText(row.value, smallFormat.Get(), rowRect, D2D1::ColorF(0xF5F5F7), DWRITE_TEXT_ALIGNMENT_TRAILING);
            y += rowHeight;
        }
        y += sectionGap - rowHeight;
    }
    d2dContext_->PopAxisAlignedClip();

    if (maxScroll > 0.0f)
    {
        const float thumbMargin = Scale(4, scale);
        const float thumbWidth = Scale(3, scale);
        const float trackHeight = bottom - contentTop;
        const float thumbHeight = std::max(Scale(24, scale), trackHeight * (visibleHeight / metrics.contentHeight));
        const float thumbTop = contentTop + (trackHeight - thumbHeight) * (scrollOffset / maxScroll);
        SetBrush(D2D1::ColorF(0x5A5A5E, 0.85f));
        d2dContext_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(clientWidth - thumbMargin - thumbWidth, thumbTop, clientWidth - thumbMargin, thumbTop + thumbHeight),
                thumbWidth * 0.5f, thumbWidth * 0.5f),
            overlayBrush.Get());
    }
}

D2D1_RECT_F D3D11On12Overlay::ToRectF(RECT rect)
{
    return D2D1::RectF(static_cast<float>(rect.left), static_cast<float>(rect.top),
        static_cast<float>(rect.right), static_cast<float>(rect.bottom));
}

// Windows-11-Photos-style unified title bar: action buttons, centered
// filename, Open With, and the (D2D-drawn, NC-hit-tested) system
// min/max/close — all laid out by Chrome::UpdateLayout.
void D3D11On12Overlay::DrawTitleBar(const OverlayInfo& overlay, const Chrome& chrome, float clientWidth, float scale)
{
    const RECT barRectI = chrome.TitleBarRect();
    const float barHeight = static_cast<float>(barRectI.bottom);
    // Fullscreen: skip the strip's own background/divider entirely (the
    // buttons/filename below still draw at the same rects, unmoved) so
    // the grey bar disappears and only its floating controls remain over
    // the full-bleed viewport, rather than reading as a translucent bar.
    if (!overlay.isFullscreen)
    {
        SetBrush(D2D1::ColorF(0x2C2C2E));
        d2dContext_->FillRectangle(D2D1::RectF(0, 0, clientWidth, barHeight), overlayBrush.Get());
        SetBrush(D2D1::ColorF(0x3A3A3C));
        d2dContext_->FillRectangle(D2D1::RectF(0, barHeight - 1, clientWidth, barHeight), overlayBrush.Get());
    }

    const std::wstring filename = overlay.filename.empty() ? L"Preview 3D" : overlay.filename;
    DrawText(filename, filenameFormat.Get(), ToRectF(chrome.FilenameRect()), D2D1::ColorF(0xF5F5F7),
        DWRITE_TEXT_ALIGNMENT_CENTER);

    // Icon-only action buttons (vector glyphs, OverlayIconKind/DrawIcon above),
    // matching the old owner-drawn toolbar button palette
    // (primary/secondary/active/hover/pressed).
    auto drawActionButton = [&](Chrome::Part part, OverlayIconKind icon, bool active)
    {
        const Chrome::ButtonState& state = chrome.Button(part);
        const bool hovered = chrome.hover == part;
        const bool pressedNow = chrome.pressed == part;
        DrawIconButton(state.rect, icon, state.visible, state.enabled, active, hovered, pressedNow, scale);
    };
    drawActionButton(Chrome::Part::Grid, OverlayIconKind::Grid, overlay.gridVisible);
    drawActionButton(Chrome::Part::GroundAxis, OverlayIconKind::GroundAxis, false);
    const Chrome::ButtonState& groundAxis = chrome.Button(Chrome::Part::GroundAxis);
    if (groundAxis.visible)
    {
        const std::wstring label = GroundAxisName(overlay.effectiveGroundAxis);
        RECT labelRect = groundAxis.rect;
        labelRect.bottom -= static_cast<LONG>(Scale(2.0f, scale));
        DrawText(label, gizmoFormat.Get(), ToRectF(labelRect), D2D1::ColorF(0xF5F5F7),
            DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    drawActionButton(Chrome::Part::GroundDirection,
        overlay.groundAxisInverted ? OverlayIconKind::GroundDirectionNegative
                                   : OverlayIconKind::GroundDirectionPositive,
        overlay.groundAxisInverted);
    drawActionButton(Chrome::Part::AxisSnap, OverlayIconKind::AxisSnap, overlay.axisSnapEnabled);
    drawActionButton(Chrome::Part::Speed, OverlayIconKind::Speed, false);
    drawActionButton(Chrome::Part::Fit, OverlayIconKind::Fit, false);
    drawActionButton(Chrome::Part::Reset, OverlayIconKind::Reset, false);
    drawActionButton(Chrome::Part::Share, OverlayIconKind::Share, false);
    drawActionButton(Chrome::Part::Overflow, OverlayIconKind::Overflow, false);
    drawActionButton(Chrome::Part::OpenWith, OverlayIconKind::OpenWith, false);

    // System caption buttons: simple vector glyphs, matching Windows 11's
    // minimize/maximize-or-restore/close. DWM (via DwmDefWindowProc,
    // Preview3D.cpp) draws the hover/press background for these — the
    // hover/press tint here is a same-frame fallback for any gap before
    // that first paints.
    auto drawSystemButton = [&](Chrome::Part part, auto drawGlyph, bool closeButton)
    {
        const Chrome::ButtonState& state = chrome.Button(part);
        if (!state.visible) return;
        const D2D1_RECT_F rect = ToRectF(state.rect);
        const bool hovered = chrome.hover == part;
        const bool pressedNow = chrome.pressed == part;
        if (pressedNow) { SetBrush(closeButton ? D2D1::ColorF(0xC42B1C) : D2D1::ColorF(0x3F3F42)); d2dContext_->FillRectangle(rect, overlayBrush.Get()); }
        else if (hovered) { SetBrush(closeButton ? D2D1::ColorF(0xE81123) : D2D1::ColorF(0x35353A)); d2dContext_->FillRectangle(rect, overlayBrush.Get()); }
        SetBrush(D2D1::ColorF(0xF5F5F7));
        const float cx = (rect.left + rect.right) * 0.5f;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        drawGlyph(cx, cy);
    };
    const float glyphHalf = Scale(5, scale);
    drawSystemButton(Chrome::Part::Minimize, [&](float cx, float cy)
    {
        d2dContext_->DrawLine(D2D1::Point2F(cx - glyphHalf, cy), D2D1::Point2F(cx + glyphHalf, cy), overlayBrush.Get(), 1.0f);
    }, false);
    drawSystemButton(Chrome::Part::Maximize, [&](float cx, float cy)
    {
        if (chrome.Maximized())
        {
            const float inset = Scale(2, scale);
            d2dContext_->DrawRectangle(D2D1::RectF(cx - glyphHalf + inset, cy - glyphHalf, cx + glyphHalf, cy + glyphHalf - inset), overlayBrush.Get(), 1.0f);
            d2dContext_->DrawRectangle(D2D1::RectF(cx - glyphHalf, cy - glyphHalf + inset, cx + glyphHalf - inset, cy + glyphHalf), overlayBrush.Get(), 1.0f);
        }
        else
        {
            d2dContext_->DrawRectangle(D2D1::RectF(cx - glyphHalf, cy - glyphHalf, cx + glyphHalf, cy + glyphHalf), overlayBrush.Get(), 1.0f);
        }
    }, false);
    drawSystemButton(Chrome::Part::Close, [&](float cx, float cy)
    {
        d2dContext_->DrawLine(D2D1::Point2F(cx - glyphHalf, cy - glyphHalf), D2D1::Point2F(cx + glyphHalf, cy + glyphHalf), overlayBrush.Get(), 1.0f);
        d2dContext_->DrawLine(D2D1::Point2F(cx - glyphHalf, cy + glyphHalf), D2D1::Point2F(cx + glyphHalf, cy - glyphHalf), overlayBrush.Get(), 1.0f);
    }, true);
}

// Photos-style "drop-down under the button with a slider and a number
// value" for travel speed — a small floating panel below the Speed
// button, drawn on top of the viewport. The app owns the panel/track
// rects and drag math (Preview3D.cpp); this only draws them.
void D3D11On12Overlay::DrawSpeedFlyout(const OverlayInfo& overlay, float scale)
{
    if (!overlay.speedFlyoutOpen) return;
    const D2D1_RECT_F panel = ToRectF(overlay.speedFlyoutRect);
    SetBrush(D2D1::ColorF(0x242426, 0.98f));
    d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(panel, Scale(10, scale), Scale(10, scale)), overlayBrush.Get());
    SetBrush(D2D1::ColorF(0x3A3A3C));
    d2dContext_->DrawRoundedRectangle(D2D1::RoundedRect(panel, Scale(10, scale), Scale(10, scale)), overlayBrush.Get(), 1.0f);

    DrawText(L"Travel speed", smallFormat.Get(),
        D2D1::RectF(panel.left + Scale(16, scale), panel.top + Scale(8, scale), panel.right - Scale(16, scale), panel.top + Scale(26, scale)),
        D2D1::ColorF(0xA1A1A6));
    DrawText(overlay.speedValueText, smallFormat.Get(),
        D2D1::RectF(panel.left + Scale(16, scale), panel.top + Scale(8, scale), panel.right - Scale(16, scale), panel.top + Scale(26, scale)),
        D2D1::ColorF(0xF5F5F7), DWRITE_TEXT_ALIGNMENT_TRAILING);

    const D2D1_RECT_F track = ToRectF(overlay.speedFlyoutTrackRect);
    const float trackY = (track.top + track.bottom) * 0.5f;
    SetBrush(D2D1::ColorF(0x48484C));
    d2dContext_->DrawLine(D2D1::Point2F(track.left, trackY), D2D1::Point2F(track.right, trackY), overlayBrush.Get(), Scale(3, scale));
    const float thumbX = track.left + (track.right - track.left) * std::clamp(overlay.speedSliderT, 0.0f, 1.0f);
    SetBrush(D2D1::ColorF(0x0A84FF));
    d2dContext_->DrawLine(D2D1::Point2F(track.left, trackY), D2D1::Point2F(thumbX, trackY), overlayBrush.Get(), Scale(3, scale));
    SetBrush(D2D1::ColorF(0xF5F5F7));
    d2dContext_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, trackY), Scale(7, scale), Scale(7, scale)), overlayBrush.Get());
}

// Reusable "label + switch" row: a leading text label and a trailing
// pill-shaped on/off switch. Drawing only — SettingsPanel-related rects
// are hit-tested by Preview3D.cpp against the same rects this is given.
void D3D11On12Overlay::DrawToggleRow(const D2D1_RECT_F& rowRect, const D2D1_RECT_F& switchRect,
    const std::wstring& label, bool on, float scale)
{
    DrawText(label, smallFormat.Get(),
        D2D1::RectF(rowRect.left, rowRect.top, switchRect.left - Scale(8, scale), rowRect.bottom),
        D2D1::ColorF(0xF5F5F7));
    const float radius = (switchRect.bottom - switchRect.top) * 0.5f;
    SetBrush(on ? D2D1::ColorF(0x0A84FF) : D2D1::ColorF(0x3A3A3C));
    d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(switchRect, radius, radius), overlayBrush.Get());
    const float thumbRadius = radius - Scale(2, scale);
    const float thumbX = on ? switchRect.right - radius : switchRect.left + radius;
    const float thumbY = (switchRect.top + switchRect.bottom) * 0.5f;
    SetBrush(D2D1::ColorF(0xF5F5F7));
    d2dContext_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, thumbY), thumbRadius, thumbRadius), overlayBrush.Get());
}

void D3D11On12Overlay::DrawSettingsPanel(const OverlayInfo& overlay, float scale)
{
    if (!overlay.settingsPanelOpen) return;
    const D2D1_RECT_F panel = ToRectF(overlay.settingsPanelRect);
    SetBrush(D2D1::ColorF(0x242426, 0.98f));
    d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(panel, Scale(10, scale), Scale(10, scale)), overlayBrush.Get());
    SetBrush(D2D1::ColorF(0x3A3A3C));
    d2dContext_->DrawRoundedRectangle(D2D1::RoundedRect(panel, Scale(10, scale), Scale(10, scale)), overlayBrush.Get(), 1.0f);

    DrawToggleRow(ToRectF(overlay.nativeOrientationRowRect), ToRectF(overlay.nativeOrientationSwitchRect),
        L"Show model in its original orientation", overlay.showNativeOrientation, scale);
    DrawToggleRow(ToRectF(overlay.hideCursorRowRect), ToRectF(overlay.hideCursorSwitchRect),
        L"Hide cursor while dragging", overlay.hideCursorWhileDragging, scale);
}

// A small dark bubble naming the button under the cursor, shown once
// Preview3D.cpp's hover-delay timer decides the pointer has parked on a
// button for a while (see ComputeTooltipInfo). Anchored below title-bar
// buttons and above bottom-bar ones (overlay.tooltipBelow) so it never
// reads as covering the button it describes.
void D3D11On12Overlay::DrawTooltip(const OverlayInfo& overlay, float clientWidth, float scale)
{
    if (!overlay.tooltipVisible || overlay.tooltipText.empty() || !smallFormat) return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(writeFactory_->CreateTextLayout(overlay.tooltipText.c_str(), static_cast<UINT32>(overlay.tooltipText.size()),
        smallFormat.Get(), Scale(300, scale), Scale(200, scale), &layout))) return;
    // smallFormat is shared and mutated in place by every other DrawText()
    // call this frame (e.g. DrawBottomBar's TRAILING-aligned percent
    // readout, drawn just before this) — pin this layout's own alignment
    // so it never inherits whatever that last call left behind (with a
    // wide layout box, TRAILING alignment pushed the text off past the
    // right edge of the bubble, making it silently invisible).
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return;

    const float paddingX = Scale(10, scale);
    const float paddingY = Scale(6, scale);
    const float gap = Scale(8, scale);
    const float margin = Scale(4, scale);
    const float bubbleWidth = metrics.width + paddingX * 2.0f;
    const float bubbleHeight = metrics.height + paddingY * 2.0f;

    const D2D1_RECT_F anchor = ToRectF(overlay.tooltipAnchorRect);
    const float anchorCenterX = (anchor.left + anchor.right) * 0.5f;
    const float left = std::clamp(anchorCenterX - bubbleWidth * 0.5f, margin, std::max(margin, clientWidth - bubbleWidth - margin));
    const float top = overlay.tooltipBelow ? anchor.bottom + gap : anchor.top - gap - bubbleHeight;
    const D2D1_RECT_F bubble = D2D1::RectF(left, top, left + bubbleWidth, top + bubbleHeight);

    SetBrush(D2D1::ColorF(0x1C1C1E, 0.97f));
    d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(bubble, Scale(5, scale), Scale(5, scale)), overlayBrush.Get());
    SetBrush(D2D1::ColorF(0x48484C));
    d2dContext_->DrawRoundedRectangle(D2D1::RoundedRect(bubble, Scale(5, scale), Scale(5, scale)), overlayBrush.Get(), 1.0f);
    SetBrush(D2D1::ColorF(0xF5F5F7));
    d2dContext_->DrawTextLayout(D2D1::Point2F(left + paddingX, top + paddingY), layout.Get(), overlayBrush.Get());
}

void D3D11On12Overlay::DrawOverlay(const DirectX::XMFLOAT4& orientation, const OverlayInfo& overlay, const NavGizmo& gizmo, const Chrome& chrome)
{
    if (!d2dContext_ || !overlayBrush) return;
    highContrastFrame_ = overlay.highContrast;
    if (!CreateTextFormats(overlay.dpiScale)) return;
    const float scale = overlay.dpiScale;
    const float toolbar = static_cast<float>(overlay.barToolbarHeight);
    const float clientWidth = static_cast<float>(swapChain_->Width());
    const float clientHeight = static_cast<float>(swapChain_->Height());
    // Bottom-anchored chrome (status pill, warning badge, HUD pills) sits
    // above the bottom bar and left of the Information panel, both of
    // which are only reserved while a model is loaded (see
    // OverlayInfo::barBottomBarHeight/infoPanelWidth).
    const float contentBottom = clientHeight - static_cast<float>(overlay.barBottomBarHeight);
    const float contentRight = clientWidth - static_cast<float>(overlay.infoPanelWidth);

    // Drawn first (so the system caption buttons never disappear while a
    // model is loading — this used to be skipped entirely during
    // ViewerState::Loading, which is what made the window briefly
    // uncontrollable right after launching with a file or dropping one
    // in) — including in Fullscreen, where it becomes a floating toolbar
    // (Chrome::UpdateLayout hides Minimize/Maximize/Close/Open With there
    // instead, keeping just the action buttons) drawn over the
    // full-monitor viewport rather than pushed out of it: Preview3D.cpp's
    // EffectiveToolbarHeight still collapses the *reserved* space to 0 in
    // that state and bypasses the title bar's NC hit-testing to match, so
    // the viewport fills the whole window with no dead strip of chrome
    // reserved at the top, while OverlayInfo::barToolbarHeight (`toolbar`
    // above) keeps its real value so the bar still draws and its buttons
    // still hit-test as ordinary client-area ones.
    DrawTitleBar(overlay, chrome, clientWidth, scale);
    if (overlay.state == ViewerState::Loading || overlay.state == ViewerState::Partial)
    {
        const float centerX = clientWidth * 0.5f;
        const float centerY = clientHeight * 0.5f;
        if (overlay.state == ViewerState::Loading)
            DrawSpinner(D2D1::Point2F(centerX, centerY), overlay.animationPhase,
                Scale(10.0f, scale), Scale(17.0f, scale), Scale(2.4f, scale));
        DrawText(overlay.loadingStatus, smallFormat.Get(), D2D1::RectF(centerX - Scale(240, scale),
            centerY + Scale(25, scale), centerX + Scale(240, scale), centerY + Scale(55, scale)),
            D2D1::ColorF(0xA1A1A6), DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    const D2D1_COLOR_F primaryText = D2D1::ColorF(0xF5F5F7);
    const D2D1_COLOR_F secondaryText = D2D1::ColorF(0xA1A1A6);

    if (overlay.state == ViewerState::Empty)
    {
        const float centerX = clientWidth * 0.5f;
        const float centerY = toolbar + (clientHeight - toolbar) * 0.48f;
        const float cardWidth = std::min(Scale(520, scale), clientWidth - Scale(36, scale));
        const float cardHeight = Scale(230, scale);
        const D2D1_ROUNDED_RECT card = D2D1::RoundedRect(
            D2D1::RectF(centerX - cardWidth * 0.5f, centerY - cardHeight * 0.5f,
                centerX + cardWidth * 0.5f, centerY + cardHeight * 0.5f), Scale(14, scale), Scale(14, scale));
        SetBrush(D2D1::ColorF(0x242426, 0.92f));
        d2dContext_->FillRoundedRectangle(card, overlayBrush.Get());
        SetBrush(D2D1::ColorF(0x3A3A3C));
        d2dContext_->DrawRoundedRectangle(card, overlayBrush.Get(), Scale(1, scale), dashedStroke.Get());

        SetBrush(D2D1::ColorF(0x0A84FF));
        d2dContext_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(centerX, centerY - Scale(53, scale)),
            Scale(22, scale), Scale(22, scale)), overlayBrush.Get(), Scale(2, scale));
        d2dContext_->DrawLine(D2D1::Point2F(centerX, centerY - Scale(64, scale)),
            D2D1::Point2F(centerX, centerY - Scale(42, scale)), overlayBrush.Get(), Scale(2, scale));
        d2dContext_->DrawLine(D2D1::Point2F(centerX - Scale(7, scale), centerY - Scale(49, scale)),
            D2D1::Point2F(centerX, centerY - Scale(42, scale)), overlayBrush.Get(), Scale(2, scale));
        d2dContext_->DrawLine(D2D1::Point2F(centerX + Scale(7, scale), centerY - Scale(49, scale)),
            D2D1::Point2F(centerX, centerY - Scale(42, scale)), overlayBrush.Get(), Scale(2, scale));

        DrawText(L"Drop a 3D model here", headingFormat.Get(), D2D1::RectF(centerX - cardWidth * 0.45f,
            centerY - Scale(12, scale), centerX + cardWidth * 0.45f, centerY + Scale(34, scale)), primaryText,
            DWRITE_TEXT_ALIGNMENT_CENTER);
        // DrawText(L"or choose Open to browse", bodyFormat.Get(), D2D1::RectF(centerX - cardWidth * 0.45f,
        //     centerY + Scale(40, scale), centerX + cardWidth * 0.45f, centerY + Scale(68, scale)), secondaryText,
        //     DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(L"Supported formats: GLB/GLTF, STL, PLY, OBJ/MTL, FBX, USD/Z, 3MF, STEP/STP", smallFormat.Get(), D2D1::RectF(centerX - cardWidth * 0.45f,
            centerY + Scale(74, scale), centerX + cardWidth * 0.45f, centerY + Scale(100, scale)),
            D2D1::ColorF(0x747B86), DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    if (overlay.state == ViewerState::Failed)
    {
        SetBrush(D2D1::ColorF(0x0B0C0F, 0.40f));
        d2dContext_->FillRectangle(D2D1::RectF(0, toolbar, clientWidth, clientHeight), overlayBrush.Get());
        const RECT cardPixels = CalculateErrorCardRect(static_cast<int>(swapChain_->Width()), static_cast<int>(swapChain_->Height()), overlay.toolbarHeight, scale);
        const D2D1_ROUNDED_RECT card = D2D1::RoundedRect(D2D1::RectF(static_cast<float>(cardPixels.left),
            static_cast<float>(cardPixels.top), static_cast<float>(cardPixels.right), static_cast<float>(cardPixels.bottom)),
            Scale(12, scale), Scale(12, scale));
        SetBrush(D2D1::ColorF(0x242426));
        d2dContext_->FillRoundedRectangle(card, overlayBrush.Get());
        SetBrush(D2D1::ColorF(0x3A3A3C));
        d2dContext_->DrawRoundedRectangle(card, overlayBrush.Get(), 1.0f);

        const float left = static_cast<float>(cardPixels.left) + Scale(28, scale);
        const float right = static_cast<float>(cardPixels.right) - Scale(28, scale);
        const float top = static_cast<float>(cardPixels.top) + Scale(24, scale);
        SetBrush(D2D1::ColorF(0xFF453A));
        d2dContext_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(left + Scale(12, scale), top + Scale(12, scale)),
            Scale(12, scale), Scale(12, scale)), overlayBrush.Get());
        DrawText(L"!", filenameFormat.Get(), D2D1::RectF(left, top + Scale(1, scale), left + Scale(24, scale),
            top + Scale(24, scale)), D2D1::ColorF(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(overlay.errorSummary, filenameFormat.Get(), D2D1::RectF(left + Scale(38, scale), top - Scale(2, scale),
            right, top + Scale(34, scale)), primaryText);
        DrawText(overlay.errorDetails, bodyFormat.Get(), D2D1::RectF(left, top + Scale(52, scale), right,
            static_cast<float>(cardPixels.bottom) - Scale(70, scale)), secondaryText);
        DrawText(overlay.failureContext, smallFormat.Get(), D2D1::RectF(left, static_cast<float>(cardPixels.bottom) - Scale(101, scale),
            right, static_cast<float>(cardPixels.bottom) - Scale(76, scale)), D2D1::ColorF(0x777F8B));
    }

    if (!overlay.warning.empty() && overlay.hasModel)
    {
        const float size = Scale(30, scale);
        const float right = contentRight - Scale(14, scale);
        const float bottom = contentBottom - Scale(14, scale);
        SetBrush(D2D1::ColorF(0xFF9F0A, 0.92f));
        d2dContext_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(right - size * 0.5f, bottom - size * 0.5f),
            size * 0.5f, size * 0.5f), overlayBrush.Get());
        DrawText(L"!", filenameFormat.Get(), D2D1::RectF(right - size, bottom - size + Scale(2, scale), right, bottom),
            D2D1::ColorF(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // Transient mode readouts (fly speed, grid/projection toggles).
    auto drawHud = [&](const std::wstring& text, float alpha, float bottomOffset)
    {
        if (alpha <= 0.01f || text.empty() || overlay.state != ViewerState::Ready) return;
        const float pillHeight = Scale(26, scale);
        const float pillWidth = std::min(contentRight - Scale(28, scale),
            std::max(Scale(96, scale), Scale(14, scale) + static_cast<float>(text.size()) * Scale(7.6f, scale)));
        const float left = (contentRight - pillWidth) * 0.5f;
        const float top = contentBottom - bottomOffset - pillHeight;
        const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(D2D1::RectF(left, top, left + pillWidth, top + pillHeight),
            pillHeight * 0.5f, pillHeight * 0.5f);
        SetBrush(D2D1::ColorF(0x111318, 0.84f * alpha));
        d2dContext_->FillRoundedRectangle(pill, overlayBrush.Get());
        DrawText(text, smallFormat.Get(), D2D1::RectF(left, top, left + pillWidth, top + pillHeight),
            D2D1::ColorF(0xE8EAED, alpha), DWRITE_TEXT_ALIGNMENT_CENTER);
    };
    drawHud(overlay.speedHud, overlay.speedHudAlpha, Scale(84, scale));
    drawHud(overlay.modeHud, overlay.modeHudAlpha, Scale(50, scale));

    DrawBottomBar(overlay, clientWidth, clientHeight, scale);
    DrawInfoPanel(overlay, clientWidth, clientHeight, scale);

    // Navigation gizmo on top of everything else, except a floating
    // flyout (Speed, Settings), which floats above even that.
    if ((overlay.state == ViewerState::Ready || overlay.state == ViewerState::Partial) && overlay.hasModel)
    {
        DrawGizmo(orientation, gizmo, overlay, scale);
    }
    DrawSpeedFlyout(overlay, scale);
    DrawSettingsPanel(overlay, scale);
    DrawTooltip(overlay, clientWidth, scale);
    if (overlay.keyboardFocusVisible)
    {
        SetBrush(D2D1::ColorF(0xFFFFFF));
        const D2D1_RECT_F focus = ToRectF(overlay.keyboardFocusRect);
        d2dContext_->DrawRectangle(focus, overlayBrush.Get(), Scale(2.0f, scale), dashedStroke.Get());
    }

}


HRESULT D3D11On12Overlay::DrawChrome(UINT backBufferIndex, const DirectX::XMFLOAT4& orientation,
                                    const OverlayFrame& frame)
{
    if (BeginDraw(backBufferIndex) == nullptr) return E_FAIL;
    DrawOverlay(orientation, frame.info, frame.gizmo, frame.chrome);
    return EndDraw(backBufferIndex);
}
