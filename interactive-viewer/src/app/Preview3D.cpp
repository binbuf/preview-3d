#include "framework.h"
#include "Preview3D.h"
#include "Chrome.h"
#include "D3D12ImportBridge.h"
#include "D3D12ViewerPath.h"
#include "InfoPanel.h"
#include "Model.h"
#include "Renderer.h"
#include "NavGizmo.h"
#include "Settings.h"
#include "ShellIntegration.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <cwctype>
#include <iomanip>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr wchar_t kWindowClass[] = L"Preview3DWindow";
constexpr wchar_t kApplicationName[] = L"3D Preview";
constexpr UINT kLoadCompleteMessage = WM_APP + 2;
// The --d3d12 opt-in path's own load-complete message -- kept distinct from
// kLoadCompleteMessage rather than reusing it, since the payload shape
// (d3d12_import_bridge::ImportResult vs. Model.cpp's LoadResult) differs and
// the two loaders never run for the same open (BeginOpen branches on
// useD3D12 before spawning either background thread).
constexpr UINT kD3D12ImportCompleteMessage = WM_APP + 3;
constexpr float kArrowPixelsPerSecond = 340.0f;
constexpr double kHudVisibleSeconds = 1.3;
constexpr double kHudFadeSeconds = 0.30;
constexpr double kToggleRepeatGuardSeconds = 0.30;
constexpr double kClickMaxSeconds = 0.5;
constexpr float kClickDragThresholdPixels = 6.0f;
// Caps how many queued messages the main loop drains before it is forced to
// re-check animation state and render, so any burst or self-sustaining
// message flood can never fully starve rendering.
constexpr int kMaxDrainedMessagesPerIteration = 32;
// How long the pointer has to sit still over a toolbar button before its
// tooltip appears (see ComputeTooltipInfo/UpdateTooltipTracking) — longer
// than a system tooltip's default so it stays out of the way during normal
// clicking, but still short enough to answer "what does this button do?".
constexpr UINT_PTR kTooltipTimerId = 1;
constexpr UINT kTooltipDelayMs = 1500;

// Exclusive pointer state machine. Exactly one mode owns camera/selection
// input at a time; the mode is chosen at button-down and stays locked for the
// whole gesture, so mid-drag modifier changes never switch modes.
//
//   None        idle; gizmo hover tracking only
//   Orbit       LMB or MMB held on the canvas: orbit-drags the camera. An LMB
//               gesture that stays under the click/drag threshold also
//               click-selects on release (drag and click share LMB).
//   GizmoOrbit  LMB held on the gizmo ball: wrapped orbit drag (same math as
//               Orbit, kept distinct only because it started on the gizmo).
//   (Axis nodes/stems snap on press, so they never enter a drag mode.)
//   FlyLook     RMB held: Unreal-style capture. Cursor is hidden and
//               recentered each move; WASD/Q/E fly, wheel adjusts speed.
//   Truck       MMB held: trucks along the world ground plane (flattened
//               forward/right), optionally snapped to the nearest world axis.
//   DollyDrag   Ctrl+MMB: smooth exponential dolly on vertical drag.
enum class PointerMode
{
    None,
    GizmoOrbit,
    FlyLook,
    Orbit,
    Truck,
    DollyDrag
};

struct CompleteMessage
{
    std::uint64_t generation = 0;
    std::wstring path;
    LoadResult result;
};

struct D3D12CompleteMessage
{
    std::uint64_t generation = 0;
    std::wstring path;
    d3d12_import_bridge::ImportResult result;
};

struct ViewerApp
{
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND retryButton = nullptr;
    HWND openAnotherButton = nullptr;
    HWND copyButton = nullptr;
    HWND tooltip = nullptr;
    HFONT buttonFont = nullptr;
    UINT dpi = 96;
    float dpiScale = 1.0f;
    int toolbarHeight = 52;
    int bottomBarHeight = 44;
    bool infoPanelVisible = false;
    float infoPanelScrollOffset = 0.0f;   // logical px, clamped against content each RenderScene/wheel tick
    bool speedFlyoutOpen = false;
    bool speedSliderDragging = false;
    bool zoomSliderDragging = false;
    bool infoButtonHover = false;
    bool infoButtonPressed = false;
    bool fullscreenButtonHover = false;
    bool fullscreenButtonPressed = false;
    bool isFullscreen = false;
    // Hover-delay tooltip (see ComputeTooltipInfo/UpdateTooltipTracking):
    // tooltipTargetId identifies the hovered button (0 == none) so tracking
    // can tell "still the same button" from "moved to a new one" without
    // re-deriving text/rect every mouse move.
    int tooltipTargetId = 0;
    bool tooltipVisible = false;
    RECT tooltipAnchorRect{};
    std::wstring tooltipText;
    bool tooltipAnchorBelow = true;
    WINDOWPLACEMENT savedWindowPlacement{ sizeof(WINDOWPLACEMENT) };
    bool rendererReady = false;
    bool closing = false;
    // Opt-in via --d3d12 (see wWinMain's argument scan) -- mutually
    // exclusive with `renderer` below; see D3D12ViewerPath.h for scope.
    bool useD3D12 = false;
    D3D12ViewerPath d3d12Path;
    Renderer renderer;
    Camera camera;
    NavGizmo gizmo;
    Chrome chrome;
    ViewerState state = ViewerState::Empty;
    PointerMode pointerMode = PointerMode::None;
    POINT lastPointer{};
    bool flyLook = false;               // RMB capture active
    POINT flyPressPoint{};              // screen point to restore the cursor to
    bool wrapDrag = false;              // cursor wrapping (infinite drag) active
    bool selectDragged = false;
    POINT selectDownPoint{};
    double selectDownSeconds = 0.0;
    double orbitVelocityX = 0.0;
    double orbitVelocityY = 0.0;
    double lastOrbitMoveSeconds = 0.0;
    double panVelocityX = 0.0;
    double panVelocityY = 0.0;
    double lastPanMoveSeconds = 0.0;
    bool moveForward = false;
    bool moveBackward = false;
    bool moveLeft = false;
    bool moveRight = false;
    bool moveUp = false;
    bool moveDown = false;
    bool rollLeft = false;
    bool rollRight = false;
    bool arrowLeft = false;
    bool arrowRight = false;
    bool arrowUp = false;
    bool arrowDown = false;
    double lastFrameSeconds = 0.0;
    std::unordered_map<UINT32, POINT> touchPoints;
    POINT touchCenter{};
    double touchSpan = 0.0;
    std::wstring initialPath;
    std::wstring currentPath;
    std::wstring failedPath;
    std::wstring filename;
    std::wstring warning;
    std::wstring errorSummary;
    std::wstring errorDetails;
    std::shared_ptr<ModelData> loadedModel;  // CPU copy kept alive for picking
    bool meshSelected = false;
    bool gridVisible = true;
    bool axisSnapEnabled = false;
    // Display the loaded model in its native/source orientation instead of
    // this app's normalized Z-up correction (Model.h's ModelData::
    // upAxisCorrection). Persisted via Settings.h; default matches
    // ViewerSettings' default (normalized).
    bool showNativeOrientation = false;
    bool settingsPanelOpen = false;
    std::wstring speedHudText;
    double speedHudUntil = 0.0;
    std::wstring modeHudText;
    double modeHudUntil = 0.0;
    int lastToggleId = 0;
    double lastToggleSeconds = 0.0;
    std::uint64_t generation = 0;
    std::shared_ptr<std::atomic_bool> cancellation;
    std::shared_ptr<std::atomic_bool> alive = std::make_shared<std::atomic_bool>(true);
};

HBRUSH gBackgroundBrush = nullptr;
HWND gMainWindow = nullptr;

int Scale(const ViewerApp& app, int logical)
{
    return MulDiv(logical, static_cast<int>(app.dpi), 96);
}

double NowSeconds()
{
    static const double frequency = []() -> double
    {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / frequency;
}

// The bottom bar and Information panel only reserve screen space while a
// model is actually loaded and navigable (same condition as CanNavigate,
// inlined here since CanNavigate is defined later in this file).
bool HasNavigableModel(const ViewerApp& app)
{
    return app.state == ViewerState::Ready && app.renderer.HasModel();
}

// Reserved bottom-bar VIEWPORT inset (not the bar's own drawn height, see
// OverlayInfo::barBottomBarHeight/RenderScene below): space is only reserved
// while a model is loaded, matching HasNavigableModel, and collapsed to 0 in
// Fullscreen so the viewport reclaims that space and fills the whole monitor
// — the bar itself keeps drawing there as a floating toolbar overlaying that
// now-full-bleed viewport instead of vanishing along with the reserved space.
int EffectiveBottomBarHeight(const ViewerApp& app)
{
    return (HasNavigableModel(app) && !app.isFullscreen) ? app.bottomBarHeight : 0;
}

// Reserved title-bar VIEWPORT inset (see EffectiveBottomBarHeight just
// above): the real toolbarHeight normally, but 0 in Fullscreen so the
// viewport, gizmo, and hit-testing all agree the whole client area is now
// live — Fullscreen actually reclaims the space the title bar used to
// occupy, rather than just resizing the window over the taskbar while
// leaving a dead strip of chrome at the top. The title bar keeps drawing
// there regardless, as a floating toolbar over that full-bleed viewport
// (OverlayInfo::barToolbarHeight, always the real height); NC hit-testing
// (WM_NCHITTEST) still uses this Effective value so a topmost, monitor-
// filling window in that state has no HTCAPTION/system-caption-button role,
// while the plain client-area mouse handlers use the real app.toolbarHeight
// instead so the floating toolbar's own buttons stay clickable.
int EffectiveToolbarHeight(const ViewerApp& app)
{
    return app.isFullscreen ? 0 : app.toolbarHeight;
}

// Fixed logical width of the Information panel, docked to the right edge
// while open; 0 when closed or no model is loaded. Mirrors
// InfoPanel::ComputeInfoPanelLayout's own clamp so callers that only need the
// width don't have to build a full layout (and don't recurse into an
// already-narrowed viewport width).
int InfoPanelWidthPixels(const ViewerApp& app)
{
    if (!app.infoPanelVisible || !HasNavigableModel(app)) return 0;
    RECT client{};
    GetClientRect(app.window, &client);
    return static_cast<int>(std::lround(ComputeInfoPanelLayout(
        client.right, client.bottom, app.toolbarHeight, app.bottomBarHeight, app.dpiScale).Width()));
}

// Client-px rect of the Information panel itself (as opposed to
// InfoPanelWidthPixels' viewport-inset width alone) — used to route
// WM_MOUSEWHEEL to the panel's own scrolling instead of camera dolly while
// the cursor is over it. Empty (all zero) when the panel is closed.
RECT InfoPanelRect(const ViewerApp& app)
{
    if (!app.infoPanelVisible || !HasNavigableModel(app)) return RECT{};
    RECT client{};
    GetClientRect(app.window, &client);
    const InfoPanelLayout layout = ComputeInfoPanelLayout(
        client.right, client.bottom, app.toolbarHeight, app.bottomBarHeight, app.dpiScale);
    return RECT{ static_cast<int>(std::lround(layout.left)), static_cast<int>(std::lround(layout.top)),
        static_cast<int>(std::lround(layout.right)), static_cast<int>(std::lround(layout.bottom)) };
}

// The root transform currently mapping the loaded model's native/source axes
// into the app's Z-up world: identity while "show native orientation" is on
// (or there's no model), else ModelData::upAxisCorrection. Camera, grid, and
// the Information panel must always reason about the model in this
// transformed (effective) space — see EffectiveBounds below — while picking
// (ClickSelect) must invert it to reach PickMesh's native-space vertices.
DirectX::XMMATRIX ActiveModelTransform(const ViewerApp& app)
{
    if (!app.loadedModel || app.showNativeOrientation) return DirectX::XMMatrixIdentity();
    return DirectX::XMLoadFloat4x4(&app.loadedModel->upAxisCorrection);
}

// The bounds actually occupying the app's Z-up world right now. Every
// consumer that used to read ModelData::boundsMin/boundsMax directly for
// display/camera purposes must use this instead, since those raw fields stay
// in the model's native/source space regardless of the toggle.
void EffectiveBounds(const ViewerApp& app, DirectX::XMFLOAT3& outMin, DirectX::XMFLOAT3& outMax)
{
    if (!app.loadedModel)
    {
        outMin = DirectX::XMFLOAT3{};
        outMax = DirectX::XMFLOAT3{};
        return;
    }
    TransformBounds(app.loadedModel->boundsMin, app.loadedModel->boundsMax,
        ActiveModelTransform(app), outMin, outMax);
}

// How far app.infoPanelScrollOffset may go before the section list's last row
// reaches the panel's bottom edge — WM_MOUSEWHEEL clamps against this on
// every tick (Renderer::DrawInfoPanel clamps again from the same
// ComputeInfoPanelScrollMetrics when it draws, so the two can't disagree).
float InfoPanelMaxScroll(const ViewerApp& app)
{
    if (!app.infoPanelVisible || !app.loadedModel) return 0.0f;
    const RECT panel = InfoPanelRect(app);
    const float panelHeight = static_cast<float>(panel.bottom - panel.top);
    if (panelHeight <= 0.0f) return 0.0f;
    DirectX::XMFLOAT3 effectiveMin{};
    DirectX::XMFLOAT3 effectiveMax{};
    EffectiveBounds(app, effectiveMin, effectiveMax);
    const std::vector<InfoPanelSection> sections = BuildInfoPanelSections(
        app.loadedModel->stats, app.loadedModel->triangleCount, app.loadedModel->vertices.size(),
        effectiveMin, effectiveMax);
    const InfoPanelScrollMetrics metrics = ComputeInfoPanelScrollMetrics(sections, app.dpiScale);
    const float visibleHeight = std::max(0.0f, panelHeight - metrics.headerHeight);
    return std::max(0.0f, metrics.contentHeight - visibleHeight);
}

float ViewportAspect(const ViewerApp& app)
{
    RECT client{};
    GetClientRect(app.window, &client);
    return static_cast<float>(std::max(1L, client.right - InfoPanelWidthPixels(app))) /
        static_cast<float>(std::max(1L, client.bottom - EffectiveToolbarHeight(app) - EffectiveBottomBarHeight(app)));
}

// The 3D viewport is the client area below the title bar, above the bottom
// bar, and left of the Information panel (when open) — full-window in
// Fullscreen, since both bars collapse to 0 there.
RECT ViewportRect(const ViewerApp& app)
{
    RECT viewport{};
    GetClientRect(app.window, &viewport);
    viewport.top = EffectiveToolbarHeight(app);
    viewport.bottom -= EffectiveBottomBarHeight(app);
    viewport.right -= InfoPanelWidthPixels(app);
    return viewport;
}

// Gate for pointer messages: true only inside the actual 3D viewport, so
// clicks landing in the bottom bar or the Information panel never reach
// camera navigation, gizmo hit-testing, or mesh picking.
bool PointInViewport(const ViewerApp& app, POINT point)
{
    const RECT viewport = ViewportRect(app);
    return PtInRect(&viewport, point) != FALSE;
}

std::wstring FileNameFromPath(const std::wstring& path)
{
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool HasGlbExtension(const std::wstring& path)
{
    const std::size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring extension = path.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t value)
        { return static_cast<wchar_t>(std::towlower(value)); });
    return extension == L".glb";
}

