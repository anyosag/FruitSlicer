// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Mesh repair: each case breaks a clean shape one way and checks that it
// comes back closed, the same size, and with nothing else changed.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "layers/LayerStack.h"
#include "mesh/MeshRepair.h"
#include "mesh/ModelCheck.h"
#include "mesh/ModelLoad.h"
#include "mesh/Primitives.h"

using namespace fruit;
using Catch::Approx;

namespace {

Vec3 centre(const TriangleMesh& m, const Face& f) {
    return (m.vertices[static_cast<size_t>(f[0])] + m.vertices[static_cast<size_t>(f[1])] +
            m.vertices[static_cast<size_t>(f[2])]) / 3.0;
}

// The mesh without the faces `drop` picks.
template <class Pick>
TriangleMesh without(TriangleMesh m, Pick drop) {
    size_t i = 0;
    std::erase_if(m.faces, [&](const Face& f) { return drop(i++, f); });
    return m;
}

// Cut along z = `cut` into two sheets that share no points: the upper one
// gets its own copies of the points along the cut, moved by `shift`.
TriangleMesh cracked(TriangleMesh m, double cut, const Vec3& shift) {
    std::vector<char> below(m.vertices.size(), 0);
    for (const Face& f : m.faces)
        if (centre(m, f).z <= cut)
            for (int32_t i : f) below[static_cast<size_t>(i)] = 1;
    std::vector<int32_t> copy(m.vertices.size(), -1);
    for (Face& f : m.faces) {
        if (centre(m, f).z <= cut) continue;
        for (int32_t& i : f) {
            if (!below[static_cast<size_t>(i)]) continue;
            if (copy[static_cast<size_t>(i)] < 0) {
                copy[static_cast<size_t>(i)] = static_cast<int32_t>(m.vertices.size());
                m.vertices.push_back(m.vertices[static_cast<size_t>(i)] + shift);
            }
            i = copy[static_cast<size_t>(i)];
        }
    }
    return m;
}

// A cube as six squares that share no points, each shrunk towards its own
// middle by `shrink` of its size: gaps along every edge, as a CAD export
// with unjoined surfaces has them.
TriangleMesh looseCube(double size, double shrink) {
    const TriangleMesh box = makeBox({0, 0, 0}, {size, size, size});
    TriangleMesh m;
    for (size_t q = 0; q < 6; ++q) {
        const Face& a = box.faces[2 * q];
        const Face& b = box.faces[2 * q + 1];
        const int32_t quad[4] = {a[0], a[1], a[2], b[2]};
        Vec3 c;
        for (int32_t i : quad) c += box.vertices[static_cast<size_t>(i)] / 4.0;
        const auto base = static_cast<int32_t>(m.vertices.size());
        for (int32_t i : quad) m.vertices.push_back(c + (box.vertices[static_cast<size_t>(i)] - c) * (1 - shrink));
        m.faces.push_back({base, base + 1, base + 2});
        m.faces.push_back({base, base + 2, base + 3});
    }
    return m;
}

// An L-shaped block (outline 20 × 20 with a 10 × 10 corner cut out, so 300
// mm²) of height `h` with no top: its outline has a corner every `step` mm.
TriangleMesh openL(double h, double step) {
    const Vec2 corners[6] = {{0, 0}, {20, 0}, {20, 10}, {10, 10}, {10, 20}, {0, 20}};
    std::vector<Vec2> outline;
    for (int c = 0; c < 6; ++c) {
        const Vec2 a = corners[c], b = corners[(c + 1) % 6];
        const int n = static_cast<int>(std::lround(norm(b - a) / step));
        for (int i = 0; i < n; ++i) outline.push_back(a + (b - a) * (static_cast<double>(i) / n));
    }
    TriangleMesh m;
    const auto n = static_cast<int32_t>(outline.size());
    for (const Vec2& p : outline) m.vertices.emplace_back(p.x, p.y, 0.0);
    for (const Vec2& p : outline) m.vertices.emplace_back(p.x, p.y, h);
    const int32_t middle = 2 * n;
    m.vertices.emplace_back(5.0, 5.0, 0.0);  // sees the whole L
    for (int32_t i = 0; i < n; ++i) {
        const int32_t j = (i + 1) % n;
        m.faces.push_back({middle, j, i});  // the bottom, facing down
        m.faces.push_back({i, j, n + j});
        m.faces.push_back({i, n + j, n + i});
    }
    return m;
}

double sliceArea(const TriangleMesh& placed, double layer_height, size_t layer) {
    return area(sliceModel(placed, layer_height).slices[layer]);
}

}  // namespace

