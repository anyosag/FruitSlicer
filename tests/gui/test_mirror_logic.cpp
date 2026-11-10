// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Mirror: the model as its mirror image, with the outside still outside.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include <memory>

#include <miniz.h>
#include <nlohmann/json.hpp>

#include "Document.h"
#include "JobFile.h"
#include "Orient.h"
#include "Placing.h"
#include "mesh/StlIO.h"

using Catch::Approx;
using namespace gui;
namespace fs = std::filesystem;

namespace {

const fruit::PrinterProfile kPrinter = fruit::PrinterProfile::saturn4Ultra16k();

fs::path dir() {
    const auto d = fs::temp_directory_path() / "fruit_mirror_tests";
    fs::create_directories(d);
    return d;
}

std::shared_ptr<Document> docFor(const char* name) {
    const auto path = dir() / name;
    fruit::writeStl(path.string(), demoModel());
    return loadDocument(path.string(), kPrinter, fruit::JobControl::none());
}

nlohmann::json jobJson(const std::string& path) {
    mz_zip_archive z{};
    REQUIRE(mz_zip_reader_init_file(&z, path.c_str(), 0));
    std::size_t n = 0;
    void* data = mz_zip_reader_extract_file_to_heap(&z, "job.json", &n, 0);
    REQUIRE(data);
    const std::string text(static_cast<const char*>(data), n);
    mz_free(data);
    mz_zip_reader_end(&z);
    return nlohmann::json::parse(text);
}

void checkSameVertices(const fruit::TriangleMesh& a, const fruit::TriangleMesh& b) {
    REQUIRE(a.vertices.size() == b.vertices.size());
    for (std::size_t i = 0; i < a.vertices.size(); ++i) {
        CHECK(a.vertices[i].x == Approx(b.vertices[i].x).margin(1e-9));
        CHECK(a.vertices[i].y == Approx(b.vertices[i].y).margin(1e-9));
        CHECK(a.vertices[i].z == Approx(b.vertices[i].z).margin(1e-9));
    }
}

// Placements that turn, scale and lift the model as well.
std::vector<fruit::Placement> placements() {
    fruit::Placement plain;
    fruit::Placement turned;
    turned.rotation_deg = {30, -20, 75};
    turned.scale = 0.8;
    turned.offset = {12, -7};
    fruit::Placement flat;
    flat.on_plate = true;
    flat.rotation_deg = {0, 90, 0};  // gimbal lock
    fruit::Placement upright;
    upright.rotation_deg = {90, 0, 45};
    return {plain, turned, flat, upright};
}

}  // namespace

TEST_CASE("Mirror: the volume is the same and positive, and every face still points out", "[gui][mirror]") {
    const auto doc = docFor("vol.stl");
    for (const fruit::Placement& start : placements()) {
        doc->place(start);
        const fruit::TriangleMesh before = doc->placed();
        const double v = before.volume();
        REQUIRE(v > 0);
        for (int axis = 0; axis < 3; ++axis) {
            doc->mirror(axis);
            const fruit::TriangleMesh& m = doc->placed();
            CHECK(m.volume() > 0);
            CHECK(m.volume() == Approx(v).epsilon(1e-9));
            // Face by face: the mirror image's normal is the old one mirrored.
            REQUIRE(m.faceCount() == before.faceCount());
            for (std::size_t f = 0; f < m.faceCount(); ++f) {
                fruit::Vec3 want = before.faceNormal(f);
                // The mirror about the middle of the outline flips one component.
                (axis == 0 ? want.x : axis == 1 ? want.y : want.z) *= -1;
                const fruit::Vec3 got = m.faceNormal(f);
                CHECK(got.x == Approx(want.x).margin(1e-6));
                CHECK(got.y == Approx(want.y).margin(1e-6));
                CHECK(got.z == Approx(want.z).margin(1e-6));
            }
            doc->undo();
        }
    }
}

