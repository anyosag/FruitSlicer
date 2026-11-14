// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "layers/HollowAnalysis.h"
#include "layers/IslandGraph.h"
#include "mesh/Primitives.h"

using namespace fruit;
using Catch::Approx;

TEST_CASE("a T-shape has one island (its foot); the overhang is connected", "[islands]") {
    TriangleMesh t = makeBox({-1, -1, 1}, {1, 1, 5});   // stem
    t.append(makeBox({-6, -1, 5}, {6, 1, 6}));          // cross bar resting on the stem
    const LayerStack ls = sliceModel(t, 0.1);
    const IslandGraph g(ls, 0.0);
    const auto isl = g.islands();
    REQUIRE(isl.size() == 1);
    CHECK(g.part(isl[0]).layer == 10);  // first layer of the stem (z 1.0..1.1)
}

TEST_CASE("a floating block is its own island; mushroom cap is not", "[islands]") {
    TriangleMesh m = makeBox({0, 0, 1}, {2, 2, 3});
    m.append(makeBox({5, 0, 2}, {7, 2, 3}));  // floats: nothing below it
    // Mushroom: thin stem with a wide cap above
    m.append(makeCylinder({20, 0, 1}, 0.5, 3, 32));
    m.append(makeCylinder({20, 0, 4}, 3, 1, 64));
    const LayerStack ls = sliceModel(m, 0.1);
    const IslandGraph g(ls, 0.0);
    const auto isl = g.islands();
    REQUIRE(isl.size() == 3);  // box foot, floating block, mushroom stem foot
    int at_z2 = 0;
    for (int id : isl) at_z2 += g.part(id).layer == 20;
    CHECK(at_z2 == 1);
}

TEST_CASE("growing the parent links thin shallow wires instead of spawning islands", "[islands]") {
    // A printable 0.1 mm wire at 20° with 0.05 mm layers already overlaps
    // layer to layer; linking by growth matters only for features thinner
    // than about one layer height (or thick layers).
    {
        TriangleMesh w;
        appendFrustum(w, {0, 0, 1}, 0.05, {10 * std::cos(deg2rad(20)), 0, 1 + 10 * std::sin(deg2rad(20))}, 0.05, 12);
        CHECK(IslandGraph(sliceModel(w, 0.05), 0.0).islands().size() == 1);
    }
    // A wire of diameter d at angle θ from horizontal cuts each layer as a
    // sliver d/sin θ long that moves h/tan θ per layer, so slices stop
    // overlapping once d < h·cos θ. 0.06 mm at 20° with 0.1 mm layers:
    // sliver 0.175 mm, shift 0.275 mm -> gaps of 0.1 mm between layers.
    TriangleMesh wire;
    const Vec3 a{0, 0, 1}, b{10 * std::cos(deg2rad(20)), 0, 1 + 10 * std::sin(deg2rad(20))};
    appendFrustum(wire, a, 0.03, b, 0.03, 12);
    const LayerStack ls = sliceModel(wire, 0.1);
    const IslandGraph strict(ls, 0.0);
    const IslandGraph grown(ls, 0.12);
    CHECK(strict.islands().size() > 5);
    CHECK(grown.islands().size() == 1);
}

namespace {
// Boxes, a floating block, a mushroom, two posts joined by a bar (a part on
// two parents) and a hollow ball: islands, merges and holes in the layers.
TriangleMesh mixedModel() {
    TriangleMesh m = makeBox({0, 0, 0}, {2, 2, 3});
    m.append(makeBox({5, 0, 2}, {7, 2, 3}));
    m.append(makeCylinder({20, 0, 1}, 0.5, 3, 32));
    m.append(makeCylinder({20, 0, 4}, 3, 1, 64));
    m.append(makeBox({30, 0, 0}, {31, 1, 4}));
    m.append(makeBox({35, 0, 0}, {36, 1, 4}));
    m.append(makeBox({30, 0, 4}, {36, 1, 5}));
    TriangleMesh ball = makeSphere({45, 0, 6}, 5, 48);
    TriangleMesh inner = makeSphere({45, 0, 6}, 4, 48);
    for (auto& f : inner.faces) std::swap(f[1], f[2]);  // facing in: a closed hollow
    ball.append(inner);
    m.append(ball);
    return m;
}
}  // namespace

TEST_CASE("the island graph is the same on one thread or many", "[islands]") {
    const LayerStack ls = sliceModel(mixedModel(), 0.05);
    const IslandGraph one(ls, 0.01, 1), many(ls, 0.01, 8);
    REQUIRE(one.layerCount() == many.layerCount());
    REQUIRE(one.parts().size() == many.parts().size());
    std::size_t two_parents = 0;
    for (std::size_t i = 0; i < one.parts().size(); ++i) {
        const LayerPart &a = one.part(static_cast<int>(i)), &b = many.part(static_cast<int>(i));
        CHECK(a.layer == b.layer);
        CHECK(a.area == b.area);
        CHECK(a.shape.contour == b.shape.contour);
        CHECK(a.shape.holes == b.shape.holes);
        CHECK(a.parents == b.parents);
        CHECK(a.children == b.children);
        two_parents += a.parents.size() > 1;
    }
    for (std::size_t L = 0; L < one.layerCount(); ++L) CHECK(one.partsInLayer(L) == many.partsInLayer(L));
    CHECK(one.islands() == many.islands());
    CHECK(two_parents > 0);           // the bar on the two posts
    CHECK(one.islands().size() >= 5);  // the foot of each piece, the floating block, the ball
}

TEST_CASE("the hollows found are the same on one thread or many", "[islands][hollow]") {
    TriangleMesh m = mixedModel();  // its ball is a closed hollow: a resin trap
    // A square cup open at the top: a suction cup when printed.
    m.append(makeBox({60, 0, 0}, {70, 10, 1}));
    m.append(makeBox({60, 0, 1}, {61, 10, 6}));
    m.append(makeBox({69, 0, 1}, {70, 10, 6}));
    m.append(makeBox({60, 0, 1}, {70, 1, 6}));
    m.append(makeBox({60, 9, 1}, {70, 10, 6}));
    const LayerStack ls = sliceModel(m, 0.05);
    const HollowReport one = analyzeHollows(ls, 0.5, 1.0, 1), many = analyzeHollows(ls, 0.5, 1.0, 8);
    REQUIRE(one.components.size() == many.components.size());
    std::size_t traps = 0, cups = 0;
    for (std::size_t i = 0; i < one.components.size(); ++i) {
        const HollowComponent &a = one.components[i], &b = many.components[i];
        CHECK(a.kind == b.kind);
        CHECK(a.first_layer == b.first_layer);
        CHECK(a.last_layer == b.last_layer);
        CHECK(a.volume == b.volume);
        CHECK(a.max_area == b.max_area);
        CHECK(a.center.x == b.center.x);
        CHECK(a.center.y == b.center.y);
        traps += a.kind == HollowKind::ResinTrap;
        cups += a.kind == HollowKind::SuctionCup;
    }
    CHECK(one.trap_regions == many.trap_regions);
    CHECK(one.cup_regions == many.cup_regions);
    CHECK(traps >= 1);
    CHECK(cups >= 1);
}
