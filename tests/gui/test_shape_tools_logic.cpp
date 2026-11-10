// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The shape tools' logic: Cut, Label and Combine on the job's parts.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <filesystem>
#include <memory>

#include "Document.h"
#include "ShapeTools.h"
#include "mesh/ModelLoad.h"
#include "mesh/Primitives.h"
#include "mesh/StlIO.h"

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using namespace gui;
namespace fs = std::filesystem;

namespace {

const fruit::PrinterProfile kPrinter = fruit::PrinterProfile::saturn4Ultra16k();

std::shared_ptr<Document> docWith(const fruit::TriangleMesh& mesh, const char* name) {
    const auto d = fs::temp_directory_path() / "fruit_shape_tests";
    fs::create_directories(d);
    const auto path = d / name;
    fruit::writeStl(path.string(), mesh);
    return loadDocument(path.string(), kPrinter, fruit::JobControl::none());
}

fruit::TriangleMesh box(double w, double d, double h) { return fruit::makeBox({-w / 2, -d / 2, 0}, {w / 2, d / 2, h}); }

int addBox(Document& doc, double w, double d, double h, fruit::Vec2 at, const char* name) {
    fruit::Placement p = doc.placement();
    p.offset = at;
    return doc.addPart(NewPart{name, "", makePartModel(fruit::prepareModel(box(w, d, h))), p});
}

bool closed(const fruit::TriangleMesh& m) {
    fruit::TriangleMesh w = m;
    w.weld();
    return w.isClosedManifold();
}

ShapeOutcome cutBy(const Part& part, CutAxis axis, double position, CutKeep keep) {
    CutSettings s;
    s.axis = axis;
    cutFit(s, part);
    s.position = position;
    s.keep = keep;
    return cutPart(cutJobFor(part, s));
}

void applyCut(Document& doc, const ShapeOutcome& out) {
    const Part* old = &doc.part();
    REQUIRE(doc.editParts([&](PartsEdit& e) { cutIntoParts(e, *old, out, doc.printer); }));
}

}  // namespace

TEST_CASE("Cut: both pieces, volumes add up, each stands at the lift, and the cut faces are filled", "[gui][shape]") {
    const auto doc = docWith(box(20, 30, 40), "cut_both.stl");
    const double v = doc->part().placed().volume();
    const int old_id = doc->selectedId();
    const int rev = doc->revision();
    const fruit::BBox3 before = doc->part().box();

    const ShapeOutcome out = cutBy(doc->part(), CutAxis::Z, 15, CutKeep::Both);
    REQUIRE(out.ok);
    REQUIRE(out.pieces.size() == 2);
    applyCut(*doc, out);

    REQUIRE(doc->partCount() == 2);
    CHECK(doc->findPart(old_id) == nullptr);
    CHECK(doc->revision() != rev);
    const Part& bottom = doc->parts()[0];
    const Part& top = doc->parts()[1];
    CHECK(doc->selectedId() == bottom.id());
    CHECK(bottom.name() == "cut_both (bottom)");
    CHECK(top.name() == "cut_both (top)");
    CHECK(bottom.placed().volume() + top.placed().volume() == Approx(v).epsilon(1e-6));
    CHECK(bottom.placed().volume() == Approx(20 * 30 * 15.0).epsilon(1e-6));
    CHECK(closed(bottom.placed()));
    CHECK(closed(top.placed()));
    CHECK(bottom.box().min.z == Approx(doc->placement().lift()).margin(1e-9));
    CHECK(top.box().min.z == Approx(doc->placement().lift()).margin(1e-9));
    CHECK(top.box().size().z == Approx(25).margin(1e-6));
    // The bottom piece stays where it was; the top goes beside it, with room.
    CHECK(bottom.box().min.x == Approx(before.min.x).margin(1e-9));
    CHECK(bottom.box().min.y == Approx(before.min.y).margin(1e-9));
    const fruit::BBox2 a = bottom.footprintBox(), b = top.footprintBox();
    CHECK_FALSE(a.overlaps(b, kPartGap - 1e-6));

    // One undo step brings the part back, and with it the revision.
    doc->undo();
    REQUIRE(doc->partCount() == 1);
    CHECK(doc->selectedId() == old_id);
    CHECK(doc->revision() == rev);
    CHECK(doc->part().placed().volume() == Approx(v).epsilon(1e-9));
}