TEST_CASE("Repair: a clean mesh is left exactly as it is", "[repair]") {
    for (const TriangleMesh& clean : {makeBox({0, 0, 0}, {30, 20, 4}), makeSphere({0, 0, 10}, 10, 48)}) {
        TriangleMesh m = clean;
        const RepairReport r = repairMesh(m);
        CHECK(r.open_edges_before == 0);
        CHECK(r.watertight());
        CHECK_FALSE(r.changed());
        CHECK(m.faces == clean.faces);
        CHECK(m.vertices == clean.vertices);
    }
}

TEST_CASE("Repair: boxes that touch along an edge are not broken", "[repair]") {
    // Read as one mesh they share the edge's two points, and four faces
    // meet at it: two each way round, so every layer still closes.
    TriangleMesh m = makeBox({0, 0, 0}, {10, 10, 10});
    m.append(makeBox({10, 0, 10}, {20, 10, 20}));
    m.weld(1e-9);
    REQUIRE(m.vertexCount() == 14);
    CHECK_FALSE(m.isClosedManifold());
    CHECK(countOpenEdges(m) == 0);
    const TriangleMesh before = m;
    CHECK_FALSE(repairMesh(m).changed());
    CHECK(m.faces == before.faces);
    CHECK(sliceArea(m, 1.0, 5) == Approx(100));
    CHECK(sliceArea(m, 1.0, 15) == Approx(100));
}

TEST_CASE("Repair: a missing face is put back", "[repair]") {
    TriangleMesh m = makeBox({0, 0, 0}, {30, 20, 4});
    m.faces.pop_back();
    REQUIRE(countOpenEdges(m) == 3);
    const RepairReport r = repairMesh(m);
    CHECK(r.open_edges_before == 3);
    CHECK(r.watertight());
    CHECK(r.holes_filled == 1);
    CHECK(r.gaps_closed == 0);
    CHECK(r.patch_faces == 1);
    CHECK(r.faces_flipped == 0);
    CHECK(r.faces_removed == 0);
    CHECK(r.largest_hole == Approx(std::hypot(20.0, 4.0)));  // the x+ side's diagonal
    CHECK(r.patch_area == Approx(40));
    REQUIRE(m.faceCount() == 12);
    CHECK(m.isClosedManifold());
    CHECK(m.volume() == Approx(2400));
    // The patch is the last face; the file's own come first, untouched.
    const TriangleMesh box = makeBox({0, 0, 0}, {30, 20, 4});
    for (size_t i = 0; i < 11; ++i) CHECK(m.faces[i] == box.faces[i]);
}

TEST_CASE("Repair: a missing side, and a cap cut off a ball", "[repair]") {
    TriangleMesh box = without(makeBox({0, 0, 0}, {10, 10, 10}), [](size_t i, const Face&) { return i == 2 || i == 3; });
    RepairReport r = repairMesh(box);
    CHECK(r.watertight());
    CHECK(r.holes_filled == 1);
    CHECK(r.patch_faces == 2);
    CHECK(box.volume() == Approx(1000));

    // Everything above z = 16 of a 10 mm ball centred at z = 10: the hole's
    // rim is a ring, and the flat cap over it leaves the ball less the dome.
    const TriangleMesh whole = makeSphere({0, 0, 10}, 10, 96);
    TriangleMesh ball = without(whole, [&](size_t, const Face& f) { return centre(whole, f).z > 16; });
    REQUIRE(countOpenEdges(ball) == 96);
    r = repairMesh(ball);
    CHECK(r.watertight());
    CHECK(r.holes_filled == 1);
    CHECK(r.patch_faces == 94);  // a ring of 96 corners
    double rim_z = 0;
    for (size_t i = ball.faceCount() - r.patch_faces; i < ball.faceCount(); ++i)
        for (int32_t v : ball.faces[i]) rim_z = ball.vertices[static_cast<size_t>(v)].z;
    const double dome_h = 20 - rim_z;
    const double dome = kPi * dome_h * dome_h * (3 * 10 - dome_h) / 3;
    CHECK(ball.volume() == Approx(whole.volume() - dome).epsilon(0.01));
    CHECK(r.largest_hole == Approx(2 * std::sqrt(100 - (rim_z - 10) * (rim_z - 10))).epsilon(0.01));
}