std::wstring FormatMultiplier(double value)
{
    std::wostringstream text;
    text << std::fixed << std::setprecision(value >= 10.0 ? 1 : 2) << value;
    return text.str();
}

double HudAlpha(double visibleUntil, double now)
{
    const double remaining = visibleUntil - now;
    if (remaining <= 0.0) return 0.0;
    return static_cast<double>(std::clamp(remaining / kHudFadeSeconds, 0.0, 1.0));
}

void ShowSpeedHud(ViewerApp& app, const std::wstring& text)
{
    app.speedHudText = text;
    app.speedHudUntil = NowSeconds() + kHudVisibleSeconds;
    InvalidateRect(app.window, nullptr, FALSE);
}

void ShowModeHud(ViewerApp& app, const std::wstring& text)
{
    app.modeHudText = text;
    app.modeHudUntil = NowSeconds() + kHudVisibleSeconds;
    InvalidateRect(app.window, nullptr, FALSE);
}

void UpdateTitle(const ViewerApp& app)
{
    std::wstring title = kApplicationName;
    if (!app.filename.empty()) title = app.filename + L" — " + kApplicationName;
    SetWindowTextW(app.window, title.c_str());
}

bool GetClientPointerPoint(HWND window, UINT32 pointerId, POINT& point)
{
    POINTER_INFO info{};
    if (!GetPointerInfo(pointerId, &info)) return false;
    point = info.ptPixelLocation;
    return ScreenToClient(window, &point) != FALSE;
}

void ResetTouchBaseline(ViewerApp& app)
{
    if (app.touchPoints.empty())
    {
        app.touchCenter = POINT{};
        app.touchSpan = 0.0;
        return;
    }
    auto first = app.touchPoints.begin();
    if (app.touchPoints.size() == 1)
    {
        app.touchCenter = first->second;
        app.touchSpan = 0.0;
        return;
    }
    auto second = std::next(first);
    app.touchCenter = POINT{ (first->second.x + second->second.x) / 2, (first->second.y + second->second.y) / 2 };
    const double x = static_cast<double>(first->second.x - second->second.x);
    const double y = static_cast<double>(first->second.y - second->second.y);
    app.touchSpan = std::sqrt(x * x + y * y);
}

bool CanNavigate(const ViewerApp& app)
{
    if (app.state != ViewerState::Ready) return false;
    // The --d3d12 path uploads into d3d12Path, never into app.renderer (it's
    // never even Initialize()'d in that mode) -- see D3D12ViewerPath.h.
    return app.useD3D12 ? app.d3d12Path.hasModel : app.renderer.HasModel();
}

bool HasNavigationInput(const ViewerApp& app)
{
    // Flight keys only count as motion while RMB capture is active.
    if (app.flyLook && (app.moveForward || app.moveBackward || app.moveLeft || app.moveRight ||
        app.moveUp || app.moveDown || app.rollLeft || app.rollRight)) return true;
    return app.arrowLeft || app.arrowRight || app.arrowUp || app.arrowDown;
}

void StopNavigation(ViewerApp& app)
{
    app.moveForward = false;
    app.moveBackward = false;
    app.moveLeft = false;
    app.moveRight = false;
    app.moveUp = false;
    app.moveDown = false;
    app.rollLeft = false;
    app.rollRight = false;
    app.arrowLeft = false;
    app.arrowRight = false;
    app.arrowUp = false;
    app.arrowDown = false;
    app.camera.StopMotion();
}

bool SetNavigationKey(ViewerApp& app, WPARAM key, bool pressed)
{
    bool* state = nullptr;
    switch (key)
    {
    case 'W': state = &app.moveForward; break;
    case 'S': state = &app.moveBackward; break;
    case 'A': state = &app.moveLeft; break;
    case 'D': state = &app.moveRight; break;
    case 'E': state = &app.moveUp; break;
    case 'Q': state = &app.moveDown; break;
    case 'Z': state = &app.rollLeft; break;
    case 'C': state = &app.rollRight; break;
    case VK_LEFT: state = &app.arrowLeft; break;
    case VK_RIGHT: state = &app.arrowRight; break;
    case VK_UP: state = &app.arrowUp; break;
    case VK_DOWN: state = &app.arrowDown; break;
    default: return false;
    }
    *state = pressed;
    return true;
}

void UpdateGizmoLayout(ViewerApp& app)
{
    RECT client{};
    GetClientRect(app.window, &client);
    app.gizmo.UpdateLayout(client.right - InfoPanelWidthPixels(app), client.bottom,
        EffectiveToolbarHeight(app), EffectiveBottomBarHeight(app), app.dpiScale);
}

void UpdateChromeLayout(ViewerApp& app)
{
    RECT client{};
    GetClientRect(app.window, &client);
    app.chrome.UpdateLayout(client.right, app.toolbarHeight, app.dpiScale,
        HasNavigableModel(app), IsZoomed(app.window) != FALSE, app.isFullscreen);
}

// Speed flyout panel/track geometry, shared by drawing (RenderScene) and
// input handling (WM_LBUTTONDOWN/MOUSEMOVE) so they can never disagree.
RECT SpeedFlyoutRect(const ViewerApp& app)
{
    const RECT speedButton = app.chrome.Button(Chrome::Part::Speed).rect;
    const int width = Scale(app, 220);
    const int height = Scale(app, 64);
    const int gap = Scale(app, 6);
    RECT client{};
    GetClientRect(app.window, &client);
    int left = std::min(speedButton.left, client.right - Scale(app, 8) - width);
    left = std::max(left, Scale(app, 8));
    const int top = app.toolbarHeight + gap;
    return RECT{ left, top, left + width, top + height };
}

RECT SpeedFlyoutTrackRect(const ViewerApp& app)
{
    const RECT panel = SpeedFlyoutRect(app);
    const int marginX = Scale(app, 16);
    const int trackCenterY = panel.top + Scale(app, 44);
    const int halfHeight = Scale(app, 8);
    return RECT{ panel.left + marginX, trackCenterY - halfHeight, panel.right - marginX, trackCenterY + halfHeight };
}

// Settings popup panel/row/switch geometry, shared by drawing (RenderScene)
// and input handling (WM_LBUTTONDOWN) so they can never disagree. Mirrors
// SpeedFlyoutRect/SpeedFlyoutTrackRect just above, anchored under the
// Overflow ("...") button instead of the Speed button.
RECT SettingsPanelRect(const ViewerApp& app)
{
    const RECT overflowButton = app.chrome.Button(Chrome::Part::Overflow).rect;
    const int width = Scale(app, 280);
    const int height = Scale(app, 64);
    const int gap = Scale(app, 6);
    RECT client{};
    GetClientRect(app.window, &client);
    int left = std::min(overflowButton.left, client.right - Scale(app, 8) - width);
    left = std::max(left, Scale(app, 8));
    const int top = app.toolbarHeight + gap;
    return RECT{ left, top, left + width, top + height };
}

RECT SettingsToggleRowRect(const ViewerApp& app)
{
    const RECT panel = SettingsPanelRect(app);
    const int marginX = Scale(app, 16);
    return RECT{ panel.left + marginX, panel.top, panel.right - marginX, panel.bottom };
}

RECT SettingsSwitchRect(const ViewerApp& app)
{
    const RECT row = SettingsToggleRowRect(app);
    const int switchWidth = Scale(app, 36);
    const int switchHeight = Scale(app, 20);
    const int centerY = (row.top + row.bottom) / 2;
    return RECT{ row.right - switchWidth, centerY - switchHeight / 2, row.right, centerY + switchHeight / 2 };
}

void BeginWrappedDrag(ViewerApp& app, const POINT& point)
{
    app.wrapDrag = true;
    app.lastPointer = point;
    // Keep the cursor inside the viewport during the drag so wrapping can
    // always teleport before a display edge traps the gesture.
    RECT clip = ViewportRect(app);
    MapWindowPoints(app.window, nullptr, reinterpret_cast<LPPOINT>(&clip), 2);
    ClipCursor(&clip);
}

void WrapCursorIfNeeded(ViewerApp& app)
{
    if (!app.wrapDrag) return;
    const RECT viewport = ViewportRect(app);
    const LONG margin = std::max<LONG>(2, Scale(app, 10));
    const LONG width = viewport.right - viewport.left;
    const LONG height = viewport.bottom - viewport.top;
    POINT point = app.lastPointer;
    LONG shiftX = 0;
    LONG shiftY = 0;
    if (width > margin * 2 + 2)
    {
        if (point.x <= viewport.left + margin) shiftX = width - margin * 2;
        else if (point.x >= viewport.right - margin) shiftX = -(width - margin * 2);
    }
    if (height > margin * 2 + 2)
    {
        if (point.y <= viewport.top + margin) shiftY = height - margin * 2;
        else if (point.y >= viewport.bottom - margin) shiftY = -(height - margin * 2);
    }
    if (shiftX == 0 && shiftY == 0) return;
    point.x += shiftX;
    point.y += shiftY;
    POINT screen = point;
    ClientToScreen(app.window, &screen);
    SetCursorPos(screen.x, screen.y);
    // The teleport posts one synthetic WM_MOUSEMOVE that lands exactly on
    // this point, so seeding lastPointer here keeps its delta at zero and the
    // drag continues seamlessly from the far side.
    app.lastPointer = point;
}

void EndPointer(ViewerApp& app)
{
    if (app.wrapDrag)
    {
        app.wrapDrag = false;
        ClipCursor(nullptr);
    }
    if (app.flyLook)
    {
        app.flyLook = false;
        SetCursorPos(app.flyPressPoint.x, app.flyPressPoint.y);
    }
    if (GetCapture() == app.window) ReleaseCapture();
    app.pointerMode = PointerMode::None;
}

void TrackOrbitVelocity(ViewerApp& app, float deltaX, float deltaY)
{
    const double now = NowSeconds();
    const double gap = now - app.lastOrbitMoveSeconds;
    if (gap > 0.0005 && gap < 0.15)
    {
        app.orbitVelocityX = app.orbitVelocityX * 0.62 + (deltaX / gap) * 0.38;
        app.orbitVelocityY = app.orbitVelocityY * 0.62 + (deltaY / gap) * 0.38;
    }
    else
    {
        app.orbitVelocityX = 0.0;
        app.orbitVelocityY = 0.0;
    }
    app.lastOrbitMoveSeconds = now;
}

void TrackPanVelocity(ViewerApp& app, float deltaX, float deltaY)
{
    const double now = NowSeconds();
    const double gap = now - app.lastPanMoveSeconds;
    if (gap > 0.0005 && gap < 0.15)
    {
        app.panVelocityX = app.panVelocityX * 0.62 + (deltaX / gap) * 0.38;
        app.panVelocityY = app.panVelocityY * 0.62 + (deltaY / gap) * 0.38;
    }
    else
    {
        app.panVelocityX = 0.0;
        app.panVelocityY = 0.0;
    }
    app.lastPanMoveSeconds = now;
}

void BuildPickRay(const Camera& camera, float pointerX, float pointerY,
    float viewportWidth, float viewportHeight, DirectX::XMVECTOR& origin, DirectX::XMVECTOR& direction)
{
    using namespace DirectX;
    const float width = std::max(1.0f, viewportWidth);
    const float height = std::max(1.0f, viewportHeight);
    const float ndcX = 2.0f * (pointerX / width) - 1.0f;
    const float ndcY = 1.0f - 2.0f * (pointerY / height);
    const XMVECTOR orientation = camera.Orientation();
    const float tanHalf = std::tan(kVerticalFieldOfView * 0.5f);
    if (camera.Projection() == ProjectionMode::Orthographic)
    {
        const float halfHeight = static_cast<float>(camera.distance) * tanHalf;
        const float halfWidth = halfHeight * (width / height);
        origin = camera.EyePosition() + XMVector3Rotate(
            XMVectorSet(ndcX * halfWidth, ndcY * halfHeight, 0.0f, 0.0f), orientation);
        direction = XMVector3Rotate(XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f), orientation);
    }
    else
    {
        origin = camera.EyePosition();
        direction = XMVector3Rotate(XMVector3Normalize(
            XMVectorSet(ndcX * tanHalf * (width / height), ndcY * tanHalf, -1.0f, 0.0f)), orientation);
    }
}

void ClickSelect(ViewerApp& app, const POINT& point)
{
    if (!app.loadedModel) return;
    const RECT viewport = ViewportRect(app);
    DirectX::XMVECTOR origin{};
    DirectX::XMVECTOR direction{};
    BuildPickRay(app.camera, static_cast<float>(point.x), static_cast<float>(point.y),
        static_cast<float>(viewport.right - viewport.left),
        static_cast<float>(viewport.bottom - viewport.top), origin, direction);
    // PickMesh scans vertices in the model's native/source space, which
    // differs from the ray's app-world (Z-up) space whenever an up-axis
    // correction is active — undo it on the ray rather than the mesh.
    const DirectX::XMMATRIX inverseModel = DirectX::XMMatrixInverse(nullptr, ActiveModelTransform(app));
    const DirectX::XMVECTOR localOrigin = DirectX::XMVector3TransformCoord(origin, inverseModel);
    const DirectX::XMVECTOR localDirection = DirectX::XMVector3TransformNormal(direction, inverseModel);
    DirectX::XMFLOAT3 originValue{};
    DirectX::XMFLOAT3 directionValue{};
    DirectX::XMStoreFloat3(&originValue, localOrigin);
    DirectX::XMStoreFloat3(&directionValue, localDirection);
    float hitDistance = 0.0f;
    const bool hit = PickMesh(*app.loadedModel, originValue, directionValue, hitDistance);
    if (hit && !app.meshSelected)
    {
        app.meshSelected = true;
    }
    else if (!hit && app.meshSelected)
    {
        app.meshSelected = false;
    }
    InvalidateRect(app.window, nullptr, FALSE);
}

void FrameSelectedOrAll(ViewerApp& app)
{
    if (!app.renderer.HasModel()) return;
    const float aspect = ViewportAspect(app);
    if (app.meshSelected && app.loadedModel)
    {
        DirectX::XMFLOAT3 effectiveMin{};
        DirectX::XMFLOAT3 effectiveMax{};
        EffectiveBounds(app, effectiveMin, effectiveMax);
        app.camera.FrameBox(effectiveMin, effectiveMax, aspect);
    }
    else
    {
        app.camera.Fit(aspect);
    }
    InvalidateRect(app.window, nullptr, FALSE);
}

