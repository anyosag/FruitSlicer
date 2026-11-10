// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Where a model sits on the build plate.

#include "geometry/Transform.h"
#include "mesh/TriangleMesh.h"

namespace fruit {

// How the user placed a model: uniform scale first, then rotation about X,
// then Y, then Z (degrees), then the middle of its outline put `offset` mm
// from the plate's centre in XY (0, 0: centred) with its lowest point
// `elevation` mm above the plate (0 when it stands on the plate).
struct Placement {
    double scale = 1.0;
    Vec3 rotation_deg{0, 0, 0};
    double elevation = 5.0;  // lift above the plate (raft + supports underneath)
    bool on_plate = false;   // flat on the plate: no lift, no raft
    Vec2 offset{0, 0};       // where on the plate, from its centre (+X right, +Y back)

    double lift() const { return on_plate ? 0.0 : elevation; }
    bool centred() const { return offset.x == 0 && offset.y == 0; }
    bool operator==(const Placement&) const = default;
};

// Rotate (degrees, X then Y then Z), then translate so the model is centred
// on the plate in XY and its lowest point sits at `elevation` mm. Returns the
// transform that was applied.
Transform placeOnPlate(TriangleMesh& mesh, const Vec3& rotation_deg, double elevation);

// The model-to-printer transform for `p`, computed from `model`'s vertices
// (its outline's middle at `p.offset`).
Transform placementTransform(const TriangleMesh& model, const Placement& p);

// `model` (in its own coordinates) placed on the plate per `p`.
TriangleMesh placeModel(const TriangleMesh& model, const Placement& p);

}  // namespace fruit
