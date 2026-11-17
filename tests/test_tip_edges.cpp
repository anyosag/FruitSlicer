// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Tips keep their contact on the face: inside the edges of slots and holes
// and away from walls going down by their radius plus tip_edge_margin, and
// the edges of a slot are not free edges (SupportConfig::detail_width).
#include <algorithm>
#include <cmath>

#include <catch2/catch_test_macros.hpp>

#include "mesh/Placement.h"
#include "mesh/Primitives.h"
#include "ops/MeshOps.h"
#include "support/PointGenerator.h"

using namespace fruit;
using fruit::ops::MeshResult;
using fruit::ops::subtractMeshes;

namespace {

struct Rect {
    double x0, y0, x1, y1;
};

double distanceTo(const Rect& r, const Vec2& p) {
    const double dx = std::max({r.x0 - p.x, 0.0, p.x - r.x1});
    const double dy = std::max({r.y0 - p.y, 0.0, p.y - r.y1});
    return std::hypot(dx, dy);
}

// A 1.5 mm floor, 5 mm up, with slots right through it like a coverslip
// tray's: `slot` wide, `rib` of floor between them, 8 mm long.
struct Floor {
    TriangleMesh mesh;
    std::vector<Rect> slots;
};

Floor slottedFloor(double slot, double rib, int n = 6) {
    const double width = n * slot + (n + 1) * rib + 4;
    TriangleMesh plate = makeBox({0, 0, 5}, {width, 12, 6.5});
    Floor f;
    for (int i = 0; i < n; ++i) {
        const double x0 = 2 + rib + i * (slot + rib);
        f.slots.push_back({x0, 2, x0 + slot, 10});
        const MeshResult cut = subtractMeshes(plate, makeBox({x0, 2, 4}, {x0 + slot, 10, 7.5}));
        REQUIRE(cut.ok);
        plate = cut.mesh;
    }
    f.mesh = plate;
    return f;
}

SupportConfig config() {
    SupportConfig cfg;
    cfg.layer_height = 0.05;
    return cfg;
}

// Tips on the floor's underside (not the ones holding the slots' far ends).
std::vector<SupportPoint> floorTips(const PointGenResult& r) {
    std::vector<SupportPoint> out;
    for (const auto& p : r.points)
        if (p.pos.z < 5.01) out.push_back(p);
    return out;
}

double nearestSlot(const Floor& f, const SupportPoint& p) {
    double d = 1e9;
    for (const Rect& s : f.slots) d = std::min(d, distanceTo(s, p.pos.xy()));
    return d;
}

}  // namespace

TEST_CASE("tips keep their contact out of a slot's mouth", "[points][edges]") {
    const Floor f = slottedFloor(0.6, 1.4);
    const SupportConfig cfg = config();
    const PointGenResult r = generateSupportPoints(f.mesh, cfg);
    CHECK(r.stats.uncovered == 0);
    const auto tips = floorTips(r);
    REQUIRE(tips.size() > 10);
    // Every contact stays on the floor: none reaches nearer a slot than the margin.
    for (const auto& p : tips) {
        CAPTURE(p.pos.x, p.pos.y, p.tip_diameter);
        CHECK(nearestSlot(f, p) >= p.tip_diameter / 2 + cfg.tip_edge_margin - 0.005);
    }
    // As it was: edge samples 0.02 mm inside, so the contacts reached into the slots.
    SupportConfig old = cfg;
    old.tip_edge_margin = -1;
    old.detail_width = 0;
    const auto before = floorTips(generateSupportPoints(f.mesh, old));
    std::size_t poking = 0;
    for (const auto& p : before) poking += nearestSlot(f, p) < p.tip_diameter / 2;
    CHECK(poking > 0);
}

TEST_CASE("a slot's edges get no row of tips of their own", "[points][edges]") {
    // Each side of each slot was a free edge, held by tips at most skin_reach apart along it: two rows
    // of tips per slot, their heads facing each other under it. Now the face beside it is held by the
    // coverage rule, as the rest of the floor is.
    const Floor f = slottedFloor(0.6, 1.4);
    SupportConfig cfg = config();
    const auto now = floorTips(generateSupportPoints(f.mesh, cfg));
    cfg.detail_width = 0;  // slot edges free again (the margin stays)
    const auto rows = floorTips(generateSupportPoints(f.mesh, cfg));
    auto beside = [&](const std::vector<SupportPoint>& tips) {
        std::size_t n = 0;
        for (const auto& p : tips) n += nearestSlot(f, p) < 0.6;
        return n;
    };
    CAPTURE(beside(now), beside(rows));
    CHECK(beside(now) * 2 <= beside(rows));
}

TEST_CASE("on a rib narrower than the planned tip, a smaller tip that fits", "[points][edges]") {
    // 0.5 mm ribs between 0.6 mm slots: a 0.6 mm tip needs 0.68 mm of face, a 0.4 mm one 0.48 mm.
    const Floor f = slottedFloor(0.6, 0.5);
    const SupportConfig cfg = config();
    const PointGenResult r = generateSupportPoints(f.mesh, cfg);
    CHECK(r.stats.uncovered == 0);
    std::size_t on_ribs = 0;
    for (const auto& p : floorTips(r)) {
        const double d = nearestSlot(f, p);
        if (p.pos.y < 2.5 || p.pos.y > 9.5 || p.pos.x < 2.2 || p.pos.x > f.slots.back().x1) continue;  // the ribs only
        ++on_ribs;
        CAPTURE(p.pos.x, p.pos.y, p.tip_diameter, d);
        CHECK(p.tip_diameter <= 0.4 + 1e-9);
        CHECK(d >= p.tip_diameter / 2 + cfg.tip_edge_margin - 0.005);
    }
    CHECK(on_ribs > 0);
}

TEST_CASE("a tip under a ceiling keeps its contact off the wall going down beside it", "[points][edges]") {
    // A 10 × 10 × 6 mm block, hollow from below (8 × 8 mm, 5 mm deep), 5 mm up: a post's inside.
    const MeshResult cup = subtractMeshes(makeBox({0, 0, 5}, {10, 10, 11}), makeBox({1, 1, 4}, {9, 9, 10}));
    REQUIRE(cup.ok);
    const SupportConfig cfg = config();
    const PointGenResult r = generateSupportPoints(cup.mesh, cfg);
    const Rect inside{1, 1, 9, 9};
    std::size_t ceiling = 0;
    for (const auto& p : r.points) {
        if (std::abs(p.pos.z - 10) > 0.01) continue;
        ++ceiling;
        const double to_wall = std::min({p.pos.x - inside.x0, inside.x1 - p.pos.x, p.pos.y - inside.y0, inside.y1 - p.pos.y});
        CAPTURE(p.pos.x, p.pos.y, p.tip_diameter, to_wall);
        CHECK(to_wall >= p.tip_diameter / 2 + cfg.tip_edge_margin - 0.005);
    }
    CHECK(ceiling > 0);
}
