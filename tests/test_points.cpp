#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "mesh/Placement.h"
#include "mesh/Primitives.h"
#include "support/PointGenerator.h"
#include "support/SupportIO.h"

using namespace fruit;
using Catch::Approx;

namespace {

TriangleMesh placed(TriangleMesh m, double elevation = 5.0) {
    placeOnPlate(m, {0, 0, 0}, elevation);
    return m;
}

SupportConfig fastConfig() {
    SupportConfig c;
    c.layer_height = 0.1;  // keeps tests quick; geometry is mm-scale
    return c;
}

void requireInvariants(const PointGenResult& r) {
    CHECK(r.stats.uncovered == 0);
    CHECK(r.stats.capacity_deficits == 0);
    CHECK_FALSE(r.points.empty());
}

size_t countCat(const PointGenResult& r, PointCategory c) {
    size_t n = 0;
    for (const auto& p : r.points) n += p.category == c;
    return n;
}

}  // namespace

TEST_CASE("flat-bottomed plate: covered, enough capacity, tips on the underside", "[points]") {
    const TriangleMesh m = placed(makeBox({0, 0, 0}, {20, 20, 2}));
    const SupportConfig cfg = fastConfig();
    const PointGenResult r = generateSupportPoints(m, cfg);
    requireInvariants(r);
    double cap = 0;
    for (const auto& p : r.points) {
        CHECK(p.pos.z == Approx(5.0).margin(0.06));
        cap += cfg.capacity.capacity(p.tip_diameter);
    }
    CHECK(cap >= 400.0 - 1e-6);       // 20 x 20 mm cross-section
    CHECK(r.points.size() >= 3);
    CHECK(r.points.size() <= 60);     // not a dense carpet
}

TEST_CASE("downward cone: one tip at the apex, flank covered", "[points]") {
    // 60° from vertical flank (wide cone) needs support along the surface.
    const TriangleMesh m = placed(makeCone({0, 0, 6}, 6 * std::tan(deg2rad(60)), {0, 0, 0}, 96));
    const PointGenResult r = generateSupportPoints(m, fastConfig());
    requireInvariants(r);
    REQUIRE(countCat(r, PointCategory::Island) >= 1);
    // The lowest point is the apex.
    CHECK(r.points.front().pos.z == Approx(5.0).margin(0.05));
    CHECK(std::hypot(r.points.front().pos.x, r.points.front().pos.y) < 0.2);
    CHECK(r.points.size() > 3);
}

TEST_CASE("steep cone (30° from vertical) needs no flank support", "[points]") {
    const TriangleMesh m = placed(makeCone({0, 0, 10}, 10 * std::tan(deg2rad(30)), {0, 0, 0}, 96));
    const PointGenResult r = generateSupportPoints(m, fastConfig());
    requireInvariants(r);
    CHECK(countCat(r, PointCategory::Overhang) == 0);
}

TEST_CASE("hemisphere dome: covered with a modest number of tips", "[points]") {
    TriangleMesh sph = makeSphere({0, 0, 0}, 8, 128);
    const PointGenResult r = generateSupportPoints(placed(sph), fastConfig());
    requireInvariants(r);
    CHECK(r.points.size() < 150);
}

TEST_CASE("coverage does not cross gaps: slab over two fingers gets its own tips", "[points]") {
    // Two 2x2 fingers 8 mm apart joined by a slab on top. The slab underside
    // over the slot cannot be held by tips at the fingers' feet.
    TriangleMesh m = makeBox({0, 0, 0}, {2, 2, 6});
    m.append(makeBox({10, 0, 0}, {12, 2, 6}));
    m.append(makeBox({0, 0, 6}, {12, 2, 7}));
    const PointGenResult r = generateSupportPoints(placed(m), fastConfig());
    requireInvariants(r);
    // Placement centred the model: the slot is |x| < 4, slab underside z = 11.
    size_t under_slot = 0;
    for (const auto& p : r.points)
        if (std::abs(p.pos.x) < 4.0 && p.pos.z == Approx(11.0).margin(0.15)) ++under_slot;
    CHECK(under_slot >= 1);
}

TEST_CASE("mushroom: stem island plus cap overhang", "[points]") {
    TriangleMesh m = makeCylinder({0, 0, 0}, 0.6, 4, 48);
    m.append(makeCylinder({0, 0, 4}, 6, 1.5, 128));
    const PointGenResult r = generateSupportPoints(placed(m), fastConfig());
    requireInvariants(r);
    size_t on_cap = 0;
    for (const auto& p : r.points) on_cap += p.pos.z > 8.9;
    CHECK(on_cap >= 3);
}

