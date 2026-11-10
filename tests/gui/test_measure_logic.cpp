// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Measure: two clicks, the distance and its parts, the picking, and that it
// changes nothing in the job.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "Document.h"
#include "Measure.h"
#include "mesh/AabbTree.h"
#include "mesh/Primitives.h"

using Catch::Approx;
using namespace gui;

TEST_CASE("Measure: the distance and its X, Y and Z parts", "[gui][measure]") {
    const Measurement m = measureBetween({1, 2, 3}, {4, 6, 3});
    CHECK(m.distance == Approx(5));
    CHECK(m.dx == Approx(3));
    CHECK(m.dy == Approx(4));
    CHECK(m.dz == Approx(0));
    // The order of the points doesn't matter: the parts are never negative.
    const Measurement r = measureBetween({4, 6, 3}, {1, 2, 3});
    CHECK(r.distance == Approx(5));
    CHECK(r.dx == Approx(3));
    CHECK(r.dy == Approx(4));
    CHECK(measureBetween({1, 1, 1}, {1, 1, 1}).distance == 0);
}

TEST_CASE("Measure: a third click starts again; clear empties it", "[gui][measure]") {
    MeasureState s;
    CHECK(s.empty());
    s.click({1, 0, 0});
    CHECK_FALSE(s.complete());
    s.click({2, 0, 0});
    CHECK(s.complete());
    CHECK(s.points.size() == 2);
    s.click({9, 9, 9});
    CHECK(s.points.size() == 1);
    CHECK(s.points[0].x == 9);
    CHECK_FALSE(s.complete());
    s.clear();
    CHECK(s.empty());
}

TEST_CASE("Measure: the points follow the model when it is moved, turned or scaled", "[gui][measure]") {
    fruit::TriangleMesh box = fruit::makeBox({0, 0, 0}, {10, 20, 30});
    fruit::Placement p;
    const fruit::Transform t0 = fruit::placementTransform(box, p);
    MeasureState s;
    s.click({0, 0, 0});
    s.click({10, 0, 0});  // along the model's X
    CHECK(measurementOf(s, t0)->distance == Approx(10));
    p.scale = 2;
    CHECK(measurementOf(s, fruit::placementTransform(box, p))->distance == Approx(20));
    p.scale = 1;
    p.rotation_deg = {0, 0, 90};  // along the plate's Y now
    const auto turned = measurementOf(s, fruit::placementTransform(box, p));
    CHECK(turned->distance == Approx(10));
    CHECK(turned->dx == Approx(0).margin(1e-9));
    CHECK(turned->dy == Approx(10));
    p.offset = {30, 5};
    CHECK(measurementOf(s, fruit::placementTransform(box, p))->distance == Approx(10));
    MeasureState one;
    one.click({1, 1, 1});
    CHECK_FALSE(measurementOf(one, t0).has_value());
    CHECK(pointsOnPlate(one, t0).size() == 1);
}

TEST_CASE("Measure: picking finds the surface under the ray, with its outward normal", "[gui][measure]") {
    const fruit::TriangleMesh box = fruit::makeBox({0, 0, 0}, {10, 10, 10});
    const fruit::AabbTree tree(box);
    const auto top = pickSurface(tree, {5, 5, 50}, {0, 0, -1});
    REQUIRE(top);
    CHECK(top->point.z == Approx(10));
    CHECK(top->normal.z == Approx(1));
    const auto side = pickSurface(tree, {-20, 5, 5}, {1, 0, 0});
    REQUIRE(side);
    CHECK(side->point.x == Approx(0));
    CHECK(side->normal.x == Approx(-1));
    CHECK_FALSE(pickSurface(tree, {50, 50, 50}, {0, 0, -1}));  // misses
    CHECK_FALSE(pickSurface(fruit::AabbTree{}, {5, 5, 50}, {0, 0, -1}));
}

TEST_CASE("Measure: a click on the view picks the surface point the camera sees there", "[gui][measure]") {
    const fruit::TriangleMesh box = fruit::makeBox({-10, -10, 0}, {10, 10, 20});
    const fruit::AabbTree tree(box);
    Camera cam;
    cam.preset(ViewPreset::Angled);
    cam.frame({0, 0, 10}, 30);
    const float left = 100, top = 50, w = 800, h = 600;
    const M4 vp = cam.proj(w / h) * cam.view();
    // Where three points on the box's surface are on screen, and back.
    const fruit::Vec3 wanted[] = {{0, 0, 20}, {5, -10, 8}, {10, 4, 15}};
    for (const fruit::Vec3& p : wanted) {
        float c[4];
        const float in[4] = {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z), 1};
        for (int r = 0; r < 4; ++r) c[r] = vp.at(r, 0) * in[0] + vp.at(r, 1) * in[1] + vp.at(r, 2) * in[2] + vp.at(r, 3) * in[3];
        const float px = left + (c[0] / c[3] * 0.5f + 0.5f) * w, py = top + (0.5f - c[1] / c[3] * 0.5f) * h;
        fruit::Vec3 o, d;
        rayThroughPixel(cam, px, py, left, top, w, h, &o, &d);
        const auto hit = pickSurface(tree, o, d);
        REQUIRE(hit);
        // The nearest surface along that ray is the point itself (it faces the camera).
        CHECK(hit->point.x == Approx(p.x).margin(0.05));
        CHECK(hit->point.y == Approx(p.y).margin(0.05));
        CHECK(hit->point.z == Approx(p.z).margin(0.05));
    }
    // A pixel in a corner of the view sees nothing of the box.
    fruit::Vec3 o, d;
    rayThroughPixel(cam, left + 2, top + 2, left, top, w, h, &o, &d);
    CHECK_FALSE(pickSurface(tree, o, d));
}
