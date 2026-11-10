// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The view cube at the 3D view's top right (canvas "View cube"): a cube that
// turns with the camera, its faces named; a click on a face looks from that
// side. Plain maths, so the tests can check it.

#include <vector>

#include "view3d/Math.h"

namespace gui {

// The sides, as the plate's axes: +X right, -X left, +Y back, -Y front
// (the side the printer's door faces), +Z top, -Z bottom.
enum class CubeSide { Right, Left, Back, Front, Top, Bottom };

struct CubeFace {
    CubeSide side;
    float x[4], y[4];  // corners on screen, from the cube's centre, y down
    float facing;      // how squarely it faces the camera: 0 edge on, 1 straight on
};

// The faces that face the camera, back to front (draw them in this order),
// for the camera's view matrix, `half` pixels from the centre to a face.
std::vector<CubeFace> cubeFaces(const M4& view, float half);
// The face under a point (from the cube's centre), the front-most one; -1 for none.
int cubeFaceAt(const std::vector<CubeFace>& faces, float x, float y);
const char* cubeLabel(CubeSide s);
// The direction from the model to the camera that looks at that side.
V3 cubeDirection(CubeSide s);

}  // namespace gui
