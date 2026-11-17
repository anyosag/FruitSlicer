// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "layers/Hollow.h"
#include "layers/HollowAnalysis.h"
#include "mesh/AabbTree.h"
#include "mesh/Placement.h"
#include "mesh/Primitives.h"
#include "printer/PrinterProfile.h"
#include "support/PointGenerator.h"
#include "support/SupportTree.h"
#include "support/SupportValidator.h"

using namespace fruit;
using Catch::Approx;

namespace {

// Open box: floor at z0..z0+t, walls up to z1 (mouth at the top when up=true).
TriangleMesh cup(bool mouth_up) {
    TriangleMesh m;
    if (mouth_up) appendBox(m, {0, 0, 0}, {20, 20, 2});
    else appendBox(m, {0, 0, 8}, {20, 20, 10});
    appendBox(m, {0, 0, 0}, {2, 20, 10});
    appendBox(m, {18, 0, 0}, {20, 20, 10});
    appendBox(m, {0, 0, 0}, {20, 2, 10});
    appendBox(m, {0, 18, 0}, {20, 20, 10});
    return m;
}

// A 3 mm slab with a groove 1 mm deep in its top: a closed ring 2 mm wide,
// the slab solid inside and outside it. The groove's outline is 30 × 20 mm
// and the block inside 26 × 16, so the ring is 600 − 416 = 184 mm².
// `pocket`: the block has a pocket of its own, 16 × 10 mm and 0.5 mm deep.
TriangleMesh ringGroove(bool pocket) {
    TriangleMesh m;
    appendBox(m, {0, 0, 0}, {40, 30, 2});  // under the groove
    appendBox(m, {0, 0, 2}, {40, 5, 3});   // outside it
    appendBox(m, {0, 25, 2}, {40, 30, 3});
    appendBox(m, {0, 5, 2}, {5, 25, 3});
    appendBox(m, {35, 5, 2}, {40, 25, 3});
    if (!pocket) {
        appendBox(m, {7, 7, 2}, {33, 23, 3});  // inside it
    } else {
        appendBox(m, {7, 7, 2}, {33, 23, 2.5});
        appendBox(m, {7, 7, 2.5}, {33, 10, 3});
        appendBox(m, {7, 20, 2.5}, {33, 23, 3});
        appendBox(m, {7, 10, 2.5}, {12, 20, 3});
        appendBox(m, {28, 10, 2.5}, {33, 20, 3});
    }
    return m;
}

double regionVolume(const std::vector<Paths>& regions, double layer_height) {
    double v = 0;
    for (const Paths& p : regions) v += area(p) * layer_height;
    return v;
}

TriangleMesh closedCavity() {
    TriangleMesh outer = makeBox({0, 0, 0}, {20, 20, 12});
    TriangleMesh inner = makeBox({4, 4, 3}, {16, 16, 9});
    for (auto& f : inner.faces) std::swap(f[1], f[2]);
    outer.append(inner);
    return outer;
}

SupportConfig fast() {
    SupportConfig c;
    c.layer_height = 0.1;
    return c;
}

bool hasWarning(const PointGenResult& r, const std::string& code) {
    for (const auto& w : r.warnings)
        if (w.code == code) return true;
    return false;
}

}  // namespace

TEST_CASE("hollows: cup facing the film is a suction cup, facing the plate it drains", "[safety]") {
    {
        TriangleMesh m = cup(true);
        placeOnPlate(m, {0, 0, 0}, 5);
        const HollowReport r = analyzeHollows(sliceModel(m, 0.1));
        REQUIRE(r.components.size() == 1);
        CHECK(r.components[0].kind == HollowKind::SuctionCup);
        CHECK(r.components[0].volume == Approx(16 * 16 * 8).epsilon(0.05));
    }
    {
        TriangleMesh m = cup(false);
        placeOnPlate(m, {0, 0, 0}, 5);
        CHECK(analyzeHollows(sliceModel(m, 0.1)).components.empty());
    }
    {
        TriangleMesh m = closedCavity();
        placeOnPlate(m, {0, 0, 0}, 5);
        const HollowReport r = analyzeHollows(sliceModel(m, 0.1));
        REQUIRE(r.components.size() == 1);
        CHECK(r.components[0].kind == HollowKind::ResinTrap);
    }
}

