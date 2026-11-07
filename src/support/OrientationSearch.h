// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Orientation search (milestone M3). Orientation is the biggest single lever
// on support count, scar placement and peel force (Vanek 2014, Zhang 2015,
// vendor guides), so it is searched rather than left to the user:
//
//  stage 1 — every candidate build direction is scored with cheap mesh
//            proxies: area that needs support, number of downward minima
//            (islands), peak layer area on a coarse slice, height, and
//            needing-support area on cosmetic faces;
//  stage 2 — the best few are run through the real point generator and
//            ranked by points, points on cosmetic faces, peak layer area and
//            height.
//
// Rotation about Z does not change supports, so candidates are build
// directions ("which way the model faces the plate") spread over a sphere.

#include <vector>

#include "geometry/Transform.h"
#include "mesh/TriangleMesh.h"
#include "support/CosmeticField.h"
#include "support/PointGenerator.h"
#include "support/SupportConfig.h"

namespace fruit {

struct OrientWeights {
    double support_area = 1.0;
    double minima = 1.0;
    double peak_area = 0.5;
    double height = 0.3;
    double cosmetic = 3.0;
};

struct OrientOptions {
    int directions = 64;       // Fibonacci-sphere candidates (plus the input orientation)
    int finalists = 6;         // evaluated with the full generator
    double proxy_layer = 0.5;  // coarse slicing for the peak-area proxy
    double elevation = 5.0;
    SupportConfig cfg;         // used for stage 2 (its layer height too)
    OrientWeights weights;
    // Cosmetic hints in MODEL coordinates (they rotate with the model).
    std::vector<Vec3> visible_from;
    double visible_cos = 0.3;
    // Candidates scored at the same time (0 = all cores). Results do not
    // depend on it.
    unsigned threads = 0;
    bool keep_points = true;  // keep each finalist's support points
};

struct OrientationResult {
    Transform rotation;          // applied to the input mesh
    Vec3 euler_deg;              // same rotation as --rotate X Y Z
    Vec3 down_model;             // model-frame direction facing the plate
    bool is_input = false;       // the orientation the model came in with
    // stage 1
    double support_area = 0, peak_area = 0, height = 0, cosmetic_area = 0;
    size_t minima = 0;
    double proxy = 0;
    // stage 2 (finalists only)
    bool evaluated = false;
    size_t points = 0, cosmetic_points = 0, islands = 0;
    size_t uncovered = 0, capacity_deficits = 0;
    double peak_layer_area = 0;  // mm², from the full slicing (peak_area is the coarse proxy)
    double score = 0;
    // The finalist's points (placed at OrientOptions::elevation), when
    // keep_points is set, so choosing it needs no second generator run.
    std::vector<SupportPoint> support_points;
};

// Returns finalists sorted best first, followed by the other candidates
// sorted by proxy score. Reports stages "orient.score" (first 25 %) and
// "orient.evaluate".
std::vector<OrientationResult> searchOrientations(const TriangleMesh& model, const OrientOptions& o,
                                                  const JobControl& ctl = JobControl::none());

// Euler angles (deg) with R = Rz·Ry·Rx, matching Transform::rotationXYZ.
Vec3 eulerXYZ(const Transform& r);

}  // namespace fruit
