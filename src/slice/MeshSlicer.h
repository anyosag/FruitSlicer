// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Plane slicing of a closed triangle mesh into 2D polygons.
//
// Segments are chained by mesh edge identity (not by coordinates), so the
// result is topologically exact for manifold meshes. Every shell is then
// merged with a NonZero union: overlapping and self-intersecting shells
// (kit-bashed miniatures, model + support primitives) union correctly
// instead of turning overlaps into holes.

#include <vector>

#include "core/JobControl.h"
#include "geometry/Polygon.h"
#include "mesh/TriangleMesh.h"

namespace fruit {

struct SliceStats {
    size_t open_chains = 0;  // chains that did not close (non-manifold input)
};

// Slice at each z in `zs`. Returns unioned polygons per height
// (outer contours CCW, holes CW). Runs in parallel over heights (`threads`,
// 0 = all cores) and reports stage "slice".
std::vector<Paths> sliceMesh(const TriangleMesh& mesh, const std::vector<double>& zs,
                             SliceStats* stats = nullptr, unsigned threads = 0,
                             const JobControl& ctl = JobControl::none());

Paths sliceMeshAt(const TriangleMesh& mesh, double z);

}  // namespace fruit
