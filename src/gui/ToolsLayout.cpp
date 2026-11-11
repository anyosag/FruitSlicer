// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "ToolsLayout.h"

#include <algorithm>
#include <cmath>

namespace gui {

ToolsBarFit fitToolsBar(float view_w, int buttons) {
    ToolsBarFit b;
    const float n = static_cast<float>(std::max(1, buttons));
    // The card's padding (8 each side) and border.
    auto width = [&b, n] { return n * b.button_w + (n - 1) * b.gap + 2 * 8 + 2; };
    const float room = view_w - 2 * kAxesRoom;
    if (width() > room) b.gap = 2;
    if (width() > room) b.button_w = std::max(56.0f, std::floor((room - 18 - (n - 1) * b.gap) / n));
    b.w = width();
    b.x = std::max(std::round((view_w - b.w) / 2), std::min(kAxesRoom, view_w - 16 - b.w));
    return b;
}

float railMaxHeight(float view_h, float top) { return std::max(120.0f, view_h - top - 104); }

}  // namespace gui
