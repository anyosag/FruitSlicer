// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Calibration and compatibility test prints.
//
// overlapCoupon(): the plan's week-1 blocker test. FruitSlicer exports the
// model and its supports as one STL of overlapping shells. This coupon checks
// whether a third-party slicer (Elegoo SatelLite, CHITUBOX) fills overlapping
// shells as a union (correct) or with an even-odd rule, which turns every
// overlap into a void. Expected correct slices are listed in `checks`.

#include <string>
#include <vector>

#include "mesh/TriangleMesh.h"

namespace fruit {

struct CouponCheck {
    double z;             // slice height to inspect in the slicer preview (mm)
    std::string expect;   // what a correct (union) slice shows there
};

// A round hole through one of a coupon's walls (the crowns coupon).
struct CouponHole {
    std::string name;
    Vec3 a, b;            // the middle of the hole on each face of its wall
    double ra = 0, rb = 0;  // its radius on each face
    bool bare = false;    // the planner puts no tips on its top
};

// A diamond window through a wall (the crowns coupon's lattice).
struct CouponWindow {
    Vec3 centre;          // in the middle of the wall
    double half_wide = 0, half_tall = 0;  // half its diagonals, across and up
};

struct Coupon {
    std::string name;
    TriangleMesh mesh;    // printer coordinates, plate at z = 0
    std::vector<CouponCheck> checks;
    std::vector<CouponHole> holes;
    std::vector<CouponWindow> windows;
};

Coupon overlapCoupon();

// An upright, asymmetric "F" standing on the plate in the +X/+Y quarter.
// Slice it with FruitSlicer and with SatelLite and open both .goo files in
// UVtools: the images must match (same corner, not mirrored or rotated).
Coupon orientationCoupon();

// The tops of round holes through 2 mm walls, printed with no tips: the
// planner's closing rule (SupportConfig::closing_reach), checked on a print.
// A box of four 2 mm walls, 75.6 × 42.5 × 37 mm, laid out as the user's
// TubeRack is (its walls are 2 mm, and it prints upright):
//  - front wall (-y): straight holes of 6.5, 6.5, 11.8 and 11.8 mm, whose
//    tops the planner leaves bare, and one of 14 mm that keeps its tips;
//  - end walls (∓x): TubeRack's two tapered bores, each widening outward:
//    30 mm (14.625 to 15.265 mm radius across the wall, its top rising
//    17.7°) and 17.5 mm (8.35 to 9.125, rising 21.2°);
//  - back wall (+y): TubeRack's back-wall lattice, diamond windows with
//    struts 1.2 mm wide leaning 47° from upright, which keeps its tips.
// A tapered bore's top is left bare only on a model taller than
// closing_ramp_min_height (80 mm). `tall` adds a 6 mm post in the back right
// corner up to 82 mm, so that the bores are planned as on TubeRack (the 30 mm
// one keeps a row of tips along the rim of its wide face: see the test).
// What each piece tests and how to score the print: docs/crowns-coupon.md.
Coupon crownsCoupon(bool tall = false);

}  // namespace fruit