TEST_CASE("hollows: a groove round a solid block holds the groove's resin, not its outline's", "[safety]") {
    // The dipper's loading cradle: a ring groove in the top of its base,
    // reported as a 600 mm² pocket when the block inside it was not taken out.
    {
        TriangleMesh m = ringGroove(false);
        placeOnPlate(m, {0, 0, 0}, 5);
        const LayerStack ls = sliceModel(m, 0.1);
        const HollowReport r = analyzeHollows(ls);
        REQUIRE(r.components.size() == 1);
        CHECK(r.components[0].kind == HollowKind::SuctionCup);
        CHECK(r.components[0].volume == Approx(184 * 1).epsilon(0.01));
        CHECK(r.components[0].max_area == Approx(184).epsilon(0.01));
        CHECK(r.components[0].last_layer - r.components[0].first_layer + 1 == 10);
        CHECK(regionVolume(r.cup_regions, ls.layer_height) == Approx(184).epsilon(0.01));
        // The Hollow panel's cavity, from the same regions.
        const HollowResult own = closedHollows(ls);
        REQUIRE(own.cavities.size() == 1);
        CHECK(own.cavities[0].opens_up);
        CHECK(own.volume == Approx(184).epsilon(0.01));
    }
    // A pocket in the block is a cup of its own, not part of the groove.
    {
        TriangleMesh m = ringGroove(true);
        placeOnPlate(m, {0, 0, 0}, 5);
        const LayerStack ls = sliceModel(m, 0.1);
        const HollowReport r = analyzeHollows(ls);
        REQUIRE(r.components.size() == 2);
        std::vector<double> volumes;
        for (const HollowComponent& c : r.components) {
            CHECK(c.kind == HollowKind::SuctionCup);
            volumes.push_back(c.volume);
        }
        std::sort(volumes.begin(), volumes.end());
        CHECK(volumes[0] == Approx(16 * 10 * 0.5).epsilon(0.01));
        CHECK(volumes[1] == Approx(184).epsilon(0.01));
        CHECK(regionVolume(r.cup_regions, ls.layer_height) == Approx(80 + 184).epsilon(0.01));
        CHECK(closedHollows(ls).volume == Approx(80 + 184).epsilon(0.01));
    }
    // A plain cup, nothing standing in it: all of its hole, as before.
    {
        TriangleMesh m = cup(true);
        placeOnPlate(m, {0, 0, 0}, 5);
        const LayerStack ls = sliceModel(m, 0.1);
        const HollowReport r = analyzeHollows(ls);
        REQUIRE(r.components.size() == 1);
        CHECK(r.components[0].volume == Approx(16 * 16 * 8).epsilon(0.01));
        CHECK(r.components[0].max_area == Approx(16 * 16).epsilon(0.01));
        CHECK(closedHollows(ls).volume == Approx(16 * 16 * 8).epsilon(0.01));
    }
}

TEST_CASE("closed cavity: warned, and no tips on its ceiling", "[safety]") {
    TriangleMesh m = closedCavity();
    placeOnPlate(m, {0, 0, 0}, 5);
    const PointGenResult r = generateSupportPoints(m, fast());
    CHECK(hasWarning(r, "resin_trap"));
    CHECK(hasWarning(r, "cavity_ceiling"));
    CHECK(r.stats.cavity_samples_skipped > 0);
    CHECK(r.stats.uncovered == 0);
    // Cavity occupies x,y in [-6, 6] (model centred), z in [8, 14] after placement.
    for (const auto& p : r.points)
        CHECK_FALSE((std::abs(p.pos.x) < 6 && std::abs(p.pos.y) < 6 && p.pos.z > 7.9 && p.pos.z < 14.1));
}