TEST_CASE("Cut: one side only, upper or lower, stays where it was across the plate", "[gui][shape]") {
    const auto doc = docWith(box(20, 30, 40), "cut_one.stl");
    const fruit::BBox3 before = doc->part().box();
    const ShapeOutcome top = cutBy(doc->part(), CutAxis::Z, 15, CutKeep::Above);
    REQUIRE(top.ok);
    REQUIRE(top.pieces.size() == 1);
    CHECK(top.pieces[0].suffix.empty());
    applyCut(*doc, top);
    REQUIRE(doc->partCount() == 1);
    CHECK(doc->part().name() == "cut_one");
    CHECK(doc->part().box().size().z == Approx(25).margin(1e-6));
    CHECK(doc->part().box().min.x == Approx(before.min.x).margin(1e-9));
    CHECK(doc->part().box().max.y == Approx(before.max.y).margin(1e-9));
    CHECK(doc->part().box().min.z == Approx(doc->placement().lift()).margin(1e-9));
    doc->undo();

    // Vertical: the plane is at X on the plate, so the left side ends there.
    const ShapeOutcome left = cutBy(doc->part(), CutAxis::X, -4, CutKeep::Below);
    REQUIRE(left.ok);
    applyCut(*doc, left);
    CHECK(doc->part().box().min.x == Approx(before.min.x).margin(1e-9));
    CHECK(doc->part().box().max.x == Approx(-4).margin(1e-6));
    CHECK(doc->part().placed().volume() == Approx(6.0 * 30 * 40).epsilon(1e-6));
    CHECK(left.message.find("left") != std::string::npos);
}

TEST_CASE("Cut: a part that is turned, scaled, mirrored and moved is cut at the plate's position", "[gui][shape]") {
    const auto doc = docWith(box(20, 30, 40), "cut_placed.stl");
    fruit::Placement p;
    p.rotation_deg = {0, 0, 30};
    p.scale = 1.5;
    p.mirror_x = true;
    p.offset = {25, -10};
    doc->place(p);
    const double v = doc->part().placed().volume();
    const fruit::BBox3 box_before = doc->part().box();
    const double mid = (box_before.min.y + box_before.max.y) / 2 + 3;

    const ShapeOutcome out = cutBy(doc->part(), CutAxis::Y, mid, CutKeep::Both);
    REQUIRE(out.ok);
    applyCut(*doc, out);
    REQUIRE(doc->partCount() == 2);
    const Part& front = doc->parts()[0];
    const Part& back = doc->parts()[1];
    CHECK(front.name().find("front") != std::string::npos);
    CHECK(back.name().find("back") != std::string::npos);
    CHECK(front.placed().volume() + back.placed().volume() == Approx(v).epsilon(1e-6));
    CHECK(front.placed().volume() > 0);
    CHECK(back.placed().volume() > 0);
    // The front piece is where it was: it ends at the plane.
    CHECK(front.box().min.y == Approx(box_before.min.y).margin(1e-6));
    CHECK(front.box().max.y == Approx(mid).margin(1e-6));
    CHECK(front.box().min.x >= box_before.min.x - 1e-6);  // inside where it was
    CHECK(front.box().max.x <= box_before.max.x + 1e-6);
    CHECK(closed(front.placed()));
    CHECK(closed(back.placed()));
    // It is still the mirrored, turned, scaled part.
    CHECK(front.placement().mirror_x);
    CHECK(front.placement().scale == Approx(1.5));
}

TEST_CASE("Cut: a plane that misses the part, and the ends of the range", "[gui][shape]") {
    const auto doc = docWith(box(20, 30, 40), "cut_miss.stl");
    const Part& part = doc->part();
    CutSettings s;
    cutFit(s, part);
    CHECK(s.position == Approx(20));  // the middle of the height
    const CutRange r = cutRange(part, CutAxis::Z);
    CHECK(r.lo == Approx(0).margin(1e-9));
    CHECK(r.hi == Approx(40));
    s.position = 0;
    CHECK_FALSE(cutCrosses(part, s));
    s.position = 40;
    CHECK_FALSE(cutCrosses(part, s));
    s.position = 39.5;
    CHECK(cutCrosses(part, s));
    // A position past the end is brought back inside by cutFit.
    s.position = 400;
    cutFit(s, part);
    CHECK(s.position == Approx(40));
    // Changing the axis starts at that axis's middle.
    s.axis = CutAxis::X;
    cutFit(s, part);
    CHECK(s.position == Approx(0).margin(1e-9));

    // The worker says so plainly too.
    const ShapeOutcome out = cutBy(part, CutAxis::Z, 100, CutKeep::Both);
    CHECK_FALSE(out.ok);
    CHECK_THAT(out.message, ContainsSubstring("doesn't pass through"));
    CHECK(out.pieces.empty());
}

