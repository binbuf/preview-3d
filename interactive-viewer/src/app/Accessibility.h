#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <oleacc.h>
#include <UIAutomation.h>
#include <UIAutomationCoreApi.h>

#include <functional>
#include <string>
#include <vector>

namespace viewer_accessibility
{
enum class Control
{
    None,
    Grid,
    AxisSnap,
    Speed,
    Fit,
    Reset,
    Share,
    More,
    OpenWith,
    Minimize,
    Maximize,
    Close,
    Info,
    Zoom,
    Fullscreen,
    SpeedSlider,
    NativeOrientation,
    GizmoPositiveX,
    GizmoNegativeX,
    GizmoPositiveY,
    GizmoNegativeY,
    GizmoPositiveZ,
    GizmoNegativeZ,
    ErrorRetry,
    ErrorOpenAnother,
    ErrorCopyDetails,
    // Appended to preserve the stable numeric runtime IDs of existing UIA
    // fragments. VisibleControls supplies its visual/tab order explicitly.
    GroundAxis,
    GroundDirection,
    HideCursorWhileDragging,
    InfoPanelClose,
    LightingStudio,
    LightingClay,
    LightingDirectional,
    DirectionalLightAngle,
    Wireframe,
    // Appended after Wireframe so its numeric runtime ID stays stable.
    DirectionalLightElevation,
    // Appended after DirectionalLightElevation so earlier numeric runtime IDs
    // stay stable.
    Orbit,
    OrbitSlider,
    // Appended after OrbitSlider so earlier numeric runtime IDs stay stable.
    GizmoRollCounterClockwise,
    GizmoRollClockwise,
    // Appended after GizmoRollClockwise so earlier numeric runtime IDs stay stable.
    HideModelUntilComplete,
    // Appended after HideModelUntilComplete so earlier numeric runtime IDs stay
    // stable. The popup exposes its visible app tiles through fixed slots plus
    // the pinned More apps tile.
    OpenWithTile0,
    OpenWithTile1,
    OpenWithTile2,
    OpenWithTile3,
    OpenWithTile4,
    OpenWithTile5,
    OpenWithTile6,
    OpenWithTile7,
    OpenWithMoreApps,
    Count,
};

struct ControlInfo
{
    std::wstring name;
    std::wstring description;
    std::wstring value;
    RECT rect{}; // client pixels
    LONG role = ROLE_SYSTEM_PUSHBUTTON;
    bool visible = false;
    bool enabled = true;
    bool checked = false;
    bool focused = false;
};

using Query = std::function<ControlInfo(Control)>;
using Action = std::function<void(Control)>;
using Focus = std::function<void(Control)>;
using Status = std::function<std::wstring()>;

IAccessible* CreateProvider(HWND window, Query query, Action action, Focus focus, Status status);
IRawElementProviderSimple* CreateUiaProvider(HWND window, Query query, Action action, Focus focus, Status status);
std::vector<Control> VisibleControls(const Query& query);
void Announce(HWND window, IRawElementProviderSimple* provider = nullptr, const std::wstring& text = {});
}
