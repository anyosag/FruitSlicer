// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Gaps closing from two sides through a wall: the top of a round hole through
// it holds itself up, as an arch does (SupportConfig::closing_reach). Every
// case is planned with the rule on and off, at 0.05 mm layers (where the
// widest flat closing is 6.9 mm across).
#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "calibration/Coupons.h"
#include "mesh/Placement.h"
#include "mesh/Primitives.h"
#include "ops/MeshOps.h"
#include "support/PointGenerator.h"

using namespace fruit;
using fruit::ops::MeshResult;
using fruit::ops::subtractMeshes;

namespace {

constexpr double kLift = 5.0;

SupportConfig config(bool on = true) {
    SupportConfig cfg;
    cfg.layer_height = 0.05;
    if (!on) cfg.closing_reach = 0;
    return cfg;
}

PointGenResult plan(const TriangleMesh& m, const SupportConfig& cfg, bool relieve = false) {
    PointGenOptions opts;
    opts.relieve = relieve;
    PointGenResult r = generateSupportPoints(m, cfg, opts);
    CHECK(r.stats.uncovered == 0);
    return r;
}

// A closed prism: the convex polygon `xz` (counter-clockwise seen from -y) from y0 to y1.
TriangleMesh prism(const std::vector<Vec2>& xz, double y0, double y1) {
    TriangleMesh m;
    const int n = static_cast<int>(xz.size());
    for (const Vec2& p : xz) m.vertices.push_back({p.x, y0, p.y});
    for (const Vec2& p : xz) m.vertices.push_back({p.x, y1, p.y});
    for (int i = 1; i + 1 < n; ++i) {
        m.faces.push_back({0, i + 1, i});              // the end at y0, facing -y
        m.faces.push_back({n, n + i, n + i + 1});      // the end at y1, facing +y
    }
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        m.faces.push_back({i, j, n + j});
        m.faces.push_back({i, n + j, n + i});
    }
    if (m.volume() < 0)
        for (Face& f : m.faces) std::swap(f[1], f[2]);
    return m;
}

TriangleMesh minus(const TriangleMesh& a, const TriangleMesh& b) {
    const MeshResult r = subtractMeshes(a, b);
    REQUIRE(r.ok);
    return r.mesh;
}

// A round bore along y through `m`, centre (cx, cz): radius r0 at y0, r1 at y1.
TriangleMesh bore(const TriangleMesh& m, double cx, double cz, double r0, double r1, double y0, double y1) {
    TriangleMesh cut;
    appendFrustum(cut, {cx, y0, cz}, r0, {cx, y1, cz}, r1, segmentsFor(std::max(r0, r1), 0.005));
    return minus(m, cut);
}

// A wall `wide` along x, `thick` through (y, centred on 0), `tall`, on the plate's lift.
TriangleMesh wall(double wide, double thick, double tall) {
    return makeBox({-wide / 2, -thick / 2, kLift}, {wide / 2, thick / 2, kLift + tall});
}

// A hole's top: tips within 45° of the top of the circle of radius r(y) round (cx, cz).
struct Crown {
    double cx, cz, r0, r1, y0, y1;
    double radiusAt(double y) const { return r0 + (r1 - r0) * std::clamp((y - y0) / (y1 - y0), 0.0, 1.0); }
    bool on(const SupportPoint& p) const {
        const double dx = p.pos.x - cx, dz = p.pos.z - cz;
        if (dz <= 0) return false;
        if (std::abs(std::hypot(dx, dz) - radiusAt(p.pos.y)) > 0.15) return false;
        return std::atan2(std::abs(dx), dz) <= deg2rad(45);
    }
};

std::size_t onCrowns(const std::vector<SupportPoint>& pts, const std::vector<Crown>& crowns) {
    std::size_t n = 0;
    for (const auto& p : pts)
        n += std::any_of(crowns.begin(), crowns.end(), [&](const Crown& c) { return c.on(p); });
    return n;
}

