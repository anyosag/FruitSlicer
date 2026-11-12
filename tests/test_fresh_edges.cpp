// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Fresh edges: where a layer reaches well past the one before it, its edge is
// held closer than the rest (SupportConfig::skin_advance / skin_reach).
#include <algorithm>
#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "mesh/Placement.h"
#include "mesh/Primitives.h"
#include "support/PointGenerator.h"

using namespace fruit;
using Catch::Approx;

namespace {

// A 40 × 40 × 3 mm plate tipped 15° about X, 5 mm above the plate: it starts
// along one whole edge, which then widens by 0.19 mm a layer.
TriangleMesh tippedPlate() {
    TriangleMesh m = makeBox({-20, -20, 0}, {20, 20, 3});
    placeOnPlate(m, {15, 0, 0}, 5.0);
    return m;
}

// `reach` 0 turns the rule off.
std::vector<SupportPoint> tipsFor(const TriangleMesh& m, double reach, double density = 1.0, bool on_plate = false) {
    SupportConfig cfg;
    cfg.layer_height = 0.05;
    cfg.skin_reach = reach;
    cfg.coverage.density = density;
    cfg.on_plate = on_plate;
    const PointGenResult r = generateSupportPoints(m, cfg);
    CHECK(r.stats.uncovered == 0);
    return r.points;
}

double lowest(const std::vector<SupportPoint>& pts) {
    double z = 1e9;
    for (const auto& p : pts) z = std::min(z, p.pos.z);
    return z;
}

std::size_t countBetween(const std::vector<SupportPoint>& pts, double z0, double z1) {
    std::size_t n = 0;
    for (const auto& p : pts) n += p.pos.z >= z0 && p.pos.z < z1;
    return n;
}

// The farthest any point of the plate's lowest edge (y fixed, x across) is
// from a tip, in plan.
double farthestOnLowestEdge(const TriangleMesh& m, const std::vector<SupportPoint>& pts) {
    const BBox3 b = m.bbox();
    double y_low = 0, z_low = 1e9;
    for (const Vec3& v : m.vertices)
        if (v.z < z_low) {
            z_low = v.z;
            y_low = v.y;
        }
    double worst = 0;
    for (double x = b.min.x; x <= b.max.x + 1e-9; x += 0.25) {
        double d = 1e9;
        for (const auto& p : pts) d = std::min(d, distance(p.pos.xy(), Vec2{x, y_low}));
        worst = std::max(worst, d);
    }
    return worst;
}

const double kReach = SupportConfig{}.skin_reach;

}  // namespace

TEST_CASE("the edge a tilted plate starts along is held closer than the rest of it", "[points][fresh]") {
    const TriangleMesh m = tippedPlate();
    const auto with = tipsFor(m, kReach), without = tipsFor(m, 0.0);
    SupportConfig cfg;
    // Its first layer is a sliver along the whole edge, with nothing behind
    // it: no spot on it is farther from a tip than the reach (plus half the
    // spacing of the edge samples it is checked at).
    CHECK(farthestOnLowestEdge(m, with) <= cfg.skin_reach + cfg.edge_step / 2 + 0.1);
    // The rule is what does it: without it the same edge is held from farther away.
    CHECK(farthestOnLowestEdge(m, without) > farthestOnLowestEdge(m, with) + 0.3);
    // The plate then widens by 0.19 mm a layer, which is no fresh edge: the
    // spacing higher up is the usual one.
    const double z0 = lowest(with);
    CHECK(countBetween(with, z0, z0 + 0.5) > countBetween(without, z0, z0 + 0.5));
    const double above_with = static_cast<double>(countBetween(with, z0 + 2.0, 1e9));
    const double above_without = static_cast<double>(countBetween(without, z0 + 2.0, 1e9));
    REQUIRE(above_without > 0);
    CHECK(above_with == Approx(above_without).epsilon(0.15));
}

