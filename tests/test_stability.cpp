// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "mesh/Primitives.h"
#include "stability/Stability.h"
#include "support/PointGenerator.h"
#include "support/SupportSettings.h"
#include "support/SupportTree.h"

using namespace fruit;
using Catch::Approx;

namespace {

constexpr double kH = 0.05;

Path rect(double x0, double y0, double x1, double y1) {
    return {toPoint({x0, y0}), toPoint({x1, y0}), toPoint({x1, y1}), toPoint({x0, y1})};
}

Path ring(const Vec2& c, double r, int n = 96, bool hole = false) {
    Path p;
    for (int i = 0; i < n; ++i) {
        const double a = 2 * kPi * (hole ? n - i : i) / n;
        p.push_back(toPoint({c.x + r * std::cos(a), c.y + r * std::sin(a)}));
    }
    return p;
}

// Layers drawn shape by shape. `model` gets only the shapes that are the
// model's; `print` gets everything.
struct Build {
    LayerStack print, model, supports;
    explicit Build(int layers) {
        for (LayerStack* s : {&print, &model, &supports}) {
            s->layer_height = kH;
            s->slices.resize(static_cast<size_t>(layers));
            s->sample_z.resize(static_cast<size_t>(layers));
            for (int i = 0; i < layers; ++i) s->sample_z[static_cast<size_t>(i)] = (i + 0.5) * kH;
        }
    }
    void add(int from, int to, const Path& p, bool is_model) {
        for (int L = from; L < to; ++L) {
            print.slices[static_cast<size_t>(L)].push_back(p);
            (is_model ? model : supports).slices[static_cast<size_t>(L)].push_back(p);
        }
    }
    void part(int from, int to, const Path& p) { add(from, to, p, true); }
    void support(int from, int to, const Path& p) { add(from, to, p, false); }
    StabilityResult run(const StabilityConfig& cfg, std::vector<TipContact> tips = {}) {
        for (LayerStack* s : {&print, &model})
            for (Paths& sl : s->slices) sl = unionNonZero(sl);
        StabilityInput in;
        in.print = &print;
        in.model = &model;
        in.tips = std::move(tips);
        return analyzeStability(in, cfg);
    }
    // The same with the supports given whole, as the planner gives them:
    // the part of a tip that is sunk into the model included.
    StabilityResult runWhole(const StabilityConfig& cfg, std::vector<TipContact> tips = {}) {
        for (LayerStack* s : {&print, &model, &supports})
            for (Paths& sl : s->slices) sl = unionNonZero(sl);
        StabilityInput in;
        in.print = &print;
        in.model = &model;
        in.supports = &supports;
        in.tips = std::move(tips);
        return analyzeStability(in, cfg);
    }
};

// What a tip's contact stands per mm² with the built-in table (11.1 mm² of
// layer for a 0.4 mm tip, trusted to 0.7 of where half let go).
const double kPerMm2 = 11.1 / 0.7 / (kPi * 0.4 * 0.4 / 4);

const PartRisk& at(const StabilityResult& r, int layer, const Vec2& xy) {
    const int id = r.partAt(static_cast<size_t>(layer), xy);
    REQUIRE(id >= 0);
    return r.parts[static_cast<size_t>(id)];
}

// The support's part there, where a support and the model lie in each other.
const PartRisk& supportAt(const StabilityResult& r, int layer, const Vec2& xy) {
    for (int id : r.in_layer[static_cast<size_t>(layer)]) {
        const PrintPart& p = r.where[static_cast<size_t>(id)];
        if (!r.parts[static_cast<size_t>(id)].model && p.shape->contains(xy)) return r.parts[static_cast<size_t>(id)];
    }
    FAIL("no support at that point");
    return r.parts.front();
}

}  // namespace

TEST_CASE("area moments of a rectangle, a ring and a clockwise outline", "[stability]") {
    const AreaMoments m = areaMoments({rect(10, 20, 14, 22)});
    CHECK(m.area == Approx(8));
    CHECK(m.centroid.x == Approx(12));
    CHECK(m.centroid.y == Approx(21));
    CHECK(m.sxx == Approx(2 * 4.0 * 4 * 4 / 12));  // b·h³/12 across x
    CHECK(m.syy == Approx(4 * 2.0 * 2 * 2 / 12));
    CHECK(m.sxy == Approx(0).margin(1e-9));
    // A hole counts as a hole.
    Path hole = rect(11, 20.5, 13, 21.5);
    std::reverse(hole.begin(), hole.end());
    const AreaMoments h = areaMoments({rect(10, 20, 14, 22), hole});
    CHECK(h.area == Approx(8 - 2));
    CHECK(h.centroid.x == Approx(12));
    CHECK(h.sxx == Approx(2 * 64.0 / 12 - 1 * 8.0 / 12));
    // The same outline the other way round measures the same.
    Path cw = rect(10, 20, 14, 22);
    std::reverse(cw.begin(), cw.end());
    const AreaMoments c = areaMoments({cw});
    CHECK(c.area == Approx(8));
    CHECK(c.sxx == Approx(m.sxx));
    CHECK(c.centroid.y == Approx(21));
}

TEST_CASE("the chance curve: nothing at no load, half at 1, sharper with a higher steepness", "[stability]") {
    CHECK(breakChance(0, 7) == 0);
    CHECK(breakChance(-1, 7) == 0);
    CHECK(breakChance(1, 7) == Approx(0.5));
    CHECK(breakChance(1, 3) == Approx(0.5));
    CHECK(breakChance(0.7, 7) == Approx(1 - std::exp2(-std::pow(0.7, 7))));
    CHECK(breakChance(0.7, 7) < breakChance(0.7, 5));
    CHECK(breakChance(1.2, 7) > breakChance(1.2, 5));
    double last = 0;
    for (double u = 0.1; u < 1.45; u += 0.1) {
        CHECK(breakChance(u, 7) > last);
        last = breakChance(u, 7);
    }
}

TEST_CASE("a straight column carries each layer's pull at exactly the pull per mm²", "[stability]") {
    Build b(40);
    b.part(0, 40, rect(0, 0, 4, 4));
    const StabilityResult r = b.run(StabilityConfig{});
    REQUIRE(r.parts.size() == 40);
    for (int L = 1; L < 40; ++L) {
        const PartRisk& p = at(r, L, {2, 2});
        CHECK(p.held);
        CHECK(p.stress == Approx(1.0));
        CHECK(p.load == Approx(16.0));
        CHECK(p.closeness == Approx(1.0 / kPerMm2));
        CHECK(p.cause == RiskCause::Neck);
        CHECK(p.at_layer == L);  // the same for every layer above it: the first counts
        CHECK(p.lost_chance == 0);
    }
    CHECK(at(r, 0, {2, 2}).closeness == 0);  // on the plate
    CHECK(r.stats.whole_chance == 1);
    CHECK(r.spots.empty());
    CHECK(r.stats.model_volume == Approx(16 * 40 * kH));
}

