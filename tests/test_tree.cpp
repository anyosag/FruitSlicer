// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include <algorithm>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "mesh/Placement.h"
#include "mesh/Primitives.h"
#include "printer/PrinterProfile.h"
#include "support/PointGenerator.h"
#include "support/SupportTree.h"
#include "support/SupportValidator.h"

using namespace fruit;
using Catch::Approx;

namespace {

struct Built {
    TriangleMesh model, supports;
    std::vector<SupportPoint> points;
    SupportTree tree;
    ValidationReport check;
};

Built build(TriangleMesh m, double elevation = 5.0, const TreeConfig& tc = {}) {
    placeOnPlate(m, {0, 0, 0}, elevation);
    SupportConfig cfg;
    cfg.layer_height = 0.1;
    Built b;
    b.model = m;
    b.points = generateSupportPoints(m, cfg).points;
    b.tree = buildSupportTree(m, b.points, tc, PrinterProfile::saturn4Ultra16k().minRaftThickness());
    b.supports = b.tree.mesh(tc);
    b.check = validateSupported(m, b.supports, 0.1, cfg.link_grow);
    return b;
}

}  // namespace

TEST_CASE("plate on supports: every point routed, nothing floats, raft is one piece", "[tree]") {
    const Built b = build(makeBox({0, 0, 0}, {20, 20, 2}));
    CHECK(b.tree.dropped_points == 0);
    CHECK(b.tree.heads.size() == b.points.size());
    CHECK(b.check.floating.empty());
    // Raft pads joined by links: one connected piece on the plate.
    CHECK(b.check.first_layer_islands == 1);
    CHECK(b.tree.pillars.size() <= b.tree.heads.size());
    for (const auto& P : b.tree.pillars) CHECK_FALSE(P.on_model);
    // Pillars stand on the raft and reach up to their heads.
    for (const auto& P : b.tree.pillars) CHECK(P.bottom >= b.tree.raft_top);
}

TEST_CASE("heads never pass through the model", "[tree]") {
    // A plate with a wall hanging below one edge: tips next to the wall must
    // tilt away from it rather than through it.
    TriangleMesh m = makeBox({0, 0, 3}, {20, 10, 5});
    m.append(makeBox({0, 0, 0}, {1, 10, 3.2}));
    const Built b = build(m);
    CHECK(b.tree.dropped_points == 0);
    CHECK(b.check.floating.empty());
    AabbTree tree(b.model);
    for (const auto& h : b.tree.heads) {
        // The junction (end of the head) keeps clear of the model.
        CHECK(tree.closest(h.junction).distance >= h.spec.shaft_diameter / 2 + 0.1);
    }
}

TEST_CASE("overhang above a lower part routes around it instead of standing on it", "[tree]") {
    // A low block with a shelf above that sticks out 6 mm past it on one
    // side and only 1 mm on the other: tips under the narrow side must route
    // diagonally out to a clear drop.
    TriangleMesh m = makeBox({0, 0, 0}, {10, 10, 6});
    m.append(makeBox({-1, -1, 6}, {16, 11, 8}));
    const Built b = build(m);
    CHECK(b.tree.dropped_points == 0);
    CHECK(b.check.floating.empty());
    size_t on_model = 0;
    for (const auto& P : b.tree.pillars) on_model += P.on_model;
    CHECK(on_model == 0);
}

TEST_CASE("tall supports get braces outside the flex zone", "[tree]") {
    const Built b = build(makeBox({0, 0, 0}, {30, 30, 2}), 30.0);
    CHECK_FALSE(b.tree.braces.empty());
    TreeConfig tc;
    for (const auto& r : b.tree.braces) {
        const double top = std::max(r.a.z, r.b.z);
        double nearest_head = 1e9;
        for (const auto& h : b.tree.heads)
            if (distance(h.junction.xy(), r.a.xy()) < 1e-6 || distance(h.junction.xy(), r.b.xy()) < 1e-6)
                nearest_head = std::min(nearest_head, h.junction.z);
        if (nearest_head < 1e9) CHECK(top <= nearest_head - tc.flex_zone + 1e-6);
    }
    CHECK(b.check.floating.empty());
}

