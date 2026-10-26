#include "OpenWithPopup.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
open_with_ui::LayoutInput BaseInput(int appCount, int scrollOffset = 0)
{
    open_with_ui::LayoutInput input;
    input.anchor = RECT{ 700, 41, 772, 71 };
    input.client = RECT{ 0, 0, 1000, 720 };
    input.toolbarHeight = 52;
    input.scale = 1.0f;
    input.appCount = appCount;
    input.scrollOffset = scrollOffset;
    return input;
}
}

TEST_CASE("Open With popup lays out one horizontal strip under the button", "[open-with]")
{
    const auto layout = open_with_ui::ComputeLayout(BaseInput(4));
    CHECK(layout.panel.right == 772);
    CHECK(layout.panel.top == 77);
    CHECK(layout.panel.bottom - layout.panel.top == 104);
    CHECK(layout.tiles.size() == 4);
    CHECK(layout.visibleTiles.size() == 4);
    CHECK(layout.scrollbarVisible == false);
    CHECK(layout.statusVisible == false);
    CHECK(layout.fallback.right == 764);
    CHECK(layout.fallback.left == 668);
    CHECK(layout.divider.left == 659);
    CHECK(layout.strip.left == 255);
    CHECK(layout.strip.right == 651);

    CHECK(open_with_ui::AppTileAtPoint(layout, POINT{ 300, 90 }) == 0);
    CHECK(open_with_ui::AppTileAtPoint(layout, POINT{ 360, 90 }) == 1);
    CHECK(open_with_ui::AppTileAtPoint(layout, POINT{ 690, 90 }) == -1);
    CHECK(open_with_ui::AppTileAtPoint(layout, POINT{ 10, 90 }) == -1);
}

TEST_CASE("Open With popup scrolls horizontally and keeps the fallback pinned", "[open-with]")
{
    const auto layout = open_with_ui::ComputeLayout(BaseInput(20));
    CHECK(layout.appCount == 20);
    CHECK(layout.maxScroll > 0);
    CHECK(layout.scrollbarVisible);
    CHECK(layout.visibleTiles.size() == 6);
    CHECK(layout.visibleTiles.front() == 0);
    CHECK(layout.fallback.left == 668);

    const int reveal = open_with_ui::ScrollToRevealTile(layout, 0, 8);
    CHECK(reveal == 300);
    const auto scrolled = open_with_ui::ComputeLayout(BaseInput(20, reveal));
    CHECK(scrolled.visibleTiles.front() == 3);
    CHECK(open_with_ui::AppTileAtPoint(scrolled, POINT{ 60, 90 }) == 3);
}

TEST_CASE("Open With popup clamps to the client and minimum width", "[open-with]")
{
    auto input = BaseInput(20);
    input.anchor = RECT{ 360, 41, 432, 71 };
    input.client = RECT{ 0, 0, 480, 360 };
    const auto layout = open_with_ui::ComputeLayout(input);
    CHECK(layout.panel.left >= 8);
    CHECK(layout.panel.right <= 472);
    CHECK(layout.panel.right - layout.panel.left >= 240);
    CHECK(layout.panel.bottom <= 352);
    CHECK(layout.tiles.size() == 20);
    CHECK(layout.visibleTiles.size() >= 1);
    CHECK(layout.maxScroll > 0);
}

TEST_CASE("Open With popup shows a status block instead of tiles", "[open-with]")
{
    auto input = BaseInput(0);
    input.loading = true;
    const auto layout = open_with_ui::ComputeLayout(input);
    CHECK(layout.statusVisible);
    CHECK(layout.tiles.empty());
    CHECK(layout.maxScroll == 0);
    CHECK(layout.fallback.left < layout.fallback.right);
    CHECK(layout.status.left == layout.strip.left);
    CHECK(layout.status.right == layout.strip.right);
}

TEST_CASE("Open With popup scales with DPI", "[open-with]")
{
    auto input = BaseInput(4);
    input.scale = 1.5f;
    const auto layout = open_with_ui::ComputeLayout(input);
    CHECK(layout.panel.bottom - layout.panel.top == 156); // 104 * 1.5
    CHECK(layout.tiles.size() == 4);
    CHECK(layout.tiles.front().right - layout.tiles.front().left == 144); // 96 * 1.5
}
