// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "geometry/BBox.h"
#include "geometry/Transform.h"
#include "geometry/Vec.h"

namespace fruit {

using Face = std::array<int32_t, 3>;  // counter-clockwise seen from outside

class TriangleMesh {
public:
    std::vector<Vec3> vertices;
    std::vector<Face> faces;

    size_t vertexCount() const { return vertices.size(); }
    size_t faceCount() const { return faces.size(); }
    bool empty() const { return faces.empty(); }

    BBox3 bbox() const;
    double volume() const;        // signed; positive for outward-facing closed meshes
    double surfaceArea() const;
    Vec3 faceNormal(size_t f) const;  // unit normal (zero for degenerate faces)
    double faceArea(size_t f) const;

    void transform(const Transform& t);
    void translate(const Vec3& d);
    void append(const TriangleMesh& other);

    // Merge vertices closer than `tolerance` mm and drop degenerate faces.
    // Returns the number of vertices removed.
    size_t weld(double tolerance = 1e-6);

    // Connected components (by shared vertices). Returns a shell id per face.
    std::vector<int> shellIds(int* shell_count = nullptr) const;
    // Flip shells whose signed volume is negative (inside-out exports), but
    // keep negative shells that lie inside another shell: those are cavities
    // of hollowed models. Returns the number of shells flipped.
    int orientShellsOutward(int* cavities = nullptr);

    // Every edge used exactly twice, once in each direction.
    bool isClosedManifold() const;
};

}  // namespace fruit
