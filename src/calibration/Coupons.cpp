// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "calibration/Coupons.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <stdexcept>
#include <utility>

#include "mesh/Primitives.h"
#include "ops/MeshOps.h"
#include "support/SupportGeometry.h"

namespace fruit {

namespace {

// A closed prism along y: the convex polygon `xz` (x across, z up) from y0 to y1.
void appendPrismY(TriangleMesh& m, const std::vector<Vec2>& xz, double y0, double y1) {
    TriangleMesh p;
    const int n = static_cast<int>(xz.size());
    for (const Vec2& q : xz) p.vertices.push_back({q.x, y0, q.y});
    for (const Vec2& q : xz) p.vertices.push_back({q.x, y1, q.y});
    for (int i = 1; i + 1 < n; ++i) {
        p.faces.push_back({0, i + 1, i});
        p.faces.push_back({n, n + i, n + i + 1});
    }
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        p.faces.push_back({i, j, n + j});
        p.faces.push_back({i, n + j, n + i});
    }
    if (p.volume() < 0)
        for (Face& f : p.faces) std::swap(f[1], f[2]);
    m.append(p);
}

// A round hole's cutter: through its wall from face a (radius ra) to face b
// (rb), carried on 1 mm past each face at the same taper.
void appendBore(TriangleMesh& m, const CouponHole& h) {
    const Vec3 d = h.b - h.a;
    const double len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    const Vec3 u{d.x / len, d.y / len, d.z / len};
    const double slope = (h.rb - h.ra) / len, past = 1.0;
    appendFrustum(m, h.a - u * past, h.ra - slope * past, h.b + u * past, h.rb + slope * past,
                  segmentsFor(std::max(h.ra, h.rb), 0.002));
}

}  // namespace

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

Coupon orientationCoupon() {
    Coupon c;
    c.name = "orientation";
    TriangleMesh& m = c.mesh;
    // "F" 2 mm tall: stem along +Y, arms pointing +X; placed at x 20..30, y 10..26.
    appendBox(m, {20, 10, 0}, {22, 26, 2});  // stem
    appendBox(m, {22, 24, 0}, {30, 26, 2});  // top arm
    appendBox(m, {22, 17, 0}, {27, 19, 2});  // middle arm
    c.checks = {{1.0, "An upright F (arms to the right) in the +X/+Y quarter of the plate, as seen from above. "
                      "FruitSlicer's and SatelLite's .goo must show it identically in UVtools"}};
    return c;
}