TEST_CASE("a cap on a stem: the stem's stress is the pull times cap area over stem area", "[stability]") {
    Build b(30);
    b.part(0, 20, rect(-0.5, -0.5, 0.5, 0.5));  // 1 mm²
    b.part(20, 30, rect(-5, -5, 5, 5));         // 100 mm²
    StabilityConfig cfg;
    cfg.replays = 4000;
    const StabilityResult r = b.run(cfg);
    const PartRisk& stem = at(r, 10, {0, 0});
    CHECK(stem.stress == Approx(100.0));
    CHECK(stem.load == Approx(100.0));
    CHECK(stem.at_layer == 20);  // when the cap's first layer prints
    CHECK(stem.cause == RiskCause::Neck);
    CHECK(stem.closeness == Approx(100.0 / kPerMm2));
    CHECK(stem.break_chance == Approx(breakChance(100.0 / kPerMm2, 7)));
    // The cap's own layers are pulled at 1; its first layer sits on the stem's 1 mm².
    CHECK(at(r, 25, {3, 3}).stress == Approx(1.0));
    CHECK(at(r, 20, {3, 3}).stress == Approx(100.0));
    // The stem breaks once, somewhere, not once per layer: the cap is lost as
    // often as one such neck breaks.
    const double p = breakChance(100.0 / kPerMm2, 7);
    CHECK(at(r, 25, {3, 3}).lost_chance == Approx(p).margin(0.025));
    CHECK(r.stats.whole_chance == Approx(1 - p).margin(0.025));
    // Listed once, though twenty layers of stem are equally loaded. (The cap's
    // first layer, hanging 4.5 mm past the stem all round, is listed above it.)
    int necks = 0;
    for (const WeakSpot& s : r.spots) {
        if (s.cause != RiskCause::Neck) continue;
        ++necks;
        CHECK(s.holds == Approx(kPerMm2));  // 1 mm² of resin
        CHECK(s.load == Approx(100.0));
        CHECK(s.chance == Approx(p));
        CHECK(s.at_layer == 20);
    }
    CHECK(necks >= 1);
    CHECK(necks <= 2);
    CHECK(r.spots[0].cause == RiskCause::Skin);

    // A bigger cap never makes the stem safer.
    Build bigger(30);
    bigger.part(0, 20, rect(-0.5, -0.5, 0.5, 0.5));
    bigger.part(20, 30, rect(-6, -6, 6, 6));
    const StabilityResult r2 = bigger.run(cfg);
    CHECK(at(r2, 10, {0, 0}).closeness > stem.closeness);
    CHECK(at(r2, 25, {3, 3}).lost_chance > at(r, 25, {3, 3}).lost_chance);
}

TEST_CASE("a leaning post bends: the textbook stress at its foot", "[stability]") {
    // 1 mm across the lean, 2 mm deep, stepping 0.01 mm sideways per layer.
    const int n = 60;
    const double t = 1.0, depth = 2.0, step = 0.01;
    Build b(n);
    for (int L = 0; L < n; ++L) b.part(L, L + 1, rect(L * step, 0, L * step + t, depth));
    const StabilityResult r = b.run(StabilityConfig{});
    // The joint between layers 1 and 0 while the top layer prints.
    const double pull = t * depth, shared = t - step;
    const double arm = (n - 2) * step + step / 2;  // from the joint's centre to the top layer's
    const double straight = pull / (depth * shared);
    const double bent = 6 * pull * arm / (depth * shared * shared);  // M·c/I for a rectangle
    const PartRisk& foot = at(r, 1, {step + 0.5, 1});
    CHECK(foot.stress == Approx(straight + bent).epsilon(1e-6));
    CHECK(foot.at_layer == n - 1);
    CHECK(foot.cause == RiskCause::Bend);
    CHECK(foot.load == Approx(pull));
    // Half way up there is half the arm above.
    const int mid = n / 2;
    const double arm_mid = (n - 1 - mid) * step + step / 2;
    CHECK(at(r, mid, {mid * step + 0.5, 1}).stress ==
          Approx(straight + 6 * pull * arm_mid / (depth * shared * shared)).epsilon(1e-6));
    // The same post upright is pulled, not bent.
    Build up(n);
    up.part(0, n, rect(0, 0, t, depth));
    CHECK(at(up.run(StabilityConfig{}), 1, {0.5, 1}).stress == Approx(1.0));
}

// Every piece of the model as one stiff block, however thin: the textbook
// cases of a rigid body on springs.
StabilityConfig stiffBlock() {
    StabilityConfig cfg;
    cfg.share_reach = 1e9;
    return cfg;
}

TEST_CASE("a stiff block on four tips: even shares when centred, more on the side the pull is over", "[stability]") {
    for (double off : {0.0, 2.0}) {
        Build b(30);
        for (double x : {-5.0, 5.0})
            for (double y : {-5.0, 5.0}) b.support(0, 20, rect(x - 0.2, y - 0.2, x + 0.2, y + 0.2));
        b.part(20, 30, rect(-6, -6, 6 + 2 * off, 6));  // over all four, its middle `off` to one side
        const StabilityResult r = b.run(stiffBlock());
        const double pull = 12 * (12 + 2 * off);
        // A rigid body on four equal springs: N/4 · (1 ± e/5).
        const double near = pull / 4 * (1 + off / 5), far = pull / 4 * (1 - off / 5);
        CHECK(at(r, 19, {5, 5}).load == Approx(near).epsilon(1e-6));
        CHECK(at(r, 19, {5, -5}).load == Approx(near).epsilon(1e-6));
        CHECK(at(r, 19, {-5, 5}).load == Approx(far).epsilon(1e-6));
        CHECK(at(r, 19, {-5, -5}).load == Approx(far).epsilon(1e-6));
        // The block's lowest layer: its most loaded tip.
        const PartRisk& low = at(r, 20, {off, 0});
        CHECK(low.load == Approx(near).epsilon(1e-6));
        CHECK(low.cause == RiskCause::Tip);
        // Sized from the area it shares with the model: 0.16 mm².
        const double d = 2 * std::sqrt(0.16 / kPi);
        CHECK(low.closeness == Approx(near / (11.1 * (d / 0.4) * (d / 0.4) / 0.7)).epsilon(1e-5));
        CHECK(r.stats.tips == 4);
        CHECK_FALSE(at(r, 10, {5, 5}).model);
        CHECK(at(r, 25, {off, 0}).model);
    }
}

