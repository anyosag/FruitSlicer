// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "geometry/Polygon.h"
#include "geometry/Transform.h"

using namespace fruit;
using Catch::Approx;

static Path square(double x0, double y0, double s, bool ccw = true) {
    Path p{toPoint({x0, y0}), toPoint({x0 + s, y0}), toPoint({x0 + s, y0 + s}), toPoint({x0, y0 + s})};
    if (!ccw) std::reverse(p.begin(), p.end());
    return p;
}

TEST_CASE("rotation composes X then Y then Z", "[geometry]") {
    const Transform r = Transform::rotationXYZ(90, 0, 0);
    const Vec3 v = r.apply({0, 1, 0});
    CHECK(v.x == Approx(0).margin(1e-12));
    CHECK(v.y == Approx(0).margin(1e-12));
    CHECK(v.z == Approx(1));
    const Transform rz = Transform::rotationXYZ(0, 0, 90);
    const Vec3 w = rz.apply({1, 0, 0});
    CHECK(w.y == Approx(1));
    CHECK(r.determinant() == Approx(1));
    // X=90 then Z=90: (0,1,0) -> (0,0,1) -> (0,0,1)
    const Vec3 u = Transform::rotationXYZ(90, 0, 90).apply({0, 1, 0});
    CHECK(u.z == Approx(1));
}

TEST_CASE("polygon area and offset", "[geometry]") {
    const Paths sq{square(0, 0, 10)};
    CHECK(area(sq) == Approx(100));
    // Rounded offset: s² + 4·s·d + π·d²
    const Paths grown = offset(sq, 1.0);
    CHECK(area(grown) == Approx(100 + 40 + kPi).epsilon(1e-4));
    const Paths shrunk = offset(sq, -1.0);
    CHECK(area(shrunk) == Approx(64).epsilon(1e-6));
}

TEST_CASE("NonZero union merges overlapping same-orientation shells", "[geometry]") {
    const Paths two{square(0, 0, 10), square(5, 0, 10)};
    CHECK(area(unionNonZero(two)) == Approx(150));
    // A clockwise inner square is a hole under NonZero.
    const Paths holed{square(0, 0, 10), square(2, 2, 6, false)};
    CHECK(area(unionNonZero(holed)) == Approx(100 - 36));
}

TEST_CASE("ExPolygons group holes and islands-in-holes", "[geometry]") {
    const Paths p{square(0, 0, 10), square(2, 2, 6, false), square(4, 4, 2)};
    const ExPolygons ex = toExPolygons(p);
    REQUIRE(ex.size() == 2);
    double total = 0;
    int with_hole = 0;
    for (const auto& e : ex) {
        total += e.area();
        with_hole += e.holes.size() == 1;
        CHECK(area(e.contour) > 0);
        for (const auto& h : e.holes) CHECK(area(h) < 0);
    }
    CHECK(with_hole == 1);
    CHECK(total == Approx(100 - 36 + 4));
    const auto& outer = ex[0].holes.empty() ? ex[1] : ex[0];
    CHECK(outer.contains({1, 1}));
    CHECK_FALSE(outer.contains({3, 3}));  // in the hole
}

TEST_CASE("transform: the inverse undoes rotation, scale and translation", "[geometry]") {
    Transform t = Transform::rotationXYZ(10, 25, -40);
    for (auto& row : t.m)
        for (double& v : row) v *= 1.7;
    t.t = {3, -4, 12};
    const Transform inv = t.inverse();
    for (const Vec3& p : {Vec3{0, 0, 0}, Vec3{1, 2, 3}, Vec3{-5, 7.5, 0.25}}) {
        const Vec3 back = inv.apply(t.apply(p));
        CHECK(back.x == Approx(p.x).margin(1e-12));
        CHECK(back.y == Approx(p.y).margin(1e-12));
        CHECK(back.z == Approx(p.z).margin(1e-12));
    }
    Transform flat;
    flat.m[2][2] = 0;
    CHECK_THROWS_AS(flat.inverse(), std::domain_error);
}

TEST_CASE("where a line runs inside polygons, holes and overlaps included", "[geometry]") {
    // A 10 mm square with a 4 mm hole, and a second square overlapping it.
    Paths p = {Path{toPoint({0, 0}), toPoint({10, 0}), toPoint({10, 10}), toPoint({0, 10})},
               Path{toPoint({3, 3}), toPoint({3, 7}), toPoint({7, 7}), toPoint({7, 3})}};  // the hole, clockwise
    auto spans = lineSpans(p, {-5, 5}, {1, 0});
    REQUIRE(spans.size() == 2);
    CHECK(spans[0].first == Approx(5));  // 5 mm from (-5, 5) to x = 0
    CHECK(spans[0].second == Approx(8));
    CHECK(spans[1].first == Approx(12));
    CHECK(spans[1].second == Approx(15));
    CHECK(lineSpans(p, {-5, 1}, {1, 0}).size() == 1);   // under the hole
    CHECK(lineSpans(p, {-5, 11}, {1, 0}).empty());      // above it all
    // Straight down through the middle, from above.
    spans = lineSpans(p, {5, 20}, {0, -1});
    REQUIRE(spans.size() == 2);
    CHECK(spans[0].first == Approx(10));
    CHECK(spans[1].second == Approx(20));
    // Overlapping, same way round: one stretch.
    p = {Path{toPoint({0, 0}), toPoint({10, 0}), toPoint({10, 10}), toPoint({0, 10})},
         Path{toPoint({5, 0}), toPoint({15, 0}), toPoint({15, 10}), toPoint({5, 10})}};
    spans = lineSpans(p, {0, 5}, normalized(Vec2{1, 0}));
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].second == Approx(15));
    // At a slant.
    spans = lineSpans({Path{toPoint({0, 0}), toPoint({10, 0}), toPoint({10, 10}), toPoint({0, 10})}}, {0, 0},
                      normalized(Vec2{1, 1}));
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].second - spans[0].first == Approx(10 * std::sqrt(2.0)).margin(1e-6));
}
