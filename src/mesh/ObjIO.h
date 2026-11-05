// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once

#include <string>

#include "mesh/TriangleMesh.h"

namespace fruit {

// Minimal Wavefront OBJ reader: `v` and `f` records (polygons fan-triangulated,
// v/vt/vn and negative indices supported); everything else is ignored.
TriangleMesh readObj(const std::string& path);

// Reads .stl or .obj by extension (case-insensitive).
TriangleMesh readMesh(const std::string& path);

}  // namespace fruit
