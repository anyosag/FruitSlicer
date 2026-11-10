// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Placing the model: where it is on the plate, whether it fits there, the
// handles' maths, undo of a whole drag, what each place needs first, and a
// moved model reaching the saved job and the printed image.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <memory>

#include <miniz.h>
#include <nlohmann/json.hpp>

#include "Document.h"
#include "JobFile.h"
#include "Navigation.h"
#include "Placing.h"
#include "layers/LayerStack.h"
#include "mesh/Primitives.h"
#include "mesh/StlIO.h"
#include "raster/Rasterizer.h"

using Catch::Approx;
using namespace gui;
namespace fs = std::filesystem;

namespace {

const fruit::PrinterProfile kPrinter = fruit::PrinterProfile::saturn4Ultra16k();

fs::path dir() {
    const auto d = fs::temp_directory_path() / "fruit_placing_tests";
    fs::create_directories(d);
    return d;
}

std::shared_ptr<Document> docFor(const fruit::TriangleMesh& m, const char* name) {
    const auto path = dir() / name;
    fruit::writeStl(path.string(), m);
    return loadDocument(path.string(), kPrinter, fruit::JobControl::none());
}

// The side bar from the report: 13.4 × 154 × 10 mm, lying with its long side
// along Y, deeper than the plate (118.37 mm) but narrower than it is wide.
fruit::TriangleMesh sideBar() { return fruit::makeBox({0, 0, 0}, {13.4, 154.0, 10.0}); }

// A supports result that counts as made for the document as it is now.
void freshSupports(Document& d) {
    auto r = std::make_shared<SupportResult>();
    r->choices = d.supports.choices;
    r->edits = d.supports.edits;
    d.supports.result = r;
    d.supports.result_revision = d.revision();
    d.supports.result_settings = d.supportSettings();
}

void checkSameRotation(const fruit::Vec3& a, const fruit::Transform& want) {
    const fruit::Transform got = fruit::Transform::rotationXYZ(a.x, a.y, a.z);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) CHECK(got.m[i][j] == Approx(want.m[i][j]).margin(1e-6));
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

// The middle column of what a layer at `z` mm prints, on a coarse grid laid
// out as the printer's (the plate's centre at 0, 0).
double printedMiddleX(const fruit::TriangleMesh& placed, double z) {
    fruit::RasterGrid g = fruit::RasterGrid::fromPrinter(kPrinter);
    g.px *= 40;
    g.py *= 40;
    g.width /= 40;
    g.height /= 40;
    const fruit::LayerStack ls = fruit::sliceModel(placed, 0.5);
    const auto layer = static_cast<std::size_t>(z / 0.5);
    REQUIRE(layer < ls.size());
    const auto img = fruit::rasterizeDense(ls.slices[layer], g, 256);
    double sum = 0, cols = 0;
    for (int y = 0; y < g.height; ++y)
        for (int x = 0; x < g.width; ++x) {
            const double v = img[static_cast<std::size_t>(y) * static_cast<std::size_t>(g.width) + x];
            sum += v * x;
            cols += v;
        }
    REQUIRE(cols > 0);
    return sum / cols * g.px;  // mm from the left edge
}

}  // namespace

TEST_CASE("Placing: the offset puts the middle of the outline there", "[gui][placing]") {
    const fruit::TriangleMesh m = demoModel();
    fruit::Placement p;
    CHECK(p.centred());
    p.offset = {30, -12.5};
    p.rotation_deg = {0, 0, 30};
    const fruit::TriangleMesh placed = fruit::placeModel(m, p);
    CHECK(placed.bbox().center().x == Approx(30));
    CHECK(placed.bbox().center().y == Approx(-12.5));
    CHECK(placed.bbox().min.z == Approx(5));  // the lift is as before
    // The transform agrees with the placed mesh.
    const fruit::Transform t = fruit::placementTransform(m, p);
    for (std::size_t i = 0; i < m.vertices.size(); i += 5) {
        const fruit::Vec3 a = t.apply(m.vertices[i]);
        CHECK(a.x == Approx(placed.vertices[i].x).margin(1e-9));
        CHECK(a.y == Approx(placed.vertices[i].y).margin(1e-9));
    }
}

