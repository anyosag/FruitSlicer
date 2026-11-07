// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Hollowing: walls at least as thick as asked at any slope, ceilings that
// hold themselves up, and cavities found whole.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "layers/Hollow.h"
#include "mesh/Primitives.h"

using namespace fruit;
using Catch::Approx;

namespace {

Paths square(double cx, double cy, double half) {
    return {Path{toPoint({cx - half, cy - half}), toPoint({cx + half, cy - half}), toPoint({cx + half, cy + half}),
                 toPoint({cx - half, cy + half})}};
}

double mismatch(const Paths& a, const Paths& b) {  // area of the symmetric difference
    return std::abs(area(difference(a, b))) + std::abs(area(difference(b, a)));
}

bool inside(const Paths& p, Vec2 q) {
    for (const ExPolygon& e : toExPolygons(p))
        if (e.contains(q)) return true;
    return false;
}

// The most area of any layer's interior that lies deeper than `reach` inside
// it without interior above: ceiling reaching further than `reach` from the
// solid under it. With `wider_than` > 0, only the parts of it wider than
// that count. Zero when every ceiling holds itself up.
double hanging(const std::vector<Paths>& in, double reach, double wider_than = 0) {
    double worst = 0;
    for (size_t j = 0; j < in.size(); ++j) {
        const Paths above = j + 1 < in.size() ? in[j + 1] : Paths{};
        Paths h = difference(offset(in[j], -reach), above);
        if (wider_than > 0) h = offset(offset(h, -wider_than / 2), wider_than / 2);
        worst = std::max(worst, std::abs(area(h)));
    }
    return worst;
}

HollowOptions flatCeilings() {
    HollowOptions o;
    o.roof_lean = 90;
    return o;
}

}  // namespace

TEST_CASE("a sliding-window intersection meets each window's slices", "[hollow]") {
    std::vector<Paths> in;
    for (int i = 0; i < 13; ++i)
        in.push_back(i == 9 ? Paths{} : square(2 * std::sin(i), 2 * std::cos(0.7 * i), 5 + i % 3));
    for (int m = 0; m <= 6; ++m)
        for (int n : {1, 2, 5, 9, 13}) {
            const std::vector<Paths> sub(in.begin(), in.begin() + n);
            const std::vector<Paths> out = windowIntersection(sub, m, 3);
            REQUIRE(static_cast<int>(out.size()) == n);
            for (int i = 0; i < n; ++i) {
                Paths want;  // empty where the window reaches past either end
                if (i - m >= 0 && i + m < n) {
                    want = sub[i - m];
                    for (int k = i - m + 1; k <= i + m; ++k) want = intersection(want, sub[k]);
                }
                CHECK(mismatch(out[i], want) < 1e-6);
            }
        }
}

TEST_CASE("a cube keeps its wall on every side, floor and ceiling too", "[hollow]") {
    const LayerStack ls = sliceModel(makeBox({0, 0, 0}, {20, 20, 20}), 0.05);
    HollowOptions o = flatCeilings();
    o.opening = 0;
    const HollowResult r = hollowInterior(ls, o);
    REQUIRE(r.cavities.size() == 1);
    CHECK(r.cavities[0].first_layer == 40);  // from z 2.00: a 2 mm floor
    CHECK(r.cavities[0].last_layer == 359);  // to z 18.00: a 2 mm ceiling
    CHECK(r.interior[39].empty());
    CHECK(r.interior[360].empty());
    // Inside the cube eroded by 2 mm, and at most a few hundredths thicker
    // (outlines are simplified to 5 µm, and erosions take that back).
    for (int j = 40; j <= 359; j += 29) {
        CHECK(std::abs(area(difference(r.interior[j], square(10, 10, 8)))) < 1e-9);
        CHECK(std::abs(area(difference(square(10, 10, 7.98), r.interior[j]))) < 1e-9);
    }
    CHECK(r.volume == Approx(16.0 * 16 * 16).epsilon(0.005));
    CHECK(r.cavities[0].volume == Approx(r.volume));

    const LayerStack hollow = hollowedLayers(ls, r);
    // A frame 2 mm wide, or a few hundredths more.
    CHECK(area(hollow.slices[200]) >= 400 - 256 - 1e-6);
    CHECK(area(hollow.slices[200]) <= 400 - 15.95 * 15.95);
    CHECK(area(hollow.slices[20]) == Approx(400).margin(1e-3));
    HollowResult other = r;
    other.interior.pop_back();
    CHECK_THROWS_AS(hollowedLayers(ls, other), std::invalid_argument);
}