TEST_CASE("a stiffer support takes more; one more tip never raises the most loaded", "[stability]") {
    // Three tips in a line under a centred block, the middle one twice as wide.
    Build b(30);
    b.support(0, 20, rect(-4.2, -0.2, -3.8, 0.2));
    b.support(0, 20, rect(-0.2, -0.4, 0.2, 0.4));
    b.support(0, 20, rect(3.8, -0.2, 4.2, 0.2));
    b.part(20, 30, rect(-6, -3, 6, 3));
    const StabilityResult r = b.run(stiffBlock());
    CHECK(at(r, 19, {-4, 0}).load == Approx(18.0).epsilon(1e-6));
    CHECK(at(r, 19, {0, 0}).load == Approx(36.0).epsilon(1e-6));
    CHECK(at(r, 19, {4, 0}).load == Approx(18.0).epsilon(1e-6));

    auto worstTip = [](bool fifth) {
        Build c(30);
        for (double x : {-5.0, 5.0})
            for (double y : {-5.0, 5.0}) c.support(0, 20, rect(x - 0.2, y - 0.2, x + 0.2, y + 0.2));
        if (fifth) c.support(0, 20, rect(-0.2, -0.2, 0.2, 0.2));
        c.part(20, 30, rect(-6, -6, 6, 6));
        return at(c.run(stiffBlock()), 20, {1, 1}).load;
    };
    CHECK(worstTip(true) < worstTip(false));
    CHECK(worstTip(true) == Approx(144.0 / 5).epsilon(1e-6));
}

TEST_CASE("a thin slab is held patch by patch; a tall block by all its tips together", "[stability]") {
    // Four tips, each sized from the 0.16 mm² it shares with the model,
    // under a piece whose middle is to one side of theirs.
    const double d = 2 * std::sqrt(0.16 / kPi), holds = 11.1 * (d / 0.4) * (d / 0.4) / 0.7;
    auto piece = [](double tips_at, const Path& outline, int layers_of_model) {
        Build b(20 + layers_of_model);
        for (double x : {-tips_at, tips_at})
            for (double y : {-tips_at * 0.75, tips_at * 0.75}) b.support(0, 20, rect(x - 0.2, y - 0.2, x + 0.2, y + 0.2));
        b.part(20, 20 + layers_of_model, outline);
        return b.run(StabilityConfig{});
    };
    // One layer of slab over tips 4 mm apart, every patch of it within a
    // fresh skin's reach of a tip: each tip holds what is nearest it. The
    // slab reaches 4.5 mm past the tips' middle on one side and 3 mm on the
    // other: 11.25 and 7.5 mm² a tip. (A stiff board would put 12.9 on each
    // of the near pair, by the lever rule.)
    const StabilityResult thin = piece(2, rect(-3, -2.5, 4.5, 2.5), 1);
    const double near = at(thin, 19, {2, 1.5}).load, far = at(thin, 19, {-2, 1.5}).load;
    CHECK(near + far == Approx(37.5 / 2).epsilon(1e-6));
    CHECK(near == Approx(11.25).epsilon(0.03));
    CHECK(far == Approx(7.5).epsilon(0.04));
    CHECK(thin.closeness_by_layer[20] == Approx(near / holds).epsilon(1e-5));
    // 40 mm of block on tips 10 mm apart: by its last layer all four share,
    // by the lever rule of a stiff body (67.2 mm² on each of the near pair),
    // to within a few per cent.
    const StabilityResult tall = piece(5, rect(-6, -6, 10, 6), 800);
    CHECK(tall.closeness_by_layer.back() * holds == Approx(67.2).epsilon(0.04));
    // The block itself is pulled straight: its tips balance the off-centre
    // pull between them, and nothing is left to bend it.
    CHECK(at(tall, 400, {2, 0}).stress == Approx(1.0).epsilon(1e-3));
    CHECK(at(tall, 21, {2, 0}).stress < 1.5);
}

TEST_CASE("one more tip under a thin slab takes load off the tips around it", "[stability]") {
    auto corner = [](bool fifth, double* middle) {
        Build c(21);
        for (double x : {-5.0, 5.0})
            for (double y : {-5.0, 5.0}) c.support(0, 20, rect(x - 0.2, y - 0.2, x + 0.2, y + 0.2));
        if (fifth) c.support(0, 20, rect(-0.2, -0.2, 0.2, 0.2));
        c.part(20, 21, rect(-6, -6, 6, 6));
        const StabilityResult r = c.run(StabilityConfig{});
        if (middle) *middle = at(r, 19, {0, 0}).load;
        return at(r, 19, {5, 5}).load;
    };
    double middle = 0;
    const double before = corner(false, nullptr), after = corner(true, &middle);
    CHECK(before == Approx(36.0).epsilon(1e-6));
    CHECK(after < before);
    CHECK(4 * after + middle == Approx(144.0).epsilon(1e-6));
    // The middle of a plate carries more than its corners.
    CHECK(middle > after);
}

TEST_CASE("a piece on one tip pivots: the tip takes the whole pull and no bending", "[stability]") {
    Build b(30);
    b.support(0, 20, rect(-0.2, -0.2, 0.2, 0.2));
    b.part(20, 30, rect(-1, -3, 5, 3));  // its middle is 2 mm off the tip
    const StabilityResult r = b.run(StabilityConfig{});
    const PartRisk& low = at(r, 20, {2, 0});
    CHECK(low.load == Approx(36.0).epsilon(1e-6));
    CHECK(low.cause == RiskCause::Tip);
    for (const PartRisk& p : r.parts) {
        CHECK(std::isfinite(p.closeness));
        CHECK(std::isfinite(p.stress));
    }
}

TEST_CASE("a part that starts in mid-air is unheld, and so is what prints on it alone", "[stability]") {
    Build b(30);
    b.part(0, 10, rect(0, 0, 4, 4));
    b.part(20, 30, rect(10, 0, 14, 4));  // nothing under it
    b.part(20, 21, rect(20, 0, 20.2, 0.2));  // a speck
    const StabilityResult r = b.run(StabilityConfig{});
    for (int L = 20; L < 30; ++L) {
        const PartRisk& p = at(r, L, {12, 2});
        CHECK_FALSE(p.held);
        CHECK(p.cause == RiskCause::Unheld);
        CHECK(p.lost_chance == 1);
    }
    CHECK(r.chanceAt(25, {12, 2}) == 1);
    CHECK(r.chanceAt(5, {2, 2}) == 0);
    CHECK(r.chanceAt(5, {50, 50}) == 0);  // off the print
    CHECK(r.stats.unheld == 1);
    CHECK(r.stats.unheld_specks == 1);
    REQUIRE(!r.spots.empty());
    CHECK(r.spots[0].cause == RiskCause::Unheld);
    CHECK(r.spots[0].chance == 1);
    CHECK(r.spots[0].area == Approx(16));
    CHECK(r.spots[0].layer == 20);
    CHECK(r.spots[0].lost_volume == Approx(16 * 10 * kH));  // all ten layers of it
    CHECK(r.stats.at_risk[3] == Approx((16 * 10 + 0.04) * kH));
}