TEST_CASE("Placing: the fit counts where it is, and says why it doesn't fit", "[gui][placing]") {
    const fruit::TriangleMesh bar = sideBar();
    fruit::Placement p;  // as the report's screenshot: 100 %, not turned
    fruit::PlateFit f = fruit::checkFit(bar, p, kPrinter);
    CHECK_FALSE(f.fits);
    CHECK_FALSE(f.fits_centred);
    CHECK(f.fits_turned);  // turned, 154 mm goes across a 211.68 mm plate
    CHECK_FALSE(f.overEdge());

    p.rotation_deg.z = 90;
    f = fruit::checkFit(bar, p, kPrinter);
    REQUIRE(f.fits);
    // Room to move: (211.68 - 6 - 154) / 2 in X, (118.37 - 6 - 13.4) / 2 in Y.
    CHECK(f.room.x == Approx((211.68 - 6 - 154.0) / 2).margin(1e-6));
    CHECK(f.room.y == Approx((118.37 - 6 - 13.4) / 2).margin(1e-6));
    p.offset = {f.room.x, -f.room.y};  // right up to the corner: still on
    CHECK(fruit::checkFit(bar, p, kPrinter).fits);
    p.offset.x += 0.5;  // past it
    f = fruit::checkFit(bar, p, kPrinter);
    CHECK_FALSE(f.fits);
    CHECK(f.fits_centred);
    CHECK(f.overEdge());

    // Sliding back is the least move: X back to the edge, Y untouched.
    const fruit::Placement back = slidOnto(bar, p, kPrinter);
    CHECK(back.offset.x == Approx(fruit::checkFit(bar, back, kPrinter).room.x));
    CHECK(back.offset.y == Approx(p.offset.y));
    CHECK(fruit::checkFit(bar, back, kPrinter).fits);
    // Too big however it stands: left alone.
    fruit::Placement huge;
    huge.scale = 3;
    CHECK(slidOnto(bar, huge, kPrinter) == huge);
}

TEST_CASE("Placing: a quarter turn fixes the side bar; the size dialog's full size turns it too", "[gui][placing]") {
    const fruit::TriangleMesh bar = sideBar();
    const fruit::Placement turned = turnedQuarter(bar, fruit::Placement{}, kPrinter);
    CHECK(turned.rotation_deg.z == Approx(90));
    CHECK(turned.scale == 1.0);
    CHECK(fruit::checkFit(bar, turned, kPrinter).fits);
    // Turning near the edge slides it back on rather than leave it hanging
    // over: the 30 × 20 mm bracket near the back, turned, is 30 mm deep.
    const fruit::TriangleMesh bracket = demoModel();
    fruit::Placement off;
    off.offset = {10, 45};
    REQUIRE(fruit::checkFit(bracket, off, kPrinter).fits);
    const fruit::Placement q = turnedQuarter(bracket, off, kPrinter);
    CHECK(q.rotation_deg.z == Approx(90));
    const fruit::PlateFit qf = fruit::checkFit(bracket, q, kPrinter);
    CHECK(qf.fits);
    CHECK(q.offset.y == Approx(qf.room.y));  // just back inside, not centred
    CHECK(q.offset.x == 10);

    // "Full size, turned a quarter: 100 %" in the health dialog is what is placed.
    const fruit::Placement full = sizeChoicePlacement(bar, kPrinter, SizeChoice::Full, 100);
    CHECK(full.scale == 1.0);
    CHECK(full.rotation_deg.z == Approx(90));
    CHECK(fruit::checkFit(bar, full, kPrinter).fits);
    // A model that fits as it is isn't turned.
    CHECK(sizeChoicePlacement(demoModel(), kPrinter, SizeChoice::Full, 100).rotation_deg.z == 0);
    const fruit::Placement largest = sizeChoicePlacement(bar, kPrinter, SizeChoice::Largest, 100);
    CHECK(largest.rotation_deg.z == Approx(90));
    CHECK(largest.scale > 1.0);
}

