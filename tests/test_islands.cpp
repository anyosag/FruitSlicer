#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
