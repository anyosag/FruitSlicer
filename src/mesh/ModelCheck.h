// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// What the app tells you about a model before it goes on the plate: is it a
// clean closed solid, does it fit, and how large it can be.

#include <cstddef>

#include "mesh/ModelLoad.h"
#include "mesh/Placement.h"
#include "printer/PrinterProfile.h"

namespace fruit {

struct MeshCheck {
    std::size_t triangles = 0, points = 0;
    int shells = 0;              // connected pieces
    int flipped = 0;             // inside-out shells turned around while loading
    int cavities = 0;            // closed hollows inside the model
    std::size_t open_edges = 0;  // edges still not used as often one way round as the other
    RepairReport repair;         // what loading did to close it
    double volume_ml = 0;        // at 100 %
    BBox3 bbox;                  // model coordinates at 100 %

    bool closed() const { return open_edges == 0; }
    // Under 12 mm on its longest side: perhaps drawn in inches (×25.4).
    bool maybeInches() const;
};

MeshCheck checkMesh(const LoadedModel& m);

// Room kept free between the model and the plate's edges: the raft's pads
// reach past the model's outline when it is raised on supports.
double plateMargin(const Placement& p);

struct PlateFit {
    Vec3 size;                  // placed model, mm
    double top = 0;             // highest point above the plate (size.z + lift)
    bool fits = false;          // within the margins and the build height
    double largest = 0;         // largest scale that fits as rotated now (1.0 = 100 %), floored to 1 %
    double largest_turned = 0;  // the same, turned 90° about the vertical
};

// Fit of `model` (its own coordinates) placed per `p`.
PlateFit checkFit(const TriangleMesh& model, const Placement& p, const PrinterProfile& printer);

// Downward-pointing spots of `placed` that start in mid-air and need a
// support (spots resting on the plate do not count when it sits on it).
std::size_t countLowPoints(const TriangleMesh& placed, bool on_plate);

}  // namespace fruit