// Speed flyout slider range (logical 0..kSpeedSliderMax "pixels" along its
// D2D-drawn track — see the Speed flyout drawing/drag code below). Not a
// native trackbar: the flyout always reads app.camera.FlySpeedScale() live
// each frame, so unlike the old always-visible toolbar slider there is no
// separate position to keep synced.
constexpr int kSpeedSliderMax = 100;
constexpr double kSpeedSliderMin = 0.05;   // matches Camera::kFlySpeedMin
constexpr double kSpeedSliderTop = 40.0;   // matches Camera::kFlySpeedMax

int SpeedSliderPositionFor(double speedScale)
{
    const double t = std::log(std::clamp(speedScale, kSpeedSliderMin, kSpeedSliderTop) / kSpeedSliderMin) /
        std::log(kSpeedSliderTop / kSpeedSliderMin);
    return static_cast<int>(std::lround(t * kSpeedSliderMax));
}

double SpeedForSliderPosition(int position)
{
    const double t = std::clamp(static_cast<double>(position), 0.0, static_cast<double>(kSpeedSliderMax)) / kSpeedSliderMax;
    return kSpeedSliderMin * std::pow(kSpeedSliderTop / kSpeedSliderMin, t);
}

void AdjustFlySpeed(ViewerApp& app, float wheelSteps)
{
    app.camera.SetFlySpeedScale(app.camera.FlySpeedScale() * std::pow(1.18, static_cast<double>(wheelSteps)));
    ShowSpeedHud(app, L"Travel speed ×" + FormatMultiplier(app.camera.FlySpeedScale()));
}

// Direct manipulation: sets speed immediately from a drag position within
// the Speed flyout's track (SpeedFlyoutTrackRect), same non-eased feel as
// the zoom slider's own direct-drag handling.
void SetFlySpeedFromFlyoutX(ViewerApp& app, int clientX)
{
    const RECT track = SpeedFlyoutTrackRect(app);
    const float t = std::clamp(static_cast<float>(clientX - track.left) / static_cast<float>(std::max(1L, track.right - track.left)), 0.0f, 1.0f);
    app.camera.SetFlySpeedScale(SpeedForSliderPosition(static_cast<int>(std::lround(t * kSpeedSliderMax))));
}

constexpr int kZoomSliderMax = 1000;

// Zoom slider position increases as the camera moves closer (more zoomed
// in). Distance range mirrors Camera::Dolly's own clamp (Renderer.cpp) so
// the slider can always reach the same extremes wheel-zoom can, and is
// recomputed from the live scene radius rather than cached, since it
// changes with every loaded model.
int ZoomSliderPositionFor(const Camera& camera)
{
    const double minDistance = std::max(1e-6, camera.sceneRadius * 0.025);
    const double maxDistance = std::max(minDistance * 1.0001, camera.sceneRadius * 250.0);
    const double distance = std::clamp(camera.distance, minDistance, maxDistance);
    const double t = std::log(maxDistance / distance) / std::log(maxDistance / minDistance);
    return static_cast<int>(std::lround(std::clamp(t, 0.0, 1.0) * kZoomSliderMax));
}

double ZoomDistanceForSliderPosition(const Camera& camera, int position)
{
    const double minDistance = std::max(1e-6, camera.sceneRadius * 0.025);
    const double maxDistance = std::max(minDistance * 1.0001, camera.sceneRadius * 250.0);
    const double t = std::clamp(static_cast<double>(position), 0.0, static_cast<double>(kZoomSliderMax)) / kZoomSliderMax;
    return maxDistance * std::pow(minDistance / maxDistance, t);
}

float ZoomPercentFor(const Camera& camera)
{
    return static_cast<float>(100.0 * camera.homeDistance / std::max(1e-6, camera.distance));
}

// Logical width of the small icon buttons docked in the bottom bar (Info,
// Fullscreen) — see InfoButtonRect/FullscreenButtonRect below.
constexpr int kBottomBarButtonWidth = 34;
constexpr int kBottomBarButtonHeight = 28;

// Bottom-bar button rect centered vertically in the bar, right edge at
// `right` — shared layout math for the Fullscreen toggle (below). Always
// measured from the full client width, not the (possibly narrower) viewport
// left of the Information panel — the bottom bar spans the whole window and
// sits below where that panel stops (InfoPanel::ComputeInfoPanelLayout), so
// anchoring here to the client edge instead means opening/closing the panel
// never shifts these buttons.
RECT BottomBarButtonRect(const ViewerApp& app, int right)
{
    RECT client{};
    GetClientRect(app.window, &client);
    const int width = Scale(app, kBottomBarButtonWidth);
    const int height = Scale(app, kBottomBarButtonHeight);
    const int barY = client.bottom - app.bottomBarHeight;
    const int centerY = barY + app.bottomBarHeight / 2;
    return RECT{ right - width, centerY - height / 2, right, centerY + height / 2 };
}

// Fullscreen toggle: the very right of the bottom bar, after the zoom
// slider and its percent-text readout.
RECT FullscreenButtonRect(const ViewerApp& app)
{
    RECT client{};
    GetClientRect(app.window, &client);
    const int margin = Scale(app, 14);
    return BottomBarButtonRect(app, client.right - margin);
}

// Compact zoom slider, docked in the bottom-right of the bottom bar just
// left of the D2D-drawn percent readout (DrawBottomBar's labelWidth,
// Renderer.cpp — mirrored here so the two rects never drift apart), which
// in turn sits left of the Fullscreen button. Shared by drawing
// (RenderScene) and input handling (WM_LBUTTONDOWN/MOUSEMOVE), the same
// split as the Speed flyout track above.
RECT ZoomTrackRect(const ViewerApp& app)
{
    const RECT fullscreenButton = FullscreenButtonRect(app);
    const int percentLabelWidth = Scale(app, 56);
    const int trackWidth = Scale(app, 110);
    const int gap = Scale(app, 14);
    const int right = fullscreenButton.left - gap - percentLabelWidth - gap;
    const int left = right - trackWidth;
    RECT client{};
    GetClientRect(app.window, &client);
    const int barY = client.bottom - app.bottomBarHeight;
    const int centerY = barY + app.bottomBarHeight / 2;
    const int halfHeight = Scale(app, 8);
    return RECT{ left, centerY - halfHeight, right, centerY + halfHeight };
}

// Info panel toggle: docked at the very left of the bottom bar, independent
// of the zoom/fullscreen group anchored to the right — so it never moves
// when the Information panel it opens and closes changes the viewport width,
// and the zoom/fullscreen group never moves when it's clicked.
RECT InfoButtonRect(const ViewerApp& app)
{
    RECT client{};
    GetClientRect(app.window, &client);
    const int margin = Scale(app, 14);
    const int width = Scale(app, kBottomBarButtonWidth);
    const int height = Scale(app, kBottomBarButtonHeight);
    const int barY = client.bottom - app.bottomBarHeight;
    const int centerY = barY + app.bottomBarHeight / 2;
    return RECT{ margin, centerY - height / 2, margin + width, centerY + height / 2 };
}

// What ComputeTooltipInfo resolves the current hover into: an id (0 == none,
// otherwise unique per button so UpdateTooltipTracking can tell "still this
// button" from "moved to a new one"), the button's rect (so the bubble can
// anchor to it even after the mouse moves on), its label, and which side of
// the button the bubble belongs on.
struct TooltipInfo
{
    int id = 0;
    RECT rect{};
    const wchar_t* text = L"";
    bool below = true;   // true: title-bar buttons; false: bottom-bar buttons
};

TooltipInfo ComputeTooltipInfo(const ViewerApp& app)
{
    // Suppressed mid-interaction (dragging a slider, a button already
    // pressed, the Speed flyout open) rather than just delayed, so a tooltip
    // never appears over something the user is actively using.
    if (app.chrome.pressed != Chrome::Part::None || app.infoButtonPressed || app.fullscreenButtonPressed ||
        app.speedSliderDragging || app.zoomSliderDragging || app.speedFlyoutOpen || app.settingsPanelOpen)
    {
        return {};
    }
    struct Entry { Chrome::Part part; const wchar_t* text; };
    static constexpr Entry kEntries[] = {
        { Chrome::Part::Grid, L"Toggle ground grid" },
        { Chrome::Part::AxisSnap, L"Snap truck to axis" },
        { Chrome::Part::Speed, L"Flight speed" },
        { Chrome::Part::Fit, L"Frame model in view" },
        { Chrome::Part::Reset, L"Reset view" },
        { Chrome::Part::Share, L"Share" },
        { Chrome::Part::Overflow, L"More options" },
        { Chrome::Part::OpenWith, L"Open with" },
    };
    for (const Entry& entry : kEntries)
    {
        if (app.chrome.hover == entry.part)
        {
            return { static_cast<int>(entry.part) + 1, app.chrome.Button(entry.part).rect, entry.text, true };
        }
    }
    if (app.infoButtonHover) return { 100, InfoButtonRect(app), L"Model information", false };
    if (app.fullscreenButtonHover) return { 101, FullscreenButtonRect(app), L"Fullscreen", false };
    return {};
}

// Re-evaluates the hovered button and (re)starts/cancels the hover-delay
// timer whenever it changes. Called from every place chrome.hover,
// chrome.pressed, infoButtonHover/Pressed, or fullscreenButtonHover/Pressed
// can change, so the tooltip always tracks the true current hover target
// without needing its own dedicated mouse-tracking.
void UpdateTooltipTracking(ViewerApp& app)
{
    const TooltipInfo info = ComputeTooltipInfo(app);
    if (info.id == app.tooltipTargetId) return;
    const bool wasVisible = app.tooltipVisible;
    app.tooltipTargetId = info.id;
    app.tooltipAnchorRect = info.rect;
    app.tooltipText = info.text;
    app.tooltipAnchorBelow = info.below;
    app.tooltipVisible = false;
    KillTimer(app.window, kTooltipTimerId);
    if (info.id != 0) SetTimer(app.window, kTooltipTimerId, kTooltipDelayMs, nullptr);
    if (wasVisible) InvalidateRect(app.window, nullptr, FALSE);
}

// Direct manipulation: sets distance immediately (no easing), so the view
// tracks the thumb 1:1 while dragging, unlike wheel zoom which eases toward
// targetDistance.
void SetZoomFromTrackX(ViewerApp& app, int clientX)
{
    const RECT track = ZoomTrackRect(app);
    const float t = std::clamp(static_cast<float>(clientX - track.left) / static_cast<float>(std::max(1L, track.right - track.left)), 0.0f, 1.0f);
    const double distance = ZoomDistanceForSliderPosition(app.camera, static_cast<int>(std::lround(t * kZoomSliderMax)));
    app.camera.distance = distance;
    app.camera.targetDistance = distance;
}

// Guards toggle commands against keyboard auto-repeat: holding a key must not
// strobe the grid or the projection mode.
bool ConsumeToggleCommand(ViewerApp& app, int id)
{
    const double now = NowSeconds();
    if (app.lastToggleId == id && now - app.lastToggleSeconds < kToggleRepeatGuardSeconds) return false;
    app.lastToggleId = id;
    app.lastToggleSeconds = now;
    return true;
}

void ToggleGrid(ViewerApp& app)
{
    if (!CanNavigate(app) || !ConsumeToggleCommand(app, ID_VIEW_GRID)) return;
    app.gridVisible = !app.gridVisible;
    ShowModeHud(app, app.gridVisible ? L"Ground grid shown" : L"Ground grid hidden");
    InvalidateRect(app.window, nullptr, FALSE);
}

void ToggleProjection(ViewerApp& app)
{
    if (!CanNavigate(app) || !ConsumeToggleCommand(app, ID_VIEW_PROJECTION)) return;
    const bool toOrthographic = app.camera.Projection() == ProjectionMode::Perspective;
    app.camera.SetProjection(toOrthographic ? ProjectionMode::Orthographic : ProjectionMode::Perspective);
    ShowModeHud(app, toOrthographic ? L"Orthographic" : L"Perspective");
    InvalidateRect(app.window, nullptr, FALSE);
}

void ToggleAxisSnap(ViewerApp& app)
{
    if (!CanNavigate(app) || !ConsumeToggleCommand(app, ID_VIEW_AXIS_SNAP)) return;
    app.axisSnapEnabled = !app.axisSnapEnabled;
    ShowModeHud(app, app.axisSnapEnabled ? L"Axis snap on" : L"Axis snap off");
    InvalidateRect(app.window, nullptr, FALSE);
}

// Persisted independently of whether a file is currently open (it's a
// standing preference, not a per-document action), but only re-homes the
// camera/grid when a model is actually loaded to apply against.
void ToggleShowNativeOrientation(ViewerApp& app)
{
    app.showNativeOrientation = !app.showNativeOrientation;
    if (HasNavigableModel(app))
    {
        DirectX::XMFLOAT3 effectiveMin{};
        DirectX::XMFLOAT3 effectiveMax{};
        EffectiveBounds(app, effectiveMin, effectiveMax);
        std::wstring gridError;
        app.renderer.RebuildGrid(effectiveMin, effectiveMax, gridError);
        // An instant re-home (not Fit/Reset, which animate): the model just
        // jumped ~90 degrees, so the old camera pose has no useful
        // relationship to the new one — treat this exactly like a fresh open.
        app.camera.SetBounds(effectiveMin, effectiveMax, ViewportAspect(app));
        ShowModeHud(app, app.showNativeOrientation ? L"Native orientation" : L"Normalized orientation");
    }
    InvalidateRect(app.window, nullptr, FALSE);
    ViewerSettings settings;
    settings.showNativeOrientation = app.showNativeOrientation;
    SaveSettings(settings);
}

void SnapViewCommand(ViewerApp& app, ViewDir view)
{
    if (!CanNavigate(app)) return;
    // Ctrl+Numpad1/3/7 selects the reverse views (Back, Left, Bottom).
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
    {
        if (view == ViewDir::Front) view = ViewDir::Back;
        else if (view == ViewDir::Right) view = ViewDir::Left;
        else if (view == ViewDir::Top) view = ViewDir::Bottom;
    }
    app.camera.SnapToView(CanonicalViewOrientation(view));
    InvalidateRect(app.window, nullptr, FALSE);
}