TEST_CASE("branching merges pillars into fewer trunks without touching the model", "[tree]") {
    TriangleMesh m = makeBox({0, 0, 0}, {24, 24, 2});
    placeOnPlate(m, {0, 0, 0}, 20.0);
    SupportConfig cfg;
    cfg.layer_height = 0.1;
    const auto pts = generateSupportPoints(m, cfg).points;
    TreeConfig flat;
    flat.merge_pillars = false;
    TreeConfig branch;
    const double raft = PrinterProfile::saturn4Ultra16k().minRaftThickness();
    const SupportTree a = buildSupportTree(m, pts, flat, raft);
    const SupportTree b = buildSupportTree(m, pts, branch, raft);
    CHECK(b.trunks() < a.trunks());
    CHECK_FALSE(b.branches.empty());
    // Every trunk carries at most the configured number of tips.
    for (const auto& P : b.pillars)
        if (!P.merged && !P.on_model) CHECK(P.tips <= branch.max_tips_per_trunk);
    // Branch rods keep clear of the model.
    AabbTree tree(m);
    for (const auto& r : b.branches)
        for (int k = 0; k <= 10; ++k) {
            const Vec3 q = r.a + (r.b - r.a) * (k / 10.0);
            CHECK(tree.closest(q).distance >= r.diameter / 2 + branch.clearance - 1e-6);
        }
    const ValidationReport v = validateSupported(m, b.mesh(branch), 0.1, cfg.link_grow);
    CHECK(v.floating.empty());
    CHECK(v.first_layer_islands == 1);
}

namespace {

// A box open on one side, stood so the opening faces sideways (+Y), like a
// lid stood on its edge: 30 mm cube, 2 mm walls. Tips on the ceiling deep
// inside have no clear drop (the floor is 26 mm below) and no way out.
TriangleMesh sidewaysCap() {
    TriangleMesh m = makeBox({0, 0, 0}, {30, 2, 30});  // back wall
    m.append(makeBox({0, 0, 0}, {30, 30, 2}));         // floor
    m.append(makeBox({0, 0, 28}, {30, 30, 30}));       // ceiling
    m.append(makeBox({0, 0, 0}, {2, 30, 30}));         // side walls
    m.append(makeBox({28, 0, 0}, {30, 30, 30}));
    return m;
}

// Supports that stand on the model: none (`drop` 0), or only on short drops.
TreeConfig standingUpTo(double drop) {
    TreeConfig tc;
    tc.max_model_anchor_drop = drop;
    return tc;
}

// How near the model a pillar's shaft comes, beyond its own surface.
double shaftGap(const AabbTree& tree, const Pillar& P) {
    double gap = 1e9;
    for (double z = P.top; z >= P.bottom; z -= 0.25) gap = std::min(gap, tree.closest({P.xy.x, P.xy.y, z}).distance);
    return gap - P.diameter / 2;
}

// How near the model a pillar's foot comes, beyond its own surface: the cone
// from its width where it stands up to the shaft's.
double footGapOf(const AabbTree& tree, const Pillar& P, const TreeConfig& tc, double raft_top) {
    const double base = P.base_diameter > 0 ? std::max(P.base_diameter, P.diameter) : tc.base_diameter;
    double gap = 1e9;
    for (double z = raft_top + 0.01; z < raft_top + tc.base_height; z += 0.05) {
        const double t = (z - raft_top) / tc.base_height;
        gap = std::min(gap, tree.closest({P.xy.x, P.xy.y, z}).distance - (base / 2 + (P.diameter / 2 - base / 2) * t));
    }
    return gap;
}

}  // namespace