TEST_CASE("on a sphere the wall stays between t and 1.08 t, whatever the slope", "[hollow]") {
    const double R = 15, t = 2, h = 0.1;
    const LayerStack ls = sliceModel(makeSphere({0, 0, R}, R, 128), h);
    HollowOptions o = flatCeilings();
    o.wall = t;
    o.opening = 0;
    const HollowResult r = hollowInterior(ls, o);
    REQUIRE(r.cavities.size() == 1);
    const int N = o.cylinders;
    int checked = 0;
    for (size_t i = 0; i < ls.size(); ++i) {
        const double z = std::abs(ls.sample_z[i] - R);  // from the centre
        if (z > 0.7 * R) continue;
        // Each cylinder fits where its far corners, at the window's farthest
        // slice (z + m h), are inside the sphere; the narrowest one decides.
        double expect = R;
        for (int k = 1; k <= N; ++k) {
            const double radius = t * std::sqrt(1.0 - static_cast<double>(k - 1) / N);
            const int m = static_cast<int>(std::ceil(t * std::sqrt(static_cast<double>(k) / N) / h - 1e-9));
            const double reach = z + m * h;
            expect = std::min(expect, std::sqrt(R * R - reach * reach) - radius);
        }
        // A disc this wide: never wider, and at most 40 µm narrower (outlines
        // are simplified to 5 µm and the erosions make up for it).
        const double got = std::sqrt(area(r.interior[i]) / kPi);
        CHECK(got <= expect + 0.005);
        CHECK(got >= expect - 0.04);
        // The header's bounds: inside the sphere eroded by t, and around it
        // eroded by t·√(1 + 1/N) plus the layer a half-height is rounded by.
        CHECK(got <= std::sqrt((R - t) * (R - t) - z * z) + 0.01);
        const double most = std::sqrt(t * t * (1 + 1.0 / N) + 2 * t * h + h * h);
        CHECK(got >= std::sqrt((R - most) * (R - most) - z * z) - 0.01);
        ++checked;
    }
    CHECK(checked > 150);
    CHECK(ls.layerBottom(r.cavities[0].first_layer) >= t - 1e-9);   // the floor at the pole
    CHECK(ls.layerTop(r.cavities[0].last_layer) <= 2 * R - t + 1e-9);  // and the ceiling
}

TEST_CASE("under a 45° roof a spherical cavity keeps three quarters of itself", "[hollow]") {
    // Its lower half, and over it a cone rising from the equator: anything
    // flatter can't hold itself up.
    const double R = 15, h = 0.1;
    const LayerStack ls = sliceModel(makeSphere({0, 0, R}, R, 128), h);
    const HollowResult flat = hollowInterior(ls, flatCeilings());
    const HollowResult roofed = hollowInterior(ls, HollowOptions{});
    REQUIRE(roofed.cavities.size() == 1);
    CHECK(roofed.volume / flat.volume == Approx(0.75).margin(0.03));
    CHECK(hanging(roofed.interior, 1.25 * h, 0.5) < 1e-6);
    // Within d of the layer below, but for rings a micrometre thin where the
    // sphere itself leans at 45°.
    CHECK(hanging(roofed.interior, h + 0.002, 0.01) < 1e-3);
    // The lower half is untouched.
    for (size_t i = 0; ls.sample_z[i] < R; ++i) CHECK(mismatch(roofed.interior[i], flat.interior[i]) < 0.05);
}