TEST_CASE("closer tips everywhere bring a fresh edge's tips closer too", "[points][fresh]") {
    const TriangleMesh m = tippedPlate();
    const auto normal = tipsFor(m, kReach), dense = tipsFor(m, kReach, 2.0);
    CHECK(dense.size() > normal.size());
    CHECK(farthestOnLowestEdge(m, dense) <= kReach / std::sqrt(2.0) + SupportConfig{}.edge_step / 2 + 0.1);
}

TEST_CASE("a flat face that starts in mid-air is held closer along its outline", "[points][fresh]") {
    // A table top on a long leg and a short one: the short leg's flat foot
    // starts 10 mm up, in mid-air.
    TriangleMesh m = makeBox({0, 0, 20}, {40, 10, 22});
    m.append(makeBox({0, 0, 0}, {6, 10, 20}));
    m.append(makeBox({34, 0, 10}, {40, 10, 20}));
    placeOnPlate(m, {0, 0, 0}, 5.0);
    const auto with = tipsFor(m, kReach), without = tipsFor(m, 0.0);
    // (Placing the model centres it on the plate.)
    const BBox3 box = m.bbox();
    auto under = [&](const std::vector<SupportPoint>& pts, double x0, double x1, double z) {
        std::vector<SupportPoint> out;
        for (const auto& p : pts)
            if (p.pos.x > x0 && p.pos.x < x1 && std::abs(p.pos.z - z) < 0.5) out.push_back(p);
        return out;
    };
    // Under the short leg's foot (6 × 10 mm, z 15).
    const auto foot_with = under(with, box.max.x - 6.5, box.max.x + 1, 15);
    const auto foot_without = under(without, box.max.x - 6.5, box.max.x + 1, 15);
    REQUIRE_FALSE(foot_without.empty());
    CHECK(foot_with.size() > foot_without.size());
    // Every corner of the foot has a tip within reach.
    for (double x : {box.max.x - 6.0, box.max.x})
        for (double y : {box.min.y, box.max.y}) {
            double d = 1e9;
            for (const auto& p : foot_with) d = std::min(d, distance(p.pos.xy(), Vec2{x, y}));
            CHECK(d <= kReach + 0.6);
        }
    // The table top's underside appears in one layer too (z 25): its free
    // edges are fresh, so it never has fewer tips with the rule.
    CHECK(under(with, box.min.x + 6.5, box.max.x - 6.5, 25).size() >=
          under(without, box.min.x + 6.5, box.max.x - 6.5, 25).size());
}

TEST_CASE("a ceiling held by walls all round has no free edge", "[points][fresh]") {
    // A socket: four walls and a ceiling, open towards the plate. The ceiling
    // appears in one layer, but every side of it joins a wall, so nothing
    // about it is held closer than before.
    TriangleMesh m = makeBox({0, 0, 8}, {12, 12, 10});
    m.append(makeBox({0, 0, 0}, {12, 2, 8}));
    m.append(makeBox({0, 10, 0}, {12, 12, 8}));
    m.append(makeBox({0, 2, 0}, {2, 10, 8}));
    m.append(makeBox({10, 2, 0}, {12, 10, 8}));
    placeOnPlate(m, {0, 0, 0}, 5.0);
    const auto with = tipsFor(m, kReach), without = tipsFor(m, 0.0);
    auto underCeiling = [](const std::vector<SupportPoint>& pts) {
        std::size_t n = 0;
        for (const auto& p : pts) n += std::abs(p.pos.z - 13.0) < 0.5;
        return n;
    };
    REQUIRE(underCeiling(without) > 0);
    CHECK(underCeiling(with) == underCeiling(without));
}