TEST_CASE("Placing: angles are tidied and turns about the plate's axes compose", "[gui][placing]") {
    CHECK(tidyAngle(270) == Approx(-90));
    CHECK(tidyAngle(-180) == Approx(180));
    CHECK(tidyAngle(360.0000000001) == 0);
    CHECK(tidyAngle(-0.0) == 0);
    CHECK(snapTo(44, 15) == 45);
    CHECK(snapTo(-7.4, 15) == 0);

    // About Z: only Z changes, exactly.
    const fruit::Vec3 z = turnAbout({10, 20, 30}, 2, 90);
    CHECK(z.x == 10);
    CHECK(z.y == 20);
    CHECK(z.z == 120);
    // About X from upright: just X.
    const fruit::Vec3 x = turnAbout({0, 0, 0}, 0, 35);
    CHECK(x.x == Approx(35));
    CHECK(x.y == Approx(0).margin(1e-9));
    CHECK(x.z == Approx(0).margin(1e-9));
    // Past 90° about Y the fields carry on (0, 100, 0) rather than jump to (180, 80, 180).
    const fruit::Vec3 y = turnAbout({0, 80, 0}, 1, 20);
    CHECK(y.x == Approx(0).margin(1e-6));
    CHECK(y.y == Approx(100));
    CHECK(y.z == Approx(0).margin(1e-6));

    // Any turn about X or Y is the same rotation as the matrices say,
    // including through gimbal lock (Y at ±90°).
    const fruit::Vec3 starts[] = {{0, 0, 0}, {11.78, 4.52, 0}, {30, 60, 90}, {-45, 90, 10}, {170, -89, -120}};
    for (const fruit::Vec3& s : starts)
        for (int axis = 0; axis < 2; ++axis)
            for (double deg : {15.0, 90.0, -37.5, 180.0}) {
                const fruit::Transform want =
                    fruit::Transform::rotationXYZ(s.x, s.y, s.z)
                        .then(fruit::Transform::rotationXYZ(axis == 0 ? deg : 0, axis == 1 ? deg : 0, 0));
                checkSameRotation(turnAbout(s, axis, deg), want);
            }
    // The decomposition alone round-trips.
    for (const fruit::Vec3& s : starts) checkSameRotation(eulerXYZ(fruit::Transform::rotationXYZ(s.x, s.y, s.z)),
                                                          fruit::Transform::rotationXYZ(s.x, s.y, s.z));
}

TEST_CASE("Placing: handles follow the mouse ray on the plate and round the rings", "[gui][placing]") {
    fruit::BBox3 box;
    box.extend({-10, -5, 5});
    box.extend({10, 5, 25});
    const HandleFrame f = handleFrame(box);
    CHECK(f.centre.z == Approx(15));
    CHECK(f.arrow_gap.x > 10);  // outside the outline
    CHECK(f.radius > 0.5 * std::sqrt(20.0 * 20 + 10 * 10 + 20 * 20));

    // A ray straight down onto the plate.
    const auto hit = hitPlane({3, 4, 100}, {0, 0, -1}, {0, 0, 0}, {0, 0, 1});
    REQUIRE(hit);
    CHECK(hit->x == Approx(3));
    CHECK_FALSE(hitPlane({0, 0, 100}, {0, 0, 1}, {0, 0, 0}, {0, 0, 1}));  // pointing away

    // The X arrow: grabbed at x = 25, let go above x = 37.34 → 12.3 mm further.
    fruit::Placement p;
    HandleDrag d;
    REQUIRE(startDrag(d, Handle::MoveX, p, f, {25, 0, 100}, {0, 0, -1}));
    fruit::Placement q = dragTo(d, {37.34, 3, 100}, {0, 0, -1}, false);
    CHECK(q.offset.x == Approx(12.3));
    CHECK(q.offset.y == 0);  // the X arrow moves in X only
    q = dragTo(d, {37.34, 3, 100}, {0, 0, -1}, true);  // ⇧: whole millimetres
    CHECK(q.offset.x == Approx(12));
    // A ray along the plate doesn't move it.
    CHECK(dragTo(d, {0, 0, 0}, {1, 0, 0}, false) == q);

    // The Z ring, from straight above: a quarter of the way round is +90°.
    REQUIRE(startDrag(d, Handle::TurnZ, p, f, {f.radius, 0, 100}, {0, 0, -1}));
    for (int a = 10; a <= 90; a += 10) {
        const double r = a * 3.14159265358979 / 180;
        q = dragTo(d, {f.radius * std::cos(r), f.radius * std::sin(r), 100}, {0, 0, -1}, false);
    }
    CHECK(q.rotation_deg.z == Approx(90));
    // Round past 180° it keeps counting rather than jumping back.
    for (int a = 100; a <= 270; a += 10) {
        const double r = a * 3.14159265358979 / 180;
        q = dragTo(d, {f.radius * std::cos(r), f.radius * std::sin(r), 100}, {0, 0, -1}, false);
    }
    CHECK(d.turned == Approx(270));
    CHECK(q.rotation_deg.z == Approx(-90));
    // Seen edge on, a ring can't be grabbed.
    CHECK_FALSE(startDrag(d, Handle::TurnZ, p, f, {0, -100, 15}, {0, 1, 0}));
    // With ⇧ a turn goes in 15° steps.
    CHECK(draggedBy(p, Handle::TurnX, 50, true).rotation_deg.x == Approx(45));
    CHECK(draggedBy(p, Handle::TurnX, 50, false).rotation_deg.x == Approx(50));
}

