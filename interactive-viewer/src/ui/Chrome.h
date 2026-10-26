#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Windows-11-Photos-style unified title bar: pure layout/hit-test state, no
// drawing or COM dependency — mirrors NavGizmo's split from the overlay drawing.
// D3D11On12Overlay::DrawTitleBar consumes this class's rects to paint the D2D content;
// Preview3D.cpp's WM_NCHITTEST/WM_LBUTTONDOWN handlers consume HitTest to route
// input. All rects are in client pixel coordinates.
//
// Layout, left to right: the navigation action buttons (Grid/Ground axis/Ground
// direction/Snap/Speed/Orbit/Share/Overflow), an empty drag strip, the centered
// filename, more empty drag strip, then Open With and the system min/max/close.
// Fit is laid out but currently hidden behind Chrome.cpp's kShowFitButton flag;
// Reset has moved out of the title bar to a floating viewport button that only
// appears once the camera has left its home framing.
class Chrome
{
public:
    enum class Part
    {
        None,
        Caption,       // empty drag strip
        Grid,
        GroundAxis,
        GroundDirection,
        AxisSnap,
        Speed,
        Orbit,
        Fit,
        Share,
        Overflow,      // "..." menu (Controls/warnings/About)
        OpenWith,
        Update,        // circular up-arrow shown when a newer release exists
        Minimize,
        Maximize,
        Close
    };

    struct ButtonState
    {
        RECT rect{};
        bool enabled = true;
        bool visible = true;
    };

    // Recomputes every button rect for the current client width and DPI.
    // `hasModel` hides the navigation-only buttons (Grid/Snap/Speed/Fit/Share/
    // Overflow) the same way the old toolbar disabled them with nothing
    // loaded. `isFullscreen` additionally hides Minimize/Maximize/Close/Open
    // With, which don't apply to the topmost, monitor-filling window the
    // floating toolbar overlays there (see Preview3D.cpp's ToggleFullscreen).
    void UpdateLayout(int clientWidth, int titleBarHeight, float dpiScale, bool hasModel, bool isMaximized,
        bool isFullscreen, bool updateAvailable);

    // Hit-tests a CLIENT-coordinate point (already converted from screen
    // coordinates by the caller) against every button, falling back to
    // Part::Caption inside the bar's empty drag area and Part::None outside it
    // entirely.
    Part HitTest(POINT clientPoint) const;

    const ButtonState& Button(Part part) const;
    RECT TitleBarRect() const { return titleBarRect_; }
    RECT FilenameRect() const { return filenameRect_; }
    bool Maximized() const { return isMaximized_; }

    Part hover = Part::None;
    Part pressed = Part::None;

private:
    RECT titleBarRect_{};
    RECT filenameRect_{};
    bool isMaximized_ = false;
    ButtonState grid_;
    ButtonState groundAxis_;
    ButtonState groundDirection_;
    ButtonState axisSnap_;
    ButtonState speed_;
    ButtonState orbit_;
    ButtonState fit_;
    ButtonState share_;
    ButtonState overflow_;
    ButtonState openWith_;
    ButtonState update_;
    ButtonState minimize_;
    ButtonState maximize_;
    ButtonState close_;
};
