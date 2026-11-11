// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// On plate: laying a flat face on the plate.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include <memory>

#include "Document.h"
#include "JobFile.h"
#include "LayFace.h"
#include "Placing.h"
#include "mesh/Primitives.h"
#include "mesh/StlIO.h"

using Catch::Approx;
using namespace gui;
namespace fs = std::filesystem;

namespace {

const fruit::PrinterProfile kPrinter = fruit::PrinterProfile::saturn4Ultra16k();

std::shared_ptr<Document> docFor(const fruit::TriangleMesh& m, const char* name) {
    const auto d = fs::temp_directory_path() / "fruit_lay_face_tests";
    fs::create_directories(d);
    const auto path = d / name;
    fruit::writeStl(path.string(), m);
    return loadDocument(path.string(), kPrinter, fruit::JobControl::none());
}

// The lowest corner of a face's triangles and the highest, in printer coordinates.
void faceHeights(const Document& doc, const fruit::FlatFace& f, double* lo, double* hi) {
    const fruit::Transform& t = doc.part().transform();
    const fruit::TriangleMesh& m = doc.part().mesh();
    *lo = 1e30;
    *hi = -1e30;
    for (const int32_t tri : f.faces)
        for (int k = 0; k < 3; ++k) {
            const double z = t.apply(m.vertices[static_cast<std::size_t>(m.faces[static_cast<std::size_t>(tri)][static_cast<std::size_t>(k)])]).z;
            *lo = std::min(*lo, z);
            *hi = std::max(*hi, z);
        }
}

void layEachFace(Document& doc) {
    const auto flat = doc.part().flatFaces();  // a copy: the part changes under it
    REQUIRE(flat.faces.size() == 6);
    const double volume = doc.placed().volume();
    for (const fruit::FlatFace& f : flat.faces) {
        const fruit::Placement before = doc.placement();
        const fruit::Vec2 middle = {doc.part().box().center().x, doc.part().box().center().y};
        doc.edit(laidFaceDown(before, f.normal));
        const fruit::Vec3 w = printerDirection(doc.placement(), f.normal);
        CHECK(w.x == Approx(0).margin(1e-6));
        CHECK(w.y == Approx(0).margin(1e-6));
        CHECK(w.z == Approx(-1).margin(1e-9));
        double lo, hi;
        faceHeights(doc, f, &lo, &hi);
        CHECK(lo == Approx(doc.placement().lift()).margin(1e-5));
        CHECK(hi == Approx(doc.placement().lift()).margin(1e-5));
        CHECK(doc.part().box().min.z == Approx(doc.placement().lift()).margin(1e-5));
        CHECK(doc.placed().volume() == Approx(volume).epsilon(1e-9));
        CHECK(doc.part().box().center().x == Approx(middle.x).margin(1e-6));
        CHECK(doc.part().box().center().y == Approx(middle.y).margin(1e-6));
        CHECK(doc.placement().offset == before.offset);
        if (!(doc.placement() == before)) {
            doc.undo();
            CHECK(doc.placement() == before);
            doc.redo();
        }
    }
}

}  // namespace

TEST_CASE("Lay face: a box laid on each of its faces", "[gui][layface]") {
    auto doc = docFor(fruit::makeBox({0, 0, 0}, {20, 30, 40}), "box.stl");
    layEachFace(*doc);
}

TEST_CASE("Lay face: from a turned and mirrored placement", "[gui][layface]") {
    auto doc = docFor(fruit::makeBox({0, 0, 0}, {20, 30, 40}), "box_mirrored.stl");
    fruit::Placement p = doc->placement();
    p.mirror_x = true;
    p.rotation_deg = {30, 40, 50};
    p.offset = {12, -7};
    doc->edit(p);
    layEachFace(*doc);
    fruit::Placement q = doc->placement();
    q.mirror_x = false;
    q.mirror_z = true;
    q.rotation_deg = {-100, 170, 20};
    doc->edit(q);
    layEachFace(*doc);
}

TEST_CASE("Lay face: a face already down changes nothing, and laying is one undo step", "[gui][layface]") {
    auto doc = docFor(fruit::makeBox({0, 0, 0}, {20, 30, 40}), "box_down.stl");
    const fruit::Placement start = doc->placement();
    const auto& flat = doc->part().flatFaces();
    int down = -1;
    for (std::size_t i = 0; i < flat.faces.size(); ++i)
        if (facesDown(printerDirection(start, flat.faces[i].normal))) down = static_cast<int>(i);
    REQUIRE(down >= 0);
    CHECK(laidFaceDown(start, flat.faces[static_cast<std::size_t>(down)].normal) == start);
    const int revision = doc->revision();
    int other = (down + 1) % 6;
    doc->edit(laidFaceDown(start, flat.faces[static_cast<std::size_t>(other)].normal));
    CHECK(doc->revision() != revision);
    doc->undo();
    CHECK(doc->placement() == start);
    CHECK(doc->revision() == revision);
}

TEST_CASE("Lay face: only the selected part turns", "[gui][layface]") {
    auto doc = docFor(fruit::makeBox({0, 0, 0}, {20, 30, 40}), "two.stl");
    NewPart second{"second", "", doc->part().shared(), doc->placement()};
    second.placement.offset = {40, 0};
    const int other = doc->addPart(second);
    doc->select(doc->parts().front().id());
    const fruit::Placement kept = doc->findPart(other)->placement();
    const auto& flat = doc->part().flatFaces();
    doc->edit(laidFaceDown(doc->placement(), flat.faces[3].normal));
    CHECK(doc->findPart(other)->placement() == kept);
    CHECK(doc->placement().rotation_deg.x + doc->placement().rotation_deg.y + doc->placement().rotation_deg.z != 0);
}