TEST_CASE("Placing: a whole drag is one undo step, and undo brings back what was made for it", "[gui][placing]") {
    const auto doc = docFor(demoModel(), "drag.stl");
    freshSupports(*doc);
    const int before = doc->revision();
    REQUIRE(doc->supports.fresh(doc->revision(), doc->supportSettings()));

    doc->beginDrag();
    fruit::Placement p = doc->placement();
    for (int i = 1; i <= 10; ++i) {
        p.offset.x = i * 3.0;
        doc->preview(p);
        CHECK(doc->placedBox().center().x == Approx(i * 3.0));  // follows at once
    }
    doc->endDrag();
    CHECK(doc->placement().offset.x == 30);
    CHECK(doc->placed().bbox().center().x == Approx(30));  // made again once it's let go
    CHECK_FALSE(doc->supports.fresh(doc->revision(), doc->supportSettings()));  // moved: out of date

    doc->undo();  // the whole drag at once
    CHECK(doc->placement().offset.x == 0);
    CHECK(doc->revision() == before);
    CHECK(doc->supports.fresh(doc->revision(), doc->supportSettings()));  // they count again
    CHECK_FALSE(doc->canUndo());
    doc->redo();
    CHECK(doc->placement().offset.x == 30);

    // Escape: back where it started, nothing to undo.
    const int at = doc->revision();
    doc->beginDrag();
    p.offset.x = 80;
    doc->preview(p);
    doc->cancelDrag();
    CHECK(doc->placement().offset.x == 30);
    CHECK(doc->revision() == at);
    CHECK(doc->placed().bbox().center().x == Approx(30));
    // Dragged back to where it began: no undo step.
    const std::size_t steps_before = doc->canUndo() ? 1 : 0;
    doc->beginDrag();
    p.offset.x = 50;
    doc->preview(p);
    p.offset.x = 30;
    doc->preview(p);
    doc->endDrag();
    CHECK(doc->revision() == at);
    doc->undo();
    CHECK(doc->placement().offset.x == 0);
    (void)steps_before;

    // A new edit after undo gets a number of its own, never one undone.
    const auto d2 = docFor(demoModel(), "drag2.stl");
    fruit::Placement a = d2->placement();
    a.scale = 2;
    d2->edit(a);
    const int r2 = d2->revision();
    d2->undo();
    a.scale = 3;
    d2->edit(a);
    CHECK(d2->revision() != r2);
    // Replacing the model's file: undo can't go back past it.
    const int r3 = d2->revision();
    CHECK(d2->replaceMesh(d2->part().id(), fruit::prepareModel(demoModel())));
    d2->clearHistory();
    CHECK(d2->revision() != r3);
    CHECK_FALSE(d2->canUndo());

    // A copy taken mid-drag (a worker's snapshot) has the placed mesh as previewed.
    d2->beginDrag();
    fruit::Placement b = d2->placement();
    b.offset = {12, -7};
    d2->preview(b);
    const Document copy(*d2);
    CHECK(copy.placed().bbox().center().x == Approx(12));
    CHECK(copy.placed().bbox().center().y == Approx(-7));
    d2->cancelDrag();
}

