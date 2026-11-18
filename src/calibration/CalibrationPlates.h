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
    // One raft under every coupon (and the marker), as a job prints on, in
    // place of a small raft each: a pad under each coupon, the pads in each
    // row joined by a bar, the rows by one bar at the side; the channels
    // between the rows open at the other side, so no solid slab or closed
    // pocket pulls on the film.
    bool one_raft = false;
    // Each shaft as wide as a job's support with that tip (shaftDiameterFor),
    // in place of `shaft_diameter`.
    bool shafts_as_jobs = false;
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
    // Breakaway necks (PinheadSpec::neck_back): each tip size with each neck,
    // at three of its loads (TipLoads::neck_loads), on the shallower contact
    // depth; plain tips at the same loads and depth are on the plate already,
    // to compare marks and holds with. With two sets, once each, in the
    // middle of the plate; with one, `replicates` times, shuffled in.
    std::vector<double> neck_backs{0.1, 0.25};
    double neck_ratio = 0.6;
    // The coupons stand on one raft, as a job does (PlateCommon::one_raft).
    bool one_raft = true;
    std::vector<double> tip_diameters{0.2, 0.3, 0.4, 0.6};
    std::vector<double> penetrations{0.1, 0.2};
    // Loads per tip, mm² of disc per tip, for each tip size: six steps of
    // about 1.4 times from one ladder (0.9 ... 14), placed per size so that
    // FruitSlicer's starting value (CapacityModel: 2.8 / 6.2 / 11.1 / 25 mm²)
    // is at or past the top and a third of it is inside. Nothing is loaded
    // far past where it is expected to let go: a disc that lets go stays in
    // the vat, so a small tip never carries a big disc. On the first plate
    // printed (2026-11-18, one ladder of 5-20 for every size, flat discs)
    // 106 of 120 discs ended up in the vat.
    struct TipLoads {
        double tip;
        std::vector<double> loads;       // plain coupons
        std::vector<double> neck_loads;  // necked ones: three of `loads`, the middle ones
    };
    std::vector<TipLoads> loads_by_tip{
        {0.2, {0.9, 1.25, 1.8, 2.5, 3.5, 5}, {1.25, 1.8, 2.5}},
        {0.3, {1.25, 1.8, 2.5, 3.5, 5, 7}, {1.8, 2.5, 3.5}},
        {0.4, {1.8, 2.5, 3.5, 5, 7, 10}, {2.5, 3.5, 5}},
        {0.6, {2.5, 3.5, 5, 7, 10, 14}, {3.5, 5, 7}},
    };
    int tips_per_coupon = 3;
    // The full disc on top of the 45° underside: these layers pull with the
    // whole load. Thin, as what a disc that lets go leaves in the vat is the
    // part printed after it let go.
    double disc_thickness = 0.5;
    // Tips this high above the plate, as a job's supports stand (the jobs
    // saved on 2026-11-18 put tips 7.5-25 mm up, on lifts of 7.5-10 mm): a
    // taller shaft sways more under the peel, and the plate should feel that.
    double tip_height = 10.0;
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
