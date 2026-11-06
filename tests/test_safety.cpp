// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
    const PointGenResult pts = generateSupportPoints(m, fast());
    TreeConfig tc;
    const SupportTree tree = buildSupportTree(m, pts.points, tc, PrinterProfile::saturn4Ultra16k().minRaftThickness());
    CHECK_FALSE(tree.braces.empty());
    CHECK(tree.braces_skipped_trap > 0);
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