void SetControlVisible(HWND control, bool visible)
{
    if (control && ((GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0) != visible)
        ShowWindow(control, visible ? SW_SHOWNA : SW_HIDE);
}

void UpdateButtonAvailability(ViewerApp& app)
{
    SetControlVisible(app.retryButton, app.state == ViewerState::Failed);
    SetControlVisible(app.openAnotherButton, app.state == ViewerState::Failed);
    SetControlVisible(app.copyButton, app.state == ViewerState::Failed);
    EnableWindow(app.retryButton, app.failedPath.empty() ? FALSE : TRUE);
}

void LayoutControls(ViewerApp& app)
{
    if (!app.window) return;
    RECT client{};
    GetClientRect(app.window, &client);
    const RECT card = CalculateErrorCardRect(client.right, client.bottom, app.toolbarHeight, app.dpiScale);
    const int actionHeight = Scale(app, 32);
    const int actionGap = Scale(app, 8);
    const int retryWidth = Scale(app, 78);
    const int anotherWidth = Scale(app, 112);
    const int copyWidth = Scale(app, 104);
    int actionX = card.left + Scale(app, 28);
    const int actionY = card.bottom - Scale(app, 52);
    MoveWindow(app.retryButton, actionX, actionY, retryWidth, actionHeight, TRUE); actionX += retryWidth + actionGap;
    MoveWindow(app.openAnotherButton, actionX, actionY, anotherWidth, actionHeight, TRUE); actionX += anotherWidth + actionGap;
    MoveWindow(app.copyButton, actionX, actionY, copyWidth, actionHeight, TRUE);

}

// Toggling the panel changes the viewport width (InfoPanelWidthPixels), so
// it needs the same full relayout a resize would trigger.
void ToggleInfoPanel(ViewerApp& app)
{
    if (!CanNavigate(app) || !ConsumeToggleCommand(app, ID_VIEW_INFO)) return;
    app.infoPanelVisible = !app.infoPanelVisible;
    if (!app.infoPanelVisible) app.infoPanelScrollOffset = 0.0f;
    LayoutControls(app);
    UpdateGizmoLayout(app);
    InvalidateRect(app.window, nullptr, FALSE);
}

// Immersive-fullscreen toggle: expands the window to exactly cover its
// current monitor (hiding the OS resize border/drop-shadow/rounded corners,
// same recipe as most "fake fullscreen" apps), raises it above the taskbar
// (HWND_TOPMOST — matching monitor bounds alone doesn't make Explorer hide a
// plain top-level window behind it), and hides our own D2D title bar and
// bottom bar (EffectiveToolbarHeight/EffectiveBottomBarHeight, Preview3D.cpp;
// DrawOverlay, Renderer.cpp) so the viewport fills the whole screen — Maximize
// instead snaps to the work area, keeps the taskbar and our chrome visible,
// and never goes topmost, so the two stay distinct. Independent of whether a
// model is loaded, same as the caption buttons (while windowed).
void ToggleFullscreen(ViewerApp& app)
{
    if (!ConsumeToggleCommand(app, ID_VIEW_FULLSCREEN)) return;
    if (!app.isFullscreen)
    {
        app.savedWindowPlacement.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(app.window, &app.savedWindowPlacement);
        MONITORINFO monitorInfo{ sizeof(MONITORINFO) };
        if (!GetMonitorInfoW(MonitorFromWindow(app.window, MONITOR_DEFAULTTOPRIMARY), &monitorInfo)) return;
        const int noRound = 1;   // DWMWCP_DONOTROUND: avoid corner clipping artifacts at exact monitor bounds
        DwmSetWindowAttribute(app.window, 33, &noRound, sizeof(noRound));
        app.isFullscreen = true;
        SetWindowPos(app.window, HWND_TOPMOST, monitorInfo.rcMonitor.left, monitorInfo.rcMonitor.top,
            monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
            monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    else
    {
        app.isFullscreen = false;
        const int cornerPreference = 2;   // DWMWCP_ROUNDSMALL, matches WM_CREATE
        DwmSetWindowAttribute(app.window, 33, &cornerPreference, sizeof(cornerPreference));
        SetWindowPlacement(app.window, &app.savedWindowPlacement);
        // HWND_NOTOPMOST undoes the HWND_TOPMOST above — otherwise the window
        // would stay pinned above every other app (taskbar included) even
        // after returning to windowed/maximized.
        SetWindowPos(app.window, HWND_NOTOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    LayoutControls(app);
    UpdateGizmoLayout(app);
    UpdateChromeLayout(app);
    InvalidateRect(app.window, nullptr, TRUE);
}

void RecreateButtonFont(ViewerApp& app)
{
    if (app.buttonFont) DeleteObject(app.buttonFont);
    app.buttonFont = CreateFontW(-Scale(app, 12), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
    for (HWND button : { app.retryButton, app.openAnotherButton, app.copyButton })
    {
        if (button) SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(app.buttonFont), TRUE);
    }
}

HWND CreateButton(ViewerApp& app, int id, const wchar_t* text)
{
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 10, 10, app.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app.instance, nullptr);
}

void AddTooltip(ViewerApp& app, HWND control, const wchar_t* text)
{
    TOOLINFOW info{ sizeof(info) };
    info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    info.hwnd = app.window;
    info.uId = reinterpret_cast<UINT_PTR>(control);
    info.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(app.tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
}

void CreateControls(ViewerApp& app)
{
    // Fit/Reset/Grid/Snap/Speed/Info/Share/Overflow/Open-With and the system
    // min/max/close now live in the D2D-drawn title bar (Chrome +
    // Renderer::DrawTitleBar) instead of as owner-drawn child buttons — see
    // the WM_NCHITTEST/WM_LBUTTONDOWN handling in WindowProcedure. The zoom
    // slider is D2D-drawn too (ZoomTrackRect/DrawBottomBar) with its own
    // pointer handling, same split as the Speed flyout. Only the
    // error-state action buttons remain real HWND controls.
    app.retryButton = CreateButton(app, ID_VIEW_RETRY, L"Retry");
    app.openAnotherButton = CreateButton(app, ID_VIEW_OPEN_ANOTHER, L"Open another");
    app.copyButton = CreateButton(app, ID_VIEW_COPY_DETAILS, L"Copy details");
    app.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, app.window, nullptr, app.instance, nullptr);
    SetWindowPos(app.tooltip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SendMessageW(app.tooltip, TTM_SETMAXTIPWIDTH, 0, Scale(app, 360));
    AddTooltip(app, app.retryButton, L"Try opening this file again");
    AddTooltip(app, app.openAnotherButton, L"Choose a different GLB model");
    AddTooltip(app, app.copyButton, L"Copy technical error details without the file path");
    RecreateButtonFont(app);
    UpdateButtonAvailability(app);
    LayoutControls(app);
}

void SetFailure(ViewerApp& app, const std::wstring& summary, const std::wstring& details,
    const std::wstring& failedPath = {})
{
    app.state = ViewerState::Failed;
    app.errorSummary = summary;
    app.errorDetails = details;
    app.failedPath = failedPath;
    StopNavigation(app);
    EndPointer(app);
    UpdateButtonAvailability(app);
    LayoutControls(app);
    InvalidateRect(app.window, nullptr, FALSE);
}

void CancelOpen(ViewerApp& app)
{
    if (app.state != ViewerState::Loading) return;
    if (app.cancellation) app.cancellation->store(true, std::memory_order_relaxed);
    ++app.generation;
    app.cancellation.reset();
    app.state = app.renderer.HasModel() ? ViewerState::Ready : ViewerState::Empty;
    if (app.renderer.HasModel()) app.filename = FileNameFromPath(app.currentPath);
    else app.filename.clear();
    UpdateTitle(app);
    UpdateButtonAvailability(app);
    LayoutControls(app);
    InvalidateRect(app.window, nullptr, FALSE);
}

void BeginOpen(ViewerApp& app, const std::wstring& path)
{
    if (path.empty()) return;
    if (app.cancellation)
    {
        app.cancellation->store(true, std::memory_order_relaxed);
        app.cancellation.reset();
        ++app.generation;
    }
    if (path.rfind(L"\\\\", 0) == 0)
    {
        app.filename = FileNameFromPath(path);
        UpdateTitle(app);
        SetFailure(app, L"Remote model paths are not opened.",
            L"Choose a GLB stored on a local drive for this preview slice.", path);
        return;
    }
    std::optional<d3d12_import_bridge::SourceFormat> d3d12Format;
    if (app.useD3D12)
    {
        d3d12Format = d3d12_import_bridge::ClassifyByExtension(path);
        if (!d3d12Format)
        {
            app.filename = FileNameFromPath(path);
            UpdateTitle(app);
            SetFailure(app, L"This format is not included in the current slice.",
                L"Open a .glb, .stl, or .ply file. Other model formats are deliberately deferred.", path);
            return;
        }
    }
    else if (!HasGlbExtension(path))
    {
        app.filename = FileNameFromPath(path);
        UpdateTitle(app);
        SetFailure(app, L"This format is not included in the current slice.",
            L"Open a self-contained .glb file. Other model formats are deliberately deferred.", path);
        return;
    }

    const auto alive = app.alive;
    const std::uint64_t generation = ++app.generation;
    const HWND window = app.window;

    app.state = ViewerState::Loading;
    StopNavigation(app);
    EndPointer(app);
    app.failedPath.clear();
    app.errorSummary.clear();
    app.errorDetails.clear();
    app.filename = FileNameFromPath(path);
    app.warning.clear();
    UpdateTitle(app);
    UpdateButtonAvailability(app);
    LayoutControls(app);
    InvalidateRect(app.window, nullptr, FALSE);

    if (app.useD3D12)
    {
        // No cancellation token for this path yet -- RunImport runs to
        // completion (or the sandboxed worker's own bounded timeouts), see
        // the plan's "Explicitly deferred" list. The try/catch is not
        // optional defensiveness: platform::AppContainerSid::CreateOrOpen
        // throws on failure, and an exception escaping a std::thread entry
        // function calls std::terminate -- so without this, a profile-
        // creation failure killed the whole app instead of showing an error.
        // Supersede any still-running import before starting this one, then
        // hand the fresh token to the worker thread. WM_DESTROY trips
        // whichever token is current, so closing the window abandons an
        // in-flight import instead of leaving it (and its sandboxed worker)
        // running until process exit.
        if (app.cancellation) app.cancellation->store(true, std::memory_order_relaxed);
        app.cancellation = std::make_shared<std::atomic_bool>(false);
        const auto cancellation = app.cancellation;

        d3d12_import_bridge::SourceFormat format = *d3d12Format;
        std::thread([window, generation, path, format, alive, cancellation]()
        {
            d3d12_import_bridge::ImportResult result;
            try
            {
                result = d3d12_import_bridge::RunImport(format, path, generation, [cancellation]
                {
                    return cancellation->load(std::memory_order_relaxed);
                });
            }
            catch (const std::bad_alloc&)
            {
                result.errorSummary = L"There is not enough memory to open this model.";
                result.errorDetails = L"Importing this model exceeded the available memory budget.";
            }
            catch (...)
            {
                result.errorSummary = L"This model could not be previewed.";
                result.errorDetails = L"The importer stopped unexpectedly while reading the model.";
            }
            if (!alive->load(std::memory_order_relaxed)) return;
            auto* message = new (std::nothrow) D3D12CompleteMessage{ generation, path, std::move(result) };
            if (message && !PostMessageW(window, kD3D12ImportCompleteMessage, 0, reinterpret_cast<LPARAM>(message)))
                delete message;
        }).detach();
        return;
    }

    app.cancellation = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = app.cancellation;

    std::thread([window, generation, path, cancellation, alive]()
    {
        LoadResult result;
        try
        {
            result = LoadGlb(path, cancellation, [](const wchar_t*) {});
        }
        catch (const std::bad_alloc&)
        {
            result.summary = L"There is not enough memory to open this model.";
            result.details = L"GLB parsing exceeded the available memory budget.";
        }
        catch (...)
        {
            result.summary = L"This GLB could not be previewed.";
            result.details = L"The importer stopped unexpectedly while reading the model.";
        }
        if (!alive->load(std::memory_order_relaxed)) return;
        auto* message = new (std::nothrow) CompleteMessage{ generation, path, std::move(result) };
        if (message && !PostMessageW(window, kLoadCompleteMessage, 0, reinterpret_cast<LPARAM>(message))) delete message;
    }).detach();
}

void OpenDialog(ViewerApp& app)
{
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    {
        SetFailure(app, L"The Open dialog is unavailable.", L"Windows could not create the system file picker.");
        return;
    }
    const COMDLG_FILTERSPEC filters[] = {
        { L"GLB 3D models (*.glb)", L"*.glb" },
        { L"All files (*.*)", L"*.*" }
    };
    dialog->SetFileTypes(ARRAYSIZE(filters), filters);
    dialog->SetDefaultExtension(L"glb");
    dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    const HRESULT shown = dialog->Show(app.window);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
    if (FAILED(shown))
    {
        SetFailure(app, L"The Open dialog stopped unexpectedly.", L"Try dropping a local .glb file into the window.");
        return;
    }
    ComPtr<IShellItem> item;
    PWSTR selectedPath = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &selectedPath)))
    {
        const std::wstring path(selectedPath);
        CoTaskMemFree(selectedPath);
        BeginOpen(app, path);
    }
}

void CopyErrorDetails(const ViewerApp& app)
{
    std::wstring text = app.errorSummary + L"\r\n\r\n" + app.errorDetails + L"\r\n\r\nFormat: GLB\r\nPhase: opening";
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return;
    void* destination = GlobalLock(memory);
    if (!destination) { GlobalFree(memory); return; }
    std::memcpy(destination, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (OpenClipboard(app.window))
    {
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        CloseClipboard();
    }
    else
    {
        GlobalFree(memory);
    }
}

void ShowMoreMenu(ViewerApp& app)
{
    HMENU menu = CreatePopupMenu();
    if (!app.warning.empty())
    {
        AppendMenuW(menu, MF_STRING, ID_VIEW_DIAGNOSTICS, L"Model warnings…");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING, ID_VIEW_CONTROLS, L"Controls\t?");
    AppendMenuW(menu, MF_STRING, ID_VIEW_SETTINGS, L"Settings…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_ABOUT, L"About 3D Preview");
    RECT button = app.chrome.Button(Chrome::Part::Overflow).rect;
    POINT anchor{ button.right, button.bottom };
    ClientToScreen(app.window, &anchor);
    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON, anchor.x, anchor.y, 0, app.window, nullptr);
    DestroyMenu(menu);
}

// Menu-invoked, not keyboard-repeatable, so no ConsumeToggleCommand guard is
// needed (unlike ToggleGrid/ToggleAxisSnap).
void ToggleSettingsPanel(ViewerApp& app)
{
    app.settingsPanelOpen = !app.settingsPanelOpen;
    InvalidateRect(app.window, nullptr, FALSE);
}

void ShowControls(HWND owner)
{
    MessageBoxW(owner,
        L"Select\tClick a mesh; click the background to clear\n"
        L"Orbit\tLeft drag, gizmo ball drag, or arrow keys\n"
        L"Truck (pan)\tMiddle drag or Shift+arrow keys\n"
        L"Axis snap\tToggle the Snap button to lock truck moves to X/Y\n"
        L"Zoom\tWheel, Ctrl+middle drag, +, or -\n"
        L"Fly\tHold right mouse + W/A/S/D, Q/E; wheel or slider sets speed\n"
        L"Fly faster\tHold Shift while flying (2x)\n"
        L"Roll\tHold right mouse + Z/C\n"
        L"Front / Right / Top\tNumpad 1 / 3 / 7 (Ctrl for reverse)\n"
        L"Perspective / Ortho\tNumpad 5\n"
        L"Gizmo views\tClick an axis ball in the corner\n"
        L"Frame model / selection\tF, Numpad ., or double-click\n"
        L"Ground grid\tG\n"
        L"Reset view\tHome or R\n"
        L"Open\tCtrl+O\n"
        L"Fullscreen\tF11\n"
        L"Cancel open\tEsc\n\n"
        L"Left-drag orbit and middle-drag truck wrap the cursor at the\n"
        L"viewport edge, and drags glide to a stop with exponential inertia.",
        L"3D Preview controls", MB_OK | MB_ICONINFORMATION);
}

void DrawOwnerButton(ViewerApp& app, const DRAWITEMSTRUCT& item)
{
    wchar_t text[64]{};
    GetWindowTextW(item.hwndItem, text, ARRAYSIZE(text));
    RECT bounds = item.rcItem;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    const bool hot = (item.itemState & ODS_HOTLIGHT) != 0;
    // Grid/Snap/Info/Fit/Reset/Open moved to the D2D title bar (Chrome +
    // Renderer::DrawTitleBar); this now only draws the error-state buttons.
    const bool primary = item.CtlID == ID_VIEW_RETRY;
    const bool active = false;
    COLORREF fill = primary ? RGB(10, 132, 255) : RGB(58, 58, 60);
    COLORREF border = primary ? RGB(34, 146, 255) : RGB(73, 73, 76);
    COLORREF foreground = RGB(245, 245, 247);
    if (active) { fill = RGB(40, 44, 52); border = RGB(10, 132, 255); }
    if (hot) fill = primary ? RGB(32, 145, 255) : RGB(68, 68, 71);
    if (active && hot) fill = RGB(48, 54, 64);
    if (pressed) fill = primary ? RGB(0, 113, 227) : RGB(48, 48, 50);
    if (disabled) { fill = RGB(44, 44, 46); border = RGB(51, 51, 53); foreground = RGB(112, 112, 117); }

    const bool errorAction = item.CtlID == ID_VIEW_RETRY || item.CtlID == ID_VIEW_OPEN_ANOTHER ||
        item.CtlID == ID_VIEW_COPY_DETAILS;
    HBRUSH surfaceBrush = CreateSolidBrush(errorAction ? RGB(36, 36, 38) : RGB(44, 44, 46));
    FillRect(item.hDC, &bounds, surfaceBrush);
    DeleteObject(surfaceBrush);

    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, focused ? RGB(100, 188, 255) : border);
    HGDIOBJ oldBrush = SelectObject(item.hDC, brush);
    HGDIOBJ oldPen = SelectObject(item.hDC, pen);
    RoundRect(item.hDC, bounds.left, bounds.top, bounds.right, bounds.bottom, Scale(app, 9), Scale(app, 9));
    SelectObject(item.hDC, oldBrush);
    SelectObject(item.hDC, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, foreground);
    HGDIOBJ oldFont = SelectObject(item.hDC, app.buttonFont);
    if (pressed) OffsetRect(&bounds, 0, 1);
    DrawTextW(item.hDC, text, -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(item.hDC, oldFont);
}

void RenderFallback(ViewerApp& app, HDC dc)
{
    RECT client{};
    GetClientRect(app.window, &client);
    FillRect(dc, &client, gBackgroundBrush);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(232, 234, 237));
    HFONT old = static_cast<HFONT>(SelectObject(dc, app.buttonFont));
    RECT text = client;
    text.left += Scale(app, 32);
    text.right -= Scale(app, 32);
    DrawTextW(dc, app.errorSummary.c_str(), -1, &text, DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
}

void HandleCommand(ViewerApp& app, int id)
{
    switch (id)
    {
    case ID_VIEW_OPEN:
    case ID_VIEW_OPEN_ANOTHER: OpenDialog(app); break;
    case ID_VIEW_FIT: FrameSelectedOrAll(app); break;
    case ID_VIEW_RESET:
        if (app.renderer.HasModel())
        {
            app.camera.Reset(ViewportAspect(app));
            InvalidateRect(app.window, nullptr, FALSE);
        }
        break;
    case ID_VIEW_GRID: ToggleGrid(app); break;
    case ID_VIEW_AXIS_SNAP: ToggleAxisSnap(app); break;
    case ID_VIEW_INFO: ToggleInfoPanel(app); break;
    case ID_VIEW_FULLSCREEN: ToggleFullscreen(app); break;
    case ID_VIEW_FRONT: SnapViewCommand(app, ViewDir::Front); break;
    case ID_VIEW_RIGHT: SnapViewCommand(app, ViewDir::Right); break;
    case ID_VIEW_TOP: SnapViewCommand(app, ViewDir::Top); break;
    case ID_VIEW_PROJECTION: ToggleProjection(app); break;
    case ID_VIEW_MENU: ShowMoreMenu(app); break;
    case ID_VIEW_RETRY: if (!app.failedPath.empty()) BeginOpen(app, app.failedPath); break;
    case ID_VIEW_COPY_DETAILS: CopyErrorDetails(app); break;
    case ID_VIEW_CANCEL: CancelOpen(app); break;
    case ID_VIEW_CONTROLS: ShowControls(app.window); break;
    case ID_VIEW_SETTINGS: ToggleSettingsPanel(app); break;
    case ID_VIEW_DIAGNOSTICS:
        MessageBoxW(app.window, app.warning.c_str(), L"Model warnings", MB_OK | MB_ICONWARNING);
        break;
    case IDM_ABOUT:
        MessageBoxW(app.window, L"A focused, native GLB inspection slice.\n\nNo cloud, no editing, no file modification.",
            L"About 3D Preview", MB_OK | MB_ICONINFORMATION);
        break;
    case IDM_EXIT: DestroyWindow(app.window); break;
    }
}

// Builds a real "Open with..." popup menu from the system's recommended
// handlers for the current file (ShellIntegration.h), plus the trailing
// "Choose another app..." fallback, anchored under the Open With button.
void ShowOpenWithMenu(ViewerApp& app)
{
    if (app.currentPath.empty()) return;
    std::vector<OpenWithEntry> entries = EnumerateOpenWithHandlers(app.currentPath);
    HMENU menu = CreatePopupMenu();
    constexpr UINT kBaseId = 40000;
    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        AppendMenuW(menu, MF_STRING, kBaseId + static_cast<UINT>(index), entries[index].displayName.c_str());
    }
    RECT button = app.chrome.Button(Chrome::Part::OpenWith).rect;
    POINT anchor{ button.left, button.bottom };
    ClientToScreen(app.window, &anchor);
    const int selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON,
        anchor.x, anchor.y, 0, app.window, nullptr);
    DestroyMenu(menu);
    if (selected >= static_cast<int>(kBaseId) && static_cast<std::size_t>(selected - kBaseId) < entries.size())
    {
        entries[selected - kBaseId].invoke(app.currentPath);
    }
}

void DoShare(ViewerApp& app)
{
    if (app.currentPath.empty()) return;
    std::wstring error;
    if (!ShowWindowsShare(app.window, app.currentPath, error))
    {
        ShowModeHud(app, L"Share isn't available right now");
    }
}

void ToggleSpeedFlyout(ViewerApp& app)
{
    if (!CanNavigate(app)) return;
    app.speedFlyoutOpen = !app.speedFlyoutOpen;
    InvalidateRect(app.window, nullptr, FALSE);
}

// Dispatches a click on one of the D2D-drawn title-bar buttons (Chrome +
// Renderer::DrawTitleBar). Reuses HandleCommand for the actions that already
// have a command ID (kept working via Ctrl+O/accelerators too); the rest
// (Speed flyout, Share, Open With) are new to the title bar.
void HandleChromeAction(ViewerApp& app, Chrome::Part part)
{
    switch (part)
    {
    case Chrome::Part::Grid: HandleCommand(app, ID_VIEW_GRID); break;
    case Chrome::Part::AxisSnap: HandleCommand(app, ID_VIEW_AXIS_SNAP); break;
    case Chrome::Part::Speed: ToggleSpeedFlyout(app); break;
    case Chrome::Part::Fit: HandleCommand(app, ID_VIEW_FIT); break;
    case Chrome::Part::Reset: HandleCommand(app, ID_VIEW_RESET); break;
    case Chrome::Part::Share: DoShare(app); break;
    case Chrome::Part::Overflow: HandleCommand(app, ID_VIEW_MENU); break;
    case Chrome::Part::OpenWith: ShowOpenWithMenu(app); break;
    default: break;
    }
}

FlightInput BuildFlightInput(const ViewerApp& app)
{
    FlightInput input;
    if (!CanNavigate(app)) return input;
    // Unreal-style flight only while RMB capture is active.
    if (app.flyLook)
    {
        input.right = (app.moveRight ? 1.0f : 0.0f) - (app.moveLeft ? 1.0f : 0.0f);
        input.up = (app.moveUp ? 1.0f : 0.0f) - (app.moveDown ? 1.0f : 0.0f);
        input.forward = (app.moveForward ? 1.0f : 0.0f) - (app.moveBackward ? 1.0f : 0.0f);
        input.roll = (app.rollRight ? 1.0f : 0.0f) - (app.rollLeft ? 1.0f : 0.0f);
        input.fast = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    }
    const float arrowsX = (app.arrowRight ? 1.0f : 0.0f) - (app.arrowLeft ? 1.0f : 0.0f);
    const float arrowsY = (app.arrowDown ? 1.0f : 0.0f) - (app.arrowUp ? 1.0f : 0.0f);
    if (input.fast)
    {
        input.panX = arrowsX * kArrowPixelsPerSecond;
        input.panY = arrowsY * kArrowPixelsPerSecond;
    }
    else
    {
        input.orbitX = arrowsX * kArrowPixelsPerSecond;
        input.orbitY = arrowsY * kArrowPixelsPerSecond;
    }
    RECT client{};
    GetClientRect(app.window, &client);
    input.viewportHeight = static_cast<float>(std::max(1L, client.bottom - EffectiveToolbarHeight(app) - EffectiveBottomBarHeight(app)));
    return input;
}

bool IsAnimating(const ViewerApp& app)
{
    const double now = NowSeconds();
    return app.state == ViewerState::Loading || HasNavigationInput(app) || app.camera.HasMotion() ||
        now < app.speedHudUntil || now < app.modeHudUntil;
}

void RenderScene(ViewerApp& app)
{
    OverlayInfo overlay;
    overlay.state = app.state;
    overlay.filename = app.filename;
    overlay.errorSummary = app.errorSummary;
    overlay.errorDetails = app.errorDetails;
    overlay.warning = app.warning;
    overlay.animationPhase = static_cast<float>(GetTickCount64() % 1400) / 1400.0f;
    overlay.dpiScale = app.dpiScale;
    overlay.toolbarHeight = EffectiveToolbarHeight(app);
    overlay.bottomBarHeight = EffectiveBottomBarHeight(app);
    // Always the bars' real height, Fullscreen included — see
    // OverlayInfo::barToolbarHeight/barBottomBarHeight (Renderer.h) for why
    // these are kept separate from the viewport-inset pair just above.
    overlay.barToolbarHeight = app.toolbarHeight;
    overlay.barBottomBarHeight = HasNavigableModel(app) ? app.bottomBarHeight : 0;
    overlay.infoPanelWidth = InfoPanelWidthPixels(app);
    if (overlay.infoPanelWidth > 0 && app.loadedModel)
    {
        DirectX::XMFLOAT3 effectiveMin{};
        DirectX::XMFLOAT3 effectiveMax{};
        EffectiveBounds(app, effectiveMin, effectiveMax);
        overlay.infoPanelSections = BuildInfoPanelSections(
            app.loadedModel->stats, app.loadedModel->triangleCount, app.loadedModel->vertices.size(),
            effectiveMin, effectiveMax);
        overlay.infoPanelScrollOffset = app.infoPanelScrollOffset;
    }
    overlay.zoomPercent = ZoomPercentFor(app.camera);
    if (overlay.barBottomBarHeight > 0)
    {
        overlay.zoomTrackRect = ZoomTrackRect(app);
        overlay.zoomSliderT = static_cast<float>(ZoomSliderPositionFor(app.camera)) / kZoomSliderMax;
        overlay.infoButtonRect = InfoButtonRect(app);
        overlay.infoButtonHover = app.infoButtonHover;
        overlay.infoButtonPressed = app.infoButtonPressed;
        overlay.fullscreenButtonRect = FullscreenButtonRect(app);
        overlay.fullscreenButtonHover = app.fullscreenButtonHover;
        overlay.fullscreenButtonPressed = app.fullscreenButtonPressed;
    }
    overlay.isFullscreen = app.isFullscreen;
    overlay.hasModel = app.renderer.HasModel();
    overlay.gridVisible = app.gridVisible;
    overlay.axisSnapEnabled = app.axisSnapEnabled;
    DirectX::XMStoreFloat4x4(&overlay.modelTransform, ActiveModelTransform(app));
    overlay.infoPanelVisible = app.infoPanelVisible;
    overlay.speedFlyoutOpen = app.speedFlyoutOpen && HasNavigableModel(app);
    if (overlay.speedFlyoutOpen)
    {
        overlay.speedFlyoutRect = SpeedFlyoutRect(app);
        overlay.speedFlyoutTrackRect = SpeedFlyoutTrackRect(app);
        overlay.speedSliderT = static_cast<float>(SpeedSliderPositionFor(app.camera.FlySpeedScale())) / kSpeedSliderMax;
        overlay.speedValueText = L"×" + FormatMultiplier(app.camera.FlySpeedScale());
    }
    overlay.settingsPanelOpen = app.settingsPanelOpen;
    overlay.showNativeOrientation = app.showNativeOrientation;
    if (overlay.settingsPanelOpen)
    {
        overlay.settingsPanelRect = SettingsPanelRect(app);
        overlay.settingsToggleRowRect = SettingsToggleRowRect(app);
        overlay.settingsSwitchRect = SettingsSwitchRect(app);
    }
    overlay.selectionAmount = app.meshSelected ? 1.0f : 0.0f;
    const double now = NowSeconds();
    overlay.speedHud = app.speedHudText;
    overlay.speedHudAlpha = static_cast<float>(HudAlpha(app.speedHudUntil, now));
    overlay.modeHud = app.modeHudText;
    overlay.modeHudAlpha = static_cast<float>(HudAlpha(app.modeHudUntil, now));
    overlay.tooltipVisible = app.tooltipVisible;
    overlay.tooltipAnchorRect = app.tooltipAnchorRect;
    overlay.tooltipText = app.tooltipText;
    overlay.tooltipBelow = app.tooltipAnchorBelow;
    UpdateChromeLayout(app);
    app.renderer.Render(app.camera, overlay, app.gizmo, app.chrome);
}

// Advances the camera by the wall-clock time since the last tick, from
// whichever source last ticked it (a rendered frame, or a raw mouse sample
// during fly-look — see WM_INPUT). Called once per raw mouse sample rather
// than only once per rendered frame, so WASD translation is integrated in
// step with every look update instead of catching up in one coarse jump per
// paint, which is what made turning while flying look faceted/blocky: mice
// report well above the display's refresh rate, so several look updates
// could land between two paints, all summed into a single end-of-frame
// rotation that translation then followed as one straight chord.
void TickCamera(ViewerApp& app)
{
    const double now = NowSeconds();
    double elapsed = 0.0;
    if (app.lastFrameSeconds > 0.0)
    {
        const double gap = now - app.lastFrameSeconds;
        if (gap < 0.25) elapsed = std::min(0.1, gap);
    }
    app.lastFrameSeconds = now;
    app.camera.SetInput(BuildFlightInput(app));
    app.camera.Update(elapsed);
}

void RenderFrame(ViewerApp& app)
{
    if (!app.rendererReady) return;
    if (app.useD3D12)
    {
        TickCamera(app);
        if (app.d3d12Path.hasModel) app.d3d12Path.RenderFrame(app.camera, ViewportAspect(app));
        else app.d3d12Path.RenderClearFrame();
        ValidateRect(app.window, nullptr);
        return;
    }
    TickCamera(app);
    RenderScene(app);
    ValidateRect(app.window, nullptr);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    ViewerApp* app = reinterpret_cast<ViewerApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        app = static_cast<ViewerApp*>(create->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wParam, lParam);

    switch (message)
    {
    case WM_CREATE:
    {
        app->dpi = GetDpiForWindow(window);
        app->dpiScale = static_cast<float>(app->dpi) / 96.0f;
        app->toolbarHeight = Scale(*app, 52);
        BOOL dark = TRUE;
        DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
        const int cornerPreference = 2;
        DwmSetWindowAttribute(window, 33, &cornerPreference, sizeof(cornerPreference));
        const COLORREF captionColor = RGB(36, 36, 38);
        const COLORREF borderColor = RGB(58, 58, 60);
        DwmSetWindowAttribute(window, 35, &captionColor, sizeof(captionColor));
        DwmSetWindowAttribute(window, 34, &borderColor, sizeof(borderColor));
        DragAcceptFiles(window, TRUE);
        RegisterPointerInputTarget(window, PT_TOUCH);
        RegisterPointerInputTarget(window, PT_PEN);
        {
            // Raw mouse input for fly-look: relative device deltas, not cursor-
            // position deltas, so look is not quantized to screen pixels and
            // isn't affected by Windows' pointer-acceleration curve — this is
            // what makes look feel smooth/analog instead of steppy.
            RAWINPUTDEVICE mouseDevice{};
            mouseDevice.usUsagePage = 0x01;   // HID_USAGE_PAGE_GENERIC
            mouseDevice.usUsage = 0x02;       // HID_USAGE_GENERIC_MOUSE
            mouseDevice.dwFlags = 0;
            mouseDevice.hwndTarget = window;
            RegisterRawInputDevices(&mouseDevice, 1, sizeof(mouseDevice));
        }
        CreateControls(*app);
        UpdateGizmoLayout(*app);
        UpdateChromeLayout(*app);
        std::wstring renderError;
        app->rendererReady = app->useD3D12 ? app->d3d12Path.Initialize(window, renderError)
                                            : app->renderer.Initialize(window, renderError);
        if (!app->rendererReady)
        {
            app->errorSummary = L"Graphics could not be started.";
            app->errorDetails = renderError;
            app->state = ViewerState::Failed;
        }
        else if (app->useD3D12)
        {
            d3d12_import_bridge::EnsureImportSandboxPrepared();
        }
        UpdateButtonAvailability(*app);
        if (!app->initialPath.empty() && app->rendererReady) BeginOpen(*app, app->initialPath);
        return 0;
    }
    // --- Custom title bar: removes the native caption (WM_NCCALCSIZE) while
    // keeping the resizable frame, then takes over hit-testing so our own
    // D2D-drawn min/max/close (Chrome + Renderer::DrawTitleBar) behave like
    // real caption buttons — including DWM's Snap Layout hover flyout on
    // Maximize, via DwmDefWindowProc passthrough on every NC message below.
    // Standard recipe for "client-area title bar with a real resizable
    // frame" (same approach Windows Terminal uses).
    case WM_NCCALCSIZE:
        if (wParam)
        {
            if (app->isFullscreen)
            {
                // Give the client area the ENTIRE proposed window rect (all
                // four insets, not just the top) — otherwise the still-
                // registered WS_THICKFRAME resize-border insets on the
                // left/right/bottom eat a few pixels off the monitor-filling
                // rect ToggleFullscreen requests, leaving a sliver of desktop
                // visible along those edges instead of covering the monitor.
                return 0;
            }
            NCCALCSIZE_PARAMS& params = *reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            const LONG proposedTop = params.rgrc[0].top;
            DefWindowProcW(window, message, wParam, lParam);
            // Only the top inset (the native caption) is given back to the
            // client area; DefWindowProc's left/right/bottom resize-border
            // insets are kept so edge/corner resize still works below.
            params.rgrc[0].top = proposedTop;
            return 0;
        }
        // wParam == FALSE: lParam is a plain RECT* (the proposed window
        // rect), sent only for the window's very first sizing at creation
        // (later resizes/moves use the wParam==TRUE form above). Returning 0
        // without touching it makes the client rect equal the full window
        // rect, so the native caption never appears even for one frame.
        return 0;
    case WM_NCHITTEST:
    {
        LRESULT dwmResult = 0;
        if (DwmDefWindowProc(window, message, wParam, lParam, &dwmResult)) return dwmResult;

        const LRESULT defaultHit = DefWindowProcW(window, message, wParam, lParam);
        // Outside the client-rendered title bar (including the thin resize
        // border DefWindowProc still reports around the whole window), trust
        // its own edge/corner result rather than overriding it.
        if (defaultHit != HTCLIENT) return defaultHit;

        POINT clientPoint{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(window, &clientPoint);

        // Top-edge/top-corner resize: WM_NCCALCSIZE above hands the entire
        // top inset back to the client area, so DefWindowProc's own hit-test
        // (just consulted above) never reports HTTOP/HTTOPLEFT/HTTOPRIGHT for
        // it — the standard gotcha with this "keep a resizable frame but
        // remove the native caption" recipe (Windows Terminal's non-client
        // island window has the same manual carve-out). Detect that strip
        // ourselves, using the same metrics DefWindowProc uses for its own
        // border, so the window can still be resized — including diagonally
        // from its top corners — by dragging near the top edge.
        if (!app->isFullscreen && !IsZoomed(window))
        {
            const int resizeBorder = GetSystemMetrics(SM_CXPADDEDBORDER) + GetSystemMetrics(SM_CYSIZEFRAME);
            if (clientPoint.y < resizeBorder)
            {
                RECT client{};
                GetClientRect(window, &client);
                const int cornerWidth = resizeBorder * 2;
                if (clientPoint.x < cornerWidth) return HTTOPLEFT;
                if (clientPoint.x >= client.right - cornerWidth) return HTTOPRIGHT;
                return HTTOP;
            }
        }

        // In Fullscreen, EffectiveToolbarHeight collapses to 0: the floating
        // toolbar (drawn and hit-tested as an ordinary client-area overlay by
        // the WM_LBUTTONDOWN/MOUSEMOVE/UP handlers below, same as the bottom
        // bar's buttons) has no non-client role, so every point here reports
        // HTCLIENT rather than routing through Chrome::HitTest's HTCAPTION —
        // dragging a topmost, monitor-filling window would just look broken.
        if (clientPoint.y >= EffectiveToolbarHeight(*app)) return HTCLIENT;
        switch (app->chrome.HitTest(clientPoint))
        {
        case Chrome::Part::Minimize: return HTMINBUTTON;
        case Chrome::Part::Maximize: return HTMAXBUTTON;
        case Chrome::Part::Close: return HTCLOSE;
        case Chrome::Part::Caption: return HTCAPTION;
        default: return HTCLIENT;
        }
    }
    case WM_NCLBUTTONDOWN:
    {
        LRESULT dwmResult = 0;
        if (DwmDefWindowProc(window, message, wParam, lParam, &dwmResult)) return dwmResult;
        if (wParam == HTMINBUTTON || wParam == HTMAXBUTTON || wParam == HTCLOSE)
        {
            app->chrome.pressed = wParam == HTMINBUTTON ? Chrome::Part::Minimize
                : wParam == HTMAXBUTTON ? Chrome::Part::Maximize : Chrome::Part::Close;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        break;
    }
    case WM_NCLBUTTONUP:
    {
        LRESULT dwmResult = 0;
        if (DwmDefWindowProc(window, message, wParam, lParam, &dwmResult)) return dwmResult;
        const Chrome::Part pressedPart = app->chrome.pressed;
        app->chrome.pressed = Chrome::Part::None;
        InvalidateRect(window, nullptr, FALSE);
        if (wParam == HTMINBUTTON && pressedPart == Chrome::Part::Minimize) { ShowWindow(window, SW_MINIMIZE); return 0; }
        if (wParam == HTMAXBUTTON && pressedPart == Chrome::Part::Maximize)
        {
            ShowWindow(window, IsZoomed(window) ? SW_RESTORE : SW_MAXIMIZE);
            return 0;
        }
        if (wParam == HTCLOSE && pressedPart == Chrome::Part::Close) { DestroyWindow(window); return 0; }
        break;
    }
    case WM_NCMOUSEMOVE:
    {
        LRESULT dwmResult = 0;
        if (DwmDefWindowProc(window, message, wParam, lParam, &dwmResult)) return dwmResult;
        Chrome::Part newHover = Chrome::Part::None;
        if (wParam == HTMINBUTTON) newHover = Chrome::Part::Minimize;
        else if (wParam == HTMAXBUTTON) newHover = Chrome::Part::Maximize;
        else if (wParam == HTCLOSE) newHover = Chrome::Part::Close;
        if (app->chrome.hover != newHover)
        {
            app->chrome.hover = newHover;
            if (newHover != Chrome::Part::None)
            {
                TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE | TME_NONCLIENT, window, 0 };
                TrackMouseEvent(&track);
            }
            InvalidateRect(window, nullptr, FALSE);
        }
        break;
    }
    case WM_NCMOUSELEAVE:
    {
        LRESULT dwmResult = 0;
        if (DwmDefWindowProc(window, message, wParam, lParam, &dwmResult)) return dwmResult;
        if (app->chrome.hover == Chrome::Part::Minimize || app->chrome.hover == Chrome::Part::Maximize ||
            app->chrome.hover == Chrome::Part::Close)
        {
            app->chrome.hover = Chrome::Part::None;
            InvalidateRect(window, nullptr, FALSE);
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_VIEW_OPEN_INITIAL) BeginOpen(*app, app->initialPath);
        else HandleCommand(*app, LOWORD(wParam));
        return 0;
    case WM_DRAWITEM:
        DrawOwnerButton(*app, *reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        if (app->rendererReady)
        {
            if (app->useD3D12)
            {
                if (app->d3d12Path.hasModel) app->d3d12Path.RenderFrame(app->camera, ViewportAspect(*app));
                else app->d3d12Path.RenderClearFrame();
            }
            else RenderScene(*app);
        }
        else RenderFallback(*app, dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_SIZE:
        LayoutControls(*app);
        UpdateGizmoLayout(*app);
        UpdateChromeLayout(*app);
        if (app->rendererReady && wParam != SIZE_MINIMIZED)
        {
            std::wstring resizeError;
            bool resized = app->useD3D12
                ? app->d3d12Path.Resize(LOWORD(lParam), HIWORD(lParam), resizeError)
                : app->renderer.Resize(LOWORD(lParam), HIWORD(lParam), resizeError);
            if (!resized)
            {
                app->rendererReady = false;
                app->errorSummary = L"The viewport could not be resized.";
                app->errorDetails = resizeError;
                app->state = ViewerState::Failed;
            }
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED:
    {
        app->dpi = HIWORD(wParam);
        app->dpiScale = static_cast<float>(app->dpi) / 96.0f;
        app->toolbarHeight = Scale(*app, 52);
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
            suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        RecreateButtonFont(*app);
        LayoutControls(*app);
        UpdateGizmoLayout(*app);
        UpdateChromeLayout(*app);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_GETMINMAXINFO:
    {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = Scale(*app, 480);
        info->ptMinTrackSize.y = Scale(*app, 360);
        return 0;
    }
    case WM_DROPFILES:
    {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        if (count != 1)
        {
            DragFinish(drop);
            SetFailure(*app, L"Open one model at a time.", L"Drop exactly one local .glb file into the viewer.");
            return 0;
        }
        const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
        std::wstring path(length + 1, L'\0');
        DragQueryFileW(drop, 0, path.data(), length + 1);
        path.resize(length);
        DragFinish(drop);
        BeginOpen(*app, path);
        return 0;
    }
    case WM_POINTERDOWN:
        if (CanNavigate(*app))
        {
            const UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
            POINT point{};
            if (GetClientPointerPoint(window, pointerId, point) && PointInViewport(*app, point))
            {
                SetFocus(window);
                SetCapture(window);
                app->touchPoints.insert_or_assign(pointerId, point);
                ResetTouchBaseline(*app);
            }
        }
        return 0;
    case WM_POINTERUPDATE:
        if (CanNavigate(*app))
        {
            const UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
            const auto found = app->touchPoints.find(pointerId);
            POINT point{};
            if (found != app->touchPoints.end() && GetClientPointerPoint(window, pointerId, point))
            {
                if (app->touchPoints.size() == 1)
                {
                    app->camera.Orbit(static_cast<float>(point.x - found->second.x), static_cast<float>(point.y - found->second.y));
                    found->second = point;
                    ResetTouchBaseline(*app);
                }
                else
                {
                    found->second = point;
                    auto first = app->touchPoints.begin();
                    auto second = std::next(first);
                    const POINT center{ (first->second.x + second->second.x) / 2, (first->second.y + second->second.y) / 2 };
                    const double spanX = static_cast<double>(first->second.x - second->second.x);
                    const double spanY = static_cast<double>(first->second.y - second->second.y);
                    const double span = std::sqrt(spanX * spanX + spanY * spanY);
                    RECT client{}; GetClientRect(window, &client);
                    app->camera.Pan(static_cast<float>(center.x - app->touchCenter.x), static_cast<float>(center.y - app->touchCenter.y),
                        static_cast<float>(client.bottom - EffectiveToolbarHeight(*app)));
                    if (app->touchSpan > 1.0 && span > 1.0)
                    {
                        app->camera.Dolly(static_cast<float>(std::log(span / app->touchSpan) / 0.16));
                    }
                    app->touchCenter = center;
                    app->touchSpan = span;
                }
                InvalidateRect(window, nullptr, FALSE);
            }
        }
        return 0;
    case WM_POINTERUP:
    case WM_POINTERCAPTURECHANGED:
    {
        const UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
        app->touchPoints.erase(pointerId);
        if (app->touchPoints.empty() && GetCapture() == window && app->pointerMode == PointerMode::None) ReleaseCapture();
        ResetTouchBaseline(*app);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            if (app->flyLook)
            {
                SetCursor(nullptr);
                return TRUE;
            }
            if (app->gizmo.hover != NavGizmo::Part::None && app->pointerMode == PointerMode::None && CanNavigate(*app))
            {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            if (app->chrome.hover != Chrome::Part::None)
            {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            if (app->infoButtonHover || app->fullscreenButtonHover)
            {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    case WM_MOUSELEAVE:
        if (app->gizmo.hover != NavGizmo::Part::None)
        {
            app->gizmo.hover = NavGizmo::Part::None;
            InvalidateRect(window, nullptr, FALSE);
        }
        // Min/Max/Close hover is owned by WM_NCMOUSELEAVE, not this.
        if (app->chrome.hover != Chrome::Part::None && app->chrome.hover != Chrome::Part::Minimize &&
            app->chrome.hover != Chrome::Part::Maximize && app->chrome.hover != Chrome::Part::Close)
        {
            app->chrome.hover = Chrome::Part::None;
            InvalidateRect(window, nullptr, FALSE);
        }
        if (app->infoButtonHover || app->fullscreenButtonHover)
        {
            app->infoButtonHover = false;
            app->fullscreenButtonHover = false;
            InvalidateRect(window, nullptr, FALSE);
        }
        UpdateTooltipTracking(*app);
        return 0;
    case WM_LBUTTONDOWN:
    {
        const POINT downPoint{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (app->speedFlyoutOpen)
        {
            const RECT track = SpeedFlyoutTrackRect(*app);
            const RECT panel = SpeedFlyoutRect(*app);
            RECT hitTrack = track;
            InflateRect(&hitTrack, 0, Scale(*app, 8));
            if (PtInRect(&hitTrack, downPoint))
            {
                SetCapture(window);
                app->speedSliderDragging = true;
                SetFlySpeedFromFlyoutX(*app, downPoint.x);
                InvalidateRect(window, nullptr, FALSE);
                UpdateTooltipTracking(*app);
                return 0;
            }
            app->speedFlyoutOpen = false;
            InvalidateRect(window, nullptr, FALSE);
            if (PtInRect(&panel, downPoint)) return 0;
            // A click outside the panel closes it and still falls through,
            // so clicking a different title-bar button both dismisses the
            // flyout and performs that click in one action.
        }
        if (app->settingsPanelOpen)
        {
            const RECT switchRect = SettingsSwitchRect(*app);
            const RECT panel = SettingsPanelRect(*app);
            if (PtInRect(&switchRect, downPoint))
            {
                ToggleShowNativeOrientation(*app);
                return 0;
            }
            app->settingsPanelOpen = false;
            InvalidateRect(window, nullptr, FALSE);
            if (PtInRect(&panel, downPoint)) return 0;
            // Same outside-click semantics as the Speed flyout above: close
            // and still fall through, so a click on another button both
            // dismisses this panel and performs that click in one action.
        }
        // The raw toolbarHeight, not EffectiveToolbarHeight (0 in Fullscreen)
        // — the action buttons stay clickable there as a floating toolbar
        // overlaying the full-monitor viewport (WM_NCHITTEST above already
        // routes these points to plain client messages instead of NC ones).
        if (downPoint.y < app->toolbarHeight)
        {
            const Chrome::Part part = app->chrome.HitTest(downPoint);
            if (part != Chrome::Part::None && part != Chrome::Part::Caption)
            {
                SetCapture(window);
                app->chrome.pressed = part;
                InvalidateRect(window, nullptr, FALSE);
                UpdateTooltipTracking(*app);
                return 0;
            }
        }
        if (CanNavigate(*app))
        {
            RECT hitTrack = ZoomTrackRect(*app);
            InflateRect(&hitTrack, 0, Scale(*app, 8));
            if (PtInRect(&hitTrack, downPoint))
            {
                SetCapture(window);
                app->zoomSliderDragging = true;
                SetZoomFromTrackX(*app, downPoint.x);
                InvalidateRect(window, nullptr, FALSE);
                UpdateTooltipTracking(*app);
                return 0;
            }
            const RECT infoButton = InfoButtonRect(*app);
            if (PtInRect(&infoButton, downPoint))
            {
                SetCapture(window);
                app->infoButtonPressed = true;
                InvalidateRect(window, nullptr, FALSE);
                UpdateTooltipTracking(*app);
                return 0;
            }
            const RECT fullscreenButton = FullscreenButtonRect(*app);
            if (PtInRect(&fullscreenButton, downPoint))
            {
                SetCapture(window);
                app->fullscreenButtonPressed = true;
                InvalidateRect(window, nullptr, FALSE);
                UpdateTooltipTracking(*app);
                return 0;
            }
        }
        if (PointInViewport(*app, POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }) && CanNavigate(*app))
        {
            SetFocus(window);
            const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const NavGizmo::Part part = app->gizmo.HitTest(app->camera.Orientation(),
                static_cast<float>(point.x), static_cast<float>(point.y));
            if (part == NavGizmo::Part::Ball)
            {
                SetCapture(window);
                app->pointerMode = PointerMode::GizmoOrbit;
                BeginWrappedDrag(*app, point);
                app->camera.CancelInertia();
                app->orbitVelocityX = 0.0;
                app->orbitVelocityY = 0.0;
                app->lastOrbitMoveSeconds = NowSeconds();
            }
            else if (part != NavGizmo::Part::None)
            {
                // Axis node/stem: snap immediately on press.
                app->camera.SnapToView(CanonicalViewOrientation(app->gizmo.ViewFor(part)));
                InvalidateRect(window, nullptr, FALSE);
            }
            else
            {
                // Plain LMB: orbit-drags immediately; a release that never
                // exceeded the click/drag threshold click-selects instead.
                SetCapture(window);
                app->pointerMode = PointerMode::Orbit;
                BeginWrappedDrag(*app, point);
                app->camera.CancelInertia();
                app->orbitVelocityX = 0.0;
                app->orbitVelocityY = 0.0;
                app->lastOrbitMoveSeconds = NowSeconds();
                app->selectDragged = false;
                app->selectDownPoint = point;
                app->selectDownSeconds = NowSeconds();
            }
        }
        return 0;
    }
    case WM_MBUTTONDOWN:
        if (PointInViewport(*app, POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }) && CanNavigate(*app))
        {
            SetFocus(window);
            SetCapture(window);
            const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            // Plain MMB trucks along the ground plane; Ctrl+MMB dollies.
            app->pointerMode = control ? PointerMode::DollyDrag : PointerMode::Truck;
            BeginWrappedDrag(*app, point);
            if (app->pointerMode == PointerMode::Truck)
            {
                app->camera.CancelInertia();
                app->panVelocityX = 0.0;
                app->panVelocityY = 0.0;
                app->lastPanMoveSeconds = NowSeconds();
            }
        }
        return 0;
    case WM_RBUTTONDOWN:
        if (PointInViewport(*app, POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }) && CanNavigate(*app))
        {
            SetFocus(window);
            SetCapture(window);
            app->pointerMode = PointerMode::FlyLook;
            app->flyLook = true;
            GetCursorPos(&app->flyPressPoint);
            // Look is driven by WM_INPUT's raw relative deltas (registered at
            // WM_CREATE), not cursor-position deltas, so there is no screen
            // edge to fall off and nothing to recenter — just hide the cursor
            // for the duration and restore it (EndPointer) on release.
            SetCursor(nullptr);
        }
        return 0;
    case WM_MOUSEMOVE:
    {
        const POINT movePoint{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (app->speedSliderDragging)
        {
            if (GetCapture() == window) SetFlySpeedFromFlyoutX(*app, movePoint.x);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (app->zoomSliderDragging)
        {
            if (GetCapture() == window) SetZoomFromTrackX(*app, movePoint.x);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (app->chrome.pressed != Chrome::Part::None) return 0;
        if (app->infoButtonPressed || app->fullscreenButtonPressed) return 0;
        if (app->pointerMode == PointerMode::None)
        {
            // Idle hover tracking: the title-bar action buttons above the
            // viewport, the gizmo within it. (Min/Max/Close hover is tracked
            // separately via WM_NCMOUSEMOVE, since those points are always
            // non-client.)
            if (movePoint.y < app->toolbarHeight)
            {
                const Chrome::Part hit = app->chrome.HitTest(movePoint);
                const Chrome::Part effective = hit == Chrome::Part::Caption ? Chrome::Part::None : hit;
                if (effective != app->chrome.hover)
                {
                    app->chrome.hover = effective;
                    if (effective != Chrome::Part::None)
                    {
                        TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
                        TrackMouseEvent(&track);
                    }
                    InvalidateRect(window, nullptr, FALSE);
                }
                if (app->gizmo.hover != NavGizmo::Part::None)
                {
                    app->gizmo.hover = NavGizmo::Part::None;
                    InvalidateRect(window, nullptr, FALSE);
                }
                UpdateTooltipTracking(*app);
                return 0;
            }
            if (app->chrome.hover != Chrome::Part::None)
            {
                app->chrome.hover = Chrome::Part::None;
                InvalidateRect(window, nullptr, FALSE);
            }
            // Bottom-bar Info/Fullscreen hover tracking (only while that bar
            // is actually shown, i.e. a model is loaded).
            if (CanNavigate(*app))
            {
                RECT client{};
                GetClientRect(window, &client);
                if (movePoint.y >= client.bottom - app->bottomBarHeight)
                {
                    const RECT infoButton = InfoButtonRect(*app);
                    const RECT fullscreenButton = FullscreenButtonRect(*app);
                    const bool overInfo = PtInRect(&infoButton, movePoint) != FALSE;
                    const bool overFullscreen = PtInRect(&fullscreenButton, movePoint) != FALSE;
                    if (overInfo != app->infoButtonHover || overFullscreen != app->fullscreenButtonHover)
                    {
                        app->infoButtonHover = overInfo;
                        app->fullscreenButtonHover = overFullscreen;
                        if (overInfo || overFullscreen)
                        {
                            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
                            TrackMouseEvent(&track);
                        }
                        InvalidateRect(window, nullptr, FALSE);
                    }
                }
                else if (app->infoButtonHover || app->fullscreenButtonHover)
                {
                    app->infoButtonHover = false;
                    app->fullscreenButtonHover = false;
                    InvalidateRect(window, nullptr, FALSE);
                }
            }
            if (CanNavigate(*app) && PointInViewport(*app, movePoint))
            {
                const NavGizmo::Part part = app->gizmo.HitTest(app->camera.Orientation(),
                    static_cast<float>(movePoint.x), static_cast<float>(movePoint.y));
                if (part != app->gizmo.hover)
                {
                    app->gizmo.hover = part;
                    if (part != NavGizmo::Part::None)
                    {
                        TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
                        TrackMouseEvent(&track);
                    }
                    InvalidateRect(window, nullptr, FALSE);
                }
            }
            else if (app->gizmo.hover != NavGizmo::Part::None)
            {
                app->gizmo.hover = NavGizmo::Part::None;
                InvalidateRect(window, nullptr, FALSE);
            }
            UpdateTooltipTracking(*app);
            return 0;
        }
        if (GetCapture() == window)
        {
            const POINT pointer{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const float deltaX = static_cast<float>(pointer.x - app->lastPointer.x);
            const float deltaY = static_cast<float>(pointer.y - app->lastPointer.y);
            app->lastPointer = pointer;
            switch (app->pointerMode)
            {
            case PointerMode::FlyLook:
                // Look is driven by WM_INPUT (raw deltas); the cursor is just
                // kept hidden here since its position is otherwise unused.
                SetCursor(nullptr);
                break;
            case PointerMode::Orbit:
            case PointerMode::GizmoOrbit:
                TrackOrbitVelocity(*app, deltaX, deltaY);
                app->camera.Orbit(deltaX, deltaY);
                WrapCursorIfNeeded(*app);
                break;
            case PointerMode::Truck:
                TrackPanVelocity(*app, deltaX, deltaY);
                app->camera.Truck(deltaX, deltaY,
                    static_cast<float>(ViewportRect(*app).bottom - ViewportRect(*app).top), app->axisSnapEnabled);
                WrapCursorIfNeeded(*app);
                break;
            case PointerMode::DollyDrag:
                app->camera.DollyDrag(deltaY);
                WrapCursorIfNeeded(*app);
                break;
            default: break;
            }
            // A plain-LMB orbit gesture also owns click-select: track whether
            // it stayed under the drag threshold, so a release without a real
            // drag still click-selects (drag and click share the button).
            if (app->pointerMode == PointerMode::Orbit &&
                std::abs(pointer.x - app->selectDownPoint.x) + std::abs(pointer.y - app->selectDownPoint.y) >
                    kClickDragThresholdPixels)
            {
                app->selectDragged = true;
            }
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (app->speedSliderDragging)
        {
            app->speedSliderDragging = false;
            if (GetCapture() == window) ReleaseCapture();
            UpdateTooltipTracking(*app);
            return 0;
        }
        if (app->zoomSliderDragging)
        {
            app->zoomSliderDragging = false;
            if (GetCapture() == window) ReleaseCapture();
            UpdateTooltipTracking(*app);
            return 0;
        }
        if (app->chrome.pressed != Chrome::Part::None)
        {
            const POINT upPoint{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const Chrome::Part pressedPart = app->chrome.pressed;
            app->chrome.pressed = Chrome::Part::None;
            if (GetCapture() == window) ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
            if (upPoint.y < app->toolbarHeight && app->chrome.HitTest(upPoint) == pressedPart)
            {
                HandleChromeAction(*app, pressedPart);
            }
            UpdateTooltipTracking(*app);
            return 0;
        }
        if (app->infoButtonPressed)
        {
            const POINT upPoint{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            app->infoButtonPressed = false;
            if (GetCapture() == window) ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
            const RECT infoButton = InfoButtonRect(*app);
            if (PtInRect(&infoButton, upPoint)) HandleCommand(*app, ID_VIEW_INFO);
            UpdateTooltipTracking(*app);
            return 0;
        }
        if (app->fullscreenButtonPressed)
        {
            const POINT upPoint{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            app->fullscreenButtonPressed = false;
            if (GetCapture() == window) ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
            const RECT fullscreenButton = FullscreenButtonRect(*app);
            if (PtInRect(&fullscreenButton, upPoint)) HandleCommand(*app, ID_VIEW_FULLSCREEN);
            UpdateTooltipTracking(*app);
            return 0;
        }
        if (app->pointerMode == PointerMode::Orbit && !app->selectDragged && CanNavigate(*app) &&
            NowSeconds() - app->selectDownSeconds < kClickMaxSeconds)
        {
            ClickSelect(*app, app->selectDownPoint);
        }
        if ((app->pointerMode == PointerMode::Orbit || app->pointerMode == PointerMode::GizmoOrbit) &&
            CanNavigate(*app) && NowSeconds() - app->lastOrbitMoveSeconds < 0.07)
        {
            app->camera.SeedOrbitInertia(static_cast<float>(app->orbitVelocityX),
                static_cast<float>(app->orbitVelocityY));
        }
        EndPointer(*app);
        return 0;
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
        if (app->pointerMode == PointerMode::Truck && CanNavigate(*app) &&
            NowSeconds() - app->lastPanMoveSeconds < 0.07)
        {
            app->camera.SeedPanInertia(static_cast<float>(app->panVelocityX),
                static_cast<float>(app->panVelocityY));
        }
        EndPointer(*app);
        return 0;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        EndPointer(*app);
        return 0;
    case WM_LBUTTONDBLCLK:
        if (PointInViewport(*app, POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }) && CanNavigate(*app))
        {
            FrameSelectedOrAll(*app);
        }
        return 0;
    case WM_MOUSEWHEEL:
    {
        POINT wheelPoint{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };   // screen coords for this message
        ScreenToClient(window, &wheelPoint);
        const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        const RECT infoPanel = InfoPanelRect(*app);
        if (app->infoPanelVisible && PtInRect(&infoPanel, wheelPoint))
        {
            const float maxScroll = InfoPanelMaxScroll(*app);
            app->infoPanelScrollOffset = std::clamp(
                app->infoPanelScrollOffset - steps * static_cast<float>(Scale(*app, 48)), 0.0f, maxScroll);
            InvalidateRect(window, nullptr, FALSE);
        }
        else if (CanNavigate(*app))
        {
            if (app->flyLook) AdjustFlySpeed(*app, steps);
            else
            {
                app->camera.Dolly(steps);
                InvalidateRect(window, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_INPUT:
        if (app->flyLook)
        {
            UINT size = 0;
            GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
            if (size > 0 && size <= sizeof(RAWINPUT))
            {
                RAWINPUT raw{};
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == size &&
                    raw.header.dwType == RIM_TYPEMOUSE && (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0 &&
                    (raw.data.mouse.lLastX != 0 || raw.data.mouse.lLastY != 0))
                {
                    // Tick the camera right here, once per raw mouse sample,
                    // instead of only once per rendered frame: mice report at
                    // 125-1000Hz, well above the display refresh rate, so a
                    // render-paced tick would sum up several look updates and
                    // then move WASD translation through only their *final*
                    // orientation — a coarse, faceted approximation of the
                    // turn. Ticking per sample advances rotation and
                    // translation together at the same fine granularity, so
                    // flight curves smoothly like Unreal's.
                    app->camera.AccumulateLook(static_cast<float>(raw.data.mouse.lLastX), static_cast<float>(raw.data.mouse.lLastY));
                    TickCamera(*app);
                    InvalidateRect(window, nullptr, FALSE);
                }
            }
        }
        return DefWindowProcW(window, message, wParam, lParam);
    case WM_KEYDOWN:
        if (wParam == VK_F11) { ToggleFullscreen(*app); return 0; }
        if (wParam == VK_ESCAPE)
        {
            if (app->isFullscreen) { ToggleFullscreen(*app); return 0; }
            CancelOpen(*app);
            return 0;
        }
        if (!CanNavigate(*app)) break;
        if (SetNavigationKey(*app, wParam, true)) return 0;
        if (wParam == VK_OEM_PLUS || wParam == VK_ADD) app->camera.Dolly(1.0f);
        else if (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT) app->camera.Dolly(-1.0f);
        else break;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_KEYUP:
        if (SetNavigationKey(*app, wParam, false)) return 0;
        break;
    case WM_KILLFOCUS:
        StopNavigation(*app);
        EndPointer(*app);
        if (app->speedFlyoutOpen) { app->speedFlyoutOpen = false; InvalidateRect(window, nullptr, FALSE); }
        if (app->settingsPanelOpen) { app->settingsPanelOpen = false; InvalidateRect(window, nullptr, FALSE); }
        return 0;
    case WM_CONTEXTMENU:
        return 0;
    case WM_SYSKEYDOWN:
        if (wParam == 'M' && (lParam & (1u << 29))) { ShowMoreMenu(*app); return 0; }
        break;
    case kLoadCompleteMessage:
    {
        std::unique_ptr<CompleteMessage> complete(reinterpret_cast<CompleteMessage*>(lParam));
        if (!complete || complete->generation != app->generation) return 0;
        app->cancellation.reset();
        if (complete->result.cancelled)
        {
            app->state = app->renderer.HasModel() ? ViewerState::Ready : ViewerState::Empty;
        }
        else if (!complete->result.succeeded)
        {
            SetFailure(*app, complete->result.summary, complete->result.details, complete->path);
            return 0;
        }
        else
        {
            std::wstring uploadError;
            if (!app->renderer.UploadModel(*complete->result.model, uploadError))
            {
                SetFailure(*app, L"The model was read but could not be displayed.", uploadError, complete->path);
                return 0;
            }
            app->currentPath = complete->path;
            app->filename = FileNameFromPath(complete->path);
            app->warning = complete->result.model->warning;
            app->loadedModel = complete->result.model;
            app->meshSelected = false;
            DirectX::XMFLOAT3 effectiveMin{};
            DirectX::XMFLOAT3 effectiveMax{};
            EffectiveBounds(*app, effectiveMin, effectiveMax);
            std::wstring gridError;
            app->renderer.RebuildGrid(effectiveMin, effectiveMax, gridError);
            app->camera.SetBounds(effectiveMin, effectiveMax, ViewportAspect(*app));
            app->state = ViewerState::Ready;
            app->failedPath.clear();
            app->errorSummary.clear();
            app->errorDetails.clear();
            UpdateTitle(*app);
            SetFocus(window);
        }
        UpdateButtonAvailability(*app);
        LayoutControls(*app);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case kD3D12ImportCompleteMessage:
    {
        std::unique_ptr<D3D12CompleteMessage> complete(reinterpret_cast<D3D12CompleteMessage*>(lParam));
        if (!complete || complete->generation != app->generation) return 0;
        if (!complete->result.ok)
        {
            SetFailure(*app, complete->result.errorSummary, complete->result.errorDetails, complete->path);
            return 0;
        }
        std::wstring uploadError;
        if (!app->d3d12Path.UploadModel(complete->result.meshes, complete->result.materials,
                                         complete->result.images, uploadError))
        {
            SetFailure(*app, L"The model was read but could not be displayed.", uploadError, complete->path);
            return 0;
        }
        app->currentPath = complete->path;
        app->filename = FileNameFromPath(complete->path);

        // Host-side bounds scan (no ChunkDescriptor bounds field exists yet)
        // mirroring Model.cpp's own min/max accumulation, feeding the same
        // Camera::SetBounds call the D3D11 completion handler uses above.
        DirectX::XMFLOAT3 effectiveMin{};
        DirectX::XMFLOAT3 effectiveMax{};
        bool haveBounds = false;
        for (const auto& mesh : complete->result.meshes)
        {
            if (mesh.vertexLayoutId != model_core::VertexLayoutId::PositionNormalUv0_F32) continue;
            const auto* vertices = reinterpret_cast<const model_core::VertexPositionNormalUv0F32*>(mesh.payload.data());
            for (uint32_t i = 0; i < mesh.vertexCount; ++i)
            {
                DirectX::XMFLOAT3 p{ vertices[i].px, vertices[i].py, vertices[i].pz };
                if (!haveBounds) { effectiveMin = effectiveMax = p; haveBounds = true; continue; }
                effectiveMin.x = std::min(effectiveMin.x, p.x);
                effectiveMin.y = std::min(effectiveMin.y, p.y);
                effectiveMin.z = std::min(effectiveMin.z, p.z);
                effectiveMax.x = std::max(effectiveMax.x, p.x);
                effectiveMax.y = std::max(effectiveMax.y, p.y);
                effectiveMax.z = std::max(effectiveMax.z, p.z);
            }
        }
        if (haveBounds) app->camera.SetBounds(effectiveMin, effectiveMax, ViewportAspect(*app));

        app->state = ViewerState::Ready;
        app->failedPath.clear();
        app->errorSummary.clear();
        app->errorDetails.clear();
        UpdateTitle(*app);
        SetFocus(window);
        UpdateButtonAvailability(*app);
        LayoutControls(*app);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_TIMER:
        if (wParam == kTooltipTimerId)
        {
            KillTimer(window, kTooltipTimerId);
            if (app->tooltipTargetId != 0)
            {
                app->tooltipVisible = true;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        break;
    case WM_DESTROY:
        app->closing = true;
        app->alive->store(false, std::memory_order_relaxed);
        if (app->cancellation) app->cancellation->store(true, std::memory_order_relaxed);
        if (app->buttonFont) { DeleteObject(app->buttonFont); app->buttonFont = nullptr; }
        // GPU work must be known-idle before ViewerApp's destructor releases
        // the D3D12 objects -- RAII alone doesn't order that.
        if (app->useD3D12)
        {
            app->d3d12Path.WaitForIdle();
            app->d3d12Path.ClearModel();
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

ATOM RegisterViewerClass(HINSTANCE instance)
{
    WNDCLASSEXW windowClass{ sizeof(windowClass) };
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_PREVIEW3D));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = gBackgroundBrush;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SMALL));
    return RegisterClassExW(&windowClass);
}

bool CreateMainWindow(ViewerApp& app, int showCommand)
{
    const UINT dpi = GetDpiForSystem();
    RECT windowBounds{ 0, 0, MulDiv(1000, dpi, 96), MulDiv(720, dpi, 96) };
    AdjustWindowRectExForDpi(&windowBounds, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_ACCEPTFILES | WS_EX_CONTROLPARENT, dpi);
    const int width = windowBounds.right - windowBounds.left;
    const int height = windowBounds.bottom - windowBounds.top;
    MONITORINFO monitor{ sizeof(monitor) };
    GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const int x = monitor.rcWork.left + std::max<LONG>(0, (monitor.rcWork.right - monitor.rcWork.left - width) / 2);
    const int y = monitor.rcWork.top + std::max<LONG>(0, (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2);
    gMainWindow = CreateWindowExW(WS_EX_ACCEPTFILES | WS_EX_CONTROLPARENT, kWindowClass, kApplicationName,
        WS_OVERLAPPEDWINDOW, x, y, width, height, nullptr, nullptr, app.instance, &app);
    if (!gMainWindow) return false;
    ShowWindow(gMainWindow, showCommand);
    UpdateWindow(gMainWindow);
    return true;
}
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX commonControls{ sizeof(commonControls), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&commonControls);
    gBackgroundBrush = CreateSolidBrush(RGB(28, 28, 30));

    ViewerApp app;
    app.instance = instance;
    const ViewerSettings settings = LoadSettings();
    app.showNativeOrientation = settings.showNativeOrientation;
    int argumentCount = 0;
    PWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments)
    {
        // --d3d12 is scanned out first; the first remaining non-flag
        // argument (if any) is still treated as the initial file path,
        // preserving today's single-positional-arg behavior exactly when
        // the flag is absent.
        for (int i = 1; i < argumentCount; ++i)
        {
            if (_wcsicmp(arguments[i], L"--d3d12") == 0) app.useD3D12 = true;
            else if (app.initialPath.empty()) app.initialPath = arguments[i];
        }
        LocalFree(arguments);
    }

    if (!RegisterViewerClass(instance) || !CreateMainWindow(app, showCommand))
    {
        if (gBackgroundBrush) DeleteObject(gBackgroundBrush);
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 1;
    }

    HACCEL accelerators = LoadAcceleratorsW(instance, MAKEINTRESOURCEW(IDC_PREVIEW3D));
    MSG message{};
    int exitCode = 0;
    bool quitting = false;
    while (!quitting)
    {
        // Tracks whether animation was already active before each dispatched
        // message, so a message that newly turns it on gets rendered right
        // away — otherwise a key press+release landing in the same drain pass
        // (a quick tap, or the app falling briefly behind) would be fully
        // drained before the loop ever renders a frame with the key down,
        // leaving flight keys with no visible effect. Only rendering on that
        // on-transition (rather than after every message) avoids re-rendering
        // once per queued WM_MOUSEMOVE during a mouse-look drag, which would
        // otherwise serialize a burst of moves behind repeated vsync waits.
        bool wasAnimating = gMainWindow && app.rendererReady && !IsIconic(gMainWindow) && IsAnimating(app);
        // Bounded to a single burst: a self-recentering FlyLook mouse-move can
        // otherwise repost itself indefinitely (some input stacks emit a fresh
        // WM_MOUSEMOVE for every SetCursorPos, even a no-op one) and never let
        // PeekMessageW go empty, starving the render check below forever.
        int drained = 0;
        while (drained < kMaxDrainedMessagesPerIteration && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            ++drained;
            if (message.message == WM_QUIT)
            {
                exitCode = static_cast<int>(message.wParam);
                quitting = true;
                break;
            }
            // Accelerators are translated first: IsDialogMessageW would
            // otherwise consume keydowns before the hotkeys ever see them.
            // Escape is dispatched straight through instead: the window has
            // WS_EX_CONTROLPARENT (for the error-state child buttons), which
            // makes IsDialogMessageW treat it as dialog-like and silently eat
            // Escape as a "cancel" keystroke before WM_KEYDOWN's own
            // Escape-exits-Fullscreen/cancel-open handling ever runs.
            if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE)
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            else if (!TranslateAcceleratorW(gMainWindow, accelerators, &message))
            {
                if (gMainWindow && IsDialogMessageW(gMainWindow, &message)) continue;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            const bool isAnimatingNow = gMainWindow && app.rendererReady && !IsIconic(gMainWindow) && IsAnimating(app);
            if (isAnimatingNow && !wasAnimating) RenderFrame(app);
            wasAnimating = isAnimatingNow;
        }
        if (quitting) break;

        // Continuous, vsync-paced rendering while anything is in motion:
        // flight keys, easing, inertia, fit/reset glides, transient HUDs,
        // or the load spinner. The loop blocks in MsgWaitForMultipleObjectsEx
        // otherwise, so a still viewport costs no CPU or GPU.
        if (gMainWindow && app.rendererReady && !IsIconic(gMainWindow) && IsAnimating(app))
        {
            RenderFrame(app);
            continue;
        }
        if (gMainWindow && GetUpdateRect(gMainWindow, nullptr, FALSE))
        {
            if (app.rendererReady && !IsIconic(gMainWindow)) RenderFrame(app);
            else RedrawWindow(gMainWindow, nullptr, nullptr, RDW_INTERNALPAINT);
            continue;
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    if (gBackgroundBrush) DeleteObject(gBackgroundBrush);
    if (SUCCEEDED(comResult)) CoUninitialize();
    return exitCode;
}
