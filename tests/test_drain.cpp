// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Drain holes: where they run, what they cut, and which cavities then drain.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "layers/Drain.h"
#include "mesh/Placement.h"
#include "mesh/Primitives.h"

using namespace fruit;
using Catch::Approx;

namespace {

bool inside(const Paths& p, Vec2 q) {
    for (const ExPolygon& e : toExPolygons(p))
        if (e.contains(q)) return true;
    return false;
}

HollowOptions flatCeilings() {
    HollowOptions o;
    o.roof_lean = 90;
    return o;
}

}  // namespace

TEST_CASE("a hole's footprint in a layer: a disc, a strip, or nothing", "[drain]") {
    const double r = 1.5;
    const Path disc = holeFootprint({0, 0, -1}, {0, 0, 5}, r, 1.0, 1.05);  // straight up
    REQUIRE(!disc.empty());
    CHECK(area(disc) >= kPi * r * r);  // never narrower than the hole
    CHECK(area(disc) == Approx(kPi * r * r).epsilon(0.005));
    for (int k = 0; k < 12; ++k) {
        const double a = 2 * kPi * k / 12;
        CHECK(inside({disc}, {0.999 * r * std::cos(a), 0.999 * r * std::sin(a)}));
    }
    // Level, through the layer at its axis: a strip along it, as wide as the hole.
    const Path strip = holeFootprint({0, 0, 1}, {10, 0, 1}, 1.0, 0.975, 1.025);
    REQUIRE(!strip.empty());
    const BBox2 b = bbox(strip);
    CHECK(b.min.x == Approx(0).margin(1e-3));
    CHECK(b.max.x == Approx(10).margin(1e-3));
    CHECK(b.size().y == Approx(2.0).margin(0.02));
    // A layer it doesn't reach.
    CHECK(holeFootprint({0, 0, 1}, {10, 0, 1}, 1.0, 3.0, 3.05).empty());
    CHECK(holeFootprint({0, 0, -1}, {0, 0, 5}, r, 6.0, 6.05).empty());
}

TEST_CASE("a hollow cube gets a hole straight up through the middle of its floor", "[drain]") {
    const TriangleMesh cube = makeBox({0, 0, 0}, {20, 20, 20});
    const LayerStack ls = sliceModel(cube, 0.05);
    const HollowResult hollow = hollowInterior(ls, flatCeilings());
    REQUIRE(hollow.cavities.size() == 1);
    const Transform placed = Transform::identity();

    const std::vector<DrainHole> holes = suggestDrainHoles(cube, placed, ls, hollow, 3.0);
    REQUIRE(holes.size() == 1);
    CHECK(holes[0].at.x == Approx(10).margin(0.05));
    CHECK(holes[0].at.y == Approx(10).margin(0.05));
    CHECK(holes[0].at.z == Approx(0).margin(1e-6));
    CHECK(holes[0].axis.z == Approx(1));
    CHECK(holes[0].diameter == 3.0);

    const std::vector<PlacedHole> drilled = placeHoles(holes, placed, ls, hollow);
    REQUIRE(drilled.size() == 1);
    CHECK(drilled[0].cavity == 0);
    CHECK(drilled[0].from.z < 0);                          // from outside the floor
    CHECK(drilled[0].to.z == Approx(2.0 + 3.0).margin(0.06));  // through it, one diameter into the cavity
    const LayerStack d = drilledLayers(ls, hollow, drilled);
    CHECK_FALSE(inside(d.slices[10], {10, 10}));  // the floor has a hole...
    CHECK(inside(d.slices[10], {10, 12}));         // ...3 mm across
    CHECK(area(d.slices[10]) == Approx(400 - kPi * 1.5 * 1.5).margin(0.05));
    CHECK(area(d.slices[390]) == Approx(400).margin(1e-6));  // the ceiling isn't touched

    CHECK(cavitiesDrain(hollowedLayers(ls, hollow), hollow) == std::vector<char>{0});
    CHECK(cavitiesDrain(d, hollow) == std::vector<char>{1});
}

TEST_CASE("a hole keeps to the model when it is turned, and may then not drain", "[drain]") {
    const TriangleMesh model = makeBox({0, 0, 0}, {20, 20, 20});
    const LayerStack first = sliceModel(model, 0.1);
    const HollowResult hollow_first = hollowInterior(first, flatCeilings());
    const std::vector<DrainHole> holes = suggestDrainHoles(model, Transform::identity(), first, hollow_first, 3.0);
    REQUIRE(holes.size() == 1);

    // Turned onto its side: the hole now enters halfway up a wall, level.
    Placement p;
    p.rotation_deg = {90, 0, 0};
    p.on_plate = true;
    const Transform turned = placementTransform(model, p);
    const TriangleMesh placed = placeModel(model, p);
    const LayerStack ls = sliceModel(placed, 0.1);
    const HollowResult hollow = hollowInterior(ls, flatCeilings());
    const std::vector<PlacedHole> drilled = placeHoles(holes, turned, ls, hollow);
    REQUIRE(drilled.size() == 1);
    CHECK(drilled[0].cavity == 0);
    CHECK(std::abs(drilled[0].to.z - drilled[0].from.z) < 1e-6);  // level
    CHECK(drilled[0].to.z == Approx(10).margin(0.01));
    // Resin under it stays put: the cavity doesn't drain.
    CHECK(cavitiesDrain(drilledLayers(ls, hollow, drilled), hollow) == std::vector<char>{0});
    // A hole suggested for the new way round does.
    const std::vector<DrainHole> again = suggestDrainHoles(placed, turned, ls, hollow, 3.0);
    REQUIRE(again.size() == 1);
    const LayerStack d = drilledLayers(ls, hollow, placeHoles(again, turned, ls, hollow));
    CHECK(cavitiesDrain(d, hollow) == std::vector<char>{1});
}

