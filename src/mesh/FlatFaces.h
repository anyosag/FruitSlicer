// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The flat faces a model can rest on. A flat face is a patch of connected
// triangles that lie in one plane; it can rest on the plate when the whole
// model is on its inner side (the face is on the model's convex hull), so
// laying it down never pushes the model through the plate. Everything is in
// the mesh's own units and coordinates.

#include <cstdint>
#include <vector>

#include "mesh/TriangleMesh.h"

namespace fruit {

struct FlatFaceOptions {
    double max_angle_deg = 1.0;    // a triangle's normal may differ this much from the patch's first (largest) triangle
    double plane_tolerance = 0.02;  // and its corners may lie this far (mm) off that triangle's plane
    double rest_tolerance = 0.05;   // no vertex of the mesh may lie farther than this (mm) beyond the patch's plane
    double min_area = 5.0;          // patches smaller than this (mm²) are slivers: left out
};

struct FlatFace {
    Vec3 normal;                // outward, unit
    double area = 0;            // mm²
    Vec3 centroid;              // area-weighted middle of its triangles
    std::vector<int32_t> faces;  // its triangles (indices into the mesh's faces)
};

struct FlatFaces {
    std::vector<FlatFace> faces;           // the ones the model can rest on, largest first
    std::vector<int32_t> face_region;      // per triangle of the mesh: its index in `faces`, or -1
    // The patch a triangle belongs to (an index into `faces`), or -1.
    int regionOf(std::size_t triangle) const {
        return triangle < face_region.size() ? face_region[triangle] : -1;
    }
};

FlatFaces findFlatFaces(const TriangleMesh& mesh, const FlatFaceOptions& options = {});

}  // namespace fruit
