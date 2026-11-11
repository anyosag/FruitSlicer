// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The flat faces a model can rest on.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>

#include "mesh/FlatFaces.h"
#include "mesh/Primitives.h"

using namespace fruit;
using Catch::Approx;

TEST_CASE("Flat faces: a box has six, each with its normal, area and centroid", "[flat]") {
    const TriangleMesh box = makeBox({0, 0, 0}, {10, 20, 30});
    const FlatFaces ff = findFlatFaces(box);
    REQUIRE(ff.faces.size() == 6);
    CHECK(ff.faces[0].area == Approx(600));  // 20 x 30, the largest first
    CHECK(ff.faces[5].area == Approx(200));  // 10 x 20
    double total = 0;
    for (const FlatFace& f : ff.faces) {
        total += f.area;
        CHECK(norm(f.normal) == Approx(1));
        CHECK(f.faces.size() == 2);
    }
    CHECK(total == Approx(box.surfaceArea()));
    for (std::size_t t = 0; t < box.faces.size(); ++t) CHECK(ff.regionOf(t) >= 0);
    CHECK(ff.faces[0].centroid.x == Approx(ff.faces[0].normal.x > 0 ? 10 : 0).margin(1e-9));
}

TEST_CASE("Flat faces: a cylinder's two ends come first; its side is one flat panel per segment", "[flat]") {
    const TriangleMesh cyl = makeCylinder({0, 0, 0}, 10, 20, 64);
    const FlatFaces ff = findFlatFaces(cyl);
    REQUIRE(ff.faces.size() == 66);
    for (int i = 0; i < 2; ++i) {
        CHECK(std::abs(ff.faces[static_cast<std::size_t>(i)].normal.z) == Approx(1));
        CHECK(ff.faces[static_cast<std::size_t>(i)].area > 300);
    }
    for (std::size_t i = 2; i < ff.faces.size(); ++i) CHECK(std::abs(ff.faces[i].normal.z) < 1e-6);
}

TEST_CASE("Flat faces: a face with something standing out past its plane is left out", "[flat]") {
    // An L: the foot's top is flat but the upright rises beyond it.
    TriangleMesh l = makeBox({0, 0, 0}, {30, 10, 5});
    l.append(makeBox({0, 0, 5}, {10, 10, 30}));
    const FlatFaces ff = findFlatFaces(l);
    bool top_of_foot = false;
    for (const FlatFace& f : ff.faces)
        if (f.normal.z > 0.99 && f.centroid.z == Approx(5).margin(1e-6)) top_of_foot = true;
    CHECK_FALSE(top_of_foot);
    bool bottom = false;
    for (const FlatFace& f : ff.faces)
        if (f.normal.z < -0.99) bottom = true;
    CHECK(bottom);
    // It is kept with the faces it can't rest on, and its triangles point to it there.
    int other = -1;
    for (std::size_t i = 0; i < ff.others.size(); ++i)
        if (ff.others[i].normal.z > 0.99 && ff.others[i].centroid.z == Approx(5).margin(1e-6)) other = static_cast<int>(i);
    REQUIRE(other >= 0);
    CHECK(ff.others[static_cast<std::size_t>(other)].area == Approx(30 * 10).margin(1e-6));  // the foot box's whole top, under the upright too
    for (const int32_t t : ff.others[static_cast<std::size_t>(other)].faces) {
        CHECK(ff.otherOf(static_cast<std::size_t>(t)) == other);
        CHECK(ff.regionOf(static_cast<std::size_t>(t)) == -1);
    }
}

TEST_CASE("Flat faces: slivers are ignored", "[flat]") {
    const TriangleMesh thin = makeBox({0, 0, 0}, {50, 50, 0.01});
    const FlatFaces ff = findFlatFaces(thin);
    REQUIRE(ff.faces.size() == 2);  // the two big sides only; the 0.5 mm² edges are slivers
    CHECK(ff.faces[0].area == Approx(2500));
}

TEST_CASE("Flat faces: triangles that do not share corners still join", "[flat]") {
    TriangleMesh box = makeBox({0, 0, 0}, {10, 10, 10});
    TriangleMesh loose;
    for (const Face& f : box.faces) {
        const int32_t base = static_cast<int32_t>(loose.vertices.size());
        for (int k = 0; k < 3; ++k) loose.vertices.push_back(box.vertices[static_cast<std::size_t>(f[static_cast<std::size_t>(k)])]);
        loose.faces.push_back({base, base + 1, base + 2});
    }
    CHECK(findFlatFaces(loose).faces.size() == 6);
}

TEST_CASE("Flat faces: a 200 000 triangle mesh takes well under a second", "[flat]") {
    const TriangleMesh sphere = makeSphere({0, 0, 0}, 30, 447);  // about 200 000 triangles
    REQUIRE(sphere.faces.size() > 150000);
    const auto t0 = std::chrono::steady_clock::now();
    const FlatFaces ff = findFlatFaces(sphere);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(s < 1.0);
    CHECK(ff.faces.size() == 0);  // a ball has no flat face worth the name
}
