// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// 3MF model files (3D Manufacturing Format): a zip package of XML parts.
// Only geometry is read; colours, materials and slicer settings are ignored.

#include <cstddef>
#include <string>

#include "mesh/TriangleMesh.h"

namespace fruit {

struct ThreeMfInfo {
    int meshes = 0;    // mesh objects placed (an object placed twice counts twice)
    int skipped = 0;   // build items or objects left out: not printable, or
                       // support / surface / other objects rather than models
    std::string unit;  // as written in the file ("millimeter" when absent)
};

// Every printable build item, with item and component transforms applied,
// converted to millimetres. Components may point into other model parts of
// the package (3MF Production extension, p:path), as Bambu Studio writes.
// Throws std::runtime_error with a readable message on bad input.
TriangleMesh read3mf(const std::string& path, ThreeMfInfo* info = nullptr);
TriangleMesh read3mfFromMemory(const void* data, std::size_t size, ThreeMfInfo* info = nullptr);

}  // namespace fruit