TEST_CASE("Repair: faces turned the wrong way are turned back", "[repair]") {
    const TriangleMesh whole = makeSphere({0, 0, 10}, 10, 48);
    TriangleMesh m = whole;
    size_t turned = 0;
    for (size_t i = 0; i < m.faceCount(); i += 3, ++turned) std::swap(m.faces[i][1], m.faces[i][2]);
    REQUIRE(countOpenEdges(m) > 0);
    LoadedModel fixed = prepareModel(m);
    CHECK(fixed.repair.watertight());
    CHECK(fixed.repair.faces_flipped == turned);
    CHECK(fixed.repair.patch_faces == 0);
    CHECK(fixed.repair.faces_removed == 0);
    CHECK(fixed.shells_flipped == 0);
    CHECK(fixed.mesh.faces == whole.faces);

    // Most of them wrong: the fewer are turned to match, which leaves the
    // ball inside out, and a piece that needed repair is then turned to face
    // outward. In the end just the wrong ones were turned.
    m = whole;
    for (size_t i = 0; i < m.faceCount(); ++i)
        if (i % 3) std::swap(m.faces[i][1], m.faces[i][2]);
    fixed = prepareModel(m);
    CHECK(fixed.repair.watertight());
    CHECK(fixed.repair.faces_flipped == m.faceCount() - turned);
    CHECK(fixed.shells_flipped == 0);
    CHECK(fixed.mesh.faces == whole.faces);
    CHECK(checkMesh(fixed).closed());
}

TEST_CASE("Repair: doubled faces go, the wall between glued solids stays", "[repair]") {
    // Three faces there twice.
    TriangleMesh m = makeBox({0, 0, 0}, {10, 10, 10});
    for (size_t i : {1u, 4u, 9u}) m.faces.push_back(m.faces[i]);
    RepairReport r = repairMesh(m);
    CHECK(r.watertight());
    CHECK(r.faces_removed == 3);
    CHECK(r.patch_faces == 0);
    CHECK(m.faceCount() == 12);
    CHECK(m.volume() == Approx(1000));

    // A face there a second time in mirror image: the copy goes, the face
    // stays, and nothing is patched.
    const TriangleMesh box = makeBox({0, 0, 0}, {10, 10, 10});
    m = box;
    m.faces.push_back({m.faces[5][0], m.faces[5][2], m.faces[5][1]});
    r = repairMesh(m);
    CHECK(r.watertight());
    CHECK(r.faces_removed == 1);
    CHECK(r.patch_faces == 0);
    CHECK(m.faces == box.faces);

    // Two pyramids glued base to base, each closed on its own. Their two
    // bases face each other and cancel in every layer: they are no fault,
    // and stay when a doubled face elsewhere has the mesh repaired.
    TriangleMesh two;
    two.vertices = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {3, 3, 8}, {3, 3, -8}};
    two.faces = {{0, 1, 3}, {1, 2, 3}, {2, 0, 3}, {0, 2, 1},   // the upper, its base facing down
                 {1, 0, 4}, {2, 1, 4}, {0, 2, 4}, {0, 1, 2}};  // the lower, its base facing up
    REQUIRE(countOpenEdges(two) == 0);
    const TriangleMesh glued = two;
    two.faces.push_back(two.faces[0]);
    r = repairMesh(two);
    CHECK(r.watertight());
    CHECK(r.faces_removed == 1);
    CHECK(two.faces == glued.faces);
    CHECK(two.volume() == Approx(2 * 50.0 * 8 / 3));
}

TEST_CASE("Repair: an edge split on one side only is closed with a face of no area", "[repair]") {
    TriangleMesh m = makeBox({0, 0, 0}, {10, 10, 10});
    // The first face runs 0 -> 2 -> 3: put a point half-way along 0 -> 2.
    REQUIRE(m.faces[0] == Face{0, 2, 3});
    const auto mid = static_cast<int32_t>(m.vertices.size());
    m.vertices.push_back((m.vertices[0] + m.vertices[2]) / 2.0);
    m.faces[0] = {0, mid, 3};
    m.faces.push_back({mid, 2, 3});
    REQUIRE(countOpenEdges(m) == 3);
    const RepairReport r = repairMesh(m);
    CHECK(r.watertight());
    CHECK(r.gaps_closed == 1);
    CHECK(r.holes_filled == 0);
    CHECK(r.patch_faces == 1);
    CHECK(r.patch_area == Approx(0).margin(1e-12));
    CHECK(m.isClosedManifold());
    CHECK(m.volume() == Approx(1000));
}

