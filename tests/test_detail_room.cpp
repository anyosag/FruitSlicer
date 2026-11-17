// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Slots and holes no support goes into (DetailMap), and shafts that take the
// spot with the most room (TreeConfig::wanted_gap), not the first that keeps
// the gap.
#include <algorithm>
#include <cmath>
#include <memory>

#include <catch2/catch_test_macros.hpp>

#include "layers/LayerStack.h"
#include "mesh/AabbTree.h"
#include "mesh/Primitives.h"
#include "ops/MeshOps.h"
#include "printer/PrinterProfile.h"
#include "support/DetailMap.h"
#include "support/PointGenerator.h"
#include "support/SupportCheck.h"
#include "support/SupportTree.h"

using namespace fruit;
using fruit::ops::MeshResult;
using fruit::ops::subtractMeshes;

namespace {

// A 1.5 mm floor 5 mm up with six 0.6 mm slots right through it, 1.4 mm apart.
TriangleMesh slottedFloor() {
    TriangleMesh plate = makeBox({0, 0, 5}, {16, 12, 6.5});
    for (int i = 0; i < 6; ++i) {
        const double x0 = 3.4 + i * 2.0;
        const MeshResult cut = subtractMeshes(plate, makeBox({x0, 2, 4}, {x0 + 0.6, 10, 7.5}));
        REQUIRE(cut.ok);
        plate = cut.mesh;
    }
    return plate;
}

// The nearest the model comes to pillar P's shaft, below its top.
double pillarGap(const AabbTree& tree, const Pillar& P) {
    double gap = 1e9;
    for (double z = P.bottom; z <= P.top - 0.5; z += 0.1)
        gap = std::min(gap, tree.distanceBelow({P.xy.x, P.xy.y, z}, 10.0) - P.diameter / 2);
    return gap;
}

}  // namespace

TEST_CASE("the detail map finds narrow slots and leaves out inside corners and wide gaps", "[details]") {
    TriangleMesh m = slottedFloor();
    // An L of two blocks (an inside corner) and a 2.5 mm gap between two more: no slots.
    m.append(makeBox({30, 0, 5}, {40, 3, 8}));
    m.append(makeBox({30, 3, 5}, {33, 10, 8}));
    m.append(makeBox({50, 0, 5}, {53, 10, 8}));
    m.append(makeBox({55.5, 0, 5}, {58.5, 10, 8}));
    const LayerStack ls = sliceModel(m, 0.05);
    const DetailMap d = DetailMap::build(ls, 2.0, 2.0);
    // One region per slot (the same shape all the way up), each with the space under its mouth.
    CHECK(d.slotCount() == 6);
    CHECK(d.regions().size() == 12);
    for (const DetailRegion& r : d.regions()) {
        CAPTURE(r.box.min.x, r.box.max.x, r.z0, r.z1, r.mouth);
        CHECK(r.box.max.x < 16);  // all on the slotted floor
        if (r.mouth) CHECK(r.z0 < 5 - 1.9);
        else CHECK(r.z1 - r.z0 > 1.4);
    }
    CHECK(d.reaches({3.7, 6, 5.5}, 0));        // in a slot
    CHECK(d.reaches({3.7, 6, 4}, 0));          // under its mouth
    CHECK_FALSE(d.reaches({3.7, 6, 2.5}, 0));  // further down
    CHECK_FALSE(d.reaches({4.7, 6, 5.5}, 0));  // in the floor between two slots...
    CHECK(d.reaches({4.7, 6, 5.5}, 0.8));      // ...but within 0.8 mm of one
    CHECK(d.mesh().faces.size() > 0);
}

TEST_CASE("heads beside a slot keep out of it and the space under its mouth", "[details][tree]") {
    const TriangleMesh m = slottedFloor();
    SupportConfig cfg;
    cfg.layer_height = 0.05;
    const PointGenResult pts = generateSupportPoints(m, cfg);
    TreeConfig tc;
    auto details = std::make_shared<const DetailMap>(DetailMap::build(*pts.analysis.layers, cfg.detail_width, 2.0));
    tc.details = details;
    const double raft = PrinterProfile::saturn4Ultra16k().minRaftThickness();
    const SupportTree t = buildSupportTree(m, pts.points, tc, raft);
    CHECK(t.dropped_points == 0);
    std::size_t near_slots = 0;
    for (const Head& h : t.heads) {
        const Vec3 start = h.contact + h.dir * (h.spec.tip_diameter / 2 + 0.05);
        near_slots += details->reaches(h.contact, 0.6);
        for (const double f : {0.4, 0.7, 1.0}) {
            const Vec3 q = start + (h.junction - start) * f;
            const double r = h.spec.tip_diameter / 2 + (h.spec.shaft_diameter - h.spec.tip_diameter) / 2 * f;
            CAPTURE(h.contact.x, h.contact.y, f);
            CHECK_FALSE(details->reaches(q, r));
        }
    }
    CHECK(near_slots > 0);  // there are heads right beside the slots
    for (const Pillar& P : t.pillars)
        for (double z = P.bottom; z <= P.top; z += 0.25) CHECK_FALSE(details->reaches({P.xy.x, P.xy.y, z}, P.diameter / 2));
}