TEST_CASE("Places: each opens any time and says what it needs first", "[gui][placing]") {
    const auto doc = docFor(sideBar(), "places.stl");
    // As in the report: too big as it lies. Model is the place to fix it;
    // the search can't help at this size; supports need it to fit.
    CHECK(needFor(*doc, Step::Model) == Need::Nothing);
    CHECK(markFor(*doc, Step::Model) == Mark::Problem);
    CHECK(needFor(*doc, Step::Orient) == Need::Fit);
    CHECK(needFor(*doc, Step::Supports) == Need::Fit);
    CHECK(needFor(*doc, Step::Layers) == Need::Fit);

    // Turned: supports straight away, without Orient.
    doc->edit(turnedQuarter(doc->part().mesh(), doc->placement(), kPrinter));
    CHECK(markFor(*doc, Step::Model) == Mark::None);
    CHECK(needFor(*doc, Step::Supports) == Need::Nothing);
    CHECK_FALSE(doc->orient().decided);
    CHECK(needFor(*doc, Step::Layers) == Need::Supports);
    CHECK(needFor(*doc, Step::Print) == Need::Supports);

    freshSupports(*doc);
    CHECK(markFor(*doc, Step::Supports) == Mark::Done);
    CHECK(needFor(*doc, Step::Layers) == Need::Nothing);  // opening it cuts the layers
    CHECK(needFor(*doc, Step::Print) == Need::Layers);
    auto layers = std::make_shared<LayerResult>();
    doc->layers.result = layers;
    doc->layers.for_supports = doc->supports.result;
    CHECK(needFor(*doc, Step::Print) == Need::Nothing);
    CHECK(markFor(*doc, Step::Layers) == Mark::Done);

    // Moved over the edge: out of date and doesn't fit; the search still can.
    fruit::Placement p = doc->placement();
    p.offset.x = 60;
    doc->edit(p);
    CHECK(doc->fit().overEdge());
    CHECK(needFor(*doc, Step::Orient) == Need::Nothing);
    CHECK(needFor(*doc, Step::Supports) == Need::Fit);
    CHECK(needFor(*doc, Step::Print) == Need::Fit);
    doc->edit(slidOnto(doc->part().mesh(), doc->placement(), kPrinter));
    CHECK(needFor(*doc, Step::Print) == Need::FreshSupports);
    doc->undo();
    doc->undo();  // back to where they were made
    CHECK(needFor(*doc, Step::Print) == Need::Nothing);
}

TEST_CASE("Jobs: where it is on the plate is saved; older jobs open centred", "[gui][placing][jobs]") {
    const auto doc = docFor(demoModel(), "where.stl");
    const auto centred = (dir() / "centred.fruitjob").string();
    saveJob(centred, *doc);
    // A centred model is written as before, so older FruitSlicers open it.
    nlohmann::json j = jobJson(centred);
    CHECK(j["version"] == 1);
    CHECK_FALSE(j["placement"].contains("offset"));
    OpenedJob o = openJob(centred, kPrinter, fruit::JobControl::none());
    CHECK(o.doc->placement().centred());

    fruit::Placement p = doc->placement();
    p.offset = {-42.5, 17};
    doc->edit(p);
    const auto moved = (dir() / "moved.fruitjob").string();
    saveJob(moved, *doc);
    j = jobJson(moved);
    CHECK(j["version"] == 2);  // older FruitSlicers refuse it rather than open it centred
    REQUIRE(j["placement"].contains("offset"));
    o = openJob(moved, kPrinter, fruit::JobControl::none());
    CHECK(o.doc->placement().offset.x == -42.5);
    CHECK(o.doc->placement().offset.y == 17);
    CHECK(o.doc->placed().bbox().center().x == Approx(-42.5));
    CHECK_FALSE(o.doc->unsaved());
}

TEST_CASE("Placing: a moved model prints where it stands on the plate", "[gui][placing]") {
    const auto doc = docFor(demoModel(), "printed.stl");
    const double centred = printedMiddleX(doc->placed(), 10);
    fruit::Placement p = doc->placement();
    p.offset = {40, 0};
    doc->edit(p);
    const double moved = printedMiddleX(doc->placed(), 10);
    CHECK(moved - centred == Approx(40).margin(0.6));  // within a coarse pixel
}
