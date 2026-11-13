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
//     2 tips ≤ 6.5 mm, ≥ 3 for larger islands);
//  4. each tip's own load (when asked: PointGenOptions::relieve) — no tip
//     carries more than it is trusted with, as the replay of the print shares
//     each layer's pull among the tips (SupportConfig::tip_load_limit). Rule
//     2 is about the tips of a piece together; this one is about each.
//
// Bottom-up over the layers, deterministic. Weighted greedy set cover
// (gain / cost; cost penalises cosmetic regions and sharp edges), then
// reverse-delete pruning that re-checks coverage and capacity.

#include <limits>
#include <memory>
#include <vector>

#include "core/JobControl.h"
#include "layers/HollowAnalysis.h"
#include "layers/IslandGraph.h"
#include "layers/LayerStack.h"
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
    // Rule 4: how often the loads were worked out, the tips added beside one
    // that carried too much, the tips made a size bigger instead, and the
    // tips still over their load when it stopped (no room, or out of rounds).
    size_t relief_rounds = 0;
    size_t relief_added = 0;
    size_t relief_upgraded = 0;
    size_t relief_left = 0;
    double peak_layer_area = 0;
    int peak_layer = 0;
    double seconds = 0;
};

// What the generator learned about the model's layers. Shared, read-only,
// so a GUI can draw islands and hollows without slicing the model again.
struct LayerAnalysis {
    std::shared_ptr<const LayerStack> layers;
    std::shared_ptr<const IslandGraph> islands;
    std::shared_ptr<const HollowReport> hollows;
};

struct PointGenResult {
    std::vector<SupportPoint> points;
    std::vector<Warning> warnings;
    PointGenStats stats;
    LayerAnalysis analysis;  // the layers the points were generated from
    // With cfg.on_plate: the points and warnings are the same for any
    // plate_hold_height that holds at least `held_layers_min` of the lowest
    // layers and at most `held_layers_max` (a layer is held when its
    // underside is below that height). A layer counts where a piece that
    // stands on the plate would get tips or a flat-face warning, and where a
    // separate piece starts above the first layer (held, it stands on the
    // plate; not held, it starts in mid-air); when another piece floats, also
    // where one that stands has any newly exposed underside. 0 and the
    // largest int when no layer counts: then that height changes nothing.
    int held_layers_min = 0, held_layers_max = std::numeric_limits<int>::max();
};

enum class CoverStrategy {
    Greedy,      // weighted max-coverage greedy (default)
    FirstFit,    // baseline: every sample still uncovered becomes a tip, in
                 // scan order (the PrusaSlicer-style rule), for comparison
};

struct PointGenOptions {
    bool prune = true;
    // Rule 4: after the tips are planned, work out what each would carry;
    // those that carry too much get a size bigger, or tips beside them. The
    // loads are worked out afresh each round (the model, layer by layer), so
    // the orientation search and the checks leave it off.
    bool relieve = false;
    CoverStrategy strategy = CoverStrategy::Greedy;
    std::vector<SupportPoint> manual;  // pinned user points (snapped to the surface)
    const CosmeticSpec* cosmetic = nullptr;
    // Layers of this exact mesh at cfg.layer_height, if already sliced (they
    // are checked for a matching layer height); otherwise the generator
    // slices the mesh itself.
    std::shared_ptr<const LayerStack> layers;
    unsigned slice_threads = 0;  // 0 = all cores
    // Hollows that drain (drilled, or hollowed out): per layer, the regions
    // inside them. As in a sealed hollow, their ceilings get no tips, since
    // a support in there could never be removed: they have to hold
    // themselves up, and are reported ("cavity_ceiling") when they don't.
    const std::vector<Paths>* hollows = nullptr;
};

// `mesh` must already be placed in printer coordinates (lowest point at the
// elevation, Z up). Reports stages "slice", "islands" and "points".
PointGenResult generateSupportPoints(const TriangleMesh& mesh, const SupportConfig& cfg,
                                     const PointGenOptions& opts = {}, const JobControl& ctl = JobControl::none());

// What the tips the support tree could not build leave unheld. The planner's
// own tests decide, on the same layers: a surface sample counts when a dropped
// tip covered it and no built tip does; a layer counts when a part of it is
// short of hold without the dropped tips (and was not short with them).
struct DropCheck {
    size_t tips = 0;          // dropped tips that were the only cover of some surface
    size_t samples = 0;       // surface samples that only dropped tips covered
    size_t short_layers = 0;  // (layer, part) pairs short of hold only without the dropped tips
    std::vector<Vec3> at;     // where those `tips` were, lowest first
    bool any() const { return samples > 0 || short_layers > 0; }
};

// `built`: the tips that got a support; `dropped`: those that did not (see
// splitByTree). `opts` gives the layers and hollows as for planning; its tip
// options are ignored. Costs about one planning pass; nothing to do (and
// nothing reported) without dropped tips. Reports stages as planning does.
DropCheck checkDroppedTips(const TriangleMesh& mesh, const SupportConfig& cfg, const std::vector<SupportPoint>& built,
                           const std::vector<SupportPoint>& dropped, const PointGenOptions& opts = {},
                           const JobControl& ctl = JobControl::none());

// How many (layer, part) pairs are short of hold once the tips whose supports
// stand on the model (`tied`) count as what they are: ties. What a tied tip
// touches and what its foot stands on are held as one from that layer on, by
// the tips that reach the raft (`from_raft`) alone. Pairs that would be short
// with every tip counted are the planner's own report and not counted here.
// Costs about one planning pass; nothing to do without tied tips.
size_t layersShortOfRaftHold(const TriangleMesh& mesh, const SupportConfig& cfg,
                             const std::vector<SupportPoint>& from_raft, const std::vector<TiedTip>& tied,
                             const PointGenOptions& opts = {}, const JobControl& ctl = JobControl::none());

}  // namespace fruit
