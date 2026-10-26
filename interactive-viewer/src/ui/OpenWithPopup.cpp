#include "OpenWithPopup.h"

#include <algorithm>
#include <cmath>

namespace open_with_ui
{
namespace
{
int Scale(int logical, float scale)
{
    return static_cast<int>(std::lround(logical * std::max(0.75f, scale)));
}

bool Intersects(const RECT& left, const RECT& right)
{
    return left.left < right.right && left.right > right.left &&
        left.top < right.bottom && left.bottom > right.top;
}
}

Layout ComputeLayout(const LayoutInput& input)
{
    Layout layout;
    layout.appCount = std::max(0, input.appCount);

    const int padding = Scale(8, input.scale);
    const int tileWidth = Scale(96, input.scale);
    const int tileHeight = Scale(88, input.scale);
    const int tileGap = Scale(4, input.scale);
    const int dividerGap = Scale(8, input.scale);
    const int panelHeight = Scale(104, input.scale);
    const int fallbackWidth = Scale(96, input.scale);
    const int statusWidth = Scale(260, input.scale);

    const int shownTiles = layout.appCount > 0 && !input.loading
        ? std::min(layout.appCount, 6) : 0;
    const int naturalStripWidth = shownTiles > 0
        ? shownTiles * tileWidth + (shownTiles - 1) * tileGap
        : statusWidth;
    const int naturalPanelWidth = padding * 2 + naturalStripWidth + dividerGap + 1 +
        dividerGap + fallbackWidth;

    const int clientWidth = input.client.right - input.client.left;
    const int maximumPanelWidth = std::max(Scale(120, input.scale),
        std::min(Scale(760, input.scale), clientWidth - Scale(16, input.scale)));
    const int panelWidth = std::min(std::max(naturalPanelWidth, Scale(240, input.scale)),
        maximumPanelWidth);

    int left = static_cast<int>(input.anchor.right) - panelWidth;
    left = std::max(static_cast<int>(input.client.left) + Scale(8, input.scale),
        std::min(left, static_cast<int>(input.client.right) - Scale(8, input.scale) - panelWidth));
    int top = std::max(static_cast<int>(input.anchor.bottom) + Scale(6, input.scale),
        input.toolbarHeight + Scale(2, input.scale));
    top = std::min(top, static_cast<int>(input.client.bottom) - Scale(8, input.scale) - panelHeight);
    top = std::max(top, static_cast<int>(input.client.top) + Scale(8, input.scale));

    layout.panel = RECT{ left, top, left + panelWidth, top + panelHeight };
    const int tileTop = top + padding;
    const int fallbackLeft = static_cast<int>(layout.panel.right) - padding - fallbackWidth;
    layout.fallback = RECT{ fallbackLeft, tileTop, fallbackLeft + fallbackWidth, tileTop + tileHeight };
    const int dividerX = fallbackLeft - dividerGap - 1;
    layout.divider = RECT{ dividerX, tileTop + Scale(12, input.scale),
        dividerX + 1, tileTop + tileHeight - Scale(12, input.scale) };

    const int stripLeft = static_cast<int>(layout.panel.left) + padding;
    const int stripRight = std::max(stripLeft, dividerX - dividerGap);
    layout.strip = RECT{ stripLeft, tileTop, stripRight, tileTop + tileHeight };
    layout.status = layout.strip;
    layout.statusVisible = input.loading || layout.appCount == 0;

    if (!layout.statusVisible)
    {
        const int contentWidth = layout.appCount * tileWidth +
            (layout.appCount - 1) * tileGap;
        const int stripWidth = static_cast<int>(layout.strip.right) - static_cast<int>(layout.strip.left);
        layout.maxScroll = std::max(0, contentWidth - stripWidth);
        layout.scrollOffset = std::clamp(input.scrollOffset, 0, layout.maxScroll);
        layout.tiles.reserve(static_cast<std::size_t>(layout.appCount));
        for (int index = 0; index < layout.appCount; ++index)
        {
            const int tileLeft = static_cast<int>(layout.strip.left) - layout.scrollOffset +
                index * (tileWidth + tileGap);
            RECT tile{ tileLeft, tileTop, tileLeft + tileWidth, tileTop + tileHeight };
            if (Intersects(tile, layout.strip)) layout.visibleTiles.push_back(index);
            layout.tiles.push_back(tile);
        }

        if (layout.maxScroll > 0 && stripWidth > 0)
        {
            layout.scrollbarVisible = true;
            const int trackTop = static_cast<int>(layout.panel.bottom) - Scale(6, input.scale);
            const int trackBottom = static_cast<int>(layout.panel.bottom) - Scale(4, input.scale);
            layout.scrollTrack = RECT{ layout.strip.left, trackTop, layout.strip.right, trackBottom };
            const int thumbWidth = std::min(stripWidth,
                std::max(Scale(24, input.scale), stripWidth * stripWidth / contentWidth));
            const int thumbTravel = stripWidth - thumbWidth;
            const int thumbLeft = static_cast<int>(layout.strip.left) +
                (layout.maxScroll > 0 ? layout.scrollOffset * thumbTravel / layout.maxScroll : 0);
            layout.scrollThumb = RECT{ thumbLeft, trackTop, thumbLeft + thumbWidth, trackBottom };
        }
    }
    return layout;
}

int AppTileAtPoint(const Layout& layout, POINT point)
{
    if (point.x < layout.strip.left || point.x >= layout.strip.right ||
        point.y < layout.strip.top || point.y >= layout.strip.bottom) return -1;
    for (std::size_t index = 0; index < layout.tiles.size(); ++index)
    {
        const RECT& tile = layout.tiles[index];
        if (point.x >= tile.left && point.x < tile.right &&
            point.y >= tile.top && point.y < tile.bottom)
            return static_cast<int>(index);
    }
    return -1;
}

int ClampScrollOffset(const Layout& layout, int offset)
{
    return std::clamp(offset, 0, layout.maxScroll);
}

int ScrollToRevealTile(const Layout& layout, int offset, int index)
{
    if (index < 0 || static_cast<std::size_t>(index) >= layout.tiles.size())
        return ClampScrollOffset(layout, offset);
    const RECT& tile = layout.tiles[static_cast<std::size_t>(index)];
    int result = offset;
    if (tile.left < layout.strip.left)
        result -= layout.strip.left - tile.left;
    else if (tile.right > layout.strip.right)
        result += tile.right - layout.strip.right;
    return ClampScrollOffset(layout, result);
}
}