TEST_CASE("a hole that reaches no cavity isn't drilled", "[drain]") {
    TriangleMesh m = makeBox({0, 0, 0}, {20, 20, 20});
    m.append(makeBox({30, 0, 0}, {33, 20, 20}));  // a slab too thin to hollow
    const LayerStack ls = sliceModel(m, 0.1);
    const HollowResult hollow = hollowInterior(ls, flatCeilings());
    REQUIRE(hollow.cavities.size() == 1);
    const std::vector<DrainHole> holes{
        {{10, 10, 20}, {0, 0, 1}, 3.0},   // on the top, pointing out of the model
        {{31.5, 10, 0}, {0, 0, 1}, 3.0},  // up through the solid slab and out of its top
        {{30, 10, 10}, {-1, 0, 0}, 3.0},  // across the gap: out of the slab, never into the cube
    };
    const std::vector<PlacedHole> drilled = placeHoles(holes, Transform::identity(), ls, hollow);
    REQUIRE(drilled.size() == 3);
    for (const PlacedHole& p : drilled) CHECK(p.cavity == -1);
    const LayerStack d = drilledLayers(ls, hollow, drilled);
    const LayerStack h = hollowedLayers(ls, hollow);
    for (size_t i = 0; i < d.size(); i += 37) CHECK(area(d.slices[i]) == Approx(area(h.slices[i])));
}

TEST_CASE("a hole into a shallow cavity stops short of its ceiling", "[drain]") {
    // 7 mm tall: a cavity 3 mm deep between a 2 mm floor and a 2 mm roof.
    const TriangleMesh m = makeBox({0, 0, 0}, {20, 20, 7});
    const LayerStack ls = sliceModel(m, 0.05);
    const HollowResult hollow = hollowInterior(ls, flatCeilings());
    REQUIRE(hollow.cavities.size() == 1);
    const std::vector<PlacedHole> drilled =
        placeHoles({{{10, 10, 0}, {0, 0, 1}, 3.0}}, Transform::identity(), ls, hollow);
    REQUIRE(drilled[0].cavity == 0);
    CHECK(drilled[0].to.z == Approx(2.0 + 1.5).margin(0.06));  // half the cavity's depth
    const LayerStack d = drilledLayers(ls, hollow, drilled);
    CHECK(inside(d.slices[ls.layerAt(5.5)], {10, 10}));  // the roof is whole
    CHECK(cavitiesDrain(d, hollow) == std::vector<char>{1});
}

TEST_CASE("each cavity gets a hole of its own", "[drain]") {
    TriangleMesh m = makeBox({0, 0, 0}, {20, 20, 20});
    m.append(makeBox({30, 0, 5}, {50, 20, 25}));  // a second cube, floating 5 mm up
    const LayerStack ls = sliceModel(m, 0.1);
    const HollowResult hollow = hollowInterior(ls, HollowOptions{});
    REQUIRE(hollow.cavities.size() == 2);
    const std::vector<DrainHole> holes = suggestDrainHoles(m, Transform::identity(), ls, hollow, 2.5);
    REQUIRE(holes.size() == 2);
    const std::vector<PlacedHole> drilled = placeHoles(holes, Transform::identity(), ls, hollow);
    CHECK(((drilled[0].cavity == 0 && drilled[1].cavity == 1) || (drilled[0].cavity == 1 && drilled[1].cavity == 0)));
    for (const PlacedHole& p : drilled) CHECK(p.radius == 1.25);
    CHECK(cavitiesDrain(drilledLayers(ls, hollow, drilled), hollow) == std::vector<char>{1, 1});
    // One of the two alone leaves the other undrained.
    const std::vector<char> one = cavitiesDrain(drilledLayers(ls, hollow, {drilled[0]}), hollow);
    CHECK(one[drilled[0].cavity] == 1);
    CHECK(one[drilled[1].cavity] == 0);
}

TEST_CASE("the cavity a point lies in", "[drain]") {
    TriangleMesh m = makeBox({0, 0, 0}, {20, 20, 20});
    m.append(makeBox({30, 0, 0}, {50, 20, 20}));
    const LayerStack ls = sliceModel(m, 0.1);
    const HollowResult hollow = hollowInterior(ls, flatCeilings());
    REQUIRE(hollow.cavities.size() == 2);
    const size_t j = ls.layerAt(10);
    const int a = hollow.cavityAt(j, {10, 10}), b = hollow.cavityAt(j, {40, 10});
    CHECK(a >= 0);
    CHECK(b >= 0);
    CHECK(a != b);
    CHECK(hollow.cavityAt(j, {1, 10}) == -1);    // in the wall
    CHECK(hollow.cavityAt(j, {25, 10}) == -1);   // between the cubes
    CHECK(hollow.cavityAt(ls.layerAt(1), {10, 10}) == -1);  // in the floor
}
