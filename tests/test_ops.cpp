// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Solid geometry for the model tools: booleans, plane cuts, splitting into
// pieces, letters, collisions and arranging.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <utility>

#include "mesh/Primitives.h"
#include "ops/Arrange.h"
#include "ops/MeshOps.h"
#include "ops/TextSolid.h"

using namespace fruit;
using namespace fruit::ops;
using Catch::Approx;
using Catch::Matchers::ContainsSubstring;

namespace {

TriangleMesh box(double x0, double y0, double z0, double x1, double y1, double z1) {
    return makeBox({x0, y0, z0}, {x1, y1, z1});
}

// A box whose faces share no points (the way many STL files come).
TriangleMesh unwelded(const TriangleMesh& m) {
    TriangleMesh o;
    for (const Face& f : m.faces) {
        const int32_t base = static_cast<int32_t>(o.vertices.size());
        for (int k = 0; k < 3; ++k) o.vertices.push_back(m.vertices[static_cast<size_t>(f[k])]);
        o.faces.push_back({base, base + 1, base + 2});
    }
    return o;
}

TriangleMesh welded(TriangleMesh m) {
    m.weld();
    return m;
}

bool closed(const TriangleMesh& m) { return welded(m).isClosedManifold(); }

// V - E + F of a closed mesh: 2 per sphere-like shell, 0 per ring-shaped one.
int eulerNumber(const TriangleMesh& m) {
    const TriangleMesh w = welded(m);
    return static_cast<int>(w.vertexCount()) - static_cast<int>(w.faceCount() * 3 / 2) + static_cast<int>(w.faceCount());
}

}  // namespace

TEST_CASE("Solid: a clean mesh converts and comes back the same size", "[ops]") {
    const TriangleMesh b = box(0, 0, 0, 10, 20, 30);
    const MeshResult r = cleanMesh(b);
    REQUIRE(r.ok);
    CHECK(r.message.empty());
    CHECK(closed(r.mesh));
    CHECK(r.mesh.volume() == Approx(6000.0));
    CHECK(r.mesh.bbox().max.y == Approx(20.0));
}

TEST_CASE("Solid: vertices that were not shared are merged", "[ops]") {
    const TriangleMesh loose = unwelded(box(0, 0, 0, 10, 10, 10));
    CHECK_FALSE(loose.isClosedManifold());
    const MeshResult r = cleanMesh(loose);
    REQUIRE(r.ok);
    CHECK(r.mesh.vertexCount() == 8);
    CHECK(r.mesh.volume() == Approx(1000.0));
}

TEST_CASE("Solid: a mesh that is not closed is refused in plain words", "[ops]") {
    TriangleMesh open = box(0, 0, 0, 10, 10, 10);
    open.faces.pop_back();  // a hole
    const MeshResult r = cleanMesh(open);
    CHECK_FALSE(r.ok);
    CHECK(r.mesh.empty());
    CHECK_THAT(r.message, ContainsSubstring("not a closed solid"));
    CHECK_THAT(r.message, ContainsSubstring("open"));
    CHECK_THAT(whyNotSolid(open), ContainsSubstring("Repair"));
    CHECK(whyNotSolid(box(0, 0, 0, 1, 1, 1)).empty());

    const MeshResult none = cleanMesh(TriangleMesh{});
    CHECK_FALSE(none.ok);
    CHECK_THAT(none.message, ContainsSubstring("no faces"));

    // An edge shared by three faces.
    TriangleMesh three = box(0, 0, 0, 10, 10, 10);
    three.vertices.push_back({5, 5, 20});
    three.faces.push_back({0, 1, 8});
    three.faces.push_back({0, 1, 8});
    CHECK_FALSE(cleanMesh(three).ok);
}

TEST_CASE("Solid: a wholly inside-out mesh is turned round and the result says so", "[ops]") {
    TriangleMesh m = box(0, 0, 0, 10, 10, 10);
    for (Face& f : m.faces) std::swap(f[1], f[2]);
    const MeshResult r = cleanMesh(m);
    REQUIRE(r.ok);
    CHECK(r.mesh.volume() == Approx(1000.0));
    CHECK_THAT(r.message, ContainsSubstring("turned round"));
}