using TipKey = std::tuple<uint64_t, double, double, double, double>;
std::vector<TipKey> keys(const std::vector<SupportPoint>& pts, const std::vector<Crown>& leave_out = {}) {
    std::vector<TipKey> out;
    for (const auto& p : pts)
        if (std::none_of(leave_out.begin(), leave_out.end(), [&](const Crown& c) { return c.on(p); }))
            out.emplace_back(p.id, p.pos.x, p.pos.y, p.pos.z, p.tip_diameter);
    std::sort(out.begin(), out.end());
    return out;
}

// The same tips with the rule on and off (and nothing held).
void sameEitherWay(const TriangleMesh& m) {
    const PointGenResult on = plan(m, config(true)), off = plan(m, config(false));
    CHECK(on.stats.closings_held == 0);
    CHECK(keys(on.points) == keys(off.points));
}

// A 2 mm wall 20 wide and 24 tall with one straight hole of diameter `d`, its centre 10 mm up.
struct HoledWall {
    TriangleMesh mesh;
    Crown crown;
};
HoledWall holedWall(double d, double thick = 2.0, double cx = 0.0, double wide = 20.0) {
    const double cz = kLift + 10, y0 = -thick / 2 - 1, y1 = thick / 2 + 1;
    return {bore(wall(wide, thick, 24), cx, cz, d / 2, d / 2, y0, y1), {cx, cz, d / 2, d / 2, y0, y1}};
}

}  // namespace

TEST_CASE("the top of a 6.5 mm hole through a 2 mm wall holds itself up", "[points][closings]") {
    const HoledWall w = holedWall(6.5);
    const PointGenResult on = plan(w.mesh, config(true)), off = plan(w.mesh, config(false));
    CAPTURE(onCrowns(off.points, {w.crown}), onCrowns(on.points, {w.crown}));
    CHECK(onCrowns(off.points, {w.crown}) >= 4);
    CHECK(onCrowns(on.points, {w.crown}) == 0);
    CHECK(on.stats.closings_held >= 1);
    CHECK(on.stats.closing_area > 0);
    // Nothing else changes.
    CHECK(keys(on.points, {w.crown}) == keys(off.points, {w.crown}));
}

TEST_CASE("the top of an 11.8 mm hole holds itself up too", "[points][closings]") {
    const HoledWall w = holedWall(11.8);
    const PointGenResult on = plan(w.mesh, config(true)), off = plan(w.mesh, config(false));
    CAPTURE(onCrowns(off.points, {w.crown}), onCrowns(on.points, {w.crown}));
    CHECK(onCrowns(off.points, {w.crown}) >= 6);
    CHECK(onCrowns(on.points, {w.crown}) == 0);
    CHECK(keys(on.points, {w.crown}) == keys(off.points, {w.crown}));
}

TEST_CASE("a 14 mm hole's top is too wide to hold itself up", "[points][closings]") {
    // A window below where it closes, its gap is 7.2 mm: wider than a round crown whose last layer reaches
    // 0.8 mm from each side (6.9 mm).
    sameEitherWay(holedWall(14).mesh);
}

TEST_CASE("a hole through a wall too thin or too thick keeps its tips", "[points][closings]") {
    sameEitherWay(holedWall(6.5, 1.2).mesh);  // a rib, not a wall
    sameEitherWay(holedWall(6.5, 4.0).mesh);  // a bore, not a hole through a wall
}

TEST_CASE("a hole whose side is narrower than a ledge keeps its tips", "[points][closings]") {
    // A window below its top a 6.5 mm hole is 4.7 mm across: there, 0.6 mm of wall is left between it and the
    // wall's end (lower down it breaks out of the end). That sliver is not an abutment.
    const double half_there = std::sqrt(3.25 * 3.25 - 2.25 * 2.25);
    sameEitherWay(holedWall(6.5, 2.0, -10 + 0.6 + half_there).mesh);
    // With 1.6 mm left there, it is.
    const HoledWall w = holedWall(6.5, 2.0, -10 + 1.6 + half_there);
    CHECK(onCrowns(plan(w.mesh, config(true)).points, {w.crown}) == 0);
}

