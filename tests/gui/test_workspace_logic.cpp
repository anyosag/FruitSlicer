// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The workspace around the 3D view: which place each tool opens and which
// tool's panel a place shows, the Job panel's main button through a job, and
// the view cube's faces.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include <memory>
#include <set>

#include "Document.h"
#include "Navigation.h"
#include "ViewCube.h"
#include "mesh/Primitives.h"
#include "mesh/StlIO.h"
#include "view3d/Camera.h"

using Catch::Approx;
using namespace gui;
namespace fs = std::filesystem;

namespace {

const fruit::PrinterProfile kPrinter = fruit::PrinterProfile::saturn4Ultra16k();

std::shared_ptr<Document> docFor(const fruit::TriangleMesh& m, const char* name) {
    const auto d = fs::temp_directory_path() / "fruit_workspace_tests";
    fs::create_directories(d);
    const auto path = d / name;
    fruit::writeStl(path.string(), m);
    return loadDocument(path.string(), kPrinter, fruit::JobControl::none());
}

void freshSupports(Document& d) {
    auto r = std::make_shared<SupportResult>();
    r->choices = d.supports.choices;
    r->edits = d.supports.edits;
    d.supports.result = r;
    d.supports.result_revision = d.revision();
    d.supports.result_settings = d.supportSettings();
}

}  // namespace

TEST_CASE("Workspace: each tool opens its place, and each place shows its tool", "[gui]") {
    CHECK(stepFor(Tool::OnPlate) == Step::Model);
    CHECK(stepFor(Tool::Move) == Step::Model);
    CHECK(stepFor(Tool::Scale) == Step::Model);
    CHECK(stepFor(Tool::Repair) == Step::Model);
    CHECK(stepFor(Tool::Mirror) == Step::Model);
    CHECK(stepFor(Tool::Measure) == Step::Model);
    CHECK(stepFor(Tool::Rotate) == Step::Orient);
    CHECK(stepFor(Tool::Hollow) == Step::Orient);
    CHECK(stepFor(Tool::Supports) == Step::Supports);
    // The way back: a place shows the tool that opens it.
    for (Tool t : {Tool::OnPlate, Tool::Move, Tool::Rotate, Tool::Scale, Tool::Supports, Tool::Hollow, Tool::Repair, Tool::Mirror,
                   Tool::Measure})
        CHECK(toolShown(stepFor(t), t == Tool::Hollow, t) == t);
    // Model shows only its own tools; Layers and Print show none.
    CHECK(toolShown(Step::Model, false, Tool::Supports) == Tool::None);
    CHECK(toolShown(Step::Layers, false, Tool::Move) == Tool::None);
    CHECK(toolShown(Step::Print, false, Tool::None) == Tool::None);
    CHECK(inLayersMode(Step::Layers));
    CHECK(inLayersMode(Step::Print));
    CHECK_FALSE(inLayersMode(Step::Supports));
}

TEST_CASE("Workspace: the main button is always the next thing the job needs", "[gui]") {
    CHECK(mainButton(nullptr).action == MainAction::OpenModel);
    CHECK(mainButton(nullptr).enabled);

    const auto doc = docFor(fruit::makeBox({0, 0, 0}, {20, 20, 10}), "ws_box.stl");
    REQUIRE(doc->fit().fits);
    CHECK(mainButton(doc.get()).action == MainAction::MakeSupports);
    CHECK(mainButton(doc.get()).enabled);

    freshSupports(*doc);
    CHECK(mainButton(doc.get()).action == MainAction::Slice);

    // Layers cut for these supports: the button shows them.
    doc->layers.result = std::make_shared<LayerResult>();
    doc->layers.for_supports = doc->supports.result;
    CHECK(mainButton(doc.get()).action == MainAction::ShowLayers);

    // Moving it puts the supports out of date: they must be made again.
    fruit::Placement p = doc->placement();
    p.offset = {12, -8};
    doc->place(p);
    CHECK(mainButton(doc.get()).action == MainAction::RemakeSupports);
    CHECK(mainButton(doc.get()).enabled);

    // Too big where it is: still "Make supports", but it can't be pressed.
    const auto big = docFor(fruit::makeBox({0, 0, 0}, {13.4, 154.0, 10.0}), "ws_bar.stl");
    REQUIRE_FALSE(big->fit().fits);
    CHECK(mainButton(big.get()).action == MainAction::MakeSupports);
    CHECK_FALSE(mainButton(big.get()).enabled);

    CHECK(std::string(mainLabel(MainAction::MakeSupports)) == "Make supports");
    CHECK(std::string(mainLabel(MainAction::Slice)) == "Slice");
}