TEST_CASE("Combine: union, subtract and intersect of two boxes", "[ops]") {
    const TriangleMesh a = box(0, 0, 0, 10, 10, 10);
    const TriangleMesh b = box(5, 5, 5, 15, 15, 15);  // overlap 5 x 5 x 5 = 125
    const MeshResult u = unionMeshes(a, b);
    REQUIRE(u.ok);
    CHECK(closed(u.mesh));
    CHECK(u.mesh.volume() == Approx(2000.0 - 125.0));
    const MeshResult s = subtractMeshes(a, b);
    REQUIRE(s.ok);
    CHECK(closed(s.mesh));
    CHECK(s.mesh.volume() == Approx(1000.0 - 125.0));
    CHECK(s.mesh.bbox().max.x == Approx(10.0));
    const MeshResult i = intersectMeshes(a, b);
    REQUIRE(i.ok);
    CHECK(i.mesh.volume() == Approx(125.0));
    CHECK(i.mesh.bbox().min.x == Approx(5.0));
}

TEST_CASE("Combine: parts that do not meet, and parts that cover each other", "[ops]") {
    const TriangleMesh a = box(0, 0, 0, 10, 10, 10);
    const TriangleMesh far = box(20, 0, 0, 30, 10, 10);
    const MeshResult none = intersectMeshes(a, far);
    CHECK(none.ok);
    CHECK(none.mesh.empty());
    CHECK_THAT(none.message, ContainsSubstring("do not overlap"));
    const MeshResult two = unionMeshes(a, far);
    REQUIRE(two.ok);
    CHECK(two.mesh.volume() == Approx(2000.0));
    CHECK(countPieces(two.mesh) == 2);
    const MeshResult gone = subtractMeshes(box(2, 2, 2, 4, 4, 4), a);
    CHECK(gone.ok);
    CHECK(gone.mesh.empty());
    CHECK_THAT(gone.message, ContainsSubstring("nothing is left"));
    // Touching on a face fuses.
    const MeshResult fused = unionMeshes(a, box(10, 0, 0, 20, 10, 10));
    REQUIRE(fused.ok);
    CHECK(fused.mesh.volume() == Approx(2000.0));
    CHECK(countPieces(fused.mesh) == 1);
}

TEST_CASE("Combine: a mesh that is not closed names which one", "[ops]") {
    TriangleMesh open = box(0, 0, 0, 10, 10, 10);
    open.faces.pop_back();
    const TriangleMesh good = box(0, 0, 0, 5, 5, 5);
    const MeshResult first = unionMeshes(open, good);
    CHECK_FALSE(first.ok);
    CHECK_THAT(first.message, ContainsSubstring("First mesh"));
    const MeshResult second = subtractMeshes(good, open);
    CHECK_FALSE(second.ok);
    CHECK_THAT(second.message, ContainsSubstring("Second mesh"));
    CHECK(second.mesh.empty());
}

TEST_CASE("Combine: a hole through a sphere keeps the result closed", "[ops]") {
    const TriangleMesh ball = makeSphere({0, 0, 0}, 10, 48);
    const TriangleMesh peg = makeCylinder({0, 0, -15}, 3, 30, 32);
    const MeshResult r = subtractMeshes(ball, peg);
    REQUIRE(r.ok);
    CHECK(closed(r.mesh));
    CHECK(r.mesh.volume() < ball.volume() - 500.0);
    CHECK(eulerNumber(r.mesh) == 0);  // a ring: one hole through it
}

TEST_CASE("Cut: a box cut by a horizontal plane gives two closed halves", "[ops]") {
    const TriangleMesh b = box(0, 0, 0, 10, 10, 10);
    const CutResult c = cutByPlane(b, {{0, 0, 4}, {0, 0, 1}});
    REQUIRE(c.ok);
    CHECK(c.message.empty());
    CHECK(closed(c.above));
    CHECK(closed(c.below));
    CHECK(c.above.volume() == Approx(600.0));
    CHECK(c.below.volume() == Approx(400.0));
    CHECK(c.above.bbox().min.z == Approx(4.0));
    CHECK(c.below.bbox().max.z == Approx(4.0));
}

