// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Where a model sits on the build plate.

#include "geometry/Transform.h"
#include "mesh/TriangleMesh.h"

namespace fruit {

// How the user placed a model: uniform scale first, then rotation about X,
// then Y, then Z (degrees), then centred on the plate in XY with its lowest
// point `elevation` mm above the plate (0 when it stands on the plate).
struct Placement {
    double scale = 1.0;
    Vec3 rotation_deg{0, 0, 0};
    double elevation = 5.0;  // lift above the plate (raft + supports underneath)
    bool on_plate = false;   // flat on the plate: no lift, no raft

    double lift() const { return on_plate ? 0.0 : elevation; }
};

// Rotate (degrees, X then Y then Z), then translate so the model is centred
// on the plate in XY and its lowest point sits at `elevation` mm. Returns the
// transform that was applied.
Transform placeOnPlate(TriangleMesh& mesh, const Vec3& rotation_deg, double elevation);

// The model-to-printer transform for `p`, computed from `model`'s vertices.
Transform placementTransform(const TriangleMesh& model, const Placement& p);

// `model` (in its own coordinates) placed on the plate per `p`.
TriangleMesh placeModel(const TriangleMesh& model, const Placement& p);

}  // namespace fruit