TEST_CASE("View cube: from the start view it shows the top, the front and the right", "[gui]") {
    Camera cam;
    cam.preset(ViewPreset::Angled);
    const auto faces = cubeFaces(cam.view(), 40);
    std::set<CubeSide> seen;
    for (const CubeFace& f : faces) seen.insert(f.side);
    CHECK(seen == std::set<CubeSide>{CubeSide::Top, CubeSide::Front, CubeSide::Right});
    // Back to front: the front faces the camera most squarely, so it comes last.
    REQUIRE(faces.size() == 3);
    CHECK(faces.back().side == CubeSide::Front);
    for (std::size_t i = 1; i < faces.size(); ++i) CHECK(faces[i - 1].facing <= faces[i].facing);

    // A point inside the front face's middle hits the front; the corner of the
    // box around the cube hits nothing.
    const CubeFace* front = &faces.back();
    float mx = 0, my = 0;
    for (int k = 0; k < 4; ++k) {
        mx += front->x[k] / 4;
        my += front->y[k] / 4;
    }
    const int hit = cubeFaceAt(faces, mx, my);
    REQUIRE(hit >= 0);
    CHECK(faces[static_cast<std::size_t>(hit)].side == CubeSide::Front);
    CHECK(cubeFaceAt(faces, 79, -79) == -1);
    // The top shows above the front on screen (y grows downwards).
    const CubeFace* top = nullptr;
    for (const CubeFace& f : faces)
        if (f.side == CubeSide::Top) top = &f;
    REQUIRE(top);
    float ty = 0;
    for (int k = 0; k < 4; ++k) ty += top->y[k] / 4;
    CHECK(ty < my);
}

TEST_CASE("View cube: a click on a face looks from that side", "[gui]") {
    for (CubeSide s : {CubeSide::Right, CubeSide::Left, CubeSide::Back, CubeSide::Front, CubeSide::Top}) {
        Camera cam;
        cam.lookFrom(cubeDirection(s));
        const auto faces = cubeFaces(cam.view(), 40);
        REQUIRE_FALSE(faces.empty());
        // The side looked from is the one facing the camera most squarely.
        CHECK(faces.back().side == s);
    }
    CHECK(std::string(cubeLabel(CubeSide::Top)) == "Top");
}

TEST_CASE("View cube: a face's middle, its edges and its corners are spots to look from", "[gui]") {
    // Looking from the back the cube is one square, the back face.
    Camera cam;
    cam.lookFrom(cubeDirection(CubeSide::Back));
    const auto faces = cubeFaces(cam.view(), 40);
    REQUIRE(faces.size() == 1);
    // Its middle looks from the back.
    CubeSpot s = cubeSpotAt(faces, 0, 0);
    REQUIRE(s.face == 0);
    CHECK(s.sides() == 1);
    CHECK((s.dir.x == 0 && s.dir.y == 1 && s.dir.z == 0));
    CHECK(cubeSpotName(s.dir) == "back");
    // The middle's outline is the middle of the face.
    for (int k = 0; k < 4; ++k) {
        CHECK(std::abs(s.x[k]) == Approx(40 * kCubeEdge).margin(0.01));
        CHECK(std::abs(s.y[k]) == Approx(40 * kCubeEdge).margin(0.01));
    }
    // From the back, the model's left is on the right of the screen: that edge looks from the back left.
    s = cubeSpotAt(faces, 36, 0);
    CHECK(s.sides() == 2);
    CHECK((s.dir.x == -1 && s.dir.y == 1 && s.dir.z == 0));
    CHECK(cubeSpotName(s.dir) == "back left");
    // The corner at the top right of the screen (y grows downwards): top, back, left.
    s = cubeSpotAt(faces, 36, -36);
    CHECK(s.sides() == 3);
    CHECK((s.dir.x == -1 && s.dir.y == 1 && s.dir.z == 1));
    CHECK(cubeSpotName(s.dir) == "top back left");
    // The edge at the bottom of the screen.
    s = cubeSpotAt(faces, 0, 36);
    CHECK((s.dir.x == 0 && s.dir.y == 1 && s.dir.z == -1));
    CHECK(cubeSpotName(s.dir) == "bottom back");
    // Just inside the middle's edge is still the middle; off the cube is nothing.
    CHECK(cubeSpotAt(faces, 40 * kCubeEdge - 1, 0).sides() == 1);
    CHECK(cubeSpotAt(faces, 60, 0).face == -1);
    CHECK(cubeSpotName({0, -1, 0}) == "front");
    CHECK(cubeSpotName({1, -1, 0}) == "front right");
    CHECK(cubeSpotName({1, -1, 1}) == "top front right");
    CHECK(cubeSpotName({0, 0, -1}) == "bottom");

    // On the tilted cube of the start view: the front face's middle, and its corner towards the top right.
    Camera start;
    start.preset(ViewPreset::Angled);
    const auto tilted = cubeFaces(start.view(), 40);
    const CubeFace& front = tilted.back();
    REQUIRE(front.side == CubeSide::Front);
    const float mx = (front.x[0] + front.x[2]) / 2, my = (front.y[0] + front.y[2]) / 2;
    s = cubeSpotAt(tilted, mx, my);
    CHECK(s.sides() == 1);
    CHECK((s.dir.x == 0 && s.dir.y == -1 && s.dir.z == 0));
    // (the front face's third corner is its top right one: +X, +Z)
    s = cubeSpotAt(tilted, mx + 0.9f * (front.x[2] - mx), my + 0.9f * (front.y[2] - my));
    CHECK(s.sides() == 3);
    CHECK((s.dir.x == 1 && s.dir.y == -1 && s.dir.z == 1));
    CHECK(cubeSpotName(s.dir) == "top front right");
}

