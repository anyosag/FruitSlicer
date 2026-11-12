// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The view cube at the 3D view's top right (canvas "View cube"): a cube that
// turns with the camera, its faces named. A click on a face's middle looks
// from that side, a click near an edge from between two sides, a click on a
// corner from between three; dragging the cube turns the view. Plain maths,
// so the tests can check it.

#include <string>
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

// Where on the cube a point is: the middle of a face, one of its edges or
// one of its corners. The outer part of a face (past kCubeEdge of the way
// from its middle) belongs to the edge or corner there.
struct CubeSpot {
    int face = -1;                // the face it is on, index into the faces; -1: not on the cube
    V3 dir;                       // the direction to look from: each of x, y, z is -1, 0 or 1
    float x[4] = {}, y[4] = {};   // the spot's outline on screen, from the cube's centre, y down
    // How many sides it looks from between: 1 a face, 2 an edge, 3 a corner.
    int sides() const { return (dir.x != 0) + (dir.y != 0) + (dir.z != 0); }
};
inline constexpr float kCubeEdge = 0.56f;
// The spot under a point (from the cube's centre), on the front-most face there.
CubeSpot cubeSpotAt(const std::vector<CubeFace>& faces, float x, float y);
// The side or sides a direction looks from, top or bottom first: "front",
// "front right", "top front right".
std::string cubeSpotName(V3 dir);

// The cube on screen: pixels from its centre to a face.
inline constexpr float kCubeHalf = 30;

// Dragging the cube turns the view by this many degrees for each pixel: the
// cube's surface about follows the pointer (a ball around the cube is some
// 42 pixels in radius).
inline constexpr float kCubeDragDegPerPx = 1.4f;

}  // namespace gui