TEST_CASE("Mirror: the box is mirrored; the middle stays and it stays at its lift", "[gui][mirror]") {
    const auto doc = docFor("box.stl");
    for (const fruit::Placement& start : placements()) {
        doc->place(start);
        const fruit::TriangleMesh before = doc->placed();
        const fruit::BBox3 box = before.bbox();
        const fruit::Vec3 c = box.center();
        for (int axis = 0; axis < 3; ++axis) {
            doc->mirror(axis);
            const fruit::BBox3 b = doc->placed().bbox();
            CHECK(b.size().x == Approx(box.size().x).margin(1e-9));
            CHECK(b.size().y == Approx(box.size().y).margin(1e-9));
            CHECK(b.size().z == Approx(box.size().z).margin(1e-9));
            CHECK(b.center().x == Approx(c.x).margin(1e-9));
            CHECK(b.center().y == Approx(c.y).margin(1e-9));
            CHECK(b.min.z == Approx(doc->placement().lift()).margin(1e-9));
            // Each vertex is where the mirror puts it, about the middle of the outline.
            for (std::size_t i = 0; i < before.vertices.size(); ++i) {
                fruit::Vec3 want = before.vertices[i];
                if (axis == 0) want.x = 2 * c.x - want.x;
                if (axis == 1) want.y = 2 * c.y - want.y;
                if (axis == 2) want.z = 2 * c.z - want.z;
                const fruit::Vec3 got = doc->placed().vertices[i];
                CHECK(got.x == Approx(want.x).margin(1e-9));
                CHECK(got.y == Approx(want.y).margin(1e-9));
                CHECK(got.z == Approx(want.z).margin(1e-9));
            }
            doc->undo();
        }
    }
}

TEST_CASE("Mirror: twice gives the original; it is one undo step and makes supports out of date",
          "[gui][mirror]") {
    const auto doc = docFor("twice.stl");
    for (const fruit::Placement& start : placements()) {
        doc->place(start);
        const fruit::TriangleMesh before = doc->placed();
        const int rev = doc->revision();
        for (int axis = 0; axis < 3; ++axis) {
            doc->mirror(axis);
            CHECK(doc->revision() != rev);  // supports and layers made for it no longer fit
            doc->mirror(axis);
            checkSameVertices(doc->placed(), before);
            CHECK(doc->placed().volume() == Approx(before.volume()).epsilon(1e-9));
            CHECK_FALSE(doc->placement().mirrored());
            // Undo goes back one mirror at a time, and the first brings back the revision.
            doc->undo();
            doc->undo();
            CHECK(doc->revision() == rev);
            checkSameVertices(doc->placed(), before);
        }
    }
}

TEST_CASE("Mirror: a mirror image is told from a half turn, and Rotate goes on turning the way the view shows",
          "[gui][mirror]") {
    const auto doc = docFor("rotate.stl");
    doc->mirror(0);
    CHECK(doc->placement().mirrorImage());
    doc->mirror(1);  // left-right and front-back: a half turn, not a mirror image
    CHECK_FALSE(doc->placement().mirrorImage());
    doc->undo();
    // Turning the mirrored model a quarter about the vertical moves its footprint
    // the way it would for any model.
    const fruit::BBox3 b = doc->placed().bbox();
    fruit::Placement p = doc->placement();
    p.rotation_deg = turnAbout(p.rotation_deg, 2, 90);
    doc->edit(p);
    const fruit::BBox3 t = doc->placed().bbox();
    CHECK(t.size().x == Approx(b.size().y).margin(1e-9));
    CHECK(t.size().y == Approx(b.size().x).margin(1e-9));
    CHECK(doc->placed().volume() > 0);
}

TEST_CASE("Mirror: it is saved in the job and reopened the same", "[gui][mirror][jobs]") {
    const auto doc = docFor("saved.stl");
    const auto plain = (dir() / "plain.fruitjob").string();
    saveJob(plain, *doc);
    CHECK(jobJson(plain)["version"] == 1);  // not mirrored: written as before
    CHECK_FALSE(jobJson(plain)["placement"].contains("mirror"));
    doc->mirror(0);
    doc->mirror(2);
    const fruit::TriangleMesh before = doc->placed();
    const auto path = (dir() / "mirrored.fruitjob").string();
    saveJob(path, *doc);
    CHECK(jobJson(path)["version"] == 2);  // older FruitSlicers refuse it rather than open it unmirrored
    CHECK(jobJson(path)["placement"]["mirror"].size() == 3);
    const OpenedJob o = openJob(path, kPrinter, fruit::JobControl::none());
    CHECK(o.doc->placement() == doc->placement());
    checkSameVertices(o.doc->placed(), before);
    CHECK(o.doc->placed().volume() == Approx(before.volume()).epsilon(1e-9));
    CHECK_FALSE(o.doc->unsaved());
}

TEST_CASE("Mirror: the sides people will see are mirrored with it", "[gui][mirror]") {
    fruit::Placement p;
    p.mirror_x = true;
    const auto dirs = visibleInPrinter({{1, 0, 0}, {0, 1, 0}}, p);
    CHECK(dirs[0].x == Approx(-1));
    CHECK(dirs[1].y == Approx(1));
}
