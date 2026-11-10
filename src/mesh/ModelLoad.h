// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Reading a model file the way every command (and the app) needs it: holes
// and other faults repaired, shells counted and turned outward-facing,
// cavities kept.

#include <string>

#include "mesh/MeshRepair.h"
#include "mesh/TriangleMesh.h"

namespace fruit {

struct LoadedModel {
    TriangleMesh mesh;        // model coordinates, every shell facing outward
    int shells = 0;           // connected pieces
    int shells_flipped = 0;   // inside-out shells that were turned around
    int cavities = 0;         // shells kept as inner cavities
    RepairReport repair;      // what was done to make it watertight (see MeshRepair.h)
};

// STL, OBJ or 3MF by extension. Throws std::runtime_error on unreadable
// input. `repair` false reads the file as it is.
LoadedModel loadModel(const std::string& path, bool repair = true);

// Same, for a mesh already in memory.
LoadedModel prepareModel(TriangleMesh mesh, bool repair = true);

}  // namespace fruit
