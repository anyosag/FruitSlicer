// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Arrange: put several footprints (rectangles seen from above) on the plate.
// Plain rectangles, not outlines: a part's footprint is its bounding box in
// XY. Deterministic: the same input always gives the same answer.

#include <vector>

#include "geometry/Vec.h"
#include "mesh/TriangleMesh.h"

namespace fruit::ops {

struct Footprint {
    double width = 0;  // X, mm
    double depth = 0;  // Y, mm
};

// The XY size of a mesh's bounding box.
Footprint footprintOf(const TriangleMesh& mesh);

// A turn about the vertical (degrees, counter-clockwise seen from above) and
// the size of the rectangle round an outline turned by it.
struct Turn {
    double degrees = 0;
    Footprint size;
};
// The turn, between -45 and 45 degrees, that gives `outline` (a convex
// polygon seen from above) its smallest rectangle: one side of the smallest
// rectangle always lies along an edge of the outline. No turn when the
// rectangle it has now is within 0.5 % of that.
Turn smallestTurn(const std::vector<Vec2>& outline);

struct ArrangeOptions {
    double plate_width = 0;   // X, mm
    double plate_depth = 0;   // Y, mm
    double gap = 2.0;         // least distance between two footprints, mm
    double edge_margin = 0.0; // least distance from a footprint to the plate's edge, mm
    bool allow_rotate = false;  // may turn a footprint a quarter turn (swap width and depth)
};

struct ArrangedPart {
    bool placed = false;
    Vec2 centre{0, 0};    // footprint's middle, mm from the plate's centre (+X right, +Y back)
    bool rotated = false; // turned a quarter turn: its width runs along Y
};

struct ArrangeResult {
    std::vector<ArrangedPart> parts;  // same order and size as the input
    std::vector<int> not_placed;      // input indices that did not fit, ascending
    bool all_placed() const { return not_placed.empty(); }
};

// Biggest footprints first, each into the free corner that leaves the least
// room over (maximal rectangles). A footprint with no size, or bigger than
// the plate, is not placed.
ArrangeResult arrange(const std::vector<Footprint>& footprints, const ArrangeOptions& options);

}  // namespace fruit::ops
