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