TEST_CASE("suction cup is reported by the generator", "[safety]") {
    TriangleMesh m = cup(true);
    placeOnPlate(m, {0, 0, 0}, 5);
    const PointGenResult r = generateSupportPoints(m, fast());
    CHECK(hasWarning(r, "suction_cup"));
    CHECK(r.stats.suction_cups == 1);
}

TEST_CASE("on-plate mode: the base needs no tips, overhangs still do", "[safety]") {
    TriangleMesh t = makeBox({-2, -2, 0}, {2, 2, 6});
    t.append(makeBox({-10, -2, 6}, {10, 2, 7}));
    placeOnPlate(t, {0, 0, 0}, 0.0);
    SupportConfig cfg = fast();
    cfg.on_plate = true;
    const PointGenResult r = generateSupportPoints(t, cfg);
    CHECK(r.stats.uncovered == 0);
    CHECK(r.stats.capacity_deficits == 0);
    size_t base = 0, overhang = 0;
    for (const auto& p : r.points) {
        base += p.pos.z < 0.2;
        overhang += p.pos.z > 5.9;
    }
    CHECK(base == 0);
    CHECK(overhang > 0);
    TreeConfig tc;
    tc.raft = RaftStyle::None;
    const SupportTree tree = buildSupportTree(t, r.points, tc, 0.0);
    const TriangleMesh sup = tree.mesh(tc);
    CHECK(sup.bbox().min.z >= -1e-9);
    CHECK(validateSupported(t, sup, 0.1, cfg.link_grow).floating.empty());
}

TEST_CASE("hinge bias adds tips on the side away from the hinge", "[safety]") {
    SupportConfig cfg = fast();
    cfg.peel_axis = {1, 0};  // hinge on -x, far side +x
    cfg.peel_bias = 1.0;
    auto countAt = [&](double x) {
        TriangleMesh m = makeBox({0, 0, 0}, {40, 40, 1});
        placeOnPlate(m, {0, 0, 0}, 5);
        m.translate({x, 0, 0});
        return generateSupportPoints(m, cfg).points.size();
    };
    CHECK(countAt(60) > countAt(-60));
}

TEST_CASE("no bracing across model material passing between two trunks", "[safety]") {
    // A plate high above the raft, with a bar hanging below it that runs
    // across the support area, joined to the plate by a post.
    TriangleMesh m = makeBox({0, 0, 20}, {30, 30, 22});
    m.append(makeBox({0, 13, 8}, {30, 17, 10}));   // bar
    m.append(makeBox({13, 13, 10}, {17, 17, 20}));  // post
    placeOnPlate(m, {0, 0, 0}, 12.0);  // pillars under the plate reach > 15 mm: braced
    const double raft = PrinterProfile::saturn4Ultra16k().minRaftThickness();
    TreeConfig tc;
    {
        // Two tips under the plate, one on each side of the bar and 8 mm
        // apart: their pillars are each other's nearest, but a brace between
        // them would close a loop around the bar, so there is none.
        const double under = m.bbox().max.z - 2;
        std::vector<SupportPoint> two(2);
        two[0].pos = {-10, -4, under};
        two[1].pos = {-10, 4, under};
        for (SupportPoint& p : two) p.normal = {0, 0, -1};
        const SupportTree t = buildSupportTree(m, two, tc, raft);
        REQUIRE(t.trunks() == 2);
        CHECK(t.braces.empty());
        CHECK(t.braces_skipped_trap == 1);
    }
    const PointGenResult pts = generateSupportPoints(m, fast());
    const SupportTree tree = buildSupportTree(m, pts.points, tc, raft);
    CHECK_FALSE(tree.braces.empty());
    // Property: below every brace, nothing of the model lies between its pillars.
    AabbTree at(m);
    for (const auto& r : tree.braces) {
        const double top = std::max(r.a.z, r.b.z);
        for (double z = tree.raft_top + 0.25; z <= top; z += 0.25) {
            const Vec3 a{r.a.x, r.a.y, z}, b{r.b.x, r.b.y, z};
            CHECK_FALSE(at.anyHit(a, b - a, distance(a, b)));
        }
    }
}