TEST_CASE("a fresh skin fails past its reach and not within it", "[stability]") {
    Build b(22);
    b.part(0, 20, rect(-1, -1, 1, 1));
    b.part(20, 22, rect(-10, -10, 10, 10));  // a flat plate appearing in one layer
    StabilityConfig cfg;
    cfg.bulk_factor = 100;  // so the column itself is no part of it
    cfg.skin_half_reach = 3.0;
    const StabilityResult r = b.run(cfg);
    REQUIRE(r.skins.size() == 22);
    CHECK_FALSE(r.skins[20].empty());
    CHECK(r.skins[21].empty());  // the second layer lies on the first
    // 5 mm from the column: past the reach at which half fail.
    CHECK(r.chanceAt(20, {6, 0}) >= 0.89);
    // Exactly at that reach it is about a half; well inside there is nothing.
    CHECK(r.chanceAt(20, {4.02, 0}) >= 0.5);
    CHECK(r.chanceAt(20, {3.95, 0}) < 0.89);
    CHECK(r.chanceAt(20, {2.5, 0}) == 0);
    CHECK(r.chanceAt(21, {6, 0}) == 0);
    bool skin_spot = false;
    for (const WeakSpot& s : r.spots) skin_spot = skin_spot || (s.cause == RiskCause::Skin && s.layer == 20 && s.chance >= 0.89);
    CHECK(skin_spot);
    // A ledge of 1 mm holds itself.
    Build ledge(22);
    ledge.part(0, 20, rect(-1, -1, 1, 1));
    ledge.part(20, 22, rect(-2, -2, 2, 2));
    const StabilityResult l = ledge.run(cfg);
    CHECK(l.skins[20].empty());
    CHECK(l.chanceAt(20, {1.9, 1.9}) == 0);
}

TEST_CASE("a cup open to the film adds its suction to the wall around it", "[stability]") {
    for (double suction : {0.0, 4.0}) {
        Build b(30);
        b.part(0, 10, ring({0, 0}, 6));
        for (int L = 10; L < 30; ++L) {
            b.part(L, L + 1, ring({0, 0}, 6));
            b.print.slices[static_cast<size_t>(L)].push_back(ring({0, 0}, 4, 96, true));
            b.model.slices[static_cast<size_t>(L)].push_back(ring({0, 0}, 4, 96, true));
        }
        StabilityConfig cfg;
        cfg.cup_pull = suction;
        // unionNonZero in run() keeps the hole: outline one way, hole the other.
        const StabilityResult r = b.run(cfg);
        // Wall area 20 parts, opening 16 parts: 1 + suction · 16/20 on the wall,
        // and that pull over the solid floor's 36 parts below (which its own
        // layers already pulled at 1).
        CHECK(at(r, 15, {5, 0}).stress == Approx(1 + suction * 0.8).epsilon(1e-4));
        CHECK(at(r, 5, {0, 0}).stress == Approx(std::max(1.0, (20 + suction * 16) / 36)).epsilon(1e-4));
    }
}

TEST_CASE("more pull away from the hinge, as the planner has it", "[stability]") {
    Build b(20);
    b.part(0, 20, rect(48, -2, 52, 2));
    StabilityConfig cfg;
    cfg.peel_bias = 0.25;
    cfg.peel_axis = {1, 0};
    const StabilityResult r = b.run(cfg);
    CHECK(at(r, 10, {50, 0}).stress == Approx(1 + 0.25 * 0.5 * (50 / 105.84 + 1)));
}

TEST_CASE("two tips share: when one lets go the other takes its load", "[stability]") {
    // Each tip at 0.8 of where half let go. Alone, one lets go 13.5 % of the
    // time; the block comes off only when the second, then at 1.6, goes too.
    const double holds = 11.1 / 0.7, each = 0.8 * holds, width = 2 * each / 4;
    Build b(30);
    b.support(0, 20, rect(-3.2, -0.2, -2.8, 0.2));
    b.support(0, 20, rect(2.8, -0.2, 3.2, 0.2));
    b.part(20, 30, rect(-width / 2, -2, width / 2, 2));
    StabilityConfig cfg;
    cfg.bulk_factor = 10;  // only the tips can break
    cfg.replays = 4000;
    const StabilityResult r = b.run(cfg, {{{-3, 0, 1.0}, 0.4}, {{3, 0, 1.0}, 0.4}});
    const PartRisk& low = at(r, 20, {0, 0});
    CHECK(low.load == Approx(each).epsilon(1e-4));
    CHECK(low.closeness == Approx(0.8).epsilon(1e-4));  // the tip as made, not as cut
    const double one = breakChance(0.8, 7), second = breakChance(1.6, 7);
    const double both = 2 * one * second - one * one;
    CHECK(at(r, 25, {0, 0}).lost_chance == Approx(both).margin(0.03));
    CHECK(r.stats.whole_chance == Approx(1 - both).margin(0.03));
    // The supports stay on the plate either way.
    CHECK(at(r, 10, {-3, 0}).lost_chance == 0);
    // Both tips are told of, in the order they stand, each as it was given.
    REQUIRE(r.tips.size() == 2);
    const double volume = width * 4 * 10 * kH;
    for (int t = 0; t < 2; ++t) {
        const TipRisk& tip = r.tips[static_cast<size_t>(t)];
        CHECK(tip.contact == t);
        CHECK_FALSE(tip.foot);
        CHECK(tip.at.x == Approx(t == 0 ? -3 : 3));
        CHECK(tip.holds == Approx(holds));
        CHECK(tip.load == Approx(each).epsilon(1e-4));
        CHECK(tip.chance == Approx(one).epsilon(1e-3));
        // The block goes with a tip only when the other went before it: less than all of it, on average.
        CHECK(tip.lost_volume < volume);
    }
    CHECK(r.tips[0].lost_volume + r.tips[1].lost_volume > 0);
}