TEST_CASE("Cut: vertical planes, and the plane's direction picks the sides", "[ops]") {
    const TriangleMesh b = box(0, 0, 0, 10, 20, 30);
    const CutResult x = cutByPlane(b, {{2, 0, 0}, {1, 0, 0}});
    REQUIRE(x.ok);
    CHECK(x.above.volume() == Approx(8 * 20 * 30.0));
    CHECK(x.below.volume() == Approx(2 * 20 * 30.0));
    const CutResult y = cutByPlane(b, {{0, 5, 0}, {0, -1, 0}});  // reversed: "above" is toward -Y
    REQUIRE(y.ok);
    CHECK(y.above.volume() == Approx(5 * 10 * 30.0));
    CHECK(y.below.volume() == Approx(15 * 10 * 30.0));
    CHECK(closed(y.above));
    CHECK(closed(y.below));
}

TEST_CASE("Cut: a slanted plane through a sphere, and a plane that misses", "[ops]") {
    const TriangleMesh ball = makeSphere({1, 2, 3}, 10, 48);
    const CutResult c = cutByPlane(ball, {{1, 2, 3}, {1, 1, 1}});
    REQUIRE(c.ok);
    CHECK(closed(c.above));
    CHECK(closed(c.below));
    CHECK(c.above.volume() == Approx(c.below.volume()).epsilon(1e-6));
    CHECK(c.above.volume() + c.below.volume() == Approx(ball.volume()).epsilon(1e-9));

    const CutResult miss = cutByPlane(ball, {{0, 0, 50}, {0, 0, 1}});
    REQUIRE(miss.ok);
    CHECK(miss.above.empty());
    CHECK(miss.below.volume() == Approx(ball.volume()));
    CHECK_THAT(miss.message, ContainsSubstring("does not cut"));

    CHECK_FALSE(cutByPlane(ball, {{0, 0, 0}, {0, 0, 0}}).ok);
    TriangleMesh open = ball;
    open.faces.pop_back();
    CHECK_FALSE(cutByPlane(open, {{0, 0, 0}, {0, 0, 1}}).ok);
}

TEST_CASE("Cut: a part that is already two pieces is cut into all four", "[ops]") {
    TriangleMesh two = box(0, 0, 0, 10, 10, 10);
    two.append(box(20, 0, 0, 30, 10, 10));
    const CutResult c = cutByPlane(two, {{0, 0, 5}, {0, 0, 1}});
    REQUIRE(c.ok);
    CHECK(countPieces(c.above) == 2);
    CHECK(countPieces(c.below) == 2);
}

TEST_CASE("Split: one mesh per piece, biggest first", "[ops]") {
    TriangleMesh m = box(0, 0, 0, 2, 2, 2);
    m.append(box(10, 0, 0, 20, 10, 10));
    m.append(box(40, 0, 0, 44, 4, 4));
    const auto pieces = splitShells(m);
    REQUIRE(pieces.size() == 3);
    CHECK(pieces[0].volume() == Approx(1000.0));
    CHECK(pieces[1].volume() == Approx(64.0));
    CHECK(pieces[2].volume() == Approx(8.0));
    for (const auto& p : pieces) CHECK(closed(p));
    CHECK(countPieces(m) == 3);
    CHECK(splitShells(TriangleMesh{}).empty());
    CHECK(splitShells(box(0, 0, 0, 1, 1, 1)).size() == 1);
}

TEST_CASE("Split: a hollowed part stays one piece, and unwelded input still splits right", "[ops]") {
    TriangleMesh hollow = box(0, 0, 0, 20, 20, 20);
    TriangleMesh cavity = box(5, 5, 5, 15, 15, 15);
    for (Face& f : cavity.faces) std::swap(f[1], f[2]);
    hollow.append(cavity);
    hollow.append(box(50, 0, 0, 55, 5, 5));
    const auto pieces = splitShells(hollow);
    REQUIRE(pieces.size() == 2);
    CHECK(pieces[0].volume() == Approx(8000.0 - 1000.0));
    CHECK(pieces[0].faceCount() == 24);
    CHECK(pieces[1].volume() == Approx(125.0));

    TriangleMesh two = box(0, 0, 0, 5, 5, 5);
    two.append(box(10, 0, 0, 15, 5, 5));
    CHECK(countPieces(unwelded(two)) == 2);
}

