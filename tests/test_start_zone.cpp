// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The start of a part: the edge of what prints in its first millimetres is
// held closer than the rest (SupportConfig::start_zone_height / _reach).
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

std::vector<SupportPoint> tipsFor(const TriangleMesh& m, double zone, double density = 1.0, bool on_plate = false) {
    SupportConfig cfg;
    cfg.layer_height = 0.05;
    cfg.start_zone_height = zone;
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

}  // namespace

TEST_CASE("the edge a part starts along is held closer than the rest of it", "[points][start]") {
    const TriangleMesh m = tippedPlate();
    const auto with = tipsFor(m, 2.0), without = tipsFor(m, 0.0);
    SupportConfig cfg;
    // Along the starting edge no spot is farther from a tip than the start
    // reach (plus half the spacing of the edge samples it is checked at).
    const double limit = cfg.start_zone_reach + cfg.edge_step / 2 + 0.1;
    CHECK(farthestOnLowestEdge(m, with) <= limit);
    // The rule is what does it: without it the same edge is held from farther away.
    CHECK(farthestOnLowestEdge(m, without) > farthestOnLowestEdge(m, with) + 0.3);
    // More tips in the first 2 mm of height; the same spacing above that.
    const double z0 = lowest(with);
    CHECK(countBetween(with, z0, z0 + 2.0) >= countBetween(without, z0, z0 + 2.0) * 3 / 2);
    const double above_with = static_cast<double>(countBetween(with, z0 + 4.0, 1e9));
    const double above_without = static_cast<double>(countBetween(without, z0 + 4.0, 1e9));
    REQUIRE(above_without > 0);
    CHECK(above_with == Approx(above_without).epsilon(0.15));
}

TEST_CASE("closer tips everywhere bring the start's tips closer too", "[points][start]") {
    const TriangleMesh m = tippedPlate();
    const auto normal = tipsFor(m, 2.0), dense = tipsFor(m, 2.0, 2.0);
    const double z0 = lowest(normal);
    CHECK(countBetween(dense, z0, z0 + 2.0) > countBetween(normal, z0, z0 + 2.0));
    SupportConfig cfg;
    CHECK(farthestOnLowestEdge(m, dense) <= cfg.start_zone_reach / std::sqrt(2.0) + cfg.edge_step / 2 + 0.1);
}

TEST_CASE("a part that starts higher up gets its own closer start", "[points][start]") {
    // A table top on a long leg and a short one: the short leg starts 10 mm
    // up, in mid-air, long after the first one.
    TriangleMesh m = makeBox({0, 0, 20}, {40, 10, 22});
    m.append(makeBox({0, 0, 0}, {6, 10, 20}));
    m.append(makeBox({34, 0, 10}, {40, 10, 20}));
    placeOnPlate(m, {0, 0, 0}, 5.0);
    const auto with = tipsFor(m, 2.0), without = tipsFor(m, 0.0);
    // Tips under the short leg's foot (z 15), around its outline. (Placing
    // the model centres it on the plate.)
    const double leg_x = m.bbox().max.x - 6.5;
    auto underShortLeg = [&](const std::vector<SupportPoint>& pts) {
        std::size_t n = 0;
        for (const auto& p : pts) n += p.pos.x > leg_x && p.pos.z > 14.5 && p.pos.z < 15.5;
        return n;
    };
    REQUIRE(underShortLeg(without) > 0);
    CHECK(underShortLeg(with) > underShortLeg(without));
    // Under the table top, 10 mm above the short leg's start and joined to
    // the long leg, the spacing is the usual one.
    auto underTop = [](const std::vector<SupportPoint>& pts) {
        std::size_t n = 0;
        for (const auto& p : pts) n += p.pos.z > 24.5 && p.pos.z < 25.5;
        return n;
    };
    CHECK(underTop(with) == underTop(without));
}

TEST_CASE("a part standing on the plate has no start to hold closer", "[points][start]") {
    // A block on the plate with a shelf 6 mm up: the plate holds its start.
    TriangleMesh m = makeBox({0, 0, 0}, {10, 10, 12});
    m.append(makeBox({-8, 0, 6}, {0, 10, 8}));
    placeOnPlate(m, {0, 0, 0}, 0.0);
    const auto with = tipsFor(m, 2.0, 1.0, true), without = tipsFor(m, 0.0, 1.0, true);
    REQUIRE_FALSE(with.empty());
    CHECK(with.size() == without.size());
}