TEST_CASE("Lay face: faces are named by where they point, and numbered when names repeat", "[gui][layface]") {
    CHECK(faceDirectionName({0, 0, -1}) == "Bottom");
    CHECK(faceDirectionName({0, 0, 1}) == "Top");
    CHECK(faceDirectionName({0, -1, 0}) == "Front");
    CHECK(faceDirectionName({0, 1, 0}) == "Back");
    CHECK(faceDirectionName({-1, 0, 0}) == "Left side");
    CHECK(faceDirectionName({1, 0, 0}) == "Right side");
    CHECK(faceDirectionName({1, 0, 1}) == "Slanted face");
    const auto n = faceNames({{0, 1, 0}, {1, 1, 1}, {0, 1, 0.1}, {0, 0, 1}, {1, -1, 1}});
    CHECK(n[0] == "Back 1");
    CHECK(n[1] == "Slanted face 1");
    CHECK(n[2] == "Back 2");
    CHECK(n[3] == "Top");
    CHECK(n[4] == "Slanted face 2");
}

TEST_CASE("On plate: any surface can be picked: a resting face, another flat face, a curved spot", "[gui][lay-face]") {
    // An L (the foot's top is flat but can't be rested on) with a ball on the end of the foot.
    fruit::TriangleMesh m = fruit::makeBox({0, 0, 0}, {30, 10, 5});
    m.append(fruit::makeBox({0, 0, 5}, {10, 10, 30}));
    m.append(fruit::makeSphere({30, 5, 8}, 4, 48));
    const auto doc = docFor(m, "l_and_ball.stl");
    REQUIRE(doc);
    const Part& part = doc->part();
    const fruit::FlatFaces& ff = part.flatFaces();
    const fruit::Transform& tf = part.transform();

    // The bottom: a face it rests on, facing the plate already.
    int bottom = -1;
    for (std::size_t i = 0; i < ff.faces.size(); ++i)
        if (ff.faces[i].normal.z < -0.99) bottom = static_cast<int>(i);
    REQUIRE(bottom >= 0);
    const fruit::FlatFace& b = ff.faces[static_cast<std::size_t>(bottom)];
    const SurfacePick on = surfaceAt(part, b.faces.front(), tf.apply(b.centroid));
    CHECK(on.kind == SurfacePick::Kind::Resting);
    CHECK(on.region == bottom);
    CHECK(on.facing_down);
    CHECK(on.name == "Bottom");

    // The foot's top: flat, but the upright rises past it.
    int top = -1;
    for (std::size_t i = 0; i < ff.others.size(); ++i)
        if (ff.others[i].normal.z > 0.99 && ff.others[i].centroid.z == Approx(5).margin(1e-6)) top = static_cast<int>(i);
    REQUIRE(top >= 0);
    const fruit::FlatFace& t = ff.others[static_cast<std::size_t>(top)];
    const SurfacePick foot = surfaceAt(part, t.faces.front(), tf.apply(t.centroid));
    CHECK(foot.kind == SurfacePick::Kind::Flat);
    CHECK(foot.name == "Top");
    CHECK(foot.area == Approx(30 * 10).margin(1e-6));
    CHECK_FALSE(foot.facing_down);

    // A triangle of the ball: curved, its own normal.
    int ball = -1;
    for (std::size_t i = 0; i < part.mesh().faces.size(); ++i)
        if (ff.regionOf(i) < 0 && ff.otherOf(i) < 0) {
            ball = static_cast<int>(i);
            break;
        }
    REQUIRE(ball >= 0);
    const SurfacePick spot = surfaceAt(part, ball, {0, 0, 0});
    CHECK(spot.kind == SurfacePick::Kind::Curved);
    const fruit::Vec3 n = part.mesh().faceNormal(static_cast<std::size_t>(ball));
    CHECK(spot.normal.x == Approx(n.x));
    CHECK(spot.normal.y == Approx(n.y));
    CHECK(spot.normal.z == Approx(n.z));

    // Turning that spot to the plate points it straight down; the model keeps its volume and sits at the lift.
    const double volume = doc->placed().volume();
    doc->edit(laidFaceDown(doc->placement(), spot.normal));
    const fruit::Vec3 w = printerDirection(doc->placement(), spot.normal);
    CHECK(w.x == Approx(0).margin(1e-6));
    CHECK(w.y == Approx(0).margin(1e-6));
    CHECK(w.z == Approx(-1).margin(1e-9));
    CHECK(doc->placed().volume() == Approx(volume).epsilon(1e-9));
    CHECK(doc->part().box().min.z == Approx(doc->placement().lift()).margin(1e-6));
    // So is the foot's top, which then faces the plate but doesn't touch it: the upright reaches lower.
    doc->edit(laidFaceDown(doc->placement(), t.normal));
    CHECK(printerDirection(doc->placement(), t.normal).z == Approx(-1).margin(1e-9));
    double lo, hi;
    faceHeights(*doc, t, &lo, &hi);
    CHECK(lo > doc->placement().lift() + 1);
    doc->undo();
    doc->undo();
    CHECK(printerDirection(doc->placement(), b.normal).z == Approx(-1).margin(1e-9));
}
