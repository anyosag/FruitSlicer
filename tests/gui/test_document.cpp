// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The app's job document: loading, placement edits, undo and redo.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

#include "Document.h"
#include "mesh/StlIO.h"

using Catch::Approx;
using gui::Document;

namespace {

std::shared_ptr<Document> loadDemo(const char* name = "doc_test_bracket.stl") {
    const auto path = std::filesystem::temp_directory_path() / name;
    fruit::writeStl(path.string(), gui::demoModel());
    auto doc = gui::loadDocument(path.string(), fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    std::filesystem::remove(path);
    return doc;
}

}  // namespace

TEST_CASE("Document: loading checks the mesh and places it raised", "[gui]") {
    const auto doc = loadDemo();
    CHECK(doc->name == "doc_test_bracket");
    CHECK(doc->check.closed());
    CHECK(doc->check.shells == 4);
    CHECK(doc->low_points >= 1);
    CHECK(doc->fit().fits);
    CHECK(doc->placed().bbox().min.z == Approx(5.0));
    CHECK_FALSE(doc->canUndo());
    // Each load gets its own geometry id, so the 3D view re-uploads.
    CHECK(loadDemo("doc_test_other.stl")->geometry_id != doc->geometry_id);
}

TEST_CASE("Document: edits are undoable and every change bumps the revision", "[gui]") {
    const auto doc = loadDemo();
    const int r0 = doc->revision();
    fruit::Placement p = doc->placement();
    p.scale = 2;
    doc->edit(p);
    CHECK(doc->revision() > r0);
    CHECK(doc->fit().size.x == Approx(60.0));
    p.on_plate = true;
    doc->edit(p);
    CHECK(doc->placed().bbox().min.z == Approx(0.0));

    doc->undo();
    CHECK_FALSE(doc->placement().on_plate);
    CHECK(doc->placed().bbox().min.z == Approx(5.0));
    doc->undo();
    CHECK(doc->placement().scale == 1.0);
    CHECK_FALSE(doc->canUndo());
    doc->undo();  // nothing left: no change
    CHECK(doc->placement().scale == 1.0);

    doc->redo();
    CHECK(doc->placement().scale == 2.0);
    CHECK(doc->canRedo());
    p = doc->placement();
    p.rotation_deg = {0, 0, 90};
    doc->edit(p);  // a new edit drops what could be redone
    CHECK_FALSE(doc->canRedo());
    CHECK(doc->fit().size.x == Approx(40.0));  // turned: 20 mm deep becomes 40 mm wide at 200 %
}

TEST_CASE("Document: the transform and the placed mesh agree", "[gui]") {
    const auto doc = loadDemo();
    fruit::Placement p;
    p.scale = 1.5;
    p.rotation_deg = {10, 20, 30};
    doc->edit(p);
    const auto& m = doc->model.mesh.vertices;
    const auto& w = doc->placed().vertices;
    REQUIRE(m.size() == w.size());
    for (std::size_t i = 0; i < m.size(); i += 7) {
        const fruit::Vec3 q = doc->transform().apply(m[i]);
        CHECK(q.x == Approx(w[i].x));
        CHECK(q.y == Approx(w[i].y));
        CHECK(q.z == Approx(w[i].z));
    }
}

TEST_CASE("Document: an empty file is refused with a message", "[gui]") {
    const auto path = std::filesystem::temp_directory_path() / "doc_test_empty.stl";
    { std::ofstream(path) << "solid empty\nendsolid empty\n"; }
    CHECK_THROWS(gui::loadDocument(path.string(), fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none()));
    std::filesystem::remove(path);
}
