// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "mesh/Primitives.h"
#include "support/OrientationSearch.h"

using namespace fruit;
using Catch::Approx;

TEST_CASE("eulerXYZ inverts rotationXYZ", "[orient]") {
    for (const Vec3 e : {Vec3{10, 20, 30}, Vec3{-75, 40, 170}, Vec3{0, -60, -45}, Vec3{120, 10, 0}}) {
        const Transform a = Transform::rotationXYZ(e.x, e.y, e.z);
        const Vec3 back = eulerXYZ(a);
        const Transform b = Transform::rotationXYZ(back.x, back.y, back.z);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) CHECK(a.m[i][j] == Approx(b.m[i][j]).margin(1e-9));
    }
}

TEST_CASE("orientation search prefers standing a T-bar on its long flat face", "[orient]") {
    // An upside-down T lying the wrong way: its wide flange on top needs a
    // large flat support; turned over, the flange sits at the bottom and the
    // stem's end is the only underside.
    TriangleMesh t = makeBox({-1, -1, 0}, {1, 1, 8});  // stem
    t.append(makeBox({-8, -4, 8}, {8, 4, 9}));         // flange on top (overhang)
    OrientOptions o;
    o.directions = 24;
    o.finalists = 4;
    o.cfg.layer_height = 0.1;
    const auto res = searchOrientations(t, o);
    REQUIRE(res.size() >= 2);
    REQUIRE(res.front().evaluated);
    const OrientationResult* input = nullptr;
    for (const auto& r : res)
        if (r.is_input) input = &r;
    REQUIRE(input != nullptr);
    REQUIRE(input->evaluated);
    CHECK(res.front().points <= input->points);
    CHECK(res.front().score <= input->score);
    // Finalists carry the exact biggest layer from the full slicing. As it
    // came in, the 16 x 8 mm flange is a whole layer (128 mm²); no
    // orientation has a layer bigger than that.
    CHECK(input->peak_layer_area == Approx(128.0).margin(1.0));
    for (const auto& r : res)
        if (r.evaluated) {
            CHECK(r.peak_layer_area > 0.0);
            CHECK(r.peak_layer_area <= 129.0);
        }
}