TEST_CASE("a shaft in a hollow post stands where it has room, not beside the wall", "[tree][room]") {
    // A post 8 × 8 mm, hollow from below (6 × 6 mm inside, 10 mm deep), 5 mm up; a tip on its ceiling
    // 1.1 mm from a wall: straight down, its 1 mm shaft keeps 0.6 mm to the wall, past the 0.3 mm gap.
    const MeshResult post = subtractMeshes(makeBox({0, 0, 5}, {8, 8, 17}), makeBox({1, 1, 4}, {7, 7, 15}));
    REQUIRE(post.ok);
    SupportPoint p;
    p.pos = {2.1, 4, 15};
    p.normal = {0, 0, -1};
    p.tip_diameter = 0.4;
    const double raft = PrinterProfile::saturn4Ultra16k().minRaftThickness();
    AabbTree tree;
    tree.build(post.mesh);
    TreeConfig tc;
    const SupportTree t = buildSupportTree(post.mesh, {p}, tc, raft);
    REQUIRE(t.pillars.size() == 1);
    CHECK(pillarGap(tree, t.pillars[0]) >= tc.wanted_gap - 0.05);
    // As it was: the first spot that keeps the gap.
    tc.wanted_gap = 0;
    const SupportTree old = buildSupportTree(post.mesh, {p}, tc, raft);
    REQUIRE(old.pillars.size() == 1);
    CHECK(pillarGap(tree, old.pillars[0]) < 0.7);
}

TEST_CASE("the check after making lists supports near the model, tips over a slot's edge and supports in slots",
          "[details][check]") {
    const double raft = PrinterProfile::saturn4Ultra16k().minRaftThickness();
    // A shaft built the old way beside a post's wall: 0.15 mm gap, the first spot that keeps it.
    const MeshResult post = subtractMeshes(makeBox({0, 0, 5}, {8, 8, 17}), makeBox({1, 1, 4}, {7, 7, 15}));
    REQUIRE(post.ok);
    SupportPoint p;
    p.pos = {1.75, 4, 15};
    p.normal = {0, 0, -1};
    p.tip_diameter = 0.4;
    TreeConfig old;
    old.clearance = 0.15;
    old.wanted_gap = 0;
    const SupportTree near = buildSupportTree(post.mesh, {p}, old, raft);
    const TreeConfig now;
    const SupportConfig cfg;
    const GapCheck a = checkSupportGaps(post.mesh, near, now, cfg, nullptr);
    CHECK(a.near_model >= 1);
    CHECK(a.nearest < now.clearance);
    REQUIRE_FALSE(a.warnings.empty());
    CHECK(a.warnings.front().code == "shaft_near_model");
    // Made as it is now, nothing to list.
    CHECK(checkSupportGaps(post.mesh, buildSupportTree(post.mesh, {p}, now, raft), now, cfg, nullptr).near_model == 0);

    // Tips planned the old way on a slotted floor: on the slots' edges.
    const TriangleMesh floor = slottedFloor();
    SupportConfig before;
    before.layer_height = 0.05;
    before.tip_edge_margin = -1;
    before.detail_width = 0;
    const PointGenResult old_pts = generateSupportPoints(floor, before);
    auto details = std::make_shared<const DetailMap>(DetailMap::build(*old_pts.analysis.layers, 2.0, 2.0));
    const SupportTree old_tree = buildSupportTree(floor, old_pts.points, TreeConfig{}, raft);  // no slot map
    const GapCheck b = checkSupportGaps(floor, old_tree, now, cfg, details.get());
    CHECK(b.over_edge > 0);
    CHECK(b.in_slot > 0);
    // As made now: none.
    SupportConfig cur;
    cur.layer_height = 0.05;
    const PointGenResult pts = generateSupportPoints(floor, cur);
    TreeConfig tc;
    tc.details = details;
    const GapCheck c = checkSupportGaps(floor, buildSupportTree(floor, pts.points, tc, raft), tc, cur, details.get());
    CHECK(c.over_edge == 0);
    CHECK(c.in_slot == 0);
}

