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

struct Coupon {
    std::string name;
    TriangleMesh mesh;    // printer coordinates, plate at z = 0
    std::vector<CouponCheck> checks;
};

Coupon overlapCoupon();

}  // namespace fruit
