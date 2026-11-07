// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The canvas's line icons, drawn as vectors so they stay crisp at any scale.

#include <imgui.h>

namespace gui::icon {

// Each draws into a `size` × `size` box at `p`, in colour `c`.
void home(ImDrawList* dl, ImVec2 p, float size, ImU32 c);
void cube(ImDrawList* dl, ImVec2 p, float size, ImU32 c);
void target(ImDrawList* dl, ImVec2 p, float size, ImU32 c);
void droplet(ImDrawList* dl, ImVec2 p, float size, ImU32 c);
void wrench(ImDrawList* dl, ImVec2 p, float size, ImU32 c);
void gear(ImDrawList* dl, ImVec2 p, float size, ImU32 c);
void upload(ImDrawList* dl, ImVec2 p, float size, ImU32 c);
void check(ImDrawList* dl, ImVec2 p, float size, ImU32 c);
void cross(ImDrawList* dl, ImVec2 p, float size, ImU32 c);
// The FruitSlicer logo (a citrus slice), in the accent colour.
void logo(ImDrawList* dl, ImVec2 p, float size);

}  // namespace gui::icon
