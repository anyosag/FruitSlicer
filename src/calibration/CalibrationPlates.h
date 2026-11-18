// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Calibration plates (milestone M0.5). Each plate holds ~50–100 small coupons
// in a grid; the coupon, not the plate, is the unit of trial, so one print
// gives enough trials to bound failure rates (0 failures in 60 → ≤ 5 % at
// 95 % confidence). Parameter combinations are shuffled over the grid with a
// fixed seed so plate position (peel direction, vat temperature gradient) is
// not confounded with the parameter being tested.
//
//   tips    — capacity: a disc of known area held by tips of a given diameter
//             and penetration → load each tip size holds.
//   spacing — coverage: plates tilted 0–60° supported by tip grids at a given
//             spacing → largest spacing without warp; plus unsupported
//             wedges → self-supporting overhang angle.
//   islands — island minimums: small columns starting in mid-air held by
//             1–3 tips → how many tips each island size needs.
//   gaps    — the shaft gap: thin shafts standing a known gap from a wall,
//             short and long, beside an open wall and inside a cup → the
//             narrowest gap at which a shaft doesn't print stuck to the model
//             (TreeConfig::clearance).
//
// Cell (row 0, col 0) holds an L-shaped orientation marker; rows grow along
// +Y, columns along +X. The manifest maps every cell to its parameters.

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "mesh/TriangleMesh.h"
#include "printer/PrinterProfile.h"

namespace fruit {

struct PlateCoupon {
    int id = 0;
    int row = 0, col = 0;
    Vec2 center;
    nlohmann::json params;
};

struct CalibrationPlate {
    std::string kind;
    TriangleMesh mesh;
    std::vector<PlateCoupon> coupons;
    nlohmann::json settings;  // plate-level settings (elevation, raft, seed...)
    int rows = 0, cols = 0;
    double cell = 0;

    nlohmann::json manifest() const;
    std::string csvTemplate() const;
};

struct PlateCommon {
    double elevation = 5.0;        // coupon underside above the plate (mm)
    double raft_thickness = 1.0;   // must be >= printer.minRaftThickness()
    double shaft_diameter = 1.0;
    double head_length = 1.0;
    uint32_t seed = 1;
    int replicates = 2;
};

struct TipsPlateOptions {
    PlateCommon common;
    // The direction from the vat's hinge to the far side, in plate
    // coordinates (as SupportConfig::peel_axis), when the hinge is known:
    // then the plate holds two sets, each every combination `replicates`
    // times, one along the hinge's edge and one along the far edge, so the
    // fit can measure how much harder the peel pulls far from the hinge
    // (SupportConfig::peel_bias) as well as each tip's hold. Zero: one set.
    Vec2 peel_axis{0, 0};
    // Loads per tip in two sets: finer steps (about 1.32 times each), so a
    // difference of a few tens of percent between the sides shows; once each
    // per side (each combination twice in all, as one set with replicates 2).
    std::vector<double> loads_two_sides{5, 6.6, 8.7, 11.5, 15.2, 20};
    // Breakaway necks (PinheadSpec::neck_back): each tip size with each neck,
    // at three of the loads, on the shallower contact depth; plain tips at
    // the same loads and depth are on the plate already, to compare marks
    // and holds with. With two sets, once each, in the middle of the plate;
    // with one, `replicates` times, shuffled in with the rest.
    std::vector<double> neck_backs{0.1, 0.25};
    double neck_ratio = 0.6;
    std::vector<double> neck_loads{5, 10, 20};
    std::vector<double> neck_loads_two_sides{6.6, 11.5, 20};
    std::vector<double> tip_diameters{0.2, 0.3, 0.4, 0.6};
    std::vector<double> penetrations{0.1, 0.2};
    std::vector<double> loads_per_tip{5, 10, 20};  // mm² of disc area per tip
    int tips_per_coupon = 3;
    double disc_thickness = 1.0;
};

struct SpacingPlateOptions {
    PlateCommon common = [] {
        PlateCommon c;
        c.replicates = 1;  // 37 coupons of 18 mm cells fill the plate once
        return c;
    }();
    std::vector<double> angles{0, 15, 30, 45, 60};  // plate tilt from horizontal (deg)
    std::vector<double> spacings{1.5, 2, 3, 4, 5, 6};  // tip grid pitch (mm)
    std::vector<double> wedge_angles{30, 40, 45, 50, 55, 60, 65};  // overhang from vertical (deg)
    double plate_size = 15.0;
    double plate_thickness = 1.5;
    double tip_diameter = 0.4;
    double penetration = 0.15;
};

struct IslandsPlateOptions {
    PlateCommon common;
    std::vector<double> island_diameters{0.3, 0.5, 1, 2, 3, 5};
    std::vector<int> tip_counts{1, 2, 3};
    double tip_diameter = 0.3;   // shrunk per island so the tips fit
    double min_tip_diameter = 0.1;
    double penetration = 0.1;
    double column_height = 2.0;
};

struct GapsPlateOptions {
    PlateCommon common;
    std::vector<double> gaps{0.15, 0.20, 0.25, 0.30, 0.40, 0.50};  // shaft to wall (mm)
    // How far each shaft runs beside the wall (mm): a short one, and one as
    // long as the shafts that stuck in DishRack_Top's posts, which can sway.
    std::vector<double> heights{3.0, 15.0};
    // Beside an open wall, and inside a cup open only near the raft, as a
    // shaft inside a hollow post is: resin that stays in the gap through the
    // wash can glue the shaft there when it is cured.
    std::vector<std::string> places{"wall", "cup"};
    double shaft_diameter = 0.8;  // the thinnest shaft supports have: it sways the most
    double wall = 1.5;            // the wall's thickness (mm)
};

// Throws std::runtime_error if the coupons do not fit on the printer plate.
CalibrationPlate tipsPlate(const TipsPlateOptions& o, const PrinterProfile& printer);
CalibrationPlate spacingPlate(const SpacingPlateOptions& o, const PrinterProfile& printer);
CalibrationPlate islandsPlate(const IslandsPlateOptions& o, const PrinterProfile& printer);
CalibrationPlate gapsPlate(const GapsPlateOptions& o, const PrinterProfile& printer);

}  // namespace fruit
