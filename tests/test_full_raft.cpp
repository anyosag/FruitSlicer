// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "mesh/Placement.h"
#include "mesh/Primitives.h"
#include "printer/PrinterProfile.h"
#include "slice/MeshSlicer.h"
#include "support/PointGenerator.h"
#include "support/SupportTree.h"
#include "support/SupportValidator.h"

using namespace fruit;
using Catch::Approx;

namespace {

// A tree of bare pillars standing on the raft at `xy`, thickness `top`.
SupportTree bareTree(const std::vector<Vec2>& xy, double top = 1.0) {
    SupportTree t;
    t.raft_top = top;
    for (const Vec2& p : xy) {
        Pillar P;
        P.xy = p;
        P.bottom = top + 1.0;
        P.top = 6.0;
        P.diameter = 1.0;
        t.pillars.push_back(P);
    }
    return t;
}

TreeConfig fullConfig() {
    TreeConfig tc;
    tc.raft = RaftStyle::Full;
    return tc;
}

bool covers(const Paths& outline, const Vec2& p) {
    for (const ExPolygon& e : toExPolygons(outline))
        if (e.contains(p)) return true;
    return false;
}

}  // namespace

TEST_CASE("full raft: the mesh is closed and has no pads", "[tree][raft]") {
    const std::vector<Vec2> xy{{0, 0}, {6, 0}, {3, 5}};
    const SupportTree t = bareTree(xy);
    const TreeConfig tc = fullConfig();
    const TriangleMesh m = t.mesh(tc);
    CHECK(m.isClosedManifold());
    CHECK(m.volume() > 0);
    CHECK(m.bbox().min.z == Approx(0.0).margin(1e-9));
    // Nothing reaches below or above what the slab and shafts allow.
    TreeConfig pads;
    const TriangleMesh pm = t.mesh(pads);
    CHECK(pm.isClosedManifold());
    // A slab is more than three round pads.
    CHECK(m.volume() > pm.volume());
}

TEST_CASE("full raft: the outline covers every pad centre and grows past the pads", "[tree][raft]") {
    const std::vector<Vec2> xy{{0, 0}, {6, 0}, {3, 5}, {40, 0}};
    const SupportTree t = bareTree(xy);
    const TreeConfig tc = fullConfig();
    const Paths outline = t.fullRaftOutline(tc);
    for (const Vec2& p : xy) CHECK(covers(outline, p));
    for (const Path& q : outline) CHECK(std::abs(area(q)) > 1.0);  // no slivers
    // Reaches the pad's brim plus the margin on the far side.
    const BBox2 b = bbox(outline);
    CHECK(b.max.x >= 40 + tc.base_diameter / 2 + tc.pad_brim + tc.full_raft_margin - 0.05);
    // The pads near each other are one piece; the one 34 mm away is its own.
    CHECK(toExPolygons(outline).size() == 2);
    // A gap of 0 leaves the discs as they are: still one piece for overlapping ones.
    TreeConfig nogap = tc;
    nogap.full_raft_gap = 0;
    CHECK(area(t.fullRaftOutline(nogap)) <= area(outline) + 1e-9);
}

TEST_CASE("full raft: the top is at raft_top and the edge slopes in", "[tree][raft]") {
    const SupportTree t = bareTree({{0, 0}, {6, 0}, {3, 5}}, 1.0);
    const TreeConfig tc = fullConfig();
    const TriangleMesh m = t.mesh(tc);
    const double bottom = area(t.fullRaftOutline(tc, 0));
    const double top = area(t.fullRaftOutline(tc, t.fullRaftInset(tc)));
    CHECK(top < bottom);
    CHECK(top > 0);
    // Sliced: wide near the plate, narrower under the top, nothing but the
    // cones and shafts above it.
    const double a_low = std::abs(area(sliceMeshAt(m, 0.05)));
    const double a_high = std::abs(area(sliceMeshAt(m, t.raft_top - 0.05)));
    const double a_above = std::abs(area(sliceMeshAt(m, t.raft_top + 0.05)));
    CHECK(a_low == Approx(bottom).epsilon(0.05));
    CHECK(a_high < a_low);
    CHECK(a_high > 0.9 * top);
    CHECK(a_above < 0.5 * a_high);
}

TEST_CASE("full raft: thickness follows raft_top", "[tree][raft]") {
    const SupportTree t = bareTree({{0, 0}, {5, 0}}, 1.6);
    const TreeConfig tc = fullConfig();
    const TriangleMesh m = t.mesh(tc);
    CHECK(area(sliceMeshAt(m, 1.5)) > area(sliceMeshAt(m, 1.7)) * 2);
}

TEST_CASE("full raft: other styles have no outline", "[tree][raft]") {
    const SupportTree t = bareTree({{0, 0}});
    CHECK(t.fullRaftOutline(TreeConfig{}).empty());
}

TEST_CASE("full raft: two far-apart parts print on one piece", "[tree][raft]") {
    TriangleMesh m = makeBox({-45, -10, 0}, {-25, 10, 2});
    m.append(makeBox({25, -10, 0}, {45, 10, 2}));
    placeOnPlate(m, {0, 0, 0}, 5.0);
    SupportConfig cfg;
    cfg.layer_height = 0.1;
    const auto points = generateSupportPoints(m, cfg).points;
    TreeConfig tc = fullConfig();
    const SupportTree tree = buildSupportTree(m, points, tc, PrinterProfile::saturn4Ultra16k().minRaftThickness());
    const TriangleMesh sup = tree.mesh(tc);
    // Two clusters, far enough apart that the slab does not join them.
    CHECK(toExPolygons(tree.fullRaftOutline(tc)).size() == 2);
    CHECK(tree.raft_links.size() >= 1);
    const ValidationReport check = validateSupported(m, sup, 0.1, cfg.link_grow);
    CHECK(check.first_layer_islands == 1);
    CHECK(check.floating.empty());
    for (const auto& P : tree.pillars) CHECK(covers(tree.fullRaftOutline(tc), P.xy));
    // Pads as before: also one piece.
    TreeConfig pads;
    const SupportTree pt = buildSupportTree(m, points, pads, PrinterProfile::saturn4Ultra16k().minRaftThickness());
    CHECK(validateSupported(m, pt.mesh(pads), 0.1, cfg.link_grow).first_layer_islands == 1);
    CHECK(pt.raft_top == Approx(tree.raft_top));
}