TEST_CASE("Cut: a mesh that is not closed is refused, saying to repair it", "[gui][shape]") {
    const auto doc = docWith(demoFaultyModel("loose"), "cut_loose.stl");
    const ShapeOutcome out = cutBy(doc->part(), CutAxis::Z, 10, CutKeep::Both);
    CHECK_FALSE(out.ok);
    CHECK(out.pieces.empty());
    CHECK_THAT(out.message, ContainsSubstring("Couldn't cut"));
    CHECK_THAT(out.message, ContainsSubstring("Repair"));
    // Nothing in the job changed.
    CHECK(doc->partCount() == 1);
}

TEST_CASE("Cut: a cancelled job throws and gives nothing", "[gui][shape]") {
    const auto doc = docWith(box(20, 30, 40), "cut_cancel.stl");
    CutSettings s;
    cutFit(s, doc->part());
    const CutJob job = cutJobFor(doc->part(), s);
    fruit::JobControl ctl;
    ctl.cancel();
    CHECK_THROWS_AS(cutPart(job, ctl), fruit::Cancelled);
}

TEST_CASE("Cut: pieces of a part with several parts go beside the others, not on them", "[gui][shape]") {
    const auto doc = docWith(box(30, 30, 30), "cut_beside.stl");
    const int other = addBox(*doc, 30, 30, 30, {40, 0}, "other");
    doc->select(doc->parts()[0].id());
    const ShapeOutcome out = cutBy(doc->part(), CutAxis::X, 0, CutKeep::Both);
    REQUIRE(out.ok);
    applyCut(*doc, out);
    REQUIRE(doc->partCount() == 3);
    CHECK(doc->findPart(other) != nullptr);
    for (std::size_t i = 0; i < doc->partCount(); ++i)
        for (std::size_t j = i + 1; j < doc->partCount(); ++j)
            CHECK_FALSE(doc->parts()[i].footprintBox().overlaps(doc->parts()[j].footprintBox(), kPartGap - 1e-6));
}

TEST_CASE("Cut: piece names stay unique when a part is cut twice", "[gui][shape]") {
    const auto doc = docWith(box(20, 30, 40), "cut_twice.stl");
    applyCut(*doc, cutBy(doc->part(), CutAxis::Z, 10, CutKeep::Both));
    doc->select(doc->parts()[1].id());  // the top
    applyCut(*doc, cutBy(doc->part(), CutAxis::Z, 10, CutKeep::Both));
    std::vector<std::string> names;
    for (const Part& p : doc->parts()) names.push_back(p.name());
    REQUIRE(names.size() == 3);
    CHECK(names[0] == "cut_twice (bottom)");
    CHECK(names[1] == "cut_twice (top) (bottom)");
    CHECK(names[2] == "cut_twice (top) (top)");
    CHECK(pieceName("a", " (top)", {"a (top)"}) == "a (top) 2");
}

// ---- Label ----

namespace {

ShapeOutcome labelOnTop(const Part& part, const char* text, bool engrave, double height = 6, double depth = 0.8) {
    LabelSettings s;
    s.text = text;
    s.height = height;
    s.depth = depth;
    s.engrave = engrave;
    const fruit::BBox3 b = part.box();
    s.spot = labelSpotAt(part, {(b.min.x + b.max.x) / 2, (b.min.y + b.max.y) / 2, b.max.z}, {0, 0, 1});
    return labelPart(labelJobFor(part, s));
}

}  // namespace

