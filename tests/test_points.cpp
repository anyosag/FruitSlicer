// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "mesh/Placement.h"
#include "mesh/Primitives.h"
#include "stability/Stability.h"
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

// The most any tip carries, in multiples of what it is trusted with, as the
// replay shares each layer's pull (each tip on a rod of its own).
// `short_near`, `rod`: the supports of the tips within 5 mm of that point are `rod` mm long (the others reach the plate).
double mostCarried(const PointGenResult& r, const SupportConfig& cfg, const Vec3* short_near = nullptr, double rod = 0) {
    std::vector<TipContact> tips;
    for (const auto& p : r.points) {
        TipContact t;
        t.at = p.pos;
        t.diameter = p.tip_diameter;
        t.part = p.part;
        if (short_near && distance(p.pos, *short_near) < 5.0) t.rod = rod;
        tips.push_back(t);
    }
    const std::vector<TipLoad> loads = standaloneTipLoads(*r.analysis.layers, r.analysis.islands, tips, stabilityConfig(cfg));
    double most = 0;
    for (size_t i = 0; i < loads.size(); ++i) most = std::max(most, loads[i].load / cfg.capacity.capacity(r.points[i].tip_diameter));
    return most;
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

TEST_CASE("an island tip at a lowest corner faces down and not along a side wall", "[points]") {
    // A 1 x 1 x 3 mm pin whose bottom tips 1-2° so its lowest point is one
    // corner, shared with two vertical sides: the tip there must take the
    // bottom's normal (it touches the underside), however the pin is turned.
    TriangleMesh pin = makeBox({0, 0, 0}, {1, 1, 3});
    pin.vertices[0].z = 0.04;  // bottom corners: (lo, lo) highest ...
    pin.vertices[1].z = 0.02;
    pin.vertices[2].z = 0.02;
    pin.vertices[3].z = 0.0;   // ... (hi, hi) lowest
    for (int turn = 0; turn < 360; turn += 15) {
        TriangleMesh m = pin;
        placeOnPlate(m, {0, 0, static_cast<double>(turn)}, 5.0);
        const PointGenResult r = generateSupportPoints(m, fastConfig());
        size_t islands = 0;
        for (const auto& p : r.points) {
            if (p.category != PointCategory::Island) continue;
            ++islands;
            INFO("turned " << turn << "°: normal " << p.normal.x << " " << p.normal.y << " " << p.normal.z);
            CHECK(p.normal.z < -0.9);
        }
        CHECK(islands == 1);
    }
}

TEST_CASE("each tip's own load: asked, no tip carries more than it is trusted with", "[points][relief]") {
    // A wide cone hanging down: its lowest tips hold the whole of the first layers alone. The capacity
    // rule is content (the tips of the piece hold each layer together); each tip's own load is not.
    const TriangleMesh m = placed(makeCone({0, 0, 6}, 6 * std::tan(deg2rad(60)), {0, 0, 0}, 96));
    const SupportConfig cfg = fastConfig();
    const PointGenResult before = generateSupportPoints(m, cfg);
    requireInvariants(before);
    CHECK(before.stats.relief_rounds == 0);  // not asked: the planner as it was
    CHECK(mostCarried(before, cfg) > 2.0);
    PointGenOptions ask;
    ask.relieve = true;
    const PointGenResult after = generateSupportPoints(m, cfg, ask);
    requireInvariants(after);
    // Within the tenth the rule allows itself.
    CHECK(mostCarried(after, cfg) <= 1.1 + 1e-9);
    CHECK(after.stats.relief_left == 0);
    CHECK(after.stats.relief_rounds >= 1);
    CHECK(after.stats.relief_added + after.stats.relief_upgraded > 0);
    CHECK(after.points.size() == before.points.size() + after.stats.relief_added);
    // What it adds are extra-hold tips, on the underside like the others.
    CHECK(countCat(after, PointCategory::Capacity) >= countCat(before, PointCategory::Capacity) + after.stats.relief_added);
    for (const auto& p : after.points) CHECK(p.normal.z < 0);
    // The same again, tip for tip.
    const PointGenResult again = generateSupportPoints(m, cfg, ask);
    REQUIRE(again.points.size() == after.points.size());
    for (size_t i = 0; i < after.points.size(); ++i) {
        CHECK(again.points[i].id == after.points[i].id);
        CHECK(again.points[i].tip_diameter == after.points[i].tip_diameter);
    }
}

TEST_CASE("each tip's own load: one size bigger where that carries it, a tip beside one chosen small", "[points][relief]") {
    const SupportConfig cfg = fastConfig();
    PointGenOptions ask;
    ask.relieve = true;
    auto heavy = [](const PointGenResult& r) {
        size_t n = 0;
        for (const auto& p : r.points) n += p.tip_diameter > 0.5;
        return n;
    };
    auto light = [](const PointGenResult& r) {
        size_t n = 0;
        for (const auto& p : r.points) n += p.tip_diameter < 0.35;
        return n;
    };
    // A dome: its usual tips carry up to twice what they hold. One size up carries that: no tip is added.
    {
        const TriangleMesh m = placed(makeSphere({0, 0, 8}, 8, 48));
        const PointGenResult before = generateSupportPoints(m, cfg), after = generateSupportPoints(m, cfg, ask);
        CHECK(mostCarried(before, cfg) > 1.5);
        CHECK(mostCarried(after, cfg) <= 1.1 + 1e-9);
        CHECK(after.points.size() == before.points.size());
        CHECK(after.stats.relief_added == 0);
        CHECK(after.stats.relief_upgraded > 0);
        CHECK(heavy(after) == heavy(before) + after.stats.relief_upgraded);
        // Each says so, for the tip's card in the app.
        size_t marked = 0;
        for (const auto& p : after.points) marked += p.enlarged;
        CHECK(marked == after.stats.relief_upgraded);
        for (const auto& p : before.points) CHECK_FALSE(p.enlarged);
    }
    // A plate: the tips along its rim are small on purpose (beside a sharp edge). Where they carry too
    // much they stay small, and another tip goes beside them.
    {
        const TriangleMesh m = placed(makeBox({0, 0, 0}, {20, 20, 2}));
        const PointGenResult before = generateSupportPoints(m, cfg), after = generateSupportPoints(m, cfg, ask);
        requireInvariants(after);
        CHECK(mostCarried(before, cfg) > 1.1);
        CHECK(mostCarried(after, cfg) <= 1.1 + 1e-9);
        CHECK(after.stats.relief_added > 0);
        CHECK(after.stats.relief_upgraded == 0);
        CHECK(light(after) >= light(before));
        CHECK(after.points.size() == before.points.size() + after.stats.relief_added);
    }
}

TEST_CASE("each tip's own load: tips whose supports are short and stiff are pulled more than their neighbours", "[points][relief]") {
    // A plate on its tips, 30 mm up. As planned, with every support taken to reach the plate, no tip carries
    // too much.
    const TriangleMesh m = placed(makeBox({0, 0, 0}, {30, 30, 2}), 30.0);
    const SupportConfig cfg = fastConfig();
    PointGenOptions ask;
    ask.relieve = true;
    const PointGenResult plain = generateSupportPoints(m, cfg, ask);
    requireInvariants(plain);
    REQUIRE(mostCarried(plain, cfg) <= 1.1 + 1e-9);
    // Say something stands 2 mm under the plate's middle, so the supports of the tips within 5 mm of it are
    // 2 mm long and the others 30. Then that plan will not do: a short support is many times stiffer, and the
    // thin plate around hangs from those few.
    const Vec3 middle{0, 0, 30};
    auto isShort = [&](const Vec3& at) { return distance(at, middle) < 5.0; };
    CHECK(mostCarried(plain, cfg, &middle, 2.0) > 1.5);

    // The planner asks how long the supports will be: once, about the tips as they are before this rule.
    int asked = 0;
    std::vector<SupportPoint> about;
    ask.support_lengths = [&](const std::vector<SupportPoint>& pts) {
        ++asked;
        about = pts;
        std::vector<double> len(pts.size(), 0.0);
        for (size_t i = 0; i < pts.size(); ++i)
            if (isShort(pts[i].pos)) len[i] = 2.0;
        return len;
    };
    const PointGenResult told = generateSupportPoints(m, cfg, ask);
    requireInvariants(told);
    CHECK(asked == 1);
    const PointGenResult before = generateSupportPoints(m, cfg);  // without the rule
    REQUIRE(about.size() == before.points.size());
    std::vector<uint64_t> a, b;
    for (const auto& p : about) a.push_back(p.id);
    for (const auto& p : before.points) b.push_back(p.id);
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    CHECK(a == b);
    // Told so, it puts more tips there, and that plan holds. (A tip it adds is taken to stand as the one it
    // is put beside only where the model is under it too. Here nothing is, so it takes the added ones as
    // reaching the plate, which asks for more of them than the 2 mm supports they would get.)
    std::size_t there_told = 0, there_plain = 0;
    for (const auto& p : told.points) there_told += isShort(p.pos);
    for (const auto& p : plain.points) there_plain += isShort(p.pos);
    CHECK(there_told > there_plain);
    CHECK(mostCarried(told, cfg, &middle, 2.0) <= 1.1 + 1e-9);

    // Lengths of 0 (every support reaches the raft) change nothing, and an answer of the wrong length is not used.
    auto same = [&](const PointGenResult& x) {
        REQUIRE(x.points.size() == plain.points.size());
        for (size_t i = 0; i < x.points.size(); ++i) {
            CHECK(x.points[i].id == plain.points[i].id);
            CHECK(x.points[i].tip_diameter == plain.points[i].tip_diameter);
        }
    };
    ask.support_lengths = [](const std::vector<SupportPoint>& pts) { return std::vector<double>(pts.size(), 0.0); };
    same(generateSupportPoints(m, cfg, ask));
    ask.support_lengths = [](const std::vector<SupportPoint>&) { return std::vector<double>(3, 2.0); };
    same(generateSupportPoints(m, cfg, ask));
}

TEST_CASE("each tip's own load: the limit is the planner's setting, and 0 turns the rule off", "[points][relief]") {
    const TriangleMesh m = placed(makeCone({0, 0, 6}, 6 * std::tan(deg2rad(60)), {0, 0, 0}, 96));
    PointGenOptions ask;
    ask.relieve = true;
    SupportConfig off = fastConfig();
    off.tip_load_limit = 0;
    const PointGenResult bare = generateSupportPoints(m, fastConfig()), none = generateSupportPoints(m, off, ask);
    REQUIRE(none.points.size() == bare.points.size());
    for (size_t i = 0; i < bare.points.size(); ++i) {
        CHECK(none.points[i].id == bare.points[i].id);
        CHECK(none.points[i].tip_diameter == bare.points[i].tip_diameter);
    }
    CHECK(none.stats.relief_rounds == 0);
    // Twice what a tip is trusted with asks for less than once does.
    SupportConfig twice = fastConfig();
    twice.tip_load_limit = 2;
    const PointGenResult lax = generateSupportPoints(m, twice, ask), strict = generateSupportPoints(m, fastConfig(), ask);
    CHECK(mostCarried(lax, twice) <= 2 * 1.1 + 1e-9);
    CHECK(lax.stats.relief_added + lax.stats.relief_upgraded < strict.stats.relief_added + strict.stats.relief_upgraded);
    // No rounds left to work the loads out in: the tips as they were, and how many are over is said.
    SupportConfig spent = fastConfig();
    spent.relief_rounds = 0;
    CHECK(generateSupportPoints(m, spent, ask).points.size() == bare.points.size());
    // Too few rounds for this cone: it stops with tips still over, and says how many. The planner's own
    // number of rounds is enough for it, and it uses only as many as it needs.
    SupportConfig few = fastConfig();
    few.relief_rounds = 2;
    const PointGenResult cut_short = generateSupportPoints(m, few, ask);
    CHECK(cut_short.stats.relief_rounds == 2);
    CHECK(cut_short.stats.relief_left > 0);
    CHECK(strict.stats.relief_left == 0);
    CHECK(strict.stats.relief_rounds > 2);
    CHECK(strict.stats.relief_rounds < static_cast<size_t>(fastConfig().relief_rounds));
}