TEST_CASE("joints one after the other: lost if any of them breaks, and only what hangs from it", "[stability]") {
    // A support, its tip, then a thin stem under a cap.
    Build b(40);
    b.support(0, 20, rect(-0.6, -0.6, 0.6, 0.6));
    b.part(20, 30, rect(-0.5, -0.5, 0.5, 0.5));
    b.part(30, 40, rect(-1.75, -1.75, 1.75, 1.75));  // 12.25 mm²
    StabilityConfig cfg;
    cfg.bulk_factor = 0.1;  // weak resin, so the stem counts too
    cfg.replays = 4000;
    const StabilityResult r = b.run(cfg, {{{0, 0, 1.0}, 0.4}});
    const double bulk = kPerMm2 * 0.1, pull = 12.25;
    const double tip = breakChance(pull / (11.1 / 0.7), 7), stem = breakChance(pull / bulk, 7);
    CHECK(at(r, 25, {0, 0}).closeness == Approx(pull / bulk).epsilon(1e-6));
    CHECK(at(r, 20, {0, 0}).closeness == Approx(pull / (11.1 / 0.7)).epsilon(1e-6));
    // All of the pull goes down the support. Running alone under its tip it
    // is that tip's head, and is not counted as a weak joint of its own.
    CHECK(at(r, 10, {0, 0}).load == Approx(pull).epsilon(1e-6));
    CHECK(at(r, 10, {0, 0}).stress == Approx(pull / 1.44).epsilon(1e-6));
    CHECK(at(r, 10, {0, 0}).closeness == 0);
    CHECK(at(r, 1, {0, 0}).load == Approx(pull).epsilon(1e-6));
    // A tip and the support under it are one piece of strength (a tip is
    // measured with its head): the weaker of the two decides, here the tip.
    // The cap needs that and the stem to hold.
    CHECK(at(r, 35, {1, 1}).lost_chance == Approx(1 - (1 - tip) * (1 - stem)).margin(0.025));
    // The stem's lowest layer stays on its tip when the stem breaks above it.
    CHECK(at(r, 20, {0, 0}).lost_chance == Approx(tip).margin(0.02));
    // The support stays on the plate.
    CHECK(at(r, 10, {0, 0}).lost_chance == 0);
    // Listed once each: the tip (with its support) and the stem.
    int tips = 0, necks = 0;
    for (const WeakSpot& s : r.spots) {
        tips += s.cause == RiskCause::Tip || s.cause == RiskCause::Support;
        necks += s.cause == RiskCause::Neck;
    }
    CHECK(tips == 1);
    CHECK(necks == 1);
    // What goes with each. With the tip: the stem and the cap, every time.
    // With the stem: the cap and the stem above where it broke.
    REQUIRE(r.tips.size() == 1);
    CHECK(r.tips[0].contact == 0);
    CHECK(r.tips[0].chance == Approx(tip));
    CHECK(r.tips[0].load == Approx(pull).epsilon(1e-6));
    CHECK(r.tips[0].at_layer == 30);  // when the cap's first layer prints
    CHECK(r.tips[0].lost_volume == Approx((1.0 + 12.25) * 10 * kH));
    for (const WeakSpot& s : r.spots)
        if (s.cause == RiskCause::Neck) {
            CHECK(s.lost_volume >= 12.25 * 10 * kH - 1e-9);
            CHECK(s.lost_volume <= (1.0 + 12.25) * 10 * kH + 1e-9);
        }
}

TEST_CASE("a lobe on one tip is lost when the tip lets go, even though it joins the model later", "[stability]") {
    Build b(45);
    b.part(0, 40, rect(0, 0, 4, 4));             // stands on the plate
    b.support(0, 20, rect(9.4, 1.4, 10.6, 2.6));
    b.part(20, 40, rect(8, 0, 12, 4));           // the lobe: 16 mm² on one tip
    b.part(40, 45, rect(0, 0, 12, 4));           // joins the two
    StabilityConfig cfg;
    cfg.replays = 4000;
    const StabilityResult r = b.run(cfg, {{{10, 2, 1.0}, 0.4}});
    const double p = breakChance(16 / (11.1 / 0.7), 7);
    CHECK(p > 0.4);
    for (int L : {20, 30, 39}) CHECK(at(r, L, {10, 2}).lost_chance == Approx(p).margin(0.03));
    // What joins them prints on the standing part, and that part is never lost.
    CHECK(at(r, 42, {10, 2}).lost_chance == 0);
    CHECK(at(r, 30, {2, 2}).lost_chance == 0);
    // The joining layers pull on the span between the two, and the tip
    // carries its end of that span: more than under the lobe alone. Letting go
    // then loses nothing: the lobe hangs on the standing part by that time.
    CHECK(at(r, 20, {10, 2}).at_layer >= 40);
    CHECK(at(r, 20, {10, 2}).load > 16);
    CHECK(at(r, 20, {10, 2}).load < 26);
}

TEST_CASE("supports fused into a block against the model are not one giant tip", "[stability]") {
    // A wall of supports 6 mm long reaching up beside the model, which lies
    // on it by a 0.2 mm strip. Cut together, wall and model are one outline
    // in the layer they share; the tip is the strip, not the wall.
    Build b(40);
    b.support(0, 21, rect(0, 0, 6, 2));    // 12 mm², and still there in the model's first layer
    b.part(20, 40, rect(0, 1.8, 6, 8));    // 37.2 mm², over the wall's last 0.2 mm
    for (bool whole_supports : {false, true}) {
        LayerStack supports = b.print;
        for (size_t L = 0; L < supports.size(); ++L) supports.slices[L] = L <= 20 ? Paths{rect(0, 0, 6, 2)} : Paths{};
        Build copy = b;
        for (LayerStack* s : {&copy.print, &copy.model})
            for (Paths& sl : s->slices) sl = unionNonZero(sl);
        StabilityInput in;
        in.print = &copy.print;
        in.model = &copy.model;
        if (whole_supports) in.supports = &supports;
        const StabilityResult r = analyzeStability(in, StabilityConfig{});
        CHECK(r.stats.tips == 1);
        const PartRisk& low = at(r, 20, {3, 5});
        CHECK(low.cause == RiskCause::Tip);
        CHECK(low.joint_area == Approx(6 * 0.2).epsilon(1e-4));
        CHECK(low.load == Approx(37.2).epsilon(1e-4));
        const double d = 2 * std::sqrt(1.2 / kPi);
        CHECK(low.closeness == Approx(37.2 / (11.1 * (d / 0.4) * (d / 0.4) / 0.7)).epsilon(1e-4));
        // The wall is pulled hardest while the layer the two share prints: by
        // all of that layer, its own 10.8 mm² and the model's 37.2. Never overloaded.
        CHECK(at(r, 10, {3, 1}).load == Approx(48.0).epsilon(1e-3));
        CHECK(at(r, 10, {3, 1}).at_layer == 20);
        CHECK(at(r, 10, {3, 1}).closeness < 0.05);
        CHECK_FALSE(at(r, 10, {3, 1}).model);
        CHECK(at(r, 30, {3, 5}).model);
    }
}