TEST_CASE("Label: raised text adds volume, engraved removes it, and the part stays put", "[gui][shape]") {
    const auto doc = docWith(box(40, 30, 20), "label_box.stl");
    const int id = doc->selectedId();
    const double v = doc->part().placed().volume();
    const fruit::BBox3 before = doc->part().box();

    const ShapeOutcome raised = labelOnTop(doc->part(), "AB", false);
    REQUIRE(raised.ok);
    REQUIRE(raised.pieces.size() == 1);
    CHECK_THAT(raised.message, ContainsSubstring("Raised"));
    REQUIRE(doc->editParts([&](PartsEdit& e) { relabelPart(e, id, raised); }));
    CHECK(doc->partCount() == 1);
    CHECK(doc->selectedId() == id);  // the same part, its mesh changed
    CHECK(doc->part().placed().volume() > v + 1);
    CHECK(closed(doc->part().placed()));
    const fruit::BBox3 after = doc->part().box();
    CHECK(after.size().z == Approx(before.size().z + 0.8).margin(1e-6));
    // Its footprint did not move.
    CHECK(after.min.x == Approx(before.min.x).margin(1e-9));
    CHECK(after.max.y == Approx(before.max.y).margin(1e-9));
    CHECK(after.min.z == Approx(before.min.z).margin(1e-9));
    doc->undo();
    CHECK(doc->part().placed().volume() == Approx(v).epsilon(1e-9));

    const ShapeOutcome cut = labelOnTop(doc->part(), "AB", true);
    REQUIRE(cut.ok);
    CHECK_THAT(cut.message, ContainsSubstring("Engraved"));
    REQUIRE(doc->editParts([&](PartsEdit& e) { relabelPart(e, id, cut); }));
    CHECK(doc->part().placed().volume() < v - 1);
    CHECK(closed(doc->part().placed()));
    CHECK(doc->part().box().size().z == Approx(before.size().z).margin(1e-6));
}

TEST_CASE("Label: the letters stand upright on a side, and on a flat face their tops go to the back", "[gui][shape]") {
    const auto doc = docWith(box(40, 30, 20), "label_up.stl");
    const Part& part = doc->part();
    const fruit::BBox3 b = part.box();
    const LabelSpot top = labelSpotAt(part, {0, 0, b.max.z}, {0, 0, 1});
    const fruit::ops::TextPlacement pt = labelPlacement(part, top, false);
    CHECK(pt.normal.z == Approx(1).margin(1e-9));
    CHECK(pt.up.y == Approx(1));
    const LabelSpot front = labelSpotAt(part, {0, b.min.y, b.min.z + 10}, {0, -1, 0});
    const fruit::ops::TextPlacement pf = labelPlacement(part, front, true);
    CHECK(pf.normal.y == Approx(-1).margin(1e-9));
    CHECK(pf.up.z == Approx(1));
    CHECK(pf.engrave);
    CHECK(pf.point.y == Approx(b.min.y).margin(1e-9));
}

TEST_CASE("Label: a spot follows its part, and is lost when the mesh changes", "[gui][shape]") {
    const auto doc = docWith(box(40, 30, 20), "label_spot.stl");
    LabelSettings s;
    CHECK_FALSE(labelSpotStillOn(s, *doc));
    const fruit::BBox3 b = doc->part().box();
    s.spot = labelSpotAt(doc->part(), {0, 0, b.max.z}, {0, 0, 1});
    CHECK(labelSpotStillOn(s, *doc));
    const fruit::Vec3 on_plate = labelPlacement(doc->part(), *s.spot, false).point;
    // Moved: the spot moves with the part.
    fruit::Placement p = doc->placement();
    p.offset = {30, 10};
    doc->edit(p);
    CHECK(labelSpotStillOn(s, *doc));
    const fruit::Vec3 moved = labelPlacement(doc->part(), *s.spot, false).point;
    CHECK(moved.x == Approx(on_plate.x + 30).margin(1e-9));
    CHECK(moved.y == Approx(on_plate.y + 10).margin(1e-9));
    // A new mesh: no longer the surface it was picked on.
    doc->replaceMesh(doc->selectedId(), fruit::prepareModel(box(40, 30, 21)));
    CHECK_FALSE(labelSpotStillOn(s, *doc));
}