TEST_CASE("Split: an inside-out piece is turned round", "[ops]") {
    TriangleMesh m = box(0, 0, 0, 5, 5, 5);
    for (Face& f : m.faces) std::swap(f[1], f[2]);
    const auto pieces = splitShells(m);
    REQUIRE(pieces.size() == 1);
    CHECK(pieces[0].volume() == Approx(125.0));
}

TEST_CASE("Collisions: overlap, touching, a gap, far apart", "[ops]") {
    const TriangleMesh a = box(0, 0, 0, 10, 10, 10);
    const CollisionResult hit = checkCollision(a, box(5, 5, 5, 15, 15, 15));
    REQUIRE(hit.ok);
    CHECK(hit.overlaps);
    CHECK(hit.overlap_volume == Approx(125.0));
    CHECK(hit.gap == 0);

    const CollisionResult touch = checkCollision(a, box(10, 0, 0, 20, 10, 10), 1e-3, 5.0);
    REQUIRE(touch.ok);
    CHECK_FALSE(touch.overlaps);
    CHECK(touch.gap == Approx(0.0).margin(1e-9));

    const CollisionResult near = checkCollision(a, box(13, 0, 0, 20, 10, 10), 1e-3, 10.0);
    REQUIRE(near.ok);
    CHECK_FALSE(near.overlaps);
    CHECK(near.gap == Approx(3.0));

    const CollisionResult diag = checkCollision(a, box(13, 14, 10, 20, 20, 12), 1e-3, 10.0);
    REQUIRE(diag.ok);
    CHECK(diag.gap == Approx(5.0));  // 3 across X, 4 across Y

    const CollisionResult far = checkCollision(a, box(50, 0, 0, 60, 10, 10), 1e-3, 10.0);
    REQUIRE(far.ok);
    CHECK_FALSE(far.overlaps);
    CHECK(far.gap == -1);
    // Not asked for: no gap.
    CHECK(checkCollision(a, box(13, 0, 0, 20, 10, 10)).gap == -1);
}

TEST_CASE("Collisions: a sliver below the threshold is not an overlap, and a bad mesh is reported", "[ops]") {
    const TriangleMesh a = box(0, 0, 0, 10, 10, 10);
    const CollisionResult sliver = checkCollision(a, box(9.9999, 0, 0, 20, 10, 10), 0.01);
    REQUIRE(sliver.ok);
    CHECK_FALSE(sliver.overlaps);
    CHECK(sliver.overlap_volume > 0);
    CHECK(sliver.overlap_volume < 0.01);

    TriangleMesh open = a;
    open.faces.pop_back();
    const CollisionResult bad = checkCollision(open, a);
    CHECK_FALSE(bad.ok);
    CHECK_THAT(bad.message, ContainsSubstring("First mesh"));
    CHECK_FALSE(bad.overlaps);
}

TEST_CASE("Collisions: a part converted once can be tested against many", "[ops]") {
    const Solid a(box(0, 0, 0, 10, 10, 10));
    REQUIRE(a.valid());
    CHECK(a.volume() == Approx(1000.0));
    CHECK(a.bbox().max.z == Approx(10.0));
    int hits = 0;
    for (int i = 0; i < 5; ++i) {
        const Solid b(box(i * 8.0, 0, 0, i * 8.0 + 6, 6, 6));
        if (checkCollision(a, b).overlaps) ++hits;
    }
    CHECK(hits == 2);  // x 0..6 and 8..14 overlap 0..10; 16.. do not
}

