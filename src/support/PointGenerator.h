// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Support point generation (milestone M1): the fewest tips such that
//
//  1. coverage — every sample of a surface that needs support has a tip
//     within r(Δz_eff) of it, where Δz_eff = min(height above the tip,
//     material thickness at the tip), measured through material (the 3D
//     segment between tip and sample stays inside the solid, so a tip never
//     "covers" across a slot or onto a neighbouring finger);
//  2. capacity — for every connected component at every layer, the summed
//     capacity of the tips holding it is at least its peel demand (layer
//     area × peel factor);
//  3. islands — every island gets its minimum count (1 tip ≤ 1.7 mm,
//     2 tips ≤ 6.5 mm, ≥ 3 for larger islands).
//
// Bottom-up over the layers, deterministic. Weighted greedy set cover
// (gain / cost; cost penalises cosmetic regions and sharp edges), then
// reverse-delete pruning that re-checks coverage and capacity.

#include <vector>

#include "mesh/TriangleMesh.h"
#include "support/CosmeticField.h"
#include "support/SupportConfig.h"
#include "support/SupportPoint.h"

namespace fruit {

struct PointGenStats {
    size_t layers = 0;
    size_t parts = 0;
    size_t islands = 0;
    size_t samples = 0;
    size_t uncovered = 0;           // samples left without a covering tip (should be 0)
    size_t tiny_islands_skipped = 0;
    size_t cavity_samples_skipped = 0;
    size_t suction_cups = 0;
    size_t resin_traps = 0;
    size_t capacity_deficits = 0;   // component-layers short of capacity after all fixes
    size_t greedy_points = 0;       // before pruning
    size_t pruned = 0;
    double peak_layer_area = 0;
    int peak_layer = 0;
    double seconds = 0;
};

struct PointGenResult {
    std::vector<SupportPoint> points;
    std::vector<Warning> warnings;
    PointGenStats stats;
};

enum class CoverStrategy {
    Greedy,      // weighted max-coverage greedy (default)
    FirstFit,    // baseline: every sample still uncovered becomes a tip, in
                 // scan order (the PrusaSlicer-style rule), for comparison
};

struct PointGenOptions {
    bool prune = true;
    CoverStrategy strategy = CoverStrategy::Greedy;
    std::vector<SupportPoint> manual;  // pinned user points (snapped to the surface)
    const CosmeticSpec* cosmetic = nullptr;
};

// `mesh` must already be placed in printer coordinates (lowest point at the
// elevation, Z up).
PointGenResult generateSupportPoints(const TriangleMesh& mesh, const SupportConfig& cfg,
                                     const PointGenOptions& opts = {});

}  // namespace fruit
