// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
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

Built build(TriangleMesh m, double elevation = 5.0) {
    placeOnPlate(m, {0, 0, 0}, elevation);
    SupportConfig cfg;
    cfg.layer_height = 0.1;
    Built b;
    b.model = m;
    b.points = generateSupportPoints(m, cfg).points;
    TreeConfig tc;
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

}  // namespace

TEST_CASE("tips the tree drops are checked: what only they held is reported, and where", "[tree]") {
    const Built b = build(sidewaysCap());
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