TEST_CASE("something that starts in mid-air right over a slot is held from inside it", "[details][tree]") {
    // A floor 5 mm up with a 1.8 mm slot right through it, and a small block in mid-air 2 mm over the slot:
    // the only way down from the block is through the slot.
    MeshResult cut = subtractMeshes(makeBox({0, 0, 5}, {16, 12, 6.5}), makeBox({7.1, 1, 4}, {8.9, 11, 7.5}));
    REQUIRE(cut.ok);
    TriangleMesh m = cut.mesh;
    m.append(makeBox({7.7, 5.7, 8.5}, {8.3, 6.3, 9.5}));
    auto details = std::make_shared<const DetailMap>(DetailMap::build(sliceModel(m, 0.05), 2.0, 2.0));
    REQUIRE(details->slotCount() > 0);
    SupportPoint p;
    p.pos = {8, 6, 8.5};
    p.normal = {0, 0, -1};
    p.tip_diameter = 0.3;
    p.category = PointCategory::Island;
    TreeConfig tc;
    tc.details = details;
    const double raft = PrinterProfile::saturn4Ultra16k().minRaftThickness();
    const SupportTree t = buildSupportTree(m, {p}, tc, raft);
    CHECK(t.dropped_points == 0);
    REQUIRE(t.pillars.size() == 1);
    // ...and the check after making doesn't list it: it is in the slot on purpose.
    CHECK(checkSupportGaps(m, t, tc, SupportConfig{}, details.get()).in_slot == 0);
    // A tip beside the slot (not over it) still keeps out.
    SupportPoint q = p;
    q.pos = {6.4, 6, 5};
    q.category = PointCategory::Overhang;
    const SupportTree u = buildSupportTree(m, {q}, tc, raft);
    for (const Head& h : u.heads) CHECK_FALSE(details->reaches(h.junction, h.spec.shaft_diameter / 2));
}

TEST_CASE("by the plate, no support stands in a hole a foot would mostly fill; a wider hole may hold one", "[details][plate]") {
    // A block on the plate with a hole right through it, and a shelf over the hole 5 mm up held by the block.
    auto holed = [](double w) {
        MeshResult cut = subtractMeshes(makeBox({-10, -10, 0}, {10, 10, 3}), makeBox({-w / 2, -w / 2, -1}, {w / 2, w / 2, 4}));
        REQUIRE(cut.ok);
        TriangleMesh m = cut.mesh;
        m.append(makeBox({-10, -10, 5}, {10, 10, 6}));
        m.append(makeBox({-10, -10, 3}, {-8, 10, 5}));
        return m;
    };
    TreeConfig tc;
    tc.raft = RaftStyle::None;
    tc.foot_clearance = bottomLayerGap(tc.clearance, 27.5 / 1.5);
    const double limit = 2 * (tc.base_diameter + 2 * tc.foot_clearance);  // 8.8 mm
    SupportPoint p;
    p.pos = {0, 0, 5};
    p.normal = {0, 0, -1};
    p.tip_diameter = 0.4;
    for (const double w : {5.0, 6.0, 12.0}) {  // 6 mm: one Clipper once shrank to stray pieces
        CAPTURE(w);
        const TriangleMesh m = holed(w);
        const LayerStack ls = sliceModel(m, 0.05);
        DetailMap map = DetailMap::build(ls, 2.0, 2.0);
        map.addPlateHoles(ls.slices[0], 0.5, limit);
        tc.details = std::make_shared<const DetailMap>(std::move(map));
        const SupportTree t = buildSupportTree(m, {p}, tc, 0.0);
        // Over the narrow hole, the only way down is into it: the tip is dropped (and reported) instead.
        CHECK(t.dropped_points == (w < limit ? 1 : 0));
        bool in_hole = false;
        for (const Pillar& P : t.pillars)
            if (!P.on_model && std::abs(P.xy.x) < w / 2 && std::abs(P.xy.y) < w / 2) in_hole = true;
        if (w < limit) CHECK_FALSE(in_hole);  // it stands somewhere else (here: on the model)
        else CHECK(in_hole);                  // room for a foot with its gap: straight down, in the hole
    }
}