TEST_CASE("a part thinner than two walls has no interior", "[hollow]") {
    const LayerStack ls = sliceModel(makeBox({0, 0, 0}, {30, 30, 3.9}), 0.05);
    const HollowResult r = hollowInterior(ls, HollowOptions{});
    CHECK(r.cavities.empty());
    CHECK(r.volume == 0);
    CHECK(std::all_of(r.interior.begin(), r.interior.end(), [](const Paths& p) { return p.empty(); }));
}

TEST_CASE("overlapping shells hollow as one cavity, with no wall where they overlap", "[hollow]") {
    TriangleMesh m = makeBox({0, 0, 0}, {20, 20, 20});
    m.append(makeBox({12, 6, 8}, {32, 26, 28}));
    const LayerStack ls = sliceModel(m, 0.1);
    const HollowResult r = hollowInterior(ls, flatCeilings());
    REQUIRE(r.cavities.size() == 1);
    const Paths& at = r.interior[ls.layerAt(14)];
    CHECK(inside(at, {19, 15}));  // 1 mm inside the first box's side, deep in the second box
    CHECK(inside(at, {5, 5}));
    CHECK(inside(at, {28, 22}));
    CHECK_FALSE(inside(at, {31, 15}));  // in the second box's wall
}

TEST_CASE("a cube's cavity gets a hipped roof that holds itself up", "[hollow]") {
    const double h = 0.05, d = h;  // a 45° lean: d is one layer height
    const LayerStack ls = sliceModel(makeBox({0, 0, 0}, {20, 20, 20}), h);
    const HollowResult r = hollowInterior(ls, HollowOptions{});
    REQUIRE(r.cavities.size() == 1);
    // Each ceiling layer within d of the solid under it (to the micrometre
    // the roof's outlines are simplified by).
    CHECK(hanging(r.interior, d + 0.002) < 1e-3);
    const int top = r.cavities[0].last_layer;
    CHECK(top == 359);
    // The ridge: a speck in the middle, 2d across (to within the few µm
    // that count as nothing).
    CHECK(area(r.interior[top]) <= std::pow(2 * d + 0.005, 2));
    CHECK(inside(r.interior[top], {10, 10}));
    // The roof widens by d each side a layer going down: 2 (k + 1) d across
    // k layers below the top, until it meets the walls 8 mm down.
    CHECK(area(r.interior[top - 80]) == Approx(std::pow(2 * 81 * d, 2)).margin(1.0));
    CHECK(area(r.interior[top - 165]) == Approx(area(r.interior[100])).margin(0.01));  // whole again
    // A 16 mm cube of cavity whose top 8 mm is a pyramid.
    CHECK(r.volume == Approx(16 * 16 * 8 + 16 * 16 * 8 / 3.0).margin(15));
}

TEST_CASE("under two towers the ceiling rises to a ridge and the cavity stays whole", "[hollow]") {
    TriangleMesh m = makeBox({0, 0, 0}, {40, 12, 12});
    m.append(makeBox({0, 0, 12}, {12, 12, 30}));
    m.append(makeBox({28, 0, 12}, {40, 12, 30}));
    const double h = 0.1;
    const LayerStack ls = sliceModel(m, h);
    const HollowResult r = hollowInterior(ls, HollowOptions{});
    REQUIRE(r.cavities.size() == 1);  // the towers' cavities open into the base's
    CHECK(hanging(r.interior, 1.25 * h, 0.5) < 1e-6);  // nothing hangs wider than 0.5 mm
    CHECK(hanging(r.interior, 1.25 * h) < 0.02);         // and what is narrower is a speck
    // Under the base's 2 mm top skin, between the towers, the ceiling is a
    // ridge half-way between the front and back walls...
    const Paths& under = r.interior[ls.layerAt(9.95)];
    CHECK(inside(under, {20, 6}));
    CHECK_FALSE(inside(under, {20, 4}));
    CHECK_FALSE(inside(under, {20, 8}));
    CHECK(inside(under, {6, 6}));  // under a tower the cavity carries on up
    // ... and 4 mm lower (half the 8 mm between those walls, at 45°) the
    // base is hollow from wall to wall.
    CHECK(inside(r.interior[ls.layerAt(5.5)], {20, 2.5}));
    CHECK(inside(r.interior[ls.layerAt(20)], {6, 6}));
}

