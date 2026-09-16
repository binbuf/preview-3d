#include "Chrome.h"

#include <algorithm>
#include <cmath>

namespace
{
int Scale(int logical, float dpiScale)
{
    return static_cast<int>(std::lround(logical * std::max(0.75f, dpiScale)));
}

RECT MakeRect(int left, int top, int width, int height)
{
    return RECT{ left, top, left + width, top + height };
}

bool PtInRectValue(const RECT& rect, POINT point)
{
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
}
}

void Chrome::UpdateLayout(int clientWidth, int titleBarHeight, float dpiScale, bool hasModel, bool isMaximized,
    bool isFullscreen)
{
    titleBarRect_ = RECT{ 0, 0, clientWidth, titleBarHeight };
    isMaximized_ = isMaximized;

    const int margin = Scale(10, dpiScale);
    const int sysButtonWidth = Scale(46, dpiScale);
    const int actionGap = Scale(4, dpiScale);
    const int actionHeight = Scale(30, dpiScale);
    const int actionY = (titleBarHeight - actionHeight) / 2;

    // System caption buttons: rightmost, full title-bar height (matches the
    // real caption buttons' hit target so DWM's Snap Layout hover flyout on
    // Maximize feels native). `isMaximized` only affects the drawn glyph
    // (D3D11On12Overlay::DrawTitleBar), not this layout. Visible/enabled regardless
    // of whether a model is loaded — so the window can always be minimized,
    // maximized, and closed — except in Fullscreen, where the topmost,
    // monitor-filling window has nothing for them to do (no border to snap,
    // no taskbar entry to restore to by dragging) and they'd just clutter
    // the floating toolbar; Preview3D.cpp's bottom-bar Fullscreen toggle is
    // the way out instead. Reserving their layout space either way (rather
    // than reclaiming it) keeps the action-button group's position stable
    // across the Fullscreen toggle.
    int right = clientWidth;
    close_.rect = MakeRect(right - sysButtonWidth, 0, sysButtonWidth, titleBarHeight); right -= sysButtonWidth;
    maximize_.rect = MakeRect(right - sysButtonWidth, 0, sysButtonWidth, titleBarHeight); right -= sysButtonWidth;
    minimize_.rect = MakeRect(right - sysButtonWidth, 0, sysButtonWidth, titleBarHeight); right -= sysButtonWidth;
    close_.enabled = close_.visible = !isFullscreen;
    maximize_.enabled = maximize_.visible = !isFullscreen;
    minimize_.enabled = minimize_.visible = !isFullscreen;

    right -= margin;

    // Right-anchored group: Open With, just left of the caption buttons.
    // Also hidden in Fullscreen — opening a different app to handle the file
    // would leave the topmost fullscreen window stranded above it.
    const int openWithWidth = Scale(72, dpiScale);
    openWith_.rect = MakeRect(right - openWithWidth, actionY, openWithWidth, actionHeight); right -= openWithWidth;
    openWith_.enabled = openWith_.visible = hasModel && !isFullscreen;

    const int rightGroupStart = right - margin;

    // Left-anchored group: Grid, Ground axis, Ground direction, Snap, Speed,
    // Fit, Reset, Share, Overflow —
    // small square icon buttons, in that order.
    struct Entry { ButtonState* state; int width; };
    const int iconButtonWidth = Scale(40, dpiScale);
    const int overflowWidth = Scale(34, dpiScale);
    const Entry entries[] = {
        { &grid_, iconButtonWidth }, { &groundAxis_, iconButtonWidth }, { &groundDirection_, iconButtonWidth },
        { &axisSnap_, iconButtonWidth }, { &speed_, iconButtonWidth },
        { &fit_, iconButtonWidth }, { &reset_, iconButtonWidth }, { &share_, iconButtonWidth },
        { &overflow_, overflowWidth },
    };
    int left = margin;
    for (const Entry& entry : entries)
    {
        entry.state->rect = MakeRect(left, actionY, entry.width, actionHeight);
        left += entry.width + actionGap;
        entry.state->enabled = entry.state->visible = hasModel;
    }

    const int leftGroupEnd = left - actionGap + margin;
    filenameRect_ = RECT{ std::min(leftGroupEnd, rightGroupStart), 0, std::max(leftGroupEnd, rightGroupStart), titleBarHeight };
}

Chrome::Part Chrome::HitTest(POINT clientPoint) const
{
    if (!PtInRectValue(titleBarRect_, clientPoint)) return Part::None;

    const struct { Part part; const ButtonState* state; } candidates[] = {
        { Part::Close, &close_ }, { Part::Maximize, &maximize_ }, { Part::Minimize, &minimize_ },
        { Part::OpenWith, &openWith_ }, { Part::Overflow, &overflow_ }, { Part::Share, &share_ },
        { Part::Reset, &reset_ }, { Part::Fit, &fit_ }, { Part::Speed, &speed_ },
        { Part::AxisSnap, &axisSnap_ }, { Part::GroundDirection, &groundDirection_ },
        { Part::GroundAxis, &groundAxis_ }, { Part::Grid, &grid_ },
    };
    for (const auto& candidate : candidates)
    {
        if (candidate.state->visible && candidate.state->enabled && PtInRectValue(candidate.state->rect, clientPoint))
        {
            return candidate.part;
        }
    }
    return Part::Caption;
}

const Chrome::ButtonState& Chrome::Button(Part part) const
{
    switch (part)
    {
    case Part::Grid: return grid_;
    case Part::GroundAxis: return groundAxis_;
    case Part::GroundDirection: return groundDirection_;
    case Part::AxisSnap: return axisSnap_;
    case Part::Speed: return speed_;
    case Part::Fit: return fit_;
    case Part::Reset: return reset_;
    case Part::Share: return share_;
    case Part::Overflow: return overflow_;
    case Part::OpenWith: return openWith_;
    case Part::Minimize: return minimize_;
    case Part::Maximize: return maximize_;
    case Part::Close: return close_;
    default: return overflow_;
    }
}
