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

// The tools around the 3D view (canvas "Tool rail", "Model tools").
void folder(ImDrawList* dl, ImVec2 p, float size, ImU32 c);      // Open
void onPlate(ImDrawList* dl, ImVec2 p, float size, ImU32 c);     // a block coming down onto the plate
void move(ImDrawList* dl, ImVec2 p, float size, ImU32 c);        // four arrows
void rotate(ImDrawList* dl, ImVec2 p, float size, ImU32 c);      // a turning arrow
void scale(ImDrawList* dl, ImVec2 p, float size, ImU32 c);       // a box and an arrow out of its corner
void mirror(ImDrawList* dl, ImVec2 p, float size, ImU32 c);      // two triangles either side of a dashed line
void measure(ImDrawList* dl, ImVec2 p, float size, ImU32 c);     // a line with a stop at each end
void duplicate(ImDrawList* dl, ImVec2 p, float size, ImU32 c);   // a box and its copy behind
void arrange(ImDrawList* dl, ImVec2 p, float size, ImU32 c);     // four boxes laid out
void merge(ImDrawList* dl, ImVec2 p, float size, ImU32 c);       // two boxes, an arrow joining them
void split(ImDrawList* dl, ImVec2 p, float size, ImU32 c);       // a box in two halves
void supports(ImDrawList* dl, ImVec2 p, float size, ImU32 c);    // a slab on legs
void hollow(ImDrawList* dl, ImVec2 p, float size, ImU32 c);      // a box with a dashed box inside
void drainHole(ImDrawList* dl, ImVec2 p, float size, ImU32 c);   // a hole and a drop
void repair(ImDrawList* dl, ImVec2 p, float size, ImU32 c);      // a sticking plaster
void cursor(ImDrawList* dl, ImVec2 p, float size, ImU32 c);      // select and move
void addTip(ImDrawList* dl, ImVec2 p, float size, ImU32 c);      // a tip and a plus
void zone(ImDrawList* dl, ImVec2 p, float size, ImU32 c);        // a dashed box, struck through
void back(ImDrawList* dl, ImVec2 p, float size, ImU32 c);        // a chevron pointing left
void undo(ImDrawList* dl, ImVec2 p, float size, ImU32 c);
void redo(ImDrawList* dl, ImVec2 p, float size, ImU32 c);
void lookUp(ImDrawList* dl, ImVec2 p, float size, ImU32 c);      // from below: an arrow up to a line
void dots(ImDrawList* dl, ImVec2 p, float size, ImU32 c);        // ⋮, a menu

}  // namespace gui::icon