TEST_CASE("a blind hole, a cup and a tube on their side keep their tips", "[points][closings]") {
    const double cz = kLift + 10;
    // 1.5 mm into a 3 mm wall: the gap is not open past both faces.
    sameEitherWay(bore(wall(20, 3, 24), 0, cz, 3.25, 3.25, -2.5, 0.0));
    // A cup (8 mm outside, 6 inside) on its side, open at one end, and a tube 20 mm long: no wall to go through.
    TriangleMesh cup;
    appendFrustum(cup, {0, -10, cz}, 8, {0, 10, cz}, 8, segmentsFor(8, 0.005));
    sameEitherWay(bore(cup, 0, cz, 6, 6, -8, 11));
    sameEitherWay(bore(cup, 0, cz, 6, 6, -11, 11));
}

TEST_CASE("a flat-topped window keeps the tips under its ceiling", "[points][closings]") {
    // Its ceiling appears in one layer, 4 mm across: each side would reach 2 mm, not a step from the layer below.
    const double cz = kLift + 8;
    sameEitherWay(minus(wall(20, 2, 24), makeBox({-2, -2, cz}, {2, 2, cz + 6})));
}

TEST_CASE("a narrow apex and a shallow roof keep their tips", "[points][closings]") {
    const double cz = kLift + 8;
    // A rhombus window whose upper faces lean 50° from upright: 1 mm below its apex the gap is 2.4 mm, a tie the
    // walls beside it pull on, not a crown.
    const double h = 4, half = h * std::tan(deg2rad(50));
    sameEitherWay(minus(wall(24, 2, 24), prism({{0, cz - h}, {half, cz}, {0, cz + h}, {-half, cz}}, -2, 2)));
    // A window 20 mm wide with a roof leaning 80° from upright: 11 mm across a window below its apex.
    const double rise = 10 * std::tan(deg2rad(10));
    const TriangleMesh roof = prism({{-10, cz}, {10, cz}, {10, cz + 3}, {0, cz + 3 + rise}, {-10, cz + 3}}, -2, 2);
    sameEitherWay(minus(wall(30, 2, 24), roof));
}

TEST_CASE("a fin growing into a wall is a ledge, not an arch", "[points][closings]") {
    // A 1 mm fin leaning 60° from upright meets a vertical wall: only the fin comes in.
    TriangleMesh m = makeBox({0, -5, kLift}, {2, 5, kLift + 20});
    const double run = 6, rise = run / std::tan(deg2rad(60));
    const double meet = kLift + 6 + (run + 0.5) / run * rise;  // where its underside reaches into the wall
    m.append(prism({{-run, kLift + 6}, {0.5, meet}, {0.5, meet + 1}, {-run, kLift + 7}}, -5, 5));
    sameEitherWay(m);
}

TEST_CASE("a crown just under a flat plate keeps its tips; further below it holds itself up",
          "[points][closings]") {
    // The plate's underside is a flat face in one layer: the model just under it hangs from the tips there.
    const double top = kLift + 10 + 3.25;
    const auto with_plate = [&](double gap) {
        TriangleMesh m = holedWall(6.5).mesh;
        m.append(makeBox({-5, -10, top + gap}, {5, 10, top + gap + 1}));
        return m;
    };
    sameEitherWay(with_plate(0.65));
    const Crown crown = holedWall(6.5).crown;
    const PointGenResult far = plan(with_plate(3.0), config(true));
    CHECK(onCrowns(far.points, {crown}) == 0);
}

TEST_CASE("a tapered bore holds itself up only on a tall model", "[points][closings]") {
    // A cone through a 2 mm wall, its crown rising 21° across it: such bores, on small parts tilted as they
    // print, keep their tips; on a model taller than closing_ramp_min_height its top holds itself up.
    const double cz = kLift + 13, r0 = 8.35, r1 = 9.125;
    const TriangleMesh m = bore(wall(30, 2, 30), 0, cz, r0 - 0.3875, r1 + 0.3875, -2, 2);
    const Crown crown{0, cz, r0, r1, -1, 1};
    sameEitherWay(m);  // 30 mm tall: under the default height
    SupportConfig cfg = config(true);
    cfg.closing_ramp_min_height = 25;
    const PointGenResult tall = plan(m, cfg), off = plan(m, config(false));
    CAPTURE(onCrowns(off.points, {crown}), onCrowns(tall.points, {crown}));
    CHECK(onCrowns(off.points, {crown}) >= 6);
    CHECK(onCrowns(tall.points, {crown}) <= 2);
    CHECK(tall.stats.closings_held >= 1);
}

