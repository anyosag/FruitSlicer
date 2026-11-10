// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The Measure tool's logic: two points on the job's parts and the distance
// between them, and the picking that finds a point under the mouse. Plain
// functions and a small state, so the tests can check them. Nothing here
// changes the job.

#include <optional>
#include <vector>

#include "Parts.h"
#include "geometry/Transform.h"
#include "geometry/Vec.h"
#include "mesh/AabbTree.h"
#include "mesh/TriangleMesh.h"
#include "view3d/Camera.h"

namespace gui {

class Document;

// The distance between two points, and its parts along the plate's axes
// (all in mm, never negative).
struct Measurement {
    fruit::Vec3 from, to;
    double distance = 0;
    double dx = 0, dy = 0, dz = 0;
};
Measurement measureBetween(const fruit::Vec3& from, const fruit::Vec3& to);

// The points picked so far: none, one, or two. A third click starts again
// with that point. Each is kept in its own part's coordinates, so it stays on
// that part when it is moved, turned, scaled or mirrored; the two can be on
// different parts.
struct MeasureState {
    struct Point {
        fruit::Vec3 at;        // in its part's own coordinates
        int part = 0;          // the part it is on (an id)
        int geometry_id = -1;  // that part's mesh when it was picked
    };
    std::vector<Point> points;

    void click(const fruit::Vec3& model_point, int part = 0, int geometry_id = -1);
    void clear() { points.clear(); }
    bool empty() const { return points.empty(); }
    bool complete() const { return points.size() == 2; }
};

// The points still lie on the job as it is: each one's part is in the job
// with the mesh it was picked on. Not when a part was removed (until undo
// brings it back) or its mesh was replaced (a repair).
bool measureStillOn(const MeasureState& s, const Document& doc);

// The points as they are on the plate now (`frames`: each part's model ->
// plate transform; a point whose part has none is left out), and the
// distance between the two once both are there.
std::vector<fruit::Vec3> pointsOnPlate(const MeasureState& s, const PartFrames& frames);
std::optional<Measurement> measurementOf(const MeasureState& s, const PartFrames& frames);

// A point on the model's surface found by a ray.
struct SurfacePoint {
    fruit::Vec3 point;
    fruit::Vec3 normal;  // unit, outward
};
// The nearest surface under the ray, or nothing when it misses.
std::optional<SurfacePoint> pickSurface(const fruit::AabbTree& tree, const fruit::Vec3& origin, const fruit::Vec3& dir);
// The ray through pixel (`x`, `y`) of a view whose top left is (`left`, `top`)
// and which is `width` by `height` pixels.
void rayThroughPixel(const Camera& camera, float x, float y, float left, float top, float width, float height,
                     fruit::Vec3* origin, fruit::Vec3* dir);

}  // namespace gui
