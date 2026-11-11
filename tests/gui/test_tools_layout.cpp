// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The tools around the 3D view in a smaller window: the bar along its foot
// tightens and keeps clear of the axes, and the rail scrolls above them.
#include <catch2/catch_test_macros.hpp>

#include "ToolsLayout.h"

using namespace gui;

TEST_CASE("Tools bar: full size and centred when the view is wide enough", "[layout]") {
    // A 1440 px window leaves the view about 1025 px wide.
    const ToolsBarFit b = fitToolsBar(1025);
    CHECK(b.button_w == 68);
    CHECK(b.gap == 10);
    CHECK(b.w == 9 * 68 + 8 * 10 + 18);
    CHECK(b.x == (1025 - b.w) / 2 + 0.5f);  // centred (rounded)
    CHECK(b.x >= kAxesRoom);
}

TEST_CASE("Tools bar: tightens in a narrower view, still clear of the axes", "[layout]") {
    for (const float w : {865.0f, 800.0f, 760.0f, 700.0f, 670.0f}) {
        INFO("view " << w);
        const ToolsBarFit b = fitToolsBar(w);
        CHECK(b.gap == 2);
        CHECK(b.button_w >= 56);
        CHECK(b.button_w <= 68);
        CHECK(b.w == 9 * b.button_w + 8 * b.gap + 18);
        CHECK(b.x >= kAxesRoom);           // right of the axes
        CHECK(b.x + b.w <= w - 16);        // and inside the view
    }
    // Wider buttons while there is room for them.
    CHECK(fitToolsBar(865).button_w > fitToolsBar(700).button_w);
    // With no room right of the axes, as far right as it goes.
    const ToolsBarFit b = fitToolsBar(640);
    CHECK(b.x + b.w == 640 - 16);
}

TEST_CASE("Tools bar: in the smallest window it stays inside the view", "[layout]") {
    // The window's least size, 960 px, leaves the view 545 px.
    const ToolsBarFit b = fitToolsBar(545);
    CHECK(b.button_w == 56);
    CHECK(b.x >= 0);
    CHECK(b.x + b.w <= 545);
}

TEST_CASE("Rail: ends above the axes, scrolling when the view is short", "[layout]") {
    // 8 tools take about 531 px from 98 px down; a 900 px window's view is 802 px tall.
    CHECK(railMaxHeight(802, 98) >= 531);
    CHECK(railMaxHeight(600, 98) == 600 - 98 - 104);
    CHECK(railMaxHeight(300, 98) == 120);  // never less
}