TEST_CASE("on the plate, a support's foot keeps clear of the model beside it", "[tree]") {
    // A block standing on the plate with two shelves 3 mm up, like mounting
    // tabs: one sticks out 6 mm, the other only 1.6 mm, so the supports under
    // it stand right beside the block's wall.
    TriangleMesh m = makeBox({0, 0, 0}, {30, 8, 10});
    m.append(makeBox({8, -6, 3}, {22, 0.5, 6}));
    m.append(makeBox({8, 7.5, 3}, {22, 9.6, 6}));
    placeOnPlate(m, {0, 0, 0}, 0.0);
    SupportConfig cfg;
    cfg.layer_height = 0.05;
    cfg.on_plate = true;
    const auto pts = generateSupportPoints(m, cfg).points;
    TreeConfig tc;
    tc.raft = RaftStyle::None;
    const SupportTree t = buildSupportTree(m, pts, tc, 0.0);
    REQUIRE(t.trunks() > 0);
    CHECK(t.dropped_points == 0);
    const AabbTree tree(m);
    std::size_t reported = 0, narrowed = 0, full = 0;
    for (const auto& w : t.warnings) reported += w.code == "foot_near_model";
    for (const auto& P : t.pillars) {
        if (P.on_model || P.merged) continue;
        // The shaft keeps the gap, and so does the foot: never into the model.
        CHECK(shaftGap(tree, P) >= tc.clearance - 1e-6);
        const double gap = footGapOf(tree, P, tc, t.raft_top);
        CHECK(gap >= tc.clearance - 1e-6);
        // On the plate the foot keeps the wider gap, unless it is one of the
        // reported ones with no room anywhere.
        if (reported == 0) CHECK(gap >= tc.foot_clearance - 1e-6);
        REQUIRE(P.base_diameter > 0);
        CHECK(P.base_diameter >= P.diameter - 1e-9);
        CHECK(P.base_diameter <= tc.base_diameter + 1e-9);
        narrowed += P.base_diameter < tc.base_diameter - 1e-6;
        full += P.base_diameter > tc.base_diameter - 1e-6;
    }
    // Beside the wall the feet are narrower; out under the shelf's far edge
    // they have their full width.
    CHECK(narrowed > 0);
    CHECK(full > 0);
    // Model and supports together: nothing floats, and the model's own
    // outline is not grown by a foot (the two only meet at the tips).
    CHECK(validateSupported(m, t.mesh(tc), cfg.layer_height, cfg.link_grow).floating.empty());
}

namespace {

// How far to the side the model is from a head that stands on the plate,
// within the first half millimetre above it and away from its own tip: beyond
// the head's surface. 1e9 with nothing beside it.
double plateHeadSideGap(const AabbTree& tree, const Head& h) {
    const Vec3 d = normalized(h.dir);
    const double down = std::max(-d.z, 1e-6), slant = std::sqrt(std::max(0.0, 1 - down * down));
    const double r_tip = h.spec.tip_diameter / 2, r_back = h.spec.shaft_diameter / 2;
    Vec3 front = h.contact + d * (r_tip - h.spec.penetration);
    if (front.z < r_tip) front = front - d * ((r_tip - front.z) / down);
    Vec3 end = front;
    double r_end = r_tip;
    if (front.z > r_back * slant) {
        end = front + d * ((front.z - r_back * slant) / down);
        r_end = r_back;
    }
    double gap = 1e9;
    auto look = [&](const Vec3& q, double r) {
        for (int k = 0; k < 32; ++k) {
            const double a = 2 * kPi * k / 32;
            const auto hit = tree.raycast(q, {std::cos(a), std::sin(a), 0}, r + 1.0);
            if (hit) gap = std::min(gap, distance(q, hit->point) - r);
        }
    };
    for (double z = 0.02; z <= std::min(end.z, 0.5); z += 0.05) look({end.x, end.y, z}, r_end);
    const double len = distance(front, end);
    for (double t = len; t >= 0.25; t -= 0.05) {
        const Vec3 q = front + d * t;
        if (q.z > 0.5) break;
        look(q, r_tip + (r_end - r_tip) * (len > 0 ? t / len : 1.0));
    }
    return gap;
}

}  // namespace