TEST_CASE("a tip that meets a stepped face in two joints is one tip", "[stability]") {
    Build b(40);
    b.support(0, 21, rect(0.8, 0, 1.2, 0.4));  // its top layer half sunk into the model
    b.part(20, 21, rect(0, 0, 1, 4));          // the model's lowest layer: over half of the tip
    b.part(21, 40, rect(0, 0, 2, 4));          // then over all of it
    LayerStack supports = b.print;
    for (size_t L = 0; L < supports.size(); ++L) supports.slices[L] = L <= 20 ? Paths{rect(0.8, 0, 1.2, 0.4)} : Paths{};
    for (LayerStack* s : {&b.print, &b.model})
        for (Paths& sl : s->slices) sl = unionNonZero(sl);
    StabilityInput in;
    in.print = &b.print;
    in.model = &b.model;
    in.supports = &supports;
    in.tips = {{{1.0, 0.2, 1.0}, 0.4}};
    StabilityConfig cfg;
    cfg.replays = 2000;
    const StabilityResult r = analyzeStability(in, cfg);
    CHECK(r.stats.tips == 1);
    // Both joints show the whole tip: all of the pull against what one 0.4 mm tip holds.
    const double u = 8.0 / (11.1 / 0.7);
    CHECK(at(r, 20, {0.5, 2}).closeness == Approx(u).epsilon(1e-4));
    CHECK(at(r, 21, {0.5, 2}).closeness == Approx(u).epsilon(1e-4));
    CHECK(at(r, 21, {0.5, 2}).load == Approx(8.0).epsilon(1e-4));
    // They let go together, once: the model is lost as often as one tip lets go.
    CHECK(at(r, 30, {1, 2}).lost_chance == Approx(breakChance(u, 7)).margin(0.012));
    // The sunk half of the tip is lit as model, not twice.
    CHECK(r.stats.model_volume == Approx((4.0 + 19 * 8.0) * kH));
    CHECK(at(r, 19, {1, 0.2}).load == Approx(8.0).epsilon(1e-4));
}

TEST_CASE("a trunk carries the sum of the branches that join in it", "[stability]") {
    // Two branches, each under its own block, standing on one trunk.
    Build b(60);
    b.support(0, 20, rect(0, 0, 3, 1));        // the trunk, 3 mm²
    b.support(20, 40, rect(0, 0, 1, 1));       // branch under the first block
    b.support(20, 40, rect(2, 0, 3, 1));       // branch under the second
    b.part(40, 60, rect(-1, -1, 1.4, 2));      // 7.2 mm²
    b.part(40, 60, rect(1.6, -1, 5, 3));       // 13.6 mm²
    const StabilityResult r = b.run(StabilityConfig{});
    CHECK(at(r, 30, {0.5, 0.5}).load == Approx(7.2).epsilon(1e-6));
    CHECK(at(r, 30, {2.5, 0.5}).load == Approx(13.6).epsilon(1e-6));
    // All the way down the trunk: both.
    for (int L : {19, 10, 1}) {
        CHECK(at(r, L, {1.5, 0.5}).load == Approx(20.8).epsilon(1e-6));
        CHECK(at(r, L, {1.5, 0.5}).stress == Approx(20.8 / 3).epsilon(1e-6));
        CHECK(at(r, L, {1.5, 0.5}).closeness == Approx(20.8 / (3 * kPerMm2)).epsilon(1e-6));
    }
}

TEST_CASE("a pull finds its way round through a joint at the side", "[stability]") {
    // A wall and a thin post stand on one base. The post carries a shelf.
    // Above the shelf, one end of it is joined to the wall, and a tower
    // stands on the other end. The tower's pull does not have to go down
    // the post: the shelf hangs from the wall as well.
    Build b(80);
    b.part(0, 10, rect(0, 0, 24, 10));               // the base, on the plate
    b.part(10, 50, rect(0, 0, 4, 10));               // the wall
    b.part(10, 40, rect(14.5, 4.5, 15.5, 5.5));      // the post, 1 mm²
    b.part(40, 44, rect(8, 2, 22, 8));               // the shelf, 84 mm², its middle over the post
    b.part(44, 50, rect(0, 0, 12, 10));              // joins the shelf's near end to the wall
    b.part(44, 80, rect(16, 2, 22, 8));              // the tower on its far end, 36 mm²
    const StabilityResult r = b.run(StabilityConfig{});
    // The post is pulled hardest while the shelf alone hangs from it, by all
    // of the shelf and straight.
    const PartRisk& post = at(r, 25, {15, 5});
    CHECK(post.load == Approx(84.0).epsilon(1e-6));
    CHECK(post.closeness == Approx(84.0 / kPerMm2).epsilon(1e-6));
    CHECK(post.at_layer >= 40);
    CHECK(post.at_layer <= 43);
    CHECK(post.cause == RiskCause::Neck);
    // The tower's pull, off to one side of the post, never bends it: that
    // would take seven times what the post stands.
    CHECK(post.stress == Approx(84.0).epsilon(1e-6));
    // It goes through the joint to the wall instead, and bends the wall a little.
    CHECK(at(r, 30, {2, 5}).closeness < 0.5);
    CHECK(at(r, 30, {2, 5}).at_layer >= 44);
}

TEST_CASE("a second branch joining the model does not load the lowest tip with what the other tip holds", "[stability]") {
    // Two columns, each on its own tip, joined by a bridge higher up.
    Build b(80);
    b.support(0, 20, rect(1.8, 1.8, 2.2, 2.2));
    b.part(20, 70, rect(0, 0, 4, 4));
    b.support(0, 50, rect(9.8, 1.8, 10.2, 2.2));
    b.part(50, 70, rect(8, 0, 12, 4));
    b.part(70, 80, rect(0, 0, 12, 4));  // 48 mm², its middle half-way between the tips
    const StabilityResult r = b.run(StabilityConfig{});
    // Each tip holds its own column, then half of the bridge: 24 mm². (Not
    // to the last per cent: the taller column is the stiffer one, and draws a
    // little more of the bridge.)
    const double d = 2 * std::sqrt(0.16 / kPi), holds = 11.1 * (d / 0.4) * (d / 0.4) / 0.7;
    CHECK(at(r, 19, {2, 2}).load == Approx(24.0).epsilon(0.04));
    CHECK(at(r, 49, {10, 2}).load == Approx(24.0).epsilon(0.04));
    CHECK(at(r, 20, {2, 2}).closeness == Approx(24.0 / holds).epsilon(0.04));
    // And each column is pulled straight by what its own tip holds.
    CHECK(at(r, 40, {2, 2}).stress == Approx(24.0 / 16).epsilon(0.04));
    CHECK(at(r, 60, {10, 2}).stress == Approx(24.0 / 16).epsilon(0.04));
}

TEST_CASE("a tip under a small ledge of a thick piece holds the ledge, not the piece", "[stability]") {
    // A block 10 mm tall on four tips. Near its top a small ledge on its
    // side, with a tip of its own on a long support.
    Build b(220);
    for (double x : {1.0, 9.0})
        for (double y : {1.0, 9.0}) b.support(0, 20, rect(x - 0.2, y - 0.2, x + 0.2, y + 0.2));
    b.part(20, 220, rect(0, 0, 10, 10));
    b.part(170, 180, rect(10, 4, 10.6, 6));  // 1.2 mm² of ledge
    b.support(0, 170, rect(10.1, 4.8, 10.5, 5.2));
    const StabilityResult r = b.run(StabilityConfig{});
    // While the ledge prints, its tip holds it and a little of the block's
    // edge beside it. Afterwards the block's pull goes to the four tips under
    // it: the ledge's tip, on its long thin support, draws next to nothing.
    CHECK(at(r, 169, {10.3, 5}).load > 1.0);
    CHECK(at(r, 169, {10.3, 5}).load < 6.0);
    CHECK(at(r, 169, {10.3, 5}).at_layer <= 180);
    for (double x : {1.0, 9.0})
        for (double y : {1.0, 9.0}) CHECK(at(r, 19, {x, y}).load == Approx(25.0).epsilon(0.03));
}