TEST_CASE("holes round a box: their tops hold themselves up, nothing else changes", "[points][closings]") {
    // A 30 × 20 × 30 mm box of 2 mm walls: three 6.5 mm holes in front, one of 11.8 mm at the back.
    TriangleMesh m =
        minus(makeBox({-15, -10, kLift}, {15, 10, kLift + 30}), makeBox({-13, -8, kLift - 1}, {13, 8, kLift + 31}));
    std::vector<Crown> crowns;
    for (const double x : {-8.0, 0.0, 8.0}) {
        m = bore(m, x, kLift + 12, 3.25, 3.25, -11, -7);
        crowns.push_back({x, kLift + 12, 3.25, 3.25, -11, -7});
    }
    m = bore(m, 0, kLift + 14, 5.9, 5.9, 7, 11);
    crowns.push_back({0, kLift + 14, 5.9, 5.9, 7, 11});
    const PointGenResult on = plan(m, config(true)), off = plan(m, config(false));
    CAPTURE(onCrowns(off.points, crowns), onCrowns(on.points, crowns));
    CHECK(onCrowns(off.points, crowns) >= 16);
    CHECK(onCrowns(on.points, crowns) == 0);
    CHECK(keys(on.points, crowns) == keys(off.points, crowns));
    // Rule 4 (tips beside those that carry too much) puts none back on them.
    const PointGenResult relieved = plan(m, config(true), true);
    CHECK(onCrowns(relieved.points, crowns) == 0);
}

TEST_CASE("closings: the same tips every time", "[points][closings]") {
    const HoledWall w = holedWall(6.5);
    CHECK(keys(plan(w.mesh, config(true)).points) == keys(plan(w.mesh, config(true)).points));
}

TEST_CASE("closings change nothing under a ceiling held all round, a slab over a slot, or slotted floor",
          "[points][closings]") {
    // A socket: four walls and a ceiling.
    TriangleMesh socket = makeBox({0, 0, 8}, {12, 12, 10});
    socket.append(makeBox({0, 0, 0}, {12, 2, 8}));
    socket.append(makeBox({0, 10, 0}, {12, 12, 8}));
    socket.append(makeBox({0, 2, 0}, {2, 10, 8}));
    socket.append(makeBox({10, 2, 0}, {12, 10, 8}));
    placeOnPlate(socket, {0, 0, 0}, kLift);
    sameEitherWay(socket);
    // Two fingers 8 mm apart joined by a slab.
    TriangleMesh slab = makeBox({0, 0, 0}, {2, 2, 6});
    slab.append(makeBox({10, 0, 0}, {12, 2, 6}));
    slab.append(makeBox({0, 0, 6}, {12, 2, 7}));
    placeOnPlate(slab, {0, 0, 0}, kLift);
    sameEitherWay(slab);
    // A floor with six 0.6 mm slots.
    TriangleMesh floor = makeBox({0, 0, 5}, {6 * 0.6 + 7 * 1.4 + 4, 12, 6.5});
    for (int i = 0; i < 6; ++i) {
        const double x0 = 2 + 1.4 + i * 2.0;
        floor = minus(floor, makeBox({x0, 2, 4}, {x0 + 0.6, 10, 7.5}));
    }
    sameEitherWay(floor);
}

