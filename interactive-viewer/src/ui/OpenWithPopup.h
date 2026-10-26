#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <vector>

namespace open_with_ui
{
struct Layout
{
    RECT panel{};
    RECT strip{};        // scrolling app-tile viewport
    RECT fallback{};     // pinned More apps tile
    RECT divider{};
    RECT scrollTrack{};
    RECT scrollThumb{};
    RECT status{};       // loading/empty text block, inside `strip`
    std::vector<RECT> tiles;      // every app tile, positioned with scrollOffset
    std::vector<int> visibleTiles; // indices intersecting `strip`
    int appCount = 0;
    int scrollOffset = 0;
    int maxScroll = 0;
    bool scrollbarVisible = false;
    bool statusVisible = false;
};

struct LayoutInput
{
    RECT anchor{};       // Open With button, client pixels
    RECT client{};       // client area
    int toolbarHeight = 52;
    float scale = 1.0f;
    int appCount = 0;
    bool loading = false;
    int scrollOffset = 0;
};

Layout ComputeLayout(const LayoutInput& input);

// App index under `point`, or -1. Only points inside the scrolling strip count.
int AppTileAtPoint(const Layout& layout, POINT point);

int ClampScrollOffset(const Layout& layout, int offset);

// Returns the scroll offset that makes tile `index` fully visible, clamped.
int ScrollToRevealTile(const Layout& layout, int offset, int index);
}