TEST_CASE("on the plate, a low tip's head keeps clear of the wall beside it", "[tree]") {
    // A block on the plate with a shelf 1.2 mm up that sticks out only
    // 1.5 mm: its tips are so low that their heads stand on the plate
    // themselves, right beside the block's wall.
    TriangleMesh m = makeBox({0, 0, 0}, {30, 8, 10});
    m.append(makeBox({4, -1.5, 1.2}, {26, 0.5, 4}));
    placeOnPlate(m, {0, 0, 0}, 0.0);
    SupportConfig cfg;
    cfg.layer_height = 0.05;
    cfg.on_plate = true;
    cfg.self_supported_reach = 0.3;  // so that a shelf this short gets tips at all
    const auto pts = generateSupportPoints(m, cfg).points;
    REQUIRE_FALSE(pts.empty());
    const AabbTree tree(m);
    auto build = [&](double foot_clearance) {
        TreeConfig tc;
        tc.raft = RaftStyle::None;
        tc.foot_clearance = foot_clearance;
        return buildSupportTree(m, pts, tc, 0.0);
    };
    const SupportTree t = build(TreeConfig{}.foot_clearance);
    std::size_t standing = 0, reported = 0;
    for (const auto& w : t.warnings) reported += w.code == "foot_near_model";
    for (const Head& h : t.heads) {
        if (!h.on_plate) continue;
        ++standing;
        // Never into the model, and its base no wider than the usual shaft.
        const double gap = plateHeadSideGap(tree, h);
        CHECK(gap >= 0);
        CHECK(h.spec.shaft_diameter >= h.spec.tip_diameter - 1e-9);
    }
    REQUIRE(standing > 0);
    CHECK(t.dropped_points == 0);
    // Each one keeps the wider gap, or is one of those reported.
    std::size_t close = 0;
    for (const Head& h : t.heads) close += h.on_plate && plateHeadSideGap(tree, h) < TreeConfig{}.foot_clearance - 1e-6;
    CHECK(close <= reported);
    // The rule is what does it: with only the ordinary gap asked for, more of
    // them stand nearer the wall.
    const SupportTree u = build(TreeConfig{}.clearance);
    std::size_t close_before = 0;
    for (const Head& h : u.heads) close_before += h.on_plate && plateHeadSideGap(tree, h) < TreeConfig{}.foot_clearance - 1e-6;
    CHECK(close_before > close);
    TreeConfig none;
    none.raft = RaftStyle::None;
    CHECK(validateSupported(m, t.mesh(none), cfg.layer_height, cfg.link_grow).floating.empty());
}

TEST_CASE("with a low lift, a foot on the raft keeps the gap to a low part of the model", "[tree]") {
    // A step 1.5 mm above the plate with a shelf above it: supports under the
    // shelf stand beside the step, whose bottom is level with their feet.
    TriangleMesh m = makeBox({0, 0, 0}, {30, 8, 12});
    m.append(makeBox({8, -6, 5}, {22, 0.5, 8}));
    placeOnPlate(m, {0, 0, 0}, 1.5);
    SupportConfig cfg;
    cfg.layer_height = 0.05;
    const auto pts = generateSupportPoints(m, cfg).points;
    TreeConfig tc;
    const SupportTree t = buildSupportTree(m, pts, tc, PrinterProfile::saturn4Ultra16k().minRaftThickness());
    REQUIRE(t.trunks() > 0);
    const AabbTree tree(m);
    for (const auto& P : t.pillars) {
        if (P.on_model || P.merged) continue;
        CHECK(shaftGap(tree, P) >= tc.clearance - 1e-6);
        CHECK(footGapOf(tree, P, tc, t.raft_top) >= tc.clearance - 1e-6);
    }
}

TEST_CASE("a pillar's shaft keeps its gap to a thin wall beside it", "[tree]") {
    // A plate with a thin fin hanging under it: a wall that four rays around
    // a shaft can pass on either side of.
    TriangleMesh m = makeBox({0, 0, 10}, {30, 30, 12});
    m.append(makeBox({14.8, 0, 2}, {15.2, 30, 10.2}));
    const Built b = build(m);
    CHECK(b.tree.dropped_points == 0);
    const AabbTree tree(b.model);
    TreeConfig tc;
    for (const auto& P : b.tree.pillars)
        if (!P.on_model) CHECK(shaftGap(tree, P) >= tc.clearance - 1e-6);
}

