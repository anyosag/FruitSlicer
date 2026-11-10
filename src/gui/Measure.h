// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The Measure tool's logic: two points on the model and the distance between
// them, and the picking that finds a point under the mouse. Plain functions
// and a small state, so the tests can check them. Nothing here changes the job.

#include <optional>
#include <vector>

#include "geometry/Transform.h"
#include "geometry/Vec.h"
#include "mesh/AabbTree.h"
#include "mesh/TriangleMesh.h"
#include "view3d/Camera.h"

namespace gui {

// The distance between two points, and its parts along the plate's axes
// (all in mm, never negative).
struct Measurement {
    fruit::Vec3 from, to;
    double distance = 0;
    double dx = 0, dy = 0, dz = 0;
};
Measurement measureBetween(const fruit::Vec3& from, const fruit::Vec3& to);

// The points picked so far: none, one, or two. A third click starts again
// with that point. They are kept in the model's own coordinates, so they stay
// on the model when it is moved, turned or scaled (`geometry_id` says which
// model they belong to).
struct MeasureState {
    std::vector<fruit::Vec3> points;  // model coordinates
    int geometry_id = -1;

    void click(const fruit::Vec3& model_point);
    void clear() { points.clear(); }
    bool empty() const { return points.empty(); }
    bool complete() const { return points.size() == 2; }
};

// The points as they are on the plate now (`model_to_plate`: the model's
// transform), and the distance between the two once both are there.
std::vector<fruit::Vec3> pointsOnPlate(const MeasureState& s, const fruit::Transform& model_to_plate);
std::optional<Measurement> measurementOf(const MeasureState& s, const fruit::Transform& model_to_plate);

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
