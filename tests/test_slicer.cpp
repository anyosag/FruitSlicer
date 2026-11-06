// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "calibration/Coupons.h"
#include "layers/LayerStack.h"
#include "mesh/Primitives.h"
#include "slice/MeshSlicer.h"

using namespace fruit;
using Catch::Approx;

static double ngonArea(double r, int n) { return 0.5 * n * r * r * std::sin(2 * kPi / n); }

TEST_CASE("box and cylinder slices match analytic areas", "[slice]") {
    const TriangleMesh box = makeBox({-5, -3, 0}, {5, 3, 2});
    const Paths s = sliceMeshAt(box, 1.0);
    REQUIRE(s.size() == 1);
    CHECK(area(s) == Approx(60));

    const int n = 72;
    const TriangleMesh cyl = makeCylinder({1, 1, 0}, 3, 4, n);
    CHECK(area(sliceMeshAt(cyl, 2.0)) == Approx(ngonArea(3, n)).margin(2 * kPi * 3 * 1e-4));
    // Slicing outside the mesh gives nothing.
    CHECK(sliceMeshAt(cyl, 5.0).empty());
}

TEST_CASE("sphere slice area within 0.5% of the analytic circle", "[slice]") {
    const double r = 5;
    const TriangleMesh sph = makeSphere({0, 0, r}, r, 256);
    for (double z : {1.0, 2.5, 5.0, 8.0}) {
        const double rr = std::sqrt(r * r - (z - r) * (z - r));
        CHECK(area(sliceMeshAt(sph, z)) == Approx(kPi * rr * rr).epsilon(0.005));
    }
}

TEST_CASE("overlapping shells union; embedded shells do not become voids", "[slice]") {
    TriangleMesh m = makeBox({0, 0, 0}, {10, 10, 2});
    m.append(makeBox({5, 0, 0}, {15, 10, 2}));      // overlaps half
    m.append(makeBox({2, 2, 0.5}, {4, 4, 1.5}));    // fully embedded
    CHECK(area(sliceMeshAt(m, 1.0)) == Approx(150));
}

TEST_CASE("hollow shell (inner inverted box) slices as a ring", "[slice]") {
    TriangleMesh outer = makeBox({0, 0, 0}, {10, 10, 10});
    TriangleMesh inner = makeBox({2, 2, 2}, {8, 8, 8});
    for (auto& f : inner.faces) std::swap(f[1], f[2]);  // cavity: normals point into the void
    outer.append(inner);
    const Paths s = sliceMeshAt(outer, 5.0);
    CHECK(area(s) == Approx(100 - 36));
    CHECK(s.size() == 2);
}

TEST_CASE("layer stack samples mid-layer heights and slices in parallel consistently", "[slice]") {
    const TriangleMesh cone = makeCone({0, 0, 0}, 5, {0, 0, 10}, 64);
    const LayerStack ls = sliceModel(cone, 0.5);
    REQUIRE(ls.size() == 20);
    CHECK(ls.sample_z[0] == Approx(0.25));
    for (size_t i = 0; i < ls.size(); ++i) {
        const double z = ls.sample_z[i];
        const double rr = 5 * (1 - z / 10);
        // Coordinates are rounded to 0.1 µm, so allow perimeter × 0.1 µm.
        CHECK(area(ls.slices[i]) == Approx(ngonArea(rr, 64)).margin(2 * kPi * rr * 1e-4));
        // Same result as slicing the single height.
        CHECK(area(sliceMeshAt(cone, z)) == Approx(area(ls.slices[i])));
    }
    CHECK(ls.layerAt(0.74) == 1);
}

TEST_CASE("overlap coupon reference slices are solid", "[slice][coupon]") {
    const Coupon c = overlapCoupon();
    // At the tip penetration height the plate must be a single solid square.
    const Paths tips = sliceMeshAt(c.mesh, 5.025);
    CHECK(area(tips) == Approx(400).epsilon(1e-6));
    CHECK(toExPolygons(tips).size() == 1);
    // Embedded box height: still a full 20×20 plate, no pocket.
    const Paths mid = sliceMeshAt(c.mesh, 6.0);
    CHECK(area(mid) == Approx(400).epsilon(1e-6));
    for (const auto& e : toExPolygons(mid)) CHECK(e.holes.empty());
    // Cross above the plate: two 16×2 bars overlapping in a 2×2 square.
    CHECK(area(sliceMeshAt(c.mesh, 8.0)) == Approx(2 * 32 - 4).epsilon(1e-6));
    // Below the plate: 9 shafts of Ø1 mm.
    const Paths shafts = sliceMeshAt(c.mesh, 2.5);
    CHECK(toExPolygons(shafts).size() == 9);
}
