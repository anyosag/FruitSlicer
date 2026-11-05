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