TEST_CASE("Label: on a mirrored part the letters still come out, and text that misses the surface says so", "[gui][shape]") {
    const auto doc = docWith(box(40, 30, 20), "label_mirror.stl");
    doc->mirror(0);
    const double v = doc->part().placed().volume();
    const ShapeOutcome out = labelOnTop(doc->part(), "F", false);
    REQUIRE(out.ok);
    const fruit::TriangleMesh& own = out.pieces[0].model->model.mesh;
    CHECK(closed(own));
    CHECK(own.volume() > v);  // its own coordinates: still outward, positive

    // A point well off the surface: nothing for the letters to join.
    LabelSettings s;
    s.text = "F";
    s.spot = labelSpotAt(doc->part(), {0, 0, doc->part().box().max.z + 30}, {0, 0, 1});
    const ShapeOutcome none = labelPart(labelJobFor(doc->part(), s));
    CHECK_FALSE(none.ok);
    CHECK_THAT(none.message, ContainsSubstring("Couldn't put the text"));
}

TEST_CASE("Label: the preview's outline is made for the text, height and depth, and kept until they change", "[gui][shape]") {
    LabelSettings s;
    CHECK_FALSE(labelPreview(s).ok);
    CHECK_THAT(labelPreview(s).message, ContainsSubstring("Type"));
    s.text = "Hi";
    const LabelPreview& p = labelPreview(s);
    REQUIRE(p.ok);
    CHECK(p.outline.size() > 8);
    const std::string key = p.key;
    CHECK(labelPreview(s).key == key);
    s.height = 9;
    CHECK(labelPreview(s).key != key);
    // The outline stands on the plate's surface, then moves to where the letters go.
    fruit::ops::TextPlacement at;
    at.point = {10, 20, 30};
    at.normal = {0, 0, 1};
    at.up = {0, 1, 0};
    const auto moved = labelOutlineOnPlate(labelPreview(s), at);
    REQUIRE(moved.size() == labelPreview(s).outline.size());
    for (const auto& e : moved) {
        CHECK(e.first.z == Approx(30.05).margin(1e-6));
        CHECK(e.first.x > 0);
    }
    // Letters the font lacks are listed, and text of only spaces gives nothing.
    s.text = "   ";
    CHECK_FALSE(labelPreview(s).ok);
}

// ---- Combine ----

namespace {

// A: 20 cubed at the middle; B: 20 wide, 20 deep, 10 tall, 10 mm to the right (overlap 10 x 20 x 10).
std::shared_ptr<Document> twoBoxes(const char* name, int* b_id) {
    const auto doc = docWith(box(20, 20, 20), name);
    *b_id = addBox(*doc, 20, 20, 10, {10, 0}, "B");
    doc->select(doc->parts()[0].id());
    return doc;
}

ShapeOutcome combineOf(Document& doc, int b, CombineMode mode) {
    CombineJob j;
    j.a = doc.part().placed();
    j.b = doc.findPart(b)->placed();
    j.mode = mode;
    j.a_name = doc.part().name();
    j.b_name = doc.findPart(b)->name();
    return combineParts(j);
}

}  // namespace

TEST_CASE("Combine: join, subtract and overlap give one part with the right volume", "[gui][shape]") {
    int b = -1;
    const auto doc = twoBoxes("comb_a.stl", &b);
    const double va = 20.0 * 20 * 20, vb = 20.0 * 20 * 10, overlap = 10.0 * 20 * 10;

    struct Case {
        CombineMode mode;
        double volume;
    };
    for (const Case& c : {Case{CombineMode::Join, va + vb - overlap}, Case{CombineMode::Subtract, va - overlap},
                          Case{CombineMode::Overlap, overlap}}) {
        const ShapeOutcome out = combineOf(*doc, b, c.mode);
        REQUIRE(out.ok);
        REQUIRE(out.pieces.size() == 1);
        const int a_id = doc->selectedId();
        const Part* pa = doc->findPart(a_id);
        const Part* pb = doc->findPart(b);
        int id = -1;
        REQUIRE(doc->editParts([&](PartsEdit& e) { id = combineIntoPart(e, *pa, *pb, out); }));
        REQUIRE(doc->partCount() == 1);
        CHECK(doc->selectedId() == id);
        CHECK(doc->part().name() == "comb_a");
        CHECK(doc->part().placed().volume() == Approx(c.volume).epsilon(1e-6));
        CHECK(closed(doc->part().placed()));
        CHECK(doc->part().box().min.z == Approx(doc->placement().lift()).margin(1e-9));
        if (c.mode == CombineMode::Join) {
            // It is where the two were: the union's outline.
            CHECK(doc->part().box().min.x == Approx(-10).margin(1e-6));
            CHECK(doc->part().box().max.x == Approx(20).margin(1e-6));
        } else if (c.mode == CombineMode::Overlap) {
            CHECK(doc->part().box().min.x == Approx(0).margin(1e-6));
            CHECK(doc->part().box().max.x == Approx(10).margin(1e-6));
            CHECK(doc->part().box().size().z == Approx(10).margin(1e-6));
        } else {
            // A's low part is left of B; its top, above B, stays whole.
            CHECK(doc->part().box().min.x == Approx(-10).margin(1e-6));
            CHECK(doc->part().box().max.x == Approx(10).margin(1e-6));
            CHECK(doc->part().box().size().z == Approx(20).margin(1e-6));
        }
        doc->undo();  // both parts are back, as they were
        REQUIRE(doc->partCount() == 2);
        CHECK(doc->findPart(a_id) != nullptr);
        CHECK(doc->findPart(b) != nullptr);
        doc->select(a_id);
    }
}