TEST_CASE("Text: letters with holes come out closed and right", "[ops][text]") {
    TextStyle st;
    st.height = 10;
    st.depth = 2;
    const TextResult h = makeTextSolid("H", st);
    REQUIRE(h.ok);
    CHECK(closed(h.mesh));
    CHECK(h.mesh.bbox().min.z == Approx(0.0));
    CHECK(h.mesh.bbox().max.z == Approx(2.0));
    CHECK(h.mesh.bbox().size().y == Approx(10.0).epsilon(0.01));  // capital height
    CHECK(h.mesh.bbox().center().x == Approx(0.0).margin(1e-3));
    CHECK(h.mesh.bbox().center().y == Approx(0.0).margin(1e-3));
    CHECK(h.mesh.volume() > 0);
    CHECK(eulerNumber(h.mesh) == 2);  // no hole

    // One counter each: a ring, so V - E + F is 0 per letter.
    for (const char* s : {"o", "a", "e", "O", "D"}) {
        INFO(s);
        const TextResult r = makeTextSolid(s, st);
        REQUIRE(r.ok);
        CHECK(closed(r.mesh));
        CHECK(eulerNumber(r.mesh) == 0);
        CHECK(countPieces(r.mesh) == 1);
    }
    const TextResult oae = makeTextSolid("oae", st);
    REQUIRE(oae.ok);
    CHECK(closed(oae.mesh));
    CHECK(countPieces(oae.mesh) == 3);
    CHECK(eulerNumber(oae.mesh) == 0);  // three rings
    const TextResult b = makeTextSolid("B", st);
    REQUIRE(b.ok);
    CHECK(eulerNumber(b.mesh) == -2);  // two counters
    const TextResult i = makeTextSolid("i", st);
    REQUIRE(i.ok);
    CHECK(countPieces(i.mesh) == 2);  // stem and dot
    CHECK(eulerNumber(i.mesh) == 4);
}

TEST_CASE("Text: the hole of an o is really empty", "[ops][text]") {
    TextStyle st;
    st.height = 20;
    st.depth = 1;
    const TextResult o = makeTextSolid("o", st);
    REQUIRE(o.ok);
    const Solid s(o.mesh);
    REQUIRE(s.valid());
    // A disc through the middle: material only in the ring.
    const BBox3 b = s.bbox();
    const Solid core(makeBox({-0.2, -0.2, -1}, {0.2, 0.2, 2}));
    const CollisionResult c = checkCollision(s, core);
    REQUIRE(c.ok);
    CHECK_FALSE(c.overlaps);
    CHECK(b.size().x > 5.0);
    // The bounding box's filled area is far more than the letter's.
    CHECK(s.volume() < 0.8 * b.size().x * b.size().y * 1.0);
}

TEST_CASE("Text: size, spacing, lines and letters the font lacks", "[ops][text]") {
    TextStyle st;
    st.height = 8;
    st.depth = 1;
    const TextResult one = makeTextSolid("I", st);
    const TextResult three = makeTextSolid("III", st);
    const TextResult spaced = [&] {
        TextStyle s2 = st;
        s2.extra_spacing = 3;
        return makeTextSolid("III", s2);
    }();
    REQUIRE(one.ok);
    REQUIRE(three.ok);
    REQUIRE(spaced.ok);
    CHECK(three.mesh.bbox().size().x > 2 * one.mesh.bbox().size().x);
    CHECK(spaced.mesh.bbox().size().x == Approx(three.mesh.bbox().size().x + 6.0).epsilon(0.01));
    const TextResult lines = makeTextSolid("I\nI", st);
    REQUIRE(lines.ok);
    CHECK(lines.mesh.bbox().size().y == Approx(8.0 + 1.5 * 8.0).epsilon(0.01));
    CHECK(countPieces(lines.mesh) == 2);

    const TextResult accent = makeTextSolid("\xC3\xA9", st);  // e with an acute accent
    REQUIRE(accent.ok);
    CHECK(closed(accent.mesh));
    CHECK(countPieces(accent.mesh) == 2);  // the letter and its accent

    const TextResult missing = makeTextSolid("A\xF0\x9F\x98\x80", st);  // emoji
    REQUIRE(missing.ok);
    CHECK_THAT(missing.message, ContainsSubstring("Left out"));

    CHECK_FALSE(makeTextSolid("   ", st).ok);
    CHECK_FALSE(makeTextSolid("", st).ok);
    TextStyle bad = st;
    bad.height = 0;
    CHECK_FALSE(makeTextSolid("A", bad).ok);
    bad = st;
    bad.depth = -1;
    CHECK_FALSE(makeTextSolid("A", bad).ok);
}