TEST_CASE("Repair: a crack is zipped shut, not capped on each side", "[repair]") {
    const TriangleMesh whole = makeSphere({0, 0, 10}, 10, 96);
    // The two halves 0.02 mm apart along the cut.
    TriangleMesh m = cracked(whole, 10.0, {0, 0, 0.02});
    REQUIRE(countOpenEdges(m) == 2 * 96);
    RepairReport r = repairMesh(m);
    CHECK(r.watertight());
    CHECK(r.gaps_closed == 1);
    CHECK(r.holes_filled == 0);
    CHECK(r.patch_faces == 2 * 96);
    CHECK(r.patch_area == Approx(2 * kPi * 10 * 0.02).epsilon(0.05));  // a band round the ball
    CHECK(r.widest_gap == Approx(0.02).epsilon(0.1));
    CHECK(r.faces_flipped == 0);
    CHECK(m.volume() == Approx(whole.volume()).epsilon(0.002));
    CHECK(prepareModel(m).shells == 1);

    // Further apart than a seam is wide they are two open halves: each gets
    // its cap, and the slit between them stays.
    m = cracked(whole, 10.0, {0, 0, 0.5});
    r = repairMesh(m);
    CHECK(r.watertight());
    CHECK(r.holes_filled == 2);
    CHECK(r.gaps_closed == 0);
    CHECK(prepareModel(m).shells == 2);

    // One half inside out as well: turned to match the other across the seam.
    m = cracked(whole, 10.0, {0, 0, 0.02});
    size_t upper = 0;
    for (Face& f : m.faces)
        if (centre(m, f).z > 10) std::swap(f[1], f[2]), ++upper;
    r = repairMesh(m);
    CHECK(r.watertight());
    CHECK(r.faces_flipped == upper);
    CHECK(r.gaps_closed == 1);
    CHECK(m.volume() == Approx(whole.volume()).epsilon(0.002));
}

TEST_CASE("Repair: surfaces that were never joined", "[repair]") {
    // Six squares with 0.014 mm between their edges.
    TriangleMesh m = looseCube(20, 0.001);
    REQUIRE(countOpenEdges(m) == 24);
    LoadedModel fixed = prepareModel(m);
    CHECK(fixed.repair.watertight());
    CHECK(fixed.repair.gaps_closed == 1);
    CHECK(fixed.repair.holes_filled == 0);
    CHECK(fixed.repair.patch_faces == 32);  // two a seam, one a corner
    CHECK(fixed.repair.faces_flipped == 0);
    CHECK(fixed.shells == 1);
    CHECK(fixed.mesh.volume() == Approx(8000).epsilon(0.001));
    CHECK(fixed.mesh.vertices == m.vertices);  // no point moved

    // Two of the squares facing inward.
    for (size_t i : {2u, 3u, 8u, 9u}) std::swap(m.faces[i][1], m.faces[i][2]);
    fixed = prepareModel(m);
    CHECK(fixed.repair.watertight());
    CHECK(fixed.repair.faces_flipped == 4);
    CHECK(fixed.repair.gaps_closed == 1);
    CHECK(fixed.mesh.volume() == Approx(8000).epsilon(0.001));

    // Too far apart to be seams (0.7 mm): each square capped on its own
    // would cancel itself and leave nothing, so the file stays as it is.
    m = looseCube(20, 0.05);
    const TriangleMesh before = m;
    const RepairReport r = repairMesh(m);
    CHECK_FALSE(r.watertight());
    CHECK(r.open_edges_after == 24);
    CHECK_FALSE(r.changed());
    CHECK(m.faces == before.faces);
}

TEST_CASE("Repair: loose surfaces with no thickness go", "[repair]") {
    // A flat sheet beside the box, and a fin standing on one of its edges.
    TriangleMesh m = makeBox({0, 0, 0}, {10, 10, 10});
    const auto base = static_cast<int32_t>(m.vertices.size());
    for (const Vec3& p : {Vec3{30, 0, 0}, Vec3{40, 0, 0}, Vec3{40, 0, 10}, Vec3{30, 0, 10}, Vec3{5, -5, -5}})
        m.vertices.push_back(p);
    m.faces.push_back({base, base + 1, base + 2});
    m.faces.push_back({base, base + 2, base + 3});
    m.faces.push_back({0, 1, base + 4});
    const RepairReport r = repairMesh(m);
    CHECK(r.watertight());
    CHECK(r.faces_removed == 3);
    CHECK(r.patch_faces == 0);
    CHECK(r.holes_filled == 0);
    CHECK(r.gaps_closed == 0);
    CHECK(m.faceCount() == 12);
    CHECK(m.vertexCount() == 8);  // their points too: the model is the box's size again
    CHECK(m.bbox().max.x == Approx(10));
    CHECK(m.bbox().min.y == Approx(0));
    CHECK(m.isClosedManifold());
}