namespace {

// Tips on the top of a coupon's hole: on its surface, within 45° of the top, anywhere through its wall.
std::vector<SupportPoint> onTop(const std::vector<SupportPoint>& pts, const CouponHole& h) {
    std::vector<SupportPoint> out;
    const double len = norm(h.b - h.a);
    const Vec3 u = (h.b - h.a) / len;  // along the hole: level
    for (const auto& p : pts) {
        const Vec3 q = p.pos - h.a;
        const double s = dot(q, u);
        if (s < -0.2 || s > len + 0.2) continue;
        const double r = h.ra + (h.rb - h.ra) * std::clamp(s / len, 0.0, 1.0);
        const Vec3 off = q - u * s;
        const double across = std::hypot(off.x, off.y);
        if (off.z > 0 && std::abs(std::hypot(across, off.z) - r) <= 0.2 && std::atan2(across, off.z) <= deg2rad(45))
            out.push_back(p);
    }
    return out;
}

}  // namespace

TEST_CASE("crowns coupon: one closed piece, a box of 2 mm walls like TubeRack's", "[calibration][closings]") {
    for (const bool tall : {false, true}) {
        const Coupon c = crownsCoupon(tall);
        CAPTURE(tall);
        CHECK(ops::whyNotSolid(c.mesh).empty());
        CHECK(ops::countPieces(c.mesh) == 1);
        const BBox3 b = c.mesh.bbox();
        CHECK(std::abs(b.max.x - b.min.x - 75.6) < 1e-3);
        CHECK(std::abs(b.max.y - b.min.y - 42.5) < 1e-3);
        CHECK(std::abs(b.max.z - b.min.z - (tall ? 82.0 : 37.0)) < 1e-3);
        CHECK(c.holes.size() == 7);
        CHECK(c.windows.size() == 23);
        // Four of the holes are planned bare on both; the two bores only on the tall one.
        CHECK(std::count_if(c.holes.begin(), c.holes.end(), [](const CouponHole& h) { return h.bare; }) ==
              (tall ? 6 : 4));
    }
}

TEST_CASE("crowns coupon: its 6.5 and 11.8 mm tops get no tips, its controls keep theirs", "[points][closings]") {
    // As the app plans it: 0.05 mm layers, lifted 5 mm, rule 4 on.
    for (const bool tall : {false, true}) {
        Coupon c = crownsCoupon(tall);
        const Transform placed = placeOnPlate(c.mesh, {0, 0, 0}, kLift);
        const PointGenResult r = plan(c.mesh, config(true), true);
        CAPTURE(tall);
        CHECK(r.stats.closings_held >= (tall ? 6 : 4));
        for (CouponHole h : c.holes) {
            h.a = placed.apply(h.a);
            h.b = placed.apply(h.b);
            const auto top = onTop(r.points, h);
            CAPTURE(h.name, top.size());
            if (!h.bare) {
                CHECK(top.size() >= 4);  // the 14 mm hole; the bores on a model under 80 mm
            } else if (h.ra == h.rb) {
                CHECK(top.empty());  // 6.5 and 11.8 mm
            } else if (h.ra < 10) {
                CHECK(top.empty());  // the 17.5 mm bore on the tall coupon
            } else {
                // The 30 mm bore on the tall coupon keeps about 15 of its 25 tips: a row along the rim of its wide
                // face, and one at the top of its narrow face. Where its top is judged, the wall's faces are looked
                // for 1.5 mm into the side beside the gap, which at the narrow end is inside the wide end's opening
                // there. The wall reads 1.87 mm deep, and what is held stops 0.03 mm short of the wide face; the
                // sliver left in each layer gets the tips. (TubeRack's own 30 mm bores read 1.93 mm, centred so
                // that they are held right across.)
                CHECK(top.size() <= 20);
            }
        }
        // The lattice keeps its tips: every window has one on its top half.
        for (CouponWindow w : c.windows) {
            w.centre = placed.apply(w.centre);
            const bool held = std::any_of(r.points.begin(), r.points.end(), [&](const SupportPoint& p) {
                const Vec3 d = p.pos - w.centre;
                const double edge = std::abs(d.x) / w.half_wide + d.z / w.half_tall;
                return std::abs(d.y) < 1.2 && d.z > 0 && edge > 0.9 && edge < 1.1;
            });
            CAPTURE(w.centre.x, w.centre.z);
            CHECK(held);
        }
    }
}