TEST_CASE("Text: raised on a surface, engraved into it, and placed on a side face", "[ops][text]") {
    const TriangleMesh base = box(0, 0, 0, 60, 30, 10);
    TextStyle st;
    st.height = 8;
    st.depth = 1;
    TextPlacement pl;
    pl.point = {30, 15, 10};
    pl.normal = {0, 0, 1};
    pl.up = {0, 1, 0};

    const MeshResult raised = applyText(base, "Hi", st, pl);
    REQUIRE(raised.ok);
    CHECK(closed(raised.mesh));
    CHECK(raised.mesh.bbox().max.z == Approx(11.0));
    CHECK(raised.mesh.volume() > base.volume() + 10.0);
    CHECK(countPieces(raised.mesh) == 1);
    const BBox3 lb = raised.mesh.bbox();
    CHECK(lb.max.x == Approx(60.0));  // the base is still the widest

    pl.engrave = true;
    const MeshResult cut = applyText(base, "Hi", st, pl);
    REQUIRE(cut.ok);
    CHECK(closed(cut.mesh));
    CHECK(cut.mesh.bbox().max.z == Approx(10.0));
    const double removed = base.volume() - cut.mesh.volume();
    CHECK(removed > 10.0);
    // The same letters raised add as much as engraving takes away.
    CHECK(raised.mesh.volume() - base.volume() == Approx(removed).epsilon(0.001));

    // Where the letters sit: raised "I" is centred on the point, up toward +Y.
    pl.engrave = false;
    const MeshResult tall = applyText(base, "I", st, pl);
    REQUIRE(tall.ok);
    const auto above = cutByPlane(tall.mesh, {{0, 0, 10.5}, {0, 0, 1}}).above.bbox();
    CHECK(above.center().x == Approx(30.0).margin(0.01));
    CHECK(above.center().y == Approx(15.0).margin(0.01));
    CHECK(above.size().y == Approx(8.0).epsilon(0.01));

    // Rotated: top of the letters toward +X.
    pl.up = {1, 0, 0};
    const MeshResult turned = applyText(base, "I", st, pl);
    REQUIRE(turned.ok);
    const auto tb = cutByPlane(turned.mesh, {{0, 0, 10.5}, {0, 0, 1}}).above.bbox();
    CHECK(tb.size().x == Approx(8.0).epsilon(0.01));
    CHECK(tb.size().y < 3.0);

    // On the front face (normal -Y), top toward +Z.
    TextPlacement side;
    side.point = {30, 0, 5};
    side.normal = {0, -1, 0};
    side.up = {0, 0, 1};
    const MeshResult front = applyText(base, "I", st, side);
    REQUIRE(front.ok);
    CHECK(front.mesh.bbox().min.y == Approx(-1.0));
    CHECK(closed(front.mesh));
}

TEST_CASE("Text: letters that miss the surface change nothing and say so", "[ops][text]") {
    const TriangleMesh base = box(0, 0, 0, 20, 20, 10);
    TextStyle st;
    st.height = 5;
    st.depth = 1;
    TextPlacement pl;
    pl.point = {10, 10, 30};  // floating well above
    const MeshResult r = applyText(base, "A", st, pl);
    CHECK_FALSE(r.ok);
    CHECK_THAT(r.message, ContainsSubstring("did not meet"));
    pl.engrave = true;
    CHECK_FALSE(applyText(base, "A", st, pl).ok);
    TriangleMesh open = base;
    open.faces.pop_back();
    pl.point = {10, 10, 10};
    CHECK_FALSE(applyText(open, "A", st, pl).ok);
    pl.normal = {0, 0, 0};
    CHECK_FALSE(applyText(base, "A", st, pl).ok);
}

TEST_CASE("Text: placing letters without combining them", "[ops][text]") {
    const TextResult t = makeTextSolid("I", TextStyle{});
    REQUIRE(t.ok);
    TextPlacement pl;
    pl.point = {5, 6, 7};
    pl.normal = {1, 0, 0};
    pl.up = {0, 0, 1};
    const TriangleMesh p = placeText(t.mesh, pl, 0);
    CHECK(p.bbox().min.x == Approx(5.0));
    CHECK(p.bbox().max.x == Approx(5.0 + 1.0));
    CHECK(p.volume() == Approx(t.mesh.volume()));  // a turn, not a mirror
    // An up that lies along the normal still gives a usable frame.
    pl.up = {1, 0, 0};
    const TriangleMesh q = placeText(t.mesh, pl, 0);
    CHECK(q.volume() == Approx(t.mesh.volume()));
}

