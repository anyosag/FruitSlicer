#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "mesh/AabbTree.h"
#include "mesh/MeshAnalysis.h"
#include "mesh/Placement.h"
#include "mesh/Primitives.h"
#include "mesh/StlIO.h"
#include "support/SupportGeometry.h"

using namespace fruit;
using Catch::Approx;

static double ngonArea(double r, int n) { return 0.5 * n * r * r * std::sin(2 * kPi / n); }

TEST_CASE("primitives are closed, outward and have the right volume", "[mesh]") {
    const TriangleMesh box = makeBox({0, 0, 0}, {2, 3, 4});
    CHECK(box.isClosedManifold());
    CHECK(box.volume() == Approx(24));

    const int n = 64;
    const TriangleMesh cyl = makeCylinder({0, 0, 0}, 2, 5, n);
    CHECK(cyl.isClosedManifold());
    CHECK(cyl.volume() == Approx(ngonArea(2, n) * 5));

    const TriangleMesh cone = makeCone({0, 0, 0}, 1, {0, 0, 3}, n);
    CHECK(cone.isClosedManifold());
    CHECK(cone.volume() == Approx(ngonArea(1, n) * 3 / 3));

    // Slanted frustum keeps outward orientation.
    TriangleMesh fr;
    appendFrustum(fr, {0, 0, 0}, 1, {1, 2, 3}, 0.5, 32);
    CHECK(fr.isClosedManifold());
    CHECK(fr.volume() > 0);

    const TriangleMesh sph = makeSphere({1, 1, 1}, 2, 96);
    CHECK(sph.isClosedManifold());
    CHECK(sph.volume() == Approx(4.0 / 3.0 * kPi * 8).epsilon(0.01));
}

TEST_CASE("segmentsFor honours chord error", "[mesh]") {
    // 0.2 mm tip at 7 µm chord error needs ~9 segments; a 1 mm shaft ~19.
    CHECK(segmentsFor(0.1, 0.007) >= 9);
    CHECK(segmentsFor(0.1, 0.007) <= 13);
    const int n = segmentsFor(0.5, 0.007);
    CHECK(0.5 * (1 - std::cos(kPi / n)) <= 0.007);
}

TEST_CASE("STL round trip preserves geometry and welds vertices", "[mesh][io]") {
    const auto dir = std::filesystem::temp_directory_path() / "fruit_test_stl";
    std::filesystem::create_directories(dir);
    const auto path = (dir / "cyl.stl").string();
    const TriangleMesh cyl = makeCylinder({1, 2, 3}, 2, 5, 48);
    writeStl(path, cyl);
    const TriangleMesh back = readStl(path);
    CHECK(back.faceCount() == cyl.faceCount());
    CHECK(back.vertexCount() == cyl.vertexCount());
    CHECK(back.isClosedManifold());
    CHECK(back.volume() == Approx(cyl.volume()).epsilon(1e-5));

    const auto ascii = (dir / "tri.stl").string();
    std::ofstream(ascii) << "solid t\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\n"
                            "vertex 0 1 0\nendloop\nendfacet\nendsolid t\n";
    const TriangleMesh tri = readStl(ascii);
    CHECK(tri.faceCount() == 1);
    CHECK(tri.vertexCount() == 3);
}

TEST_CASE("inside-out shells are flipped; mirroring keeps orientation", "[mesh]") {
    TriangleMesh m = makeBox({0, 0, 0}, {1, 1, 1});
    for (auto& f : m.faces) std::swap(f[1], f[2]);
    CHECK(m.volume() < 0);
    m.append(makeBox({5, 0, 0}, {6, 1, 1}));
    CHECK(m.orientShellsOutward() == 1);
    CHECK(m.volume() == Approx(2));

    TriangleMesh b = makeBox({0, 0, 0}, {1, 2, 3});
    Transform mirror;
    mirror.m[0][0] = -1;
    b.transform(mirror);
    CHECK(b.volume() == Approx(6));
}

TEST_CASE("AABB ray casts and closest points", "[mesh]") {
    const TriangleMesh box = makeBox({0, 0, 0}, {10, 10, 10});
    const AabbTree tree(box);
    auto hit = tree.raycast({5, 5, -5}, {0, 0, 1});
    REQUIRE(hit);
    CHECK(hit->t == Approx(5));
    CHECK(hit->point.z == Approx(0).margin(1e-12));
    CHECK_FALSE(tree.raycast({20, 5, -5}, {0, 0, 1}));
    CHECK(tree.anyHit({5, 5, 5}, {1, 0, 0}, 10));
    CHECK_FALSE(tree.anyHit({5, 5, 5}, {1, 0, 0}, 4));
    const ClosestPoint cp = tree.closest({5, 5, 12});
    CHECK(cp.distance == Approx(2));
    CHECK(localThickness(tree, {5, 5, 0}, {0, 0, -1}) == Approx(10).epsilon(1e-6));
}

TEST_CASE("minima come from the mesh: cone apex, sphere pole, box plateau", "[mesh][analysis]") {
    // Downward cone: apex at z = 0.
    const TriangleMesh cone = makeCone({0, 0, 5}, 2, {1, 1, 0}, 32);
    auto minima = findMinima(cone);
    REQUIRE(minima.size() == 1);
    CHECK(minima[0].point.z == Approx(0));
    CHECK(minima[0].point.x == Approx(1));

    const TriangleMesh sph = makeSphere({0, 0, 3}, 1, 32);
    minima = findMinima(sph);
    REQUIRE(minima.size() == 1);
    CHECK(minima[0].z == Approx(2));

    // Box bottom face is one plateau minimum at its centre.
    const TriangleMesh box = makeBox({0, 0, 1}, {4, 2, 3});
    minima = findMinima(box);
    REQUIRE(minima.size() == 1);
    CHECK(minima[0].point.x == Approx(2));
    CHECK(minima[0].point.y == Approx(1));
    CHECK(minima[0].vertices.size() == 4);

    // Two separate boxes at different heights: two minima, lowest first.
    TriangleMesh two = makeBox({0, 0, 2}, {1, 1, 3});
    two.append(makeBox({5, 0, 1}, {6, 1, 2}));
    minima = findMinima(two);
    REQUIRE(minima.size() == 2);
    CHECK(minima[0].z == Approx(1));
}

TEST_CASE("placement centres the model and sets elevation", "[mesh]") {
    TriangleMesh box = makeBox({10, 20, 30}, {12, 26, 31});
    placeOnPlate(box, {0, 0, 0}, 5.0);
    const BBox3 b = box.bbox();
    CHECK(b.min.z == Approx(5));
    CHECK(b.center().x == Approx(0).margin(1e-12));
    CHECK(b.center().y == Approx(0).margin(1e-12));
}

TEST_CASE("pinhead geometry penetrates the part by the spec'd depth", "[support]") {
    TriangleMesh m;
    PinheadSpec s;
    s.tip_diameter = 0.4;
    s.penetration = 0.15;
    const Vec3 j = appendPinhead(m, {0, 0, 5}, {0, 0, -1}, s);
    const BBox3 b = m.bbox();
    CHECK(b.max.z == Approx(5.15));
    CHECK(j.z == Approx(5 - (0.2 - 0.15) - (0.2 + 1.0 + 0.5)));
    // Each primitive is a closed outward shell.
    int shells = 0;
    m.shellIds(&shells);
    CHECK(shells == 3);
    CHECK(m.volume() > 0);
}
