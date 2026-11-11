// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Where the tools around the 3D view go when the view is small: the model
// tools' bar along its foot tightens and keeps clear of the axes at its
// bottom left, and the rail scrolls above them. Plain maths, so the tests can
// check it.

namespace gui {

inline constexpr int kToolsBarButtons = 9;  // Supports, Hollow, Drain holes, Repair, Label, Combine, Cut, Merge, Split
inline constexpr float kAxesRoom = 112;     // the axes at the view's bottom left, and as much kept at the right

struct ToolsBarFit {
    float x = 0;  // from the view's left edge
    float w = 0;
    float button_w = 68, gap = 10;
};
// The bar for a view `view_w` wide: centred with 68 px buttons 10 px apart
// when that fits between the axes and as much room at the right; else the
// gaps close to 2 px, then the buttons narrow (not below 56 px), and it moves
// right of the axes, as far as it fits.
ToolsBarFit fitToolsBar(float view_w, int buttons = kToolsBarButtons);
// The most the rail's tools card may take, from its top `top` down a view
// `view_h` tall: it ends above the axes (it scrolls when taller), never under 120.
float railMaxHeight(float view_h, float top);

}  // namespace gui