TEST_CASE("Arrange: footprints fit with the gap kept and nothing off the plate", "[ops][arrange]") {
    std::vector<Footprint> fp;
    for (int i = 0; i < 8; ++i) fp.push_back({20.0 + i, 15.0 + 2 * (i % 3)});
    ArrangeOptions o;
    o.plate_width = 100;
    o.plate_depth = 80;
    o.gap = 3;
    const ArrangeResult r = arrange(fp, o);
    REQUIRE(r.parts.size() == fp.size());
    CHECK(r.all_placed());
    for (size_t i = 0; i < fp.size(); ++i) {
        REQUIRE(r.parts[i].placed);
        const double w = r.parts[i].rotated ? fp[i].depth : fp[i].width;
        const double d = r.parts[i].rotated ? fp[i].width : fp[i].depth;
        CHECK(r.parts[i].centre.x - w / 2 >= -50.0 - 1e-9);
        CHECK(r.parts[i].centre.x + w / 2 <= 50.0 + 1e-9);
        CHECK(r.parts[i].centre.y - d / 2 >= -40.0 - 1e-9);
        CHECK(r.parts[i].centre.y + d / 2 <= 40.0 + 1e-9);
        for (size_t j = i + 1; j < fp.size(); ++j) {
            const double wj = r.parts[j].rotated ? fp[j].depth : fp[j].width;
            const double dj = r.parts[j].rotated ? fp[j].width : fp[j].depth;
            const double sx = std::abs(r.parts[i].centre.x - r.parts[j].centre.x) - (w + wj) / 2;
            const double sy = std::abs(r.parts[i].centre.y - r.parts[j].centre.y) - (d + dj) / 2;
            INFO(i << " and " << j);
            CHECK(std::max(sx, sy) >= o.gap - 1e-9);
        }
    }
}

TEST_CASE("Arrange: the same input always gives the same answer", "[ops][arrange]") {
    std::vector<Footprint> fp = {{30, 20}, {30, 20}, {10, 10}, {25, 40}, {10, 10}, {5, 60}};
    ArrangeOptions o;
    o.plate_width = 100;
    o.plate_depth = 100;
    o.allow_rotate = true;
    const ArrangeResult a = arrange(fp, o), b = arrange(fp, o);
    REQUIRE(a.parts.size() == b.parts.size());
    for (size_t i = 0; i < a.parts.size(); ++i) {
        CHECK(a.parts[i].placed == b.parts[i].placed);
        CHECK(a.parts[i].centre == b.parts[i].centre);
        CHECK(a.parts[i].rotated == b.parts[i].rotated);
    }
}

TEST_CASE("Arrange: what does not fit is reported, in input order", "[ops][arrange]") {
    ArrangeOptions o;
    o.plate_width = 50;
    o.plate_depth = 50;
    o.gap = 2;
    // Two 30 x 30 do not both fit on 50 x 50; the 60 wide one never does; zero size is skipped.
    const ArrangeResult r = arrange({{30, 30}, {60, 10}, {30, 30}, {0, 5}, {5, 5}}, o);
    REQUIRE(r.parts.size() == 5);
    CHECK_FALSE(r.all_placed());
    CHECK(r.not_placed == std::vector<int>{1, 2, 3});
    CHECK(r.parts[0].placed);
    CHECK(r.parts[4].placed);
    CHECK_FALSE(r.parts[1].placed);
    // An empty plate takes nothing; no parts is fine.
    CHECK(arrange({{1, 1}}, ArrangeOptions{}).not_placed == std::vector<int>{0});
    CHECK(arrange({}, o).all_placed());
}