TEST_CASE("a dome that widens slowly keeps its tips spread up its sides", "[points][fresh]") {
    // A 30 mm ball: after its first few layers each one reaches only a little
    // past the one before, so nothing about it is a fresh edge. The tips that
    // hold its growing layers go on up its sides as before, instead of all
    // crowding under its lowest point.
    TriangleMesh m = makeSphere({0, 0, 15}, 15, 96);
    placeOnPlate(m, {0, 0, 0}, 5.0);
    const auto with = tipsFor(m, kReach), without = tipsFor(m, 0.0);
    const double z0 = lowest(without);
    const double high_with = static_cast<double>(countBetween(with, z0 + 3.0, 1e9));
    const double high_without = static_cast<double>(countBetween(without, z0 + 3.0, 1e9));
    REQUIRE(high_without >= 10);
    CHECK(high_with >= 0.8 * high_without);
    CHECK(static_cast<double>(with.size()) <= 1.25 * static_cast<double>(without.size()));
}

TEST_CASE("on the plate, nothing within the first layers' height gets a tip", "[points][plate]") {
    // A block on the plate with two shelves: one whose underside is 0.2 mm
    // above the plate, inside the long first exposures, and one 1.5 mm up.
    TriangleMesh m = makeBox({0, 0, 0}, {30, 8, 10});
    m.append(makeBox({2, -5, 0.2}, {12, 0.5, 3}));
    m.append(makeBox({18, -5, 1.5}, {28, 0.5, 4}));
    placeOnPlate(m, {0, 0, 0}, 0.0);
    SupportConfig cfg;
    cfg.layer_height = 0.05;
    cfg.on_plate = true;
    auto lowAndHigh = [&](const SupportConfig& c) {
        std::pair<std::size_t, std::size_t> n{0, 0};
        for (const auto& p : generateSupportPoints(m, c).points) (p.pos.z < 0.8 ? n.first : n.second) += 1;
        return n;
    };
    const auto with = lowAndHigh(cfg);
    CHECK(with.first == 0);   // the plate holds what prints that low
    CHECK(with.second > 0);   // the higher shelf is held as ever
    // Without the rule the low shelf gets tips too, and the high one the same.
    SupportConfig off = cfg;
    off.plate_hold_height = 0;
    const auto without = lowAndHigh(off);
    CHECK(without.first > 0);
    CHECK(without.second == with.second);
    // On a raft the same shelf is 5 mm up and is held like any other.
    TriangleMesh lifted = makeBox({0, 0, 0}, {30, 8, 10});
    lifted.append(makeBox({2, -5, 0.2}, {12, 0.5, 3}));
    placeOnPlate(lifted, {0, 0, 0}, 5.0);
    SupportConfig raft;
    raft.layer_height = 0.05;
    std::size_t under_shelf = 0;
    for (const auto& p : generateSupportPoints(lifted, raft).points) under_shelf += std::abs(p.pos.z - 5.2) < 0.2;
    CHECK(under_shelf > 0);
}

TEST_CASE("a shelf's rim is a fresh edge wherever it is, and the plate holds a first layer", "[points][fresh]") {
    // A block on the plate with a shelf 6 mm up. Nothing is added under the
    // block itself (the plate holds it); the shelf's free rim is held closer.
    TriangleMesh m = makeBox({0, 0, 0}, {10, 10, 12});
    m.append(makeBox({-8, 0, 6}, {0, 10, 8}));
    placeOnPlate(m, {0, 0, 0}, 0.0);
    const auto with = tipsFor(m, kReach, 1.0, true), without = tipsFor(m, 0.0, 1.0, true);
    REQUIRE_FALSE(without.empty());
    for (const auto& p : with) CHECK(p.pos.z > 5.5);  // all under the shelf
    CHECK(with.size() >= without.size());
    // The shelf's outer corners have a tip within reach.
    const BBox3 box = m.bbox();
    for (double y : {box.min.y, box.max.y}) {
        double d = 1e9;
        for (const auto& p : with) d = std::min(d, distance(p.pos.xy(), Vec2{box.min.x, y}));
        CHECK(d <= kReach + 0.6);
    }
}