TEST_CASE("a support stands on the model with a tip turned upside down, and only there", "[stability]") {
    // A block on the plate. A support stands on it, its foot sunk 0.1 mm
    // in, and holds a second block by a tip. Half-way up, a step of the
    // second block lies against the support.
    Build b(70);
    b.part(0, 20, rect(0, 0, 6, 6));
    b.support(18, 19, rect(2.9, 2.9, 3.1, 3.1));    // the foot, sunk into the block: 0.04 mm²,
    b.support(19, 20, rect(2.85, 2.85, 3.15, 3.15)); // 0.09 mm²,
    b.support(20, 60, rect(2.8, 2.8, 3.2, 3.2));    // and the support at its full 0.16 mm²
    b.part(50, 60, rect(3.1, 0, 6, 6));             // the step: lies in the support by 0.04 mm²
    b.part(60, 70, rect(0, 0, 6, 6));               // 36 mm² over the support's top
    StabilityConfig cfg;
    cfg.replays = 2000;
    const StabilityResult r = b.runWhole(cfg);
    const double d = 2 * std::sqrt(0.16 / kPi), holds = 11.1 * (d / 0.4) * (d / 0.4) / 0.7;
    // Two tips: the foot, and the support's top with the step lying against it.
    CHECK(r.stats.tips == 2);
    // All of the upper block's pull goes down the support and through its
    // foot: the three layers of the foot are one tip, as wide as its widest.
    CHECK(supportAt(r, 40, {3, 3}).load == Approx(36.0).epsilon(1e-3));
    for (int L : {18, 19, 20}) {
        CHECK(supportAt(r, L, {3, 3}).closeness == Approx(36.0 / holds).epsilon(1e-3));
        CHECK(supportAt(r, L, {3, 3}).cause == RiskCause::Tip);
    }
    // The block it stands on is pulled by that too, on top of its own layers' pull.
    CHECK(at(r, 10, {1, 1}).load == Approx(36.0).epsilon(1e-3));
    CHECK(at(r, 10, {1, 1}).lost_chance == 0);
    // The upper block is lost when the foot lets go, or the tip under it.
    const double one = breakChance(36.0 / holds, 7);
    CHECK(at(r, 65, {1, 1}).lost_chance >= one - 0.03);
}

TEST_CASE("a strut between two parts of one piece is not a way to the plate", "[stability]") {
    // A base on four tips, two walls on it, and a thick lid across the
    // walls. Inside, a support stands on the base and holds the lid's
    // underside in the middle of its 8 mm span.
    Build b(200);
    for (double x : {1.0, 11.0})
        for (double y : {1.0, 9.0}) b.support(0, 20, rect(x - 0.2, y - 0.2, x + 0.2, y + 0.2));
    b.part(20, 40, rect(0, 0, 12, 10));
    b.part(40, 80, rect(0, 0, 2, 10));
    b.part(40, 80, rect(10, 0, 12, 10));
    b.part(80, 200, rect(0, 0, 12, 10));
    b.support(38, 39, rect(5.9, 4.9, 6.1, 5.1));     // the strut's foot, sunk into the base
    b.support(39, 40, rect(5.85, 4.85, 6.15, 5.15));
    b.support(40, 80, rect(5.8, 4.8, 6.2, 5.2));     // and the strut
    const StabilityResult r = b.runWhole(StabilityConfig{});
    CHECK(r.stats.tips == 6);
    // The strut is pulled hardest while the lid's first layers print: it
    // holds the thin span around it. It is not a leg of the piece: once the
    // lid is thick, the pull goes down the walls, which rise no more over
    // the strut's tip than under its foot.
    const PartRisk& strut = supportAt(r, 60, {6, 5});
    CHECK(strut.at_layer <= 85);
    CHECK(strut.load > 10);
    CHECK(strut.load < 60);
    for (double x : {1.0, 11.0}) {
        const PartRisk& wall = at(r, 60, {x, 5});
        CHECK(wall.at_layer > 100);
        CHECK(wall.stress == Approx(120.0 / 40).epsilon(0.02));
    }
    // And all of it ends on the four tips under the base.
    for (double x : {1.0, 11.0})
        for (double y : {1.0, 9.0}) CHECK(at(r, 19, {x, y}).load == Approx(30.0).epsilon(0.03));
}

TEST_CASE("a support lying along the model is a seam, each layer of it holding by itself", "[stability]") {
    // A block on one tip, off to one side, and a support that lies in its
    // other side by 0.04 mm² for 2 mm of its height.
    Build b(100);
    b.support(0, 20, rect(0.8, 2.8, 1.2, 3.2));
    b.part(20, 100, rect(0, 0, 6, 6));
    b.support(0, 60, rect(5.9, 2.8, 6.3, 3.2));
    const StabilityResult r = b.runWhole(StabilityConfig{});
    CHECK(r.stats.tips == 1);
    // The block's first layer lies on the tip and on the first 0.04 mm² of
    // the seam, and each holds what is nearest it. That first layer of the
    // seam is judged by its own 0.04 mm², not by the 1.6 mm² the seam comes
    // to: it is the joint that is closest to letting go.
    const double d = 2 * std::sqrt(0.04 / kPi), holds = 11.1 * (d / 0.4) * (d / 0.4) / 0.7;
    const PartRisk& first = at(r, 20, {3, 3});
    REQUIRE(first.cause == RiskCause::Tip);
    CHECK(first.joint_area == Approx(0.04).epsilon(1e-3));
    CHECK(first.load / first.closeness == Approx(holds).epsilon(1e-3));
    CHECK(first.load > 5);
    CHECK(first.load < 36);
}

TEST_CASE("a ring growing a little each layer has no skin out of reach", "[stability]") {
    // Each layer hangs 0.01 mm past the one below, all round. The hole is
    // smaller than the reach, so growing what is below closes it up.
    Build b(40);
    b.part(0, 10, ring({0, 0}, 2.0));
    for (int L = 10; L < 40; ++L) {
        b.print.slices[static_cast<size_t>(L)] = {ring({0, 0}, 2.0 + 0.01 * (L - 9)), ring({0, 0}, 1.5, 96, true)};
        b.model.slices[static_cast<size_t>(L)] = b.print.slices[static_cast<size_t>(L)];
    }
    StabilityConfig cfg;
    cfg.cup_pull = 0;
    const StabilityResult r = b.run(cfg);
    for (const auto& layer : r.skins) CHECK(layer.empty());
    CHECK(r.spots.empty());
    CHECK(r.chanceAt(30, {1.8, 0}) == 0);
}