TEST_CASE("the floor inside a tube standing on a hollow base goes straight down", "[hollow]") {
    TriangleMesh m = makeBox({-15, -15, 0}, {15, 15, 10});  // the base
    m.append(makeBox({-15, -15, 10}, {-9, 15, 30}));         // a square tube on it, walls 6 mm thick
    m.append(makeBox({9, -15, 10}, {15, 15, 30}));
    m.append(makeBox({-9, -15, 10}, {9, -9, 30}));
    m.append(makeBox({-9, 9, 10}, {9, 15, 30}));
    const double h = 0.1;
    const LayerStack ls = sliceModel(m, h);
    const HollowResult r = hollowInterior(ls, HollowOptions{});
    REQUIRE(r.cavities.size() == 1);
    CHECK(hanging(r.interior, 1.25 * h, 0.5) < 1e-6);
    CHECK(hanging(r.interior, 1.25 * h) < 0.02);
    // The base's top skin inside the tube has nothing around it to hang
    // from, so it stands on solid down to the cavity's floor; under the
    // tube's walls the base is hollow.
    for (double z : {7.5, 5.0, 2.5}) {
        const Paths& at = r.interior[ls.layerAt(z)];
        CHECK_FALSE(inside(at, {0, 0}));
        CHECK_FALSE(inside(at, {9, 0}));
        CHECK(inside(at, {12, 0}));
    }
    CHECK(inside(r.interior[ls.layerAt(20)], {12, 0}));  // the tube's wall
}

TEST_CASE("cavities too small for a drain hole of their own stay solid", "[hollow]") {
    TriangleMesh m = makeBox({0, 0, 0}, {20, 20, 20});
    m.append(makeBox({30, 0, 0}, {36, 6, 6}));  // its cavity: 2 × 2 × 2 mm
    const LayerStack ls = sliceModel(m, 0.1);
    HollowOptions o = flatCeilings();
    o.opening = 0;
    CHECK(hollowInterior(ls, o).cavities.size() == 1);
    o.min_volume = 5;
    const HollowResult both = hollowInterior(ls, o);
    REQUIRE(both.cavities.size() == 2);
    CHECK(both.cavities[0].volume + both.cavities[1].volume == Approx(16 * 16 * 16 + 8).epsilon(0.005));
    CHECK(both.volume == Approx(16 * 16 * 16 + 8).epsilon(0.005));
}

TEST_CASE("hollowing reports progress, can be cancelled and checks its options", "[hollow]") {
    const LayerStack ls = sliceModel(makeBox({0, 0, 0}, {20, 20, 20}), 0.1);
    std::mutex mu;
    std::vector<std::pair<std::string, double>> seen;
    hollowInterior(ls, HollowOptions{}, JobControl([&](std::string_view stage, double f) {
                       std::lock_guard<std::mutex> lock(mu);
                       seen.emplace_back(std::string(stage), f);
                   }));
    REQUIRE(!seen.empty());
    CHECK(seen.back().second == Approx(1));
    for (size_t i = 0; i < seen.size(); ++i) {
        CHECK(seen[i].first == "hollow");
        if (i > 0) CHECK(seen[i].second >= seen[i - 1].second);
    }

    JobControl stop;
    stop.cancel();
    CHECK_THROWS_AS(hollowInterior(ls, HollowOptions{}, stop), Cancelled);
    auto holder = std::make_shared<JobControl>();
    *holder = JobControl([w = std::weak_ptr<JobControl>(holder)](std::string_view, double f) {
        if (f > 0.3)
            if (auto c = w.lock()) c->cancel();
    });
    CHECK_THROWS_AS(hollowInterior(ls, HollowOptions{}, *holder), Cancelled);

    HollowOptions bad;
    bad.wall = 0;
    CHECK_THROWS_AS(hollowInterior(ls, bad), std::invalid_argument);
    bad = HollowOptions{};
    bad.roof_lean = 0;
    CHECK_THROWS_AS(hollowInterior(ls, bad), std::invalid_argument);
}