TEST_CASE("Repair: parts that are sound are left alone, whatever is next to them", "[repair]") {
    // A box that is there twice (every face doubled) closes as well as one
    // does, and stays as it is when a hole elsewhere has the mesh repaired.
    TriangleMesh m = makeBox({0, 0, 0}, {10, 10, 10});
    m.faces.insert(m.faces.end(), m.faces.begin(), m.faces.begin() + 12);
    REQUIRE(countOpenEdges(m) == 0);
    TriangleMesh other = makeBox({20, 0, 0}, {30, 10, 10});
    other.faces.pop_back();
    m.append(other);
    RepairReport r = repairMesh(m);
    CHECK(r.watertight());
    CHECK(r.faces_removed == 0);
    CHECK(r.holes_filled == 1);
    CHECK(m.faceCount() == 24 + 12);

    // Two boxes 0.02 mm apart, each missing the face that looks at the
    // other. The holes line up like a seam, but each box is closed but for
    // its hole and has an inside of its own: neither is turned. The gap
    // between the holes is bridged, and the two print as one.
    auto facing = [](bool second_inside_out) {
        TriangleMesh a = makeBox({0, 0, 0}, {10, 10, 10});
        TriangleMesh b = makeBox({10.02, 0, 0}, {20, 10, 10});
        a.faces.erase(a.faces.begin() + 10);  // on x+: corners 1, 3, 7
        b.faces.erase(b.faces.begin() + 9);   // on x-: corners 0, 6, 2
        if (second_inside_out)
            for (Face& f : b.faces) std::swap(f[1], f[2]);
        a.append(b);
        return a;
    };
    LoadedModel fixed = prepareModel(facing(false));
    CHECK(fixed.repair.watertight());
    CHECK(fixed.repair.faces_flipped == 0);
    CHECK(fixed.repair.gaps_closed == 1);
    CHECK(fixed.repair.holes_filled == 0);
    CHECK(fixed.shells == 1);
    CHECK(fixed.cavities == 0);
    CHECK(fixed.mesh.volume() == Approx(1000 + 998 + 50 * 0.02).epsilon(1e-6));

    // The second box inside out as well. It is not turned to suit the
    // first's hole; each gets its cap, and then, being a piece that needed
    // repair, it is turned to face outward: a part, not a hollow.
    fixed = prepareModel(facing(true));
    CHECK(fixed.repair.watertight());
    CHECK(fixed.repair.faces_flipped == 11);
    CHECK(fixed.cavities == 0);
    CHECK(fixed.shells_flipped == 0);
    CHECK(fixed.mesh.volume() == Approx(1000 + 998).epsilon(1e-3));
    CHECK(sliceArea(fixed.mesh, 1.0, 5) == Approx(100 + 99.8).epsilon(1e-3));
}

TEST_CASE("Repair: a flat hole with a long, bent outline gets its exact cap", "[repair]") {
    // 160 corners round an L: more than are patched in one piece, and any
    // cap but the flat L itself would change the volume.
    TriangleMesh m = openL(8.0, 0.5);
    REQUIRE(countOpenEdges(m) == 160);
    const LoadedModel fixed = prepareModel(m);
    CHECK(fixed.repair.watertight());
    CHECK(fixed.repair.holes_filled == 1);
    CHECK(fixed.repair.patch_faces == 158);
    CHECK(fixed.repair.patch_area == Approx(300).epsilon(1e-9));
    CHECK(fixed.mesh.volume() == Approx(300 * 8.0).epsilon(1e-9));
    CHECK(sliceArea(fixed.mesh, 1.0, 7) == Approx(300));
}

TEST_CASE("Repair: a rim of thousands of corners", "[repair]") {
    const int n = 6000;
    TriangleMesh m = makeCylinder({0, 0, 0}, 20, 5, n);
    const auto top = static_cast<int32_t>(m.vertices.size()) - 1;  // the top cap's middle
    m = without(m, [&](size_t, const Face& f) { return f[0] == top || f[1] == top || f[2] == top; });
    REQUIRE(countOpenEdges(m) == static_cast<size_t>(n));
    const RepairReport r = repairMesh(m);
    CHECK(r.watertight());
    CHECK(r.holes_filled == 1);
    CHECK(r.patch_faces == static_cast<size_t>(n) - 2);
    CHECK(r.largest_hole == Approx(40).epsilon(0.01));
    CHECK(m.volume() == Approx(0.5 * n * 400 * std::sin(2 * kPi / n) * 5).epsilon(1e-9));
}