TEST_CASE("View cube: an edge or a corner looks from between its sides", "[gui]") {
    auto seenFrom = [](V3 dir) {
        Camera cam;
        cam.lookFrom(dir);
        std::set<CubeSide> seen;
        for (const CubeFace& f : cubeFaces(cam.view(), 40)) seen.insert(f.side);
        return seen;
    };
    // A corner shows its three faces, an edge its two.
    CHECK(seenFrom({1, -1, 1}) == std::set<CubeSide>{CubeSide::Top, CubeSide::Front, CubeSide::Right});
    CHECK(seenFrom({-1, 1, -1}) == std::set<CubeSide>{CubeSide::Bottom, CubeSide::Back, CubeSide::Left});
    CHECK(seenFrom({1, -1, 0}) == std::set<CubeSide>{CubeSide::Front, CubeSide::Right});
    CHECK(seenFrom({0, 1, 1}) == std::set<CubeSide>{CubeSide::Top, CubeSide::Back});
    // From a corner the three faces face the camera equally.
    Camera cam;
    cam.lookFrom({1, -1, 1});
    for (const CubeFace& f : cubeFaces(cam.view(), 40)) CHECK(f.facing == Approx(1 / std::sqrt(3.0f)).margin(0.01));
}

TEST_CASE("Camera: turning by angles is what dragging in the view does", "[gui]") {
    Camera a, b;
    a.orbit(40, -20);
    b.turn(-40 * 0.35f, -20 * 0.35f);
    CHECK(a.eye().x == Approx(b.eye().x));
    CHECK(a.eye().y == Approx(b.eye().y));
    CHECK(a.eye().z == Approx(b.eye().z));
    // Held just short of straight up, and of straight down.
    Camera up;
    up.turn(0, 500);
    CHECK((up.eye().z - up.target().z) / up.distance() == Approx(std::sin(89 * 3.14159265f / 180)).margin(1e-4));
    Camera down;
    down.turn(0, -500);
    CHECK((down.eye().z - down.target().z) / down.distance() == Approx(-std::sin(89 * 3.14159265f / 180)).margin(1e-4));
    // Dragging the cube by its own width (60 pixels) turns the view by about a quarter turn.
    CHECK(60 * kCubeDragDegPerPx == Approx(90).margin(10));
}

TEST_CASE("Camera: a sideways shift moves the picture and its rays together", "[gui]") {
    Camera cam;
    cam.preset(ViewPreset::Angled);
    const float aspect = 1.4f;
    // The target sits at the middle of the picture, or as far off it as the shift.
    auto ndcOf = [&](V3 w) {
        const M4 vp = cam.proj(aspect) * cam.view();
        const float x = vp.m[0] * w.x + vp.m[4] * w.y + vp.m[8] * w.z + vp.m[12];
        const float c = vp.m[3] * w.x + vp.m[7] * w.y + vp.m[11] * w.z + vp.m[15];
        return x / c;
    };
    CHECK(ndcOf(cam.target()) == Approx(0).margin(1e-5));
    cam.setShift(0.4f);
    CHECK(ndcOf(cam.target()) == Approx(0.4).margin(1e-5));
    // The ray through the shifted middle still passes through the target.
    V3 o, d;
    cam.ray(0.4f, 0, aspect, &o, &d);
    const V3 to{cam.target().x - o.x, cam.target().y - o.y, cam.target().z - o.z};
    const float len = std::sqrt(dot(to, to));
    CHECK(dot(to, d) / len == Approx(1).margin(1e-5));
}
