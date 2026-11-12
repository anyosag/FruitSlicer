// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Support generation settings. Defaults are *starting values* taken from the
// research summarised in docs/resin-support-plan.md; the calibration plates
// (fruitslicer calibrate …) exist to replace them with measured values.

#include <string>
#include <utility>
#include <vector>

#include "geometry/Vec.h"

namespace fruit {

struct TipClass {
    std::string name;
    double diameter = 0.4;  // contact sphere diameter (mm)
};

// Coverage radius as a function of cured height above a tip. Seeded with
// PrusaSlicer 2.9's influence curve (3.2 mm at Δz 0 … 6 mm at Δz 40 mm,
// tuned there for a 0.4 mm head); must be re-calibrated on the 16K with the
// `spacing` plate — no tilt factor is stacked on top of it.
struct CoverageModel {
    std::vector<std::pair<double, double>> curve{{0.0, 3.2}, {3.9, 4.0}, {15.0, 5.0}, {40.0, 6.0}};
    double density = 1.0;  // > 1 = denser (radius / sqrt(density))

    double radius(double dz) const;
    double maxRadius() const { return radius(1e9); }
};

// Load a tip holds, expressed as mm² of layer cross-section it can carry
// under peel. Seeded from PrusaSlicer 2.6's force model (one 0.4 mm head per
// ~11.1 mm² at density 1) scaled by tip area, following Wu et al.'s
// n·A_tip·σ ≥ S·T·A. Replace with values from the `tips` plate.
struct CapacityModel {
    double reference_diameter = 0.4;
    double reference_capacity = 11.1;  // mm² per tip
    double safety = 1.0;               // > 1 = more conservative
    // Measured (diameter, capacity) pairs from a resin profile. When set they
    // replace the seed; between and beyond them capacity scales with tip area.
    std::vector<std::pair<double, double>> table;

    double capacity(double tip_diameter) const;
};

struct SupportConfig {
    double layer_height = 0.05;
    // Parts in consecutive layers are linked when the upper one overlaps the
    // lower one grown by this much (~1 Saturn 16K pixel).
    double link_grow = 0.02;

    // Surfaces leaning more than this from vertical need support.
    double critical_angle_deg = 45.0;
    // A ledge may reach this far beyond material `anchor_window` mm below
    // without support (PrusaSlicer uses 1.5 mm; 1.0 is more cautious).
    double self_supported_reach = 1.0;
    double anchor_window = 1.0;

    // Sampling of surfaces that need support.
    double edge_step = 1.0;         // along free edges (mm)
    double grid_step = 1.0;         // interior grid (mm)
    double sample_min_spacing = 0.5; // 3D decimation of samples (mm)
    double capacity_candidate_step = 1.5; // spacing of extra-tip candidates on self-supporting undersides

    // Islands (PrusaSlicer 2.9 sizes for a 0.4 mm head, to be calibrated).
    double island_one_tip_length = 1.7;
    double island_two_tip_length = 6.5;
    int large_island_min_tips = 3;
    // Islands smaller than the contact of the smallest tip (π·0.1² ≈ 0.03 mm²)
    // that merge with the model within a few layers are left unsupported
    // (Lychee: islands smaller than a light tip are often best left alone).
    double tiny_island_area = 0.03;
    int tiny_island_merge_layers = 3;
    // Slightly larger islands that join the model on the very next layer are
    // absorbed by that layer's exposure (the flake left on the film is cured
    // into it). ASSUMPTION — verify with the islands calibration plate.
    double absorb_island_area = 0.12;
    int absorb_island_layers = 1;

    // Tall parts on few tips get side tips (stabilisation).
    double stabilize_min_height = 5.0;  // mm above the lowest tip
    double slenderness = 4.0;           // height / tip spread that triggers it

    // A fresh edge. Where a layer reaches more than `skin_advance` past the
    // layer before it, that part is one cured skin with nothing behind it:
    // the first layer of a flat face, the ends of the sliver a tilted face
    // starts as (they lengthen by millimetres a layer), the rim of a shelf.
    // Where its edge is free (the part's own outline, not the side that faces
    // what holds it, nor a ceiling with walls all round) it needs a tip within
    // `skin_reach`, closer than the coverage curve asks. Slow growth (a dome
    // widening, a steady slope) is not touched. REASONING, not measured: 1.6 mm keeps a one-layer skin between
    // two tips to the span a 1.0-1.5 mm ledge is trusted with (see
    // self_supported_reach). Reach 0 = off.
    double skin_advance = 0.5;
    double skin_reach = 1.6;

    std::vector<TipClass> tip_classes{{"mini", 0.2}, {"light", 0.3}, {"medium", 0.4}, {"heavy", 0.6}};
    int default_tip = 2;  // medium
    int minima_tip = 3;   // heavy: "larger touchpoints on minima"
    int edge_tip = 1;     // light near sharp edges / cosmetic regions

    CoverageModel coverage;
    CapacityModel capacity;

    // The model stands on the build plate (no elevation, no raft): parts on
    // the first layer are held by the plate, not by tips.
    bool on_plate = false;
    // On the plate, what starts within this height of the model's underside
    // needs no tip: the part of the model that prints in the bottom layers.
    // That is their count less the height correction's layers (copies of the
    // first layer that go under the model, bottom layers themselves), times
    // the print's layer height; the app sets it from the job, and it is
    // (4 − 2) × 0.05 mm otherwise, the printer's four bottom layers and the
    // typical 0.10 mm correction. Those layers are cured many times longer
    // than the rest, which hardens the resin between them and the plate as
    // well: the plate holds them, and a tip there would only print as part of
    // the model. The transition layers after them are not counted: their
    // exposure falls to the normal one, so the last of them no longer reach
    // the plate. REASONING, not measured.
    double plate_hold_height = 0.1;

    // Tilt release: extra peel demand towards the side away from the hinge.
    double peel_bias = 0.0;  // 0 = off (hinge side unknown)
    Vec2 peel_axis{0, 0};
    // Half the build plate's size (mm): the bias ramps from the hinge side to
    // the far edge. Saturn 4 Ultra 16K: 211.68 × 118.37 mm.
    Vec2 plate_half{105.84, 59.185};

    // Cost weights for where tips should not go.
    double cosmetic_weight = 4.0;
    double edge_weight = 1.0;
    double edge_margin = 0.2;  // mm beyond the tip radius counted as "near an edge"
    double sharp_edge_angle_deg = 40.0;

    // Peak-area and flat-face warnings.
    double warn_layer_area = 2000.0;  // mm²
    double warn_flat_area = 50.0;     // mm² of new area in a single layer
};

}  // namespace fruit
