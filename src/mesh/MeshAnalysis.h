// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Mesh-level analysis that must not depend on layer height or slicing offset:
// local minima (where an island first appears), sharp edges, local thickness.

#include <vector>

#include "mesh/AabbTree.h"
#include "mesh/TriangleMesh.h"

namespace fruit {

// A downward-facing local minimum of the surface. Flat plateaus (e.g. the
// bottom face of a box) are merged into one region.
struct MinimumRegion {
    Vec3 point;                   // centroid of the plateau vertices
    double z = 0;
    std::vector<int32_t> vertices;
    double radius = 0;            // max XY distance from centroid to a plateau vertex
};

// Vertex adjacency (one-ring neighbours via edges).
std::vector<std::vector<int32_t>> vertexNeighbours(const TriangleMesh& mesh);

// Vertices with no lower neighbour, plateaus merged within `z_eps`, excluding
// up-facing minima (e.g. the floor of a bowl). Sorted by z then x, y.
std::vector<MinimumRegion> findMinima(const TriangleMesh& mesh, double z_eps = 1e-6);

// Area-weighted vertex normals.
std::vector<Vec3> vertexNormals(const TriangleMesh& mesh);

// Distance from surface point p (with outward normal n) through the material
// to the opposite surface. Returns `max_dist` if nothing is hit.
double localThickness(const AabbTree& tree, const Vec3& p, const Vec3& n, double max_dist = 1e3);

}  // namespace fruit
