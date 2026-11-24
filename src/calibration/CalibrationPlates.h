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
//   tips    — capacity: flat tiles held by the supports FruitSlicer makes for
//             them in a job (its own planner and support tree), one tip size
//             a tile, planned for a range of strengths → what each tip size
//             holds, as the replay of the print loads it. (Until 2026-11-24:
//             a disc of known area on three tips, discTipsPlate.)
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
#include "support/SupportConfig.h"

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

// The tips plate: what tips of each size hold in your resin, placed as
// FruitSlicer places them (2026-11-24). The plate printed on 2026-11-18 held
// discs of 15-60 mm² on three tips each: far fewer tips, further apart, than
// the planner ever puts under a face, on supports unlike a job's. Here each
// coupon is a flat tile on a lift, held by the supports a job of that tile
// gets: the planner's own tips (its spacing, the tips along free edges, each
// tip's own load) and the support tree's own heads, depths, shafts and feet,
// at FruitSlicer's own settings. A tile has one tip size, and is planned as
// if that size held `strength` times FruitSlicer's starting value
// (CapacityModel): the planner then puts fewer tips under it, each carrying
// more, until its spacing rule (the coverage curve) sets the fewest. What
// each of a tile's tips carries is worked out by the replay of the print, as
// it is for a job, and kept in the manifest for the fit: "load_mm2" is the
// load one tip alone would let go at as often as one of the tile's tips does
// (the replay's weakest-link curve over all of them), "load_max_mm2" the
// busiest tip's. Neither counts the extra pull far from the hinge: the fit
// measures that from the two sets.
struct TipsPlateOptions {
    // Lift 10 mm (the jobs saved in November stand on 5 and 10 mm, their tips
    // 7.5-25 mm up); one raft under every tile, as the coupons of the disc
    // plate stood on; two copies a side.
    PlateCommon common = [] {
        PlateCommon c;
        c.elevation = 10.0;
        c.one_raft = true;
        return c;
    }();
    // The direction from the vat's hinge to the far side (as
    // SupportConfig::peel_axis), when the hinge is known: two sets, by the
    // hinge and far from it, and the planner pulls harder far from it, as in
    // a job. Zero: one set, shuffled over the plate (twice as many copies).
    Vec2 peel_axis{0, 0};
    // Each size's tile (mm across) and strengths, × FruitSlicer's starting
    // value for that size, in steps of about 1.41 (kOneLoadStep): from where
    // the planner puts a tip about every millimetre to where its spacing rule
    // takes over (13 tips on a 10 mm tile, 8 on a 6 mm one; strengths past
    // that change nothing). Smaller tips on smaller tiles, so no tile gets
    // more than about 60 tips. Replayed, the busiest tip carries about 1.1
    // times its share (prototypes, 2026-11-24): 0.2 mm 1.5-8 mm² of layer,
    // 0.3 mm 2.5-12, 0.4 mm 3-18, 0.6 mm 3.5-18; per mm² of the part each
    // touches, roughly 15 to 260. That spans the disc plate's 45 and
    // FruitSlicer's starting values, about 270 at the replay's half-let-go load.
    struct Size {
        double tip = 0.3;
        double tile = 8;
        std::vector<double> strengths;
    };
    std::vector<Size> sizes{{0.2, 6, {0.5, 0.71, 1, 1.41, 2, 2.83}},
                            {0.3, 8, {0.35, 0.5, 0.71, 1, 1.41, 2}},
                            {0.4, 10, {0.25, 0.35, 0.5, 0.71, 1, 2}},
                            {0.6, 10, {0.125, 0.18, 0.25, 0.35, 0.5, 0.71}}};
    double tile_thickness = 1.0;  // thin: a tile that lets go leaves little in the vat
    double layer_height = 0.05;   // what the planner and the replay work on
    unsigned threads = 0;         // tiles made side by side (0 = all cores)
};