TEST_CASE("Repair: a model with a hollow keeps it", "[repair]") {
    TriangleMesh m = makeBox({0, 0, 0}, {20, 20, 20});
    TriangleMesh inner = makeBox({5, 5, 5}, {15, 15, 15});
    for (Face& f : inner.faces) std::swap(f[1], f[2]);
    m.append(inner);
    LoadedModel loaded = prepareModel(m);
    CHECK_FALSE(loaded.repair.changed());
    CHECK(loaded.cavities == 1);
    CHECK(loaded.mesh.volume() == Approx(8000 - 1000));

    m.faces.erase(m.faces.begin() + 3);  // a face of the outside missing
    loaded = prepareModel(m);
    CHECK(loaded.repair.watertight());
    CHECK(loaded.repair.holes_filled == 1);
    CHECK(loaded.cavities == 1);
    CHECK(loaded.shells == 2);
    CHECK(loaded.mesh.volume() == Approx(8000 - 1000));
    const MeshCheck c = checkMesh(loaded);
    CHECK(c.closed());
    CHECK(c.repair.patch_faces == 1);

    // The hollow's own shell broken instead: once patched it can't be told
    // from a part that is inside out, so it prints solid rather than risk a
    // hole.
    m = makeBox({0, 0, 0}, {20, 20, 20});
    m.append(inner);
    m.faces.pop_back();
    loaded = prepareModel(m);
    CHECK(loaded.repair.watertight());
    CHECK(loaded.cavities == 0);
    CHECK(loaded.repair.faces_flipped == 11);
    CHECK(loaded.mesh.volume() == Approx(8000 + 1000));  // both count; in the layers they are one solid
    CHECK(sliceArea(loaded.mesh, 1.0, 10) == Approx(400));
}

TEST_CASE("Repair: scattered damage on a fine mesh, and the layers that come of it", "[repair]") {
    const TriangleMesh whole = makeSphere({0, 0, 10}, 10, 256);  // 65,000 faces
    TriangleMesh m = whole;
    // Every 97th face missing, every 41st of the rest turned over.
    size_t gone = 0;
    m = without(m, [&](size_t i, const Face&) { return i % 97 == 5 ? (++gone, true) : false; });
    for (size_t i = 0; i < m.faceCount(); i += 41) std::swap(m.faces[i][1], m.faces[i][2]);
    const LoadedModel fixed = prepareModel(m);
    CHECK(fixed.repair.watertight());
    CHECK(fixed.repair.patch_faces == gone);
    CHECK(fixed.mesh.faceCount() == whole.faceCount());
    CHECK(fixed.mesh.volume() == Approx(whole.volume()).epsilon(1e-4));
    CHECK(checkMesh(fixed).closed());
    const LayerStack a = sliceModel(whole, 0.5), b = sliceModel(fixed.mesh, 0.5);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) CHECK(area(b.slices[i]) == Approx(area(a.slices[i])).margin(0.05));

    // The same again gives the same mesh, and repairing the repaired mesh
    // changes nothing.
    const LoadedModel again = prepareModel(m);
    CHECK(again.mesh.faces == fixed.mesh.faces);
    TriangleMesh twice = fixed.mesh;
    CHECK_FALSE(repairMesh(twice).changed());
    CHECK(twice.faces == fixed.mesh.faces);
}

TEST_CASE("Repair: the file as it is, when asked or when nothing can be made of it", "[repair]") {
    TriangleMesh open = makeBox({0, 0, 0}, {30, 20, 4});
    open.faces.pop_back();
    const LoadedModel raw = prepareModel(open, false);
    CHECK(raw.repair.open_edges_before == 3);
    CHECK(raw.repair.open_edges_after == 3);
    CHECK_FALSE(raw.repair.changed());
    CHECK(raw.mesh.faceCount() == 11);
    CHECK_FALSE(checkMesh(raw).closed());

    // A point that isn't a number: no telling where anything is.
    TriangleMesh bad = open;
    bad.vertices[0].x = std::numeric_limits<double>::quiet_NaN();
    const RepairReport r = repairMesh(bad);
    CHECK_FALSE(r.watertight());
    CHECK_FALSE(r.changed());
    CHECK(bad.faceCount() == 11);
}