TEST_CASE("a ceiling the raft can't be reached from is held from the model below", "[tree]") {
    // Inside the cap the floor is 26 mm under the ceiling: every tip there
    // gets a support that stands on the floor, and none is left out.
    const Built b = build(sidewaysCap());
    CHECK(b.tree.dropped_points == 0);
    CHECK(b.tree.heads.size() == b.points.size());
    CHECK(b.check.floating.empty());
    const AabbTree tree(b.model);
    TreeConfig tc;
    std::size_t standing = 0, tall = 0;
    for (const auto& P : b.tree.pillars) {
        if (!P.on_model) continue;
        ++standing;
        tall += P.top - P.anchor_contact.z > 20;
        // The foot holds what the tip holds: as wide a contact.
        REQUIRE(P.heads.size() == 1);
        CHECK(P.foot_tip >= b.tree.heads[P.heads[0]].spec.tip_diameter - 1e-9);
        CHECK(P.foot_tip >= 0.4 - 1e-9);
        // It stands on a face it can sit on, and its shaft keeps the gap to the model.
        CHECK(P.anchor_normal.z >= std::cos(deg2rad(tc.max_anchor_slope_deg)) - 1e-9);
        CHECK(shaftGap(tree, P) >= tc.clearance - 1e-6);
    }
    CHECK(standing > 0);
    CHECK(tall > 0);
    // Tall ones are braced with each other, as those on the raft are.
    std::size_t braced = 0;
    for (const auto& r : b.tree.braces)
        for (const auto& P : b.tree.pillars)
            if (P.on_model && distance(r.a.xy(), P.xy) < 1e-6) {
                ++braced;
                break;
            }
    CHECK(braced > 0);
    // Nothing is left for the check of dropped tips to find.
    std::vector<SupportPoint> built, dropped;
    splitByTree(b.tree, b.points, built, dropped);
    CHECK(dropped.empty());
}

TEST_CASE("a support never stands on a steep face or brushes a wall on its way down", "[tree]") {
    // The same cap with a thin fin and a steep cone on its floor, both under
    // the ceiling: supports that would graze the fin or land on the cone's
    // flank stand a short walk away instead.
    TriangleMesh m = sidewaysCap();
    m.append(makeBox({14, 2, 1.5}, {15, 30, 20}));
    m.append(makeCone({22, 14, 1.5}, 4, {22, 14, 16}, 48));
    const Built b = build(m);
    CHECK(b.check.floating.empty());
    const AabbTree tree(b.model);
    TreeConfig tc;
    std::size_t standing = 0;
    for (const auto& P : b.tree.pillars) {
        if (!P.on_model) continue;
        ++standing;
        CHECK(P.anchor_normal.z >= std::cos(deg2rad(tc.max_anchor_slope_deg)) - 1e-9);
        CHECK(shaftGap(tree, P) >= tc.clearance - 1e-6);
    }
    CHECK(standing > 0);
    // Whatever found no spot to stand on is reported, not hidden.
    std::size_t no_route = 0;
    for (const auto& w : b.tree.warnings) no_route += w.code == "no_route";
    CHECK(no_route == b.tree.dropped_points - static_cast<std::size_t>(std::count_if(
                                                  b.tree.warnings.begin(), b.tree.warnings.end(),
                                                  [](const Warning& w) { return w.code == "head_blocked"; })));
}

TEST_CASE("with standing on the model turned off, a tip with no way down is left out and reported", "[tree]") {
    const Built b = build(sidewaysCap(), 5.0, standingUpTo(0));
    CHECK(b.tree.dropped_points > 0);
    for (const auto& P : b.tree.pillars) CHECK_FALSE(P.on_model);
    // A limit on the drop keeps short stands only.
    const Built c = build(sidewaysCap(), 5.0, standingUpTo(10));
    CHECK(c.tree.dropped_points > 0);
    for (const auto& P : c.tree.pillars)
        if (P.on_model) CHECK(P.top - P.anchor_contact.z <= 10 + 1e-9);
}