// The tips plate as printed until 2026-11-24 (discs on three tips). Kept so
// its score sheets still read and fit, and for the fit's depth and neck
// comparisons, which it alone has.
struct DiscTipsPlateOptions {
    PlateCommon common;
    // The direction from the vat's hinge to the far side, in plate
    // coordinates (as SupportConfig::peel_axis), when the hinge is known:
    // then the plate holds two sets, each every combination `replicates`
    // times, one along the hinge's edge and one along the far edge, so the
    // fit can measure how much harder the peel pulls far from the hinge
    // (SupportConfig::peel_bias) as well as each tip's hold. Zero: one set.
    Vec2 peel_axis{0, 0};
    // Breakaway necks (PinheadSpec::neck_back): for the sizes in `neck_sets`,
    // each neck at that size's three lightest loads on its shallower depth,
    // `neck_copies` times; plain tips at the same loads and depth are on the
    // plate already, to compare marks and holds with. Marks only show on
    // discs that held, so the lightest loads, where most hold. With two
    // sets, in the middle of the plate; with one, shuffled in with the rest.
    std::vector<double> neck_backs{0.1, 0.25};
    double neck_ratio = 0.6;
    struct NeckSet {
        double tip;
        std::vector<double> loads;
    };
    // The usual tip (0.3 mm in the jobs saved on 2026-11-18) and the one for
    // spots in mid-air (0.6 mm), where a smaller mark matters most.
    std::vector<NeckSet> neck_sets{{0.3, {1.8, 2.5, 3.5}}, {0.6, {2.5, 3.5, 5}}};
    int neck_copies = 2;
    // The coupons stand on one raft, as a job does (PlateCommon::one_raft).
    bool one_raft = true;
    // The sizes on the plate; each takes its ladders from `ladders` (a size
    // with none, the nearest size's, scaled by tip area).
    std::vector<double> tip_diameters{0.2, 0.3, 0.4, 0.6};
    // Loads per tip, mm² of disc per tip, for each tip size and contact depth:
    // five steps of about 1.41 times, placed around where half are expected to
    // let go: a tip's hold grows with what it touches (contactDiameter), about
    // 45 mm² of layer per mm² of contact on the first plate printed, likely
    // more (its flat discs failed early). Nothing is loaded far past that: a
    // disc that lets go stays in the vat, and a near-certain failure tells the
    // fit nothing. `extra`: in-between loads near the expected hold, two more
    // per side, on the usual tip, so its hold is finer than a step.
    struct Ladder {
        double tip, depth;
        std::vector<double> loads;
        std::vector<double> extra = {};
    };
    std::vector<Ladder> ladders{
        {0.2, 0.1, {0.9, 1.25, 1.8, 2.5, 3.5}},
        {0.2, 0.2, {1.25, 1.8, 2.5, 3.5, 5}},
        {0.3, 0.1, {1.8, 2.5, 3.5, 5, 7}, {2.1, 3.0, 4.2}},
        {0.3, 0.2, {1.8, 2.5, 3.5, 5, 7}, {2.1, 3.0, 4.2}},
        {0.4, 0.1, {1.8, 2.5, 3.5, 5, 7}},
        {0.4, 0.2, {2.5, 3.5, 5, 7, 10}},
        {0.6, 0.1, {2.5, 3.5, 5, 7, 10}},
        {0.6, 0.2, {5, 7, 10, 14, 20}},
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
CalibrationPlate discTipsPlate(const DiscTipsPlateOptions& o, const PrinterProfile& printer);
// A tips plate's tile planned as tipsPlate plans it: FruitSlicer's own
// settings, tips of `tip` mm only, trusted with `strength` times its starting
// value, and the hinge's extra pull along `peel_axis` (zero: none).
SupportConfig tileSupportConfig(double tip, double strength, const Vec2& peel_axis, double layer_height = 0.05);
CalibrationPlate spacingPlate(const SpacingPlateOptions& o, const PrinterProfile& printer);
CalibrationPlate islandsPlate(const IslandsPlateOptions& o, const PrinterProfile& printer);
CalibrationPlate gapsPlate(const GapsPlateOptions& o, const PrinterProfile& printer);

}  // namespace fruit