TEST_CASE("large flat plate is capacity-bound: tips scale with area", "[points]") {
    const SupportConfig cfg = fastConfig();
    const PointGenResult small = generateSupportPoints(placed(makeBox({0, 0, 0}, {10, 10, 1})), cfg);
    const PointGenResult large = generateSupportPoints(placed(makeBox({0, 0, 0}, {40, 40, 1})), cfg);
    requireInvariants(small);
    requireInvariants(large);
    double cap = 0;
    for (const auto& p : large.points) cap += cfg.capacity.capacity(p.tip_diameter);
    CHECK(cap >= 1600 - 1e-6);
    CHECK(large.points.size() > 4 * small.points.size() / 2);
    bool flat_warning = false;
    for (const auto& w : large.warnings) flat_warning |= w.code == "flat_face";
    CHECK(flat_warning);
}

TEST_CASE("thin tall pin gets stabilisation tips", "[points]") {
    const PointGenResult r = generateSupportPoints(placed(makeCylinder({0, 0, 0}, 0.5, 20, 32)), fastConfig());
    requireInvariants(r);
    CHECK(countCat(r, PointCategory::Island) == 1);
    CHECK(countCat(r, PointCategory::Stabilization) >= 1);
}

TEST_CASE("generation is deterministic and pruning keeps the invariants", "[points]") {
    TriangleMesh m = makeSphere({0, 0, 0}, 5, 64);
    m.append(makeBox({-2, -2, 3}, {9, 2, 5}));
    const TriangleMesh pm = placed(m);
    const SupportConfig cfg = fastConfig();
    const PointGenResult a = generateSupportPoints(pm, cfg);
    const PointGenResult b = generateSupportPoints(pm, cfg);
    REQUIRE(a.points.size() == b.points.size());
    for (size_t i = 0; i < a.points.size(); ++i) {
        CHECK(a.points[i].id == b.points[i].id);
        CHECK(a.points[i].pos == b.points[i].pos);
    }
    PointGenOptions np;
    np.prune = false;
    const PointGenResult raw = generateSupportPoints(pm, cfg, np);
    requireInvariants(raw);
    requireInvariants(a);
    CHECK(a.points.size() + a.stats.pruned == a.stats.greedy_points);
    CHECK(a.points.size() <= raw.points.size());
}

TEST_CASE("manual edits survive regeneration", "[points][edits]") {
    const TriangleMesh m = placed(makeBox({0, 0, 0}, {20, 20, 2}));
    const SupportConfig cfg = fastConfig();
    const PointGenResult first = generateSupportPoints(m, cfg);
    REQUIRE(first.points.size() > 3);
    // Delete one auto point, add a manual one.
    const SupportPoint victim = first.points[first.points.size() / 2];
    nlohmann::json edits = {{"delete", {victim.id}}, {"add", {{{"pos", {9.0, 9.0, 5.0}}, {"tip_diameter", 0.3}}}}};
    CosmeticSpec cos;
    PointGenOptions opts;
    opts.manual = applyEdits(edits, first.points, cos);
    opts.cosmetic = &cos;
    const PointGenResult second = generateSupportPoints(m, cfg, opts);
    requireInvariants(second);
    bool manual_found = false;
    for (const auto& p : second.points) {
        if (p.category == PointCategory::Manual) {
            manual_found = true;
            CHECK(p.pinned);
            CHECK(p.tip_diameter == Approx(0.3));
        } else {
            CHECK(distance(p.pos, victim.pos) > victim.tip_diameter / 2 + 0.3 - 1e-9);
        }
    }
    CHECK(manual_found);
    // Round trip through JSON keeps ids and categories.
    const auto back = pointsFromJson(pointsToJson(second.points));
    REQUIRE(back.size() == second.points.size());
    CHECK(back.front().id == second.points.front().id);
}

TEST_CASE("cosmetic keep-out pushes tips elsewhere when coverage allows", "[points][cosmetic]") {
    const TriangleMesh m = placed(makeBox({0, 0, 0}, {30, 10, 2}));
    const SupportConfig cfg = fastConfig();
    CosmeticSpec cos;
    BBox3 keep;
    keep.extend({0, -10, 0});
    keep.extend({20, 10, 20});  // right half (x > 0) of the underside
    cos.keep_out_boxes.push_back(keep);
    PointGenOptions opts;
    opts.cosmetic = &cos;
    const PointGenResult r = generateSupportPoints(m, cfg, opts);
    requireInvariants(r);
    const PointGenResult plain = generateSupportPoints(m, cfg);
    auto inside = [&](const PointGenResult& res) {
        size_t n = 0;
        for (const auto& p : res.points) n += p.pos.x > 0;
        return n;
    };
    CHECK(inside(r) <= inside(plain));
}