TEST_CASE("where no head fits, a short column from the model just below holds the tip", "[tree]") {
    // A small block 0.6 mm above the middle of a slab, like a thread's turn
    // above the one before: a head is longer than the gap.
    TriangleMesh m = makeBox({0, 0, 0}, {20, 20, 2});
    m.append(makeBox({9, 9, 2.6}, {11, 11, 4.6}));
    placeOnPlate(m, {0, 0, 0}, 5.0);
    SupportConfig cfg;
    cfg.layer_height = 0.05;
    const auto pts = generateSupportPoints(m, cfg).points;
    std::size_t under_block = 0;
    for (const auto& p : pts) under_block += p.pos.z > 7.5;
    REQUIRE(under_block > 0);
    const double raft = PrinterProfile::saturn4Ultra16k().minRaftThickness();
    TreeConfig tc;
    const SupportTree t = buildSupportTree(m, pts, tc, raft);
    CHECK(t.dropped_points == 0);
    CHECK(t.heads.size() == pts.size());
    REQUIRE(t.stubs.size() == under_block);
    for (const Rod& r : t.stubs) {
        // Straight up, from inside the slab to inside the block, as wide as its tip.
        CHECK(distance(r.a.xy(), r.b.xy()) < 1e-9);
        CHECK(r.a.z > 7.6);
        CHECK(r.b.z < 7.0);
        CHECK(r.diameter >= 0.2 - 1e-9);
    }
    const TriangleMesh sup = t.mesh(tc);
    CHECK(validateSupported(m, sup, cfg.layer_height, cfg.link_grow).floating.empty());
    // Its tips count as tied to the slab, which the slab's own tips hold.
    std::vector<SupportPoint> from_raft;
    std::vector<TiedTip> tied;
    splitByFooting(t, pts, from_raft, tied);
    CHECK(tied.size() == under_block);
    for (const auto& x : tied) CHECK(x.foot.z == Approx(7.0).margin(0.01));
    CHECK(layersShortOfRaftHold(m, cfg, from_raft, tied) == 0);

    // Turned off, the tip is left out and the block is reported as floating.
    TreeConfig none;
    none.max_stub_length = 0;
    const SupportTree u = buildSupportTree(m, pts, none, raft);
    CHECK(u.dropped_points == under_block);
    CHECK(u.stubs.empty());
    CHECK_FALSE(validateSupported(m, u.mesh(none), cfg.layer_height, cfg.link_grow).floating.empty());
}

TEST_CASE("a tip whose support stands on the print adds no hold from the raft", "[tree]") {
    // A block on supports: every layer is 400 mm², held by the tips under it.
    TriangleMesh m = makeBox({0, 0, 0}, {20, 20, 10});
    placeOnPlate(m, {0, 0, 0}, 5.0);
    SupportConfig cfg;
    cfg.layer_height = 0.1;
    const auto pts = generateSupportPoints(m, cfg).points;
    REQUIRE(pts.size() > 4);
    // None tied: nothing to report.
    CHECK(layersShortOfRaftHold(m, cfg, pts, {}) == 0);
    // Were they all standing on the print, nothing would hold it on the raft:
    // every layer is short (the plan, counting them, had enough).
    std::vector<TiedTip> all;
    for (const auto& p : pts) all.push_back({p, p.pos});
    CHECK(layersShortOfRaftHold(m, cfg, {}, all) == 100);
    // With a couple of them tied the rest still hold it.
    std::vector<SupportPoint> most(pts.begin() + 2, pts.end());
    const std::vector<TiedTip> two{{pts[0], pts[0].pos}, {pts[1], pts[1].pos}};
    double hold = 0;
    for (const auto& p : most) hold += cfg.capacity.capacity(p.tip_diameter);
    REQUIRE(hold >= 400);
    CHECK(layersShortOfRaftHold(m, cfg, most, two) == 0);
}

TEST_CASE("a piece held only from the model below is held by what holds the model", "[tree]") {
    // A small block floating 6 mm above the middle of a wide slab, too far in
    // for a route out: its supports stand on the slab.
    TriangleMesh m = makeBox({0, 0, 0}, {60, 60, 4});
    m.append(makeBox({25, 25, 10}, {35, 35, 12}));
    placeOnPlate(m, {0, 0, 0}, 5.0);
    SupportConfig cfg;
    cfg.layer_height = 0.1;
    const auto pts = generateSupportPoints(m, cfg).points;
    TreeConfig tc;
    const SupportTree tree = buildSupportTree(m, pts, tc, PrinterProfile::saturn4Ultra16k().minRaftThickness());
    REQUIRE(tree.dropped_points == 0);
    std::vector<SupportPoint> from_raft;
    std::vector<TiedTip> tied;
    splitByFooting(tree, pts, from_raft, tied);
    CHECK(from_raft.size() + tied.size() == tree.heads.size());
    std::size_t standing = 0;
    for (const auto& P : tree.pillars) standing += P.on_model;
    REQUIRE(standing > 0);
    CHECK(tied.size() == standing);
    for (const auto& t : tied) {
        CHECK(t.tip.pos.z > 14.5);             // under the block
        CHECK(t.foot.z == Approx(9).margin(0.01));  // on top of the slab
    }
    // The block hangs from the slab, and the slab's own tips hold both.
    CHECK(layersShortOfRaftHold(m, cfg, from_raft, tied) == 0);
    // Were its supports standing on nothing that is held, the block's 20
    // layers would have no hold at all.
    std::vector<TiedTip> adrift = tied;
    for (auto& t : adrift) t.foot = {t.foot.x, t.foot.y, 1.0};
    CHECK(layersShortOfRaftHold(m, cfg, from_raft, adrift) == 20);
}