TEST_CASE("Combine: parts that do not meet", "[gui][shape]") {
    int b = -1;
    const auto doc = docWith(box(20, 20, 20), "comb_far.stl");
    b = addBox(*doc, 20, 20, 10, {60, 0}, "far");
    doc->select(doc->parts()[0].id());
    const ShapeOutcome overlap = combineOf(*doc, b, CombineMode::Overlap);
    CHECK_FALSE(overlap.ok);
    CHECK_THAT(overlap.message, ContainsSubstring("don't overlap"));
    const ShapeOutcome join = combineOf(*doc, b, CombineMode::Join);
    REQUIRE(join.ok);
    CHECK_THAT(join.message, ContainsSubstring("don't touch"));
    const ShapeOutcome sub = combineOf(*doc, b, CombineMode::Subtract);
    REQUIRE(sub.ok);
    CHECK_THAT(sub.message, ContainsSubstring("unchanged"));
    // Taking all of A away leaves nothing: said plainly.
    const int big = addBox(*doc, 40, 40, 40, {0, 0}, "big");
    const ShapeOutcome gone = combineOf(*doc, big, CombineMode::Subtract);
    CHECK_FALSE(gone.ok);
    CHECK_THAT(gone.message, ContainsSubstring("Nothing would be left"));
}

TEST_CASE("Combine: a part that is not a closed solid is named, with what to do", "[gui][shape]") {
    const auto doc = docWith(box(20, 20, 20), "comb_ok.stl");
    const int loose = doc->addPart(NewPart{"loose", "", makePartModel(fruit::prepareModel(demoFaultyModel("loose"))), doc->placement()});
    doc->select(doc->parts()[0].id());
    const ShapeOutcome out = combineOf(*doc, loose, CombineMode::Join);
    CHECK_FALSE(out.ok);
    CHECK_THAT(out.message, ContainsSubstring("loose"));
    CHECK_THAT(out.message, ContainsSubstring("Repair"));
    CHECK(doc->partCount() == 2);
}

TEST_CASE("Combine: the second part is the picked one, the only other one, or none", "[gui][shape]") {
    const auto doc = docWith(box(20, 20, 20), "comb_pick.stl");
    CombineSettings s;
    CHECK(combineSecond(s, *doc) == -1);  // one part
    const int b = addBox(*doc, 10, 10, 10, {40, 0}, "b");
    doc->select(doc->parts()[0].id());
    CHECK(combineSecond(s, *doc) == b);  // the only other
    const int c = addBox(*doc, 10, 10, 10, {-40, 0}, "c");
    doc->select(doc->parts()[0].id());
    CHECK(combineSecond(s, *doc) == -1);  // two to choose from
    s.b = c;
    CHECK(combineSecond(s, *doc) == c);
    s.b = doc->selectedId();  // A itself is not B
    CHECK(combineSecond(s, *doc) == -1);
    s.b = 999;  // gone
    CHECK(combineSecond(s, *doc) == -1);
    CHECK(std::string(combineVerb(CombineMode::Overlap)) == "Keep the overlap");
}