Coupon crownsCoupon(bool tall) {
    Coupon c;
    c.name = tall ? "crowns-tall" : "crowns";
    constexpr double t = 2.0;    // every wall, as TubeRack's
    constexpr double H = 37.0;   // the box's height: the 30 mm bore and 3.2 mm over and under it
    constexpr double zc = H / 2; // the middle of every hole
    // Front wall, left to right: the holes and the wall between them. The two
    // 6.5 mm holes 9 mm apart, as on TubeRack's front wall.
    const double front[] = {6.5, 6.5, 11.8, 11.8, 14.0};
    const double between[] = {2.5, 3.5, 3.5, 3.5};
    constexpr double margin = 4.0;  // from the end walls' inner faces
    double inner = 2 * margin;
    for (double d : front) inner += d;
    for (double g : between) inner += g;
    const double L = inner + 2 * t;  // 75.6
    // TubeRack's end walls: the radius on the outer face and on the inner one.
    constexpr double r30_out = 15.265, r30_in = 14.625;  // its 30 mm bores, rising 17.7° across the wall
    constexpr double r17_out = 9.125, r17_in = 8.35;     // its 17.5 mm bores, rising 21.2°
    const double D = 2 * t + 2 * r30_out + 2 * 3.985;    // 42.5: 4 mm of wall beside the wide bore
    const double hx = L / 2, hy = D / 2;

    // Front wall's holes, straight through it (along y).
    double x = -hx + t + margin;
    for (std::size_t i = 0; i < std::size(front); ++i) {
        const double r = front[i] / 2;
        x += r;
        CouponHole h;
        char name[48];
        std::snprintf(name, sizeof name, "%.1f mm hole", front[i]);
        h.name = name;
        h.a = {x, -hy, zc};
        h.b = {x, -hy + t, zc};
        h.ra = h.rb = r;
        h.bare = front[i] < 12;  // 14 mm: wider than a round crown whose last layer reaches 0.8 mm from each side
        c.holes.push_back(h);
        x += r + (i < std::size(between) ? between[i] : 0.0);
    }
    // End walls' bores (along x), each wider on its outer face.
    {
        CouponHole h;
        h.name = "30 mm bore, rising 17.7 deg";
        h.a = {-hx, 0, zc};
        h.b = {-hx + t, 0, zc};
        h.ra = r30_out;
        h.rb = r30_in;
        h.bare = tall;
        c.holes.push_back(h);
        h.name = "17.5 mm bore, rising 21.2 deg";
        h.a = {hx - t, 0, zc};
        h.b = {hx, 0, zc};
        h.ra = r17_in;
        h.rb = r17_out;
        c.holes.push_back(h);
    }
    // Back wall: TubeRack's lattice. Diamond windows 9.96 mm across and 9.27 mm
    // tall (edges 47.06° from upright), struts 1.2 mm wide between them: the
    // windows sit on a grid k times their half-diagonals, every other place.
    {
        constexpr double a = 4.98, b = 4.635, strut = 1.2, frame = 2.5;
        const double k = 1 + strut / 2 * std::sqrt(1 / (a * a) + 1 / (b * b));
        const double P = k * a, Q = k * b;
        const double reach = hx - t - frame;  // windows end this far from the middle
        for (int j = -2; j <= 2; ++j)
            for (int i = -9; i <= 9; ++i) {
                if ((i + j) % 2 != 0 || std::abs(i * P) + a > reach) continue;
                c.windows.push_back({{i * P, hy - t / 2, zc + j * Q}, a, b});
            }
    }

    TriangleMesh cut;
    for (const CouponHole& h : c.holes) appendBore(cut, h);
    for (const CouponWindow& w : c.windows) {
        const double wx = w.centre.x, wz = w.centre.z;
        appendPrismY(cut, {{wx, wz - w.half_tall}, {wx + w.half_wide, wz}, {wx, wz + w.half_tall}, {wx - w.half_wide, wz}},
                     hy - t - 1, hy + 1);
    }
    ops::Solid box = ops::subtract(ops::Solid(makeBox({-hx, -hy, 0}, {hx, hy, H})),
                                   ops::Solid(makeBox({-hx + t, -hy + t, -1}, {hx - t, hy - t, H + 1})));
    if (tall) {
        constexpr double post = 6.0, top = 82.0;  // past closing_ramp_min_height (80 mm)
        box = ops::unite(box, ops::Solid(makeBox({hx - post, hy - post, 0}, {hx, hy, top})));
    }
    const ops::Solid cutter(cut);
    if (!box.valid() || !cutter.valid()) throw std::logic_error("crowns coupon: " + box.message() + cutter.message());
    c.mesh = ops::subtract(box, cutter).toMesh();

    // Where each top closes (on its higher face), and what to look for there: docs/crowns-coupon.md.
    const auto crown = [&](std::size_t i) { return zc + std::max(c.holes[i].ra, c.holes[i].rb); };
    double lattice_top = 0;
    for (const CouponWindow& w : c.windows) lattice_top = std::max(lattice_top, w.centre.z + w.half_tall);
    c.checks = {
        {crown(0), "Front wall, the two 6.5 mm holes on the left: planned with no tips on their tops. Printed: "
                   "each top as round and smooth as the bottom of its hole, with no dip, flat or seam"},
        {crown(2), "Front wall, the two 11.8 mm holes: no tips on their tops; the same to look for"},
        {crown(4), "Front wall, the 14 mm hole on the right: tips on its top (the control)"},
        {crown(6), tall ? "Right end wall, the 17.5 mm bore: no tips on its top (the model is past 80 mm); the "
                          "same to look for, from both faces"
                        : "Right end wall, the 17.5 mm bore: tips on its top (a tapered top is bare only on a "
                          "model taller than 80 mm: crowns-tall)"},
        {crown(5), tall ? "Left end wall, the 30 mm bore: 15 small tips on its top, along the rim of its outer face "
                          "and one at the top of its inner face (what holds itself up is found 0.03 mm short of the "
                          "outer face); delete them to test it bare"
                        : "Left end wall, the 30 mm bore: tips on its top (bare only on crowns-tall)"},
        {lattice_top, "Back wall, the lattice: tips under the top of every window (the control); every strut whole"},
    };
    return c;
}

}  // namespace fruit