TEST_CASE("Arrange: exact fits, edge margin, and quarter turns", "[ops][arrange]") {
    ArrangeOptions o;
    o.plate_width = 40;
    o.plate_depth = 20;
    o.gap = 2;
    // 19 + 2 + 19 = 40: exactly two side by side.
    CHECK(arrange({{19, 20}, {19, 20}}, o).all_placed());
    CHECK_FALSE(arrange({{19.1, 20}, {19.1, 20}}, o).all_placed());
    o.edge_margin = 1;  // now each side loses 1 mm
    CHECK_FALSE(arrange({{19, 20}, {19, 20}}, o).all_placed());
    const ArrangeResult one = arrange({{10, 10}}, o);
    REQUIRE(one.parts[0].placed);
    CHECK(one.parts[0].centre.x - 5 >= -20 + 1 - 1e-9);
    CHECK(one.parts[0].centre.y - 5 >= -10 + 1 - 1e-9);

    ArrangeOptions tall;
    tall.plate_width = 30;
    tall.plate_depth = 100;
    CHECK_FALSE(arrange({{90, 20}}, tall).all_placed());
    tall.allow_rotate = true;
    const ArrangeResult rot = arrange({{90, 20}}, tall);
    REQUIRE(rot.all_placed());
    CHECK(rot.parts[0].rotated);

    CHECK(footprintOf(box(0, 0, 0, 7, 9, 3)).width == Approx(7.0));
    CHECK(footprintOf(box(0, 0, 0, 7, 9, 3)).depth == Approx(9.0));
    CHECK(footprintOf(TriangleMesh{}).width == 0);
}

TEST_CASE("Arrange: sixty small parts on a big plate", "[ops][arrange]") {
    std::vector<Footprint> fp(60, Footprint{20, 15});
    ArrangeOptions o;
    o.plate_width = 218;  // a large resin printer's plate
    o.plate_depth = 123;
    o.gap = 3;
    const ArrangeResult r = arrange(fp, o);
    // 9 across (9*20 + 8*3 = 204) by 7 down (7*15 + 6*3 = 123): 63 fit, so all do.
    CHECK(r.all_placed());
    std::set<std::pair<long, long>> spots;
    for (const auto& p : r.parts) spots.insert({std::lround(p.centre.x * 1000), std::lround(p.centre.y * 1000)});
    CHECK(spots.size() == 60);
}

TEST_CASE("Performance: a 200 000 triangle part is combined, cut and checked in seconds", "[ops][perf]") {
    const TriangleMesh ball = makeSphere({0, 0, 0}, 20, 450);
    REQUIRE(ball.faceCount() > 190000);
    using clock = std::chrono::steady_clock;
    auto seconds = [](clock::time_point t) { return std::chrono::duration<double>(clock::now() - t).count(); };

    auto t = clock::now();
    const Solid s(ball);
    REQUIRE(s.valid());
    const double t_convert = seconds(t);

    t = clock::now();
    const Solid peg(makeCylinder({0, 0, -30}, 4, 60, 64));
    const Solid hole = subtract(s, peg);
    REQUIRE(hole.valid());
    const double t_subtract = seconds(t);
    CHECK(hole.volume() < s.volume());

    t = clock::now();
    const auto [up, down] = cutByPlane(s, {{0, 0, 3}, {0.2, 0.1, 1}});
    REQUIRE(up.valid());
    const double t_cut = seconds(t);
    CHECK(up.volume() + down.volume() == Approx(s.volume()).epsilon(1e-6));

    t = clock::now();
    const Solid other(makeSphere({30, 0, 0}, 20, 450));
    const CollisionResult c = checkCollision(s, other, 1e-3, 5.0);
    REQUIRE(c.ok);
    const double t_collide = seconds(t);
    CHECK(c.overlaps);

    t = clock::now();
    const TriangleMesh back = hole.toMesh();
    const double t_back = seconds(t);
    CHECK(back.faceCount() > 150000);

    INFO("convert " << t_convert << " s, subtract " << t_subtract << " s, cut " << t_cut << " s, collide "
                    << t_collide << " s, back " << t_back << " s");
    // Generous: the thread sanitizer job runs this several times slower.
    CHECK(t_convert + t_subtract + t_cut + t_collide + t_back < 120.0);
    WARN("200k triangles: convert " << t_convert << " s, subtract " << t_subtract << " s, cut " << t_cut
                                    << " s, collide " << t_collide << " s, back " << t_back << " s");
}
