// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "calibration/Coupons.h"

#include "mesh/Primitives.h"
#include "support/SupportGeometry.h"

namespace fruit {

Coupon overlapCoupon() {
    Coupon c;
    c.name = "overlap";
    TriangleMesh& m = c.mesh;
    const GeometryQuality q;

    // Raft 30×30×1 mm with a 0.5 mm chamfer.
    appendRaft(m, {-15, -15}, {15, 15}, 0.0, 1.0, 0.5);

    // Plate (the "model") 20×20×2 mm, its underside at z = 5.
    const double plate_bottom = 5.0, plate_top = 7.0;
    appendBox(m, {-10, -10, plate_bottom}, {10, 10, plate_top});

    // 3×3 supports: pinhead penetrating 0.2 mm into the plate, shaft, base cone.
    PinheadSpec head;
    head.tip_diameter = 0.4;
    head.penetration = 0.2;
    head.head_length = 1.0;
    head.shaft_diameter = 1.0;
    for (int i = -1; i <= 1; ++i)
        for (int j = -1; j <= 1; ++j) {
            const Vec3 contact{6.0 * i, 6.0 * j, plate_bottom};
            const Vec3 junction = appendPinhead(m, contact, {0, 0, -1}, head, q);
            const Vec3 shaft_bottom{junction.x, junction.y, 1.6};
            appendShaft(m, junction, shaft_bottom, head.shaft_diameter, q, false);
            appendBaseCone(m, shaft_bottom, head.shaft_diameter, 3.0, 0.8, q);
        }

    // Inside the plate: a fully embedded 4×4×1 mm box (overlaps the plate
    // completely). Even-odd filling would print it as a hollow pocket.
    appendBox(m, {-2, -2, 5.5}, {2, 2, 6.5});

    // On top: two crossing bars that overlap each other and the plate.
    appendBox(m, {-8, -1, 6.5}, {8, 1, 9.0});
    appendBox(m, {-1, -8, 6.5}, {1, 8, 9.0});

    c.checks = {
        {5.05, "Plate fully solid; 9 tip circles merge into it (no rings or dots of empty space around tips)"},
        {6.0, "Plate fully solid; no 4x4 mm square hole in the centre (embedded box)"},
        {6.75, "Plate solid; the cross is not visible as hollow outlines"},
        {8.0, "A solid plus-shaped cross; the centre square where the bars overlap is filled"},
    };
    return c;
}

}  // namespace fruit