TEST_CASE("tips the tree drops are checked: what only they held is reported, and where", "[tree]") {
    const Built b = build(sidewaysCap(), 5.0, standingUpTo(10));
    REQUIRE(b.tree.dropped_points > 0);
    std::vector<SupportPoint> built, dropped;
    splitByTree(b.tree, b.points, built, dropped);
    CHECK(built.size() == b.tree.heads.size());
    CHECK(dropped.size() == b.tree.dropped_points);
    SupportConfig cfg;
    cfg.layer_height = 0.1;
    const DropCheck dc = checkDroppedTips(b.model, cfg, built, dropped);
    CHECK(dc.any());
    CHECK(dc.samples > 0);
    CHECK(dc.tips > 0);
    CHECK(dc.tips <= dropped.size());
    REQUIRE(dc.at.size() == dc.tips);
    // They were on the ceiling inside the cap, away from its opening.
    const BBox3 box = b.model.bbox();
    for (const Vec3& p : dc.at) {
        CHECK(p.z == Approx(box.max.z - 2).margin(0.2));
        CHECK(p.x > box.min.x + 2);
        CHECK(p.x < box.max.x - 2);
        CHECK(p.y < box.max.y - 5);
    }
    for (size_t i = 1; i < dc.at.size(); ++i) CHECK(dc.at[i - 1].z <= dc.at[i].z);
}

TEST_CASE("a dropped tip whose spot other tips still hold is not counted", "[tree]") {
    const Built b = build(makeBox({0, 0, 0}, {20, 20, 2}));
    REQUIRE(b.tree.dropped_points == 0);
    REQUIRE_FALSE(b.points.empty());
    // A second tip right next to a built one, "dropped": everything it covered
    // the built tips cover too, and the plan had hold to spare without it.
    SupportPoint extra = b.points.front();
    extra.pos.x += 0.2;
    SupportConfig cfg;
    cfg.layer_height = 0.1;
    const DropCheck dc = checkDroppedTips(b.model, cfg, b.points, {extra});
    CHECK_FALSE(dc.any());
    CHECK(dc.tips == 0);
    CHECK(dc.at.empty());
    // Nothing dropped: nothing to check.
    CHECK_FALSE(checkDroppedTips(b.model, cfg, b.points, {}).any());
}

TEST_CASE("on the plate, tips just above it stand on it and nothing reaches below it", "[tree]") {
    // A block flat on the plate with ledges sticking out low on one side,
    // like stubs on a bar: their undersides 1 and 2 mm up are closer to the
    // plate than a head and its joint are long, and one 0.15 mm up is
    // closer than its tip is wide.
    TriangleMesh m = makeBox({0, 0, 0}, {30, 6, 8});
    m.append(makeBox({2, -5, 1}, {7, 0.5, 4}));
    m.append(makeBox({12, -5, 2}, {17, 0.5, 4}));
    m.append(makeBox({22, -5, 0.15}, {27, 0.5, 4}));
    placeOnPlate(m, {0, 0, 0}, 0.0);
    SupportConfig cfg;
    cfg.layer_height = 0.05;
    cfg.on_plate = true;
    const auto pts = generateSupportPoints(m, cfg).points;
    TreeConfig tc;
    tc.raft = RaftStyle::None;
    const SupportTree t = buildSupportTree(m, pts, tc, 0.0);
    REQUIRE_FALSE(t.heads.empty());
    std::size_t low = 0;
    for (const auto& h : t.heads) low += h.contact.z < 2.5;
    REQUIRE(low > 0);  // tips under the ledges
    CHECK(t.dropped_points == 0);
    const TriangleMesh s = t.mesh(tc);
    CHECK(s.bbox().min.z >= -1e-9);
    // Held from the plate up: nothing floats.
    const ValidationReport v = validateSupported(m, s, cfg.layer_height, cfg.link_grow);
    CHECK(v.floating.empty());
}
