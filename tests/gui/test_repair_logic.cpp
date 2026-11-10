// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Mesh repair as the app shows it: what a faulty file loads as, the words
// for what was done, and a saved job keeping them.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>

#include "Document.h"
#include "JobFile.h"
#include "Repairs.h"
#include "mesh/Primitives.h"
#include "mesh/StlIO.h"

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using namespace gui;
namespace fs = std::filesystem;

namespace {

fs::path dir() {
    const auto d = fs::temp_directory_path() / "fruit_repair_tests";
    fs::create_directories(d);
    return d;
}

std::shared_ptr<Document> open(const fruit::TriangleMesh& m, const char* name) {
    const auto path = dir() / name;
    fruit::writeStl(path.string(), m);
    return loadDocument(path.string(), fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
}

using Level = RepairWords::Level;

}  // namespace

TEST_CASE("Repair: a sound model says nothing of repair", "[gui][repair]") {
    const auto doc = open(demoModel(), "sound.stl");
    const RepairWords w = repairWords(doc->check);
    CHECK(w.level == Level::None);
    CHECK(w.title.empty());
    CHECK(doc->check.closed());
    CHECK_FALSE(doc->check.repair.changed());
}

TEST_CASE("Repair: a worn file loads repaired, with nothing worth a look", "[gui][repair]") {
    const auto doc = open(demoFaultyModel("worn"), "worn.stl");
    const fruit::MeshCheck& c = doc->check;
    CHECK(c.closed());
    CHECK(c.repair.open_edges_before > 0);
    CHECK(c.repair.holes_filled == 1);
    CHECK(c.repair.faces_flipped == 3);
    CHECK(c.repair.faces_removed == 1);
    CHECK(c.repair.patch_faces == 2);
    CHECK(c.shells == 4);
    CHECK(c.flipped == 0);
    // The same solid as the bracket without its faults.
    CHECK(c.volume_ml == Approx(fruit::checkMesh(fruit::prepareModel(demoModel())).volume_ml).epsilon(1e-3));
    const RepairWords w = repairWords(c);
    CHECK(w.level == Level::Repaired);
    CHECK(w.title == "Repaired: now watertight");
    CHECK(w.detail == "FruitSlicer closed a hole 9.0 mm across, turned 3 faces that faced the wrong way and removed a "
                      "doubled or loose face. No point of the model was moved, and the patches are tinted on it.");
    CHECK(w.brief == "1 hole closed, 3 faces turned, 1 stray face removed.");
    // The patch is on the post's side, facing out level.
    CHECK(c.repair.biggest_patch_facing.z == Approx(0).margin(1e-6));
    CHECK(c.repair.biggest_patch_across == Approx(9.0).epsilon(0.01));
}

TEST_CASE("Repair: big holes are closed, and flagged to look at", "[gui][repair]") {
    const auto doc = open(demoFaultyModel("open"), "open.stl");
    const fruit::MeshCheck& c = doc->check;
    CHECK(c.closed());
    CHECK(c.repair.holes_filled == 2);
    CHECK(c.repair.largest_hole == Approx(std::hypot(17.0, 12.0)).epsilon(1e-3));  // the shelf's top
    CHECK(c.repair.biggest_patch_area == Approx(17.0 * 12.0).epsilon(1e-6));
    CHECK(c.repair.biggest_patch_facing.z == Approx(1));  // it faces up
    CHECK(c.volume_ml == Approx(fruit::checkMesh(fruit::prepareModel(demoModel())).volume_ml).epsilon(1e-6));
    const RepairWords w = repairWords(c);
    CHECK(w.level == Level::Check);
    CHECK(w.title == "Repaired, with a big patch");
    CHECK_THAT(w.detail, ContainsSubstring("closed 2 holes (the largest 20.8 mm across)"));
    CHECK_THAT(w.detail, ContainsSubstring("look at the tinted patches"));
    CHECK(w.brief == "2 holes closed.");
}

TEST_CASE("Repair: loose surfaces stay as they are, and say so", "[gui][repair]") {
    const auto doc = open(demoFaultyModel("loose"), "loose.stl");
    const fruit::MeshCheck& c = doc->check;
    CHECK_FALSE(c.closed());
    CHECK(c.open_edges == 24);
    CHECK(c.triangles == 12);
    CHECK_FALSE(c.repair.changed());
    const RepairWords w = repairWords(c);
    CHECK(w.level == Level::Open);
    CHECK(w.title == "Not watertight");
    CHECK_THAT(w.detail, ContainsSubstring("24 edges are open and FruitSlicer could not close them"));
    CHECK(w.brief == "24 open edges that could not be closed.");
}

TEST_CASE("Repair: the words for cracks, single faces and heavy patching", "[gui][repair]") {
    fruit::MeshCheck c;
    c.triangles = 1000;
    c.bbox.extend({0, 0, 0});
    c.bbox.extend({30, 40, 0});  // 50 mm across
    c.repair.open_edges_before = 8;
    c.repair.gaps_closed = 1;
    c.repair.widest_gap = 0.021;
    c.repair.patch_faces = 6;
    c.repair.faces_flipped = 1;
    RepairWords w = repairWords(c);
    CHECK(w.level == Level::Repaired);
    CHECK_THAT(w.detail, ContainsSubstring("closed a crack about 0.02 mm wide and turned a face that faced the wrong way."));
    CHECK(w.brief == "1 crack closed, 1 face turned.");

    c.repair.gaps_closed = 1240;
    c.repair.widest_gap = 0;
    c.repair.faces_flipped = 0;
    c.repair.patch_faces = 40;
    w = repairWords(c);
    CHECK(w.level == Level::Repaired);
    CHECK_THAT(w.detail, ContainsSubstring("FruitSlicer closed 1,240 cracks. No point"));

    // Faces only turned: nothing was patched, so no tint to speak of.
    c.repair = fruit::RepairReport{};
    c.repair.open_edges_before = 6;
    c.repair.faces_flipped = 2;
    w = repairWords(c);
    CHECK(w.level == Level::Repaired);
    CHECK(w.detail == "FruitSlicer turned 2 faces that faced the wrong way. No point of the model was moved.");

    // A hole whose patch, as a disc, is over a tenth of the model's size
    // across (5 mm here), or patches that are over a twentieth of the faces.
    c.repair = fruit::RepairReport{};
    c.repair.open_edges_before = 30;
    c.repair.holes_filled = 1;
    c.repair.largest_hole = 6;
    c.repair.patch_faces = 10;
    c.repair.biggest_patch_area = 19;  // 4.9 mm as a disc
    CHECK(repairWords(c).level == Level::Repaired);
    c.repair.biggest_patch_area = 21;  // 5.2 mm
    CHECK(repairWords(c).level == Level::Check);
    c.repair.biggest_patch_area = 2;
    c.repair.patch_faces = 51;
    w = repairWords(c);
    CHECK(w.level == Level::Check);
    CHECK(w.title == "Repaired, with a lot of patching");

    // Sizes are at full size, which for a file drawn in metres is small:
    // still digits that mean something, never "0.0 mm".
    c.repair = fruit::RepairReport{};
    c.repair.open_edges_before = 223;
    c.repair.holes_filled = 5;
    c.repair.patch_faces = 10;
    c.repair.largest_hole = 0.0439;
    CHECK_THAT(repairWords(c).detail, ContainsSubstring("closed 5 holes (the largest 0.044 mm across)"));
    c.repair.largest_hole = 0.437;
    CHECK_THAT(repairWords(c).detail, ContainsSubstring("(the largest 0.44 mm across)"));
    c.repair.largest_hole = 13.18;
    CHECK_THAT(repairWords(c).detail, ContainsSubstring("(the largest 13.2 mm across)"));
    c.repair.holes_filled = 1;
    c.repair.largest_hole = 0.0061;
    CHECK_THAT(repairWords(c).detail, ContainsSubstring("closed a hole 0.0061 mm across"));
}

TEST_CASE("Repair: a saved job keeps what was repaired", "[gui][repair][jobs]") {
    const auto doc = open(demoFaultyModel("open"), "open for job.stl");
    const auto job = (dir() / "open.fruitjob").string();
    saveJob(job, *doc);
    const OpenedJob o = openJob(job, fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    const Document& d = *o.doc;
    // The job's own copy is the repaired mesh, so there is nothing to
    // repair again; what was done the first time comes back with it.
    CHECK(d.check.closed());
    CHECK(meshHash(d.model.mesh) == meshHash(doc->model.mesh));
    CHECK(d.check.repair.holes_filled == 2);
    CHECK(d.check.repair.patch_faces == doc->check.repair.patch_faces);
    CHECK(d.check.repair.largest_hole == Approx(doc->check.repair.largest_hole));
    CHECK(d.check.repair.biggest_patch_at.z == Approx(doc->check.repair.biggest_patch_at.z));
    CHECK(d.check.repair.biggest_patch_facing.z == Approx(1));
    CHECK(repairWords(d.check).title == "Repaired, with a big patch");
    CHECK_FALSE(d.unsaved());
    // The file itself still loads to the same mesh: not "changed since".
    CHECK(meshHash(fruit::loadModel(doc->path).mesh) == meshHash(d.model.mesh));
}
