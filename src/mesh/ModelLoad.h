// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Reading a model file the way every command (and the app) needs it: shells
// counted and turned outward-facing, cavities kept.

#include <string>

#include "mesh/TriangleMesh.h"

namespace fruit {

struct LoadedModel {
    TriangleMesh mesh;        // model coordinates, every shell facing outward
    int shells = 0;           // connected pieces
    int shells_flipped = 0;   // inside-out shells that were turned around
    int cavities = 0;         // shells kept as inner cavities
};

// STL or OBJ by extension. Throws std::runtime_error on unreadable input.
LoadedModel loadModel(const std::string& path);

// Same, for a mesh already in memory (e.g. read from a 3MF archive).
LoadedModel prepareModel(TriangleMesh mesh);

}  // namespace fruit