TEST_CASE("a scrap smaller than a printer pixel is not a part of the print", "[stability]") {
    Build b(30);
    b.part(0, 30, rect(0, 0, 4, 4));
    b.part(15, 16, rect(10, 0, 10.01, 0.01));  // 0.0001 mm², in mid-air
    const StabilityResult r = b.run(StabilityConfig{});
    CHECK(r.stats.unheld == 0);
    CHECK(r.spots.empty());
    CHECK(r.stats.whole_chance == 1);
}

TEST_CASE("a support standing on the model passes its load into the model under it", "[stability]") {
    Build b(60);
    b.part(0, 20, rect(0, 0, 6, 6));               // stands on the plate
    b.support(20, 40, rect(2.8, 2.8, 3.2, 3.2));   // a support standing on it
    b.part(40, 60, rect(1, 1, 5, 5));              // 16 mm² on that support
    StabilityConfig cfg;
    cfg.replays = 2000;
    const StabilityResult r = b.run(cfg);
    const double d = 2 * std::sqrt(0.16 / kPi), holds = 11.1 * (d / 0.4) * (d / 0.4) / 0.7;
    // Its tip and its foot carry the same pull; each is sized by the 0.16 mm² it touches with.
    CHECK(at(r, 40, {3, 3}).load == Approx(16.0).epsilon(1e-6));
    CHECK(at(r, 40, {3, 3}).closeness == Approx(16.0 / holds).epsilon(1e-5));
    CHECK(at(r, 20, {3, 3}).load == Approx(16.0).epsilon(1e-6));
    CHECK(at(r, 20, {3, 3}).closeness == Approx(16.0 / holds).epsilon(1e-5));
    // The block under it is pulled by its own 36 mm² and by what the support brings.
    CHECK(at(r, 10, {3, 3}).load == Approx(36.0).epsilon(1e-6));
    CHECK(at(r, 10, {3, 3}).stress == Approx(1.0).epsilon(1e-6));
    // What stands on the plate is never lost; the top is lost if tip or foot goes.
    CHECK(at(r, 10, {3, 3}).lost_chance == 0);
    const double one = breakChance(16.0 / holds, 7);
    CHECK(at(r, 50, {3, 3}).lost_chance == Approx(1 - (1 - one) * (1 - one)).margin(0.03));
}

TEST_CASE("the same answer whatever the number of threads, and from run to run", "[stability]") {
    auto run = [](unsigned threads) {
        Build b(40);
        for (double x : {-5.0, 5.0})
            for (double y : {-5.0, 5.0}) b.support(0, 20, rect(x - 0.2, y - 0.2, x + 0.2, y + 0.2));
        b.part(20, 30, rect(-4, -6, 8, 6));
        b.part(30, 40, rect(0, 0, 1, 1));
        StabilityConfig cfg;
        cfg.threads = threads;
        return b.run(cfg);
    };
    const StabilityResult a = run(1), c = run(4), d = run(4);
    REQUIRE(a.parts.size() == c.parts.size());
    for (size_t i = 0; i < a.parts.size(); ++i) {
        CHECK(a.parts[i].closeness == c.parts[i].closeness);
        CHECK(a.parts[i].at_layer == c.parts[i].at_layer);
        CHECK(a.parts[i].lost_chance == c.parts[i].lost_chance);
        CHECK(c.parts[i].lost_chance == d.parts[i].lost_chance);
        CHECK(a.parts[i].cause == c.parts[i].cause);
    }
    CHECK(a.closeness_by_layer == c.closeness_by_layer);
    CHECK(a.spots.size() == c.spots.size());
}

TEST_CASE("the planner's settings carry over", "[stability]") {
    SupportConfig cfg;
    cfg.peel_bias = 0.25;
    cfg.peel_axis = {0, 1};
    cfg.coverage.density = 2.0;  // a job that asked for closer tips
    cfg.capacity.table = {{0.3, 5.0}, {0.6, 30.0}};
    const StabilityConfig s = stabilityConfig(cfg);
    CHECK(s.peel_bias == 0.25);
    CHECK(s.peel_axis.y == 1);
    CHECK(s.capacity.capacity(0.6) == Approx(30.0));
    // A fitted table is 0.7 of where half let go, as `fit` writes it.
    CHECK(s.usable_fraction == Approx(0.7));
    // The reach is the resin's, not the job's spacing choice.
    CHECK(s.skin_half_reach == Approx(3.2 / std::sqrt(0.7)));
    // The built-in table is borrowed: taken as a third of where half let go.
    const StabilityConfig built_in = stabilityConfig(SupportConfig{});
    CHECK(built_in.usable_fraction == Approx(1.0 / 3));
    CHECK(built_in.skin_half_reach == Approx(3.2 * std::sqrt(3.0)));
    CHECK(std::string(causeName(RiskCause::Tip)) == "tip");
}

TEST_CASE("a box on the planner's own supports: every part held, no tip past where half let go", "[stability]") {
    const TriangleMesh box = makeBox({-8, -8, 5}, {8, 8, 11});
    SupportConfig cfg;
    cfg.layer_height = 0.05;
    const PointGenResult pts = generateSupportPoints(box, cfg);
    REQUIRE(!pts.points.empty());
    TreeConfig tc;
    const SupportTree tree = buildSupportTree(box, pts.points, tc, minRaftThickness(4, 0, 0.05));
    const TriangleMesh sup = tree.mesh(tc);
    const LayerStack sup_layers = sliceModel(sup, 0.05);
    const LayerStack print = unionLayers(*pts.analysis.layers, sup_layers);
    StabilityInput in;
    in.print = &print;
    in.model = pts.analysis.layers.get();
    in.supports = &sup_layers;
    in.model_graph = pts.analysis.islands;
    for (const Head& h : tree.heads) in.tips.push_back({h.contact, h.spec.tip_diameter});
    const StabilityResult r = analyzeStability(in, stabilityConfig(cfg));
    CHECK(r.stats.unheld == 0);
    CHECK(r.stats.tips >= tree.heads.size() / 2);
    CHECK(r.stats.model_volume == Approx(16 * 16 * 6.0).epsilon(0.01));
    double worst_tip = 0;
    for (const PartRisk& p : r.parts) {
        REQUIRE(std::isfinite(p.closeness));
        if (p.cause == RiskCause::Tip) worst_tip = std::max<double>(worst_tip, p.closeness);
    }
    CHECK(worst_tip > 0.05);
    CHECK(worst_tip < 1.0);
    // The box itself is nowhere near breaking, and is not expected to be lost.
    const int mid = r.partAt(160, {0, 0});
    REQUIRE(mid >= 0);
    CHECK(r.parts[static_cast<size_t>(mid)].model);
    CHECK(r.parts[static_cast<size_t>(mid)].lost_chance < 0.05);
    CHECK(r.closeness_by_layer.size() == print.size());
}
