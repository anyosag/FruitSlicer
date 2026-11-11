// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The parts tools: Duplicate, Arrange, Split, Merge, and the Collisions check.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>

#include "Collisions.h"
#include "Document.h"
#include "PartsTools.h"
#include "mesh/ModelLoad.h"
#include "mesh/Primitives.h"
#include "ops/MeshOps.h"

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using namespace gui;

namespace {

const fruit::PrinterProfile kPrinter = fruit::PrinterProfile::saturn4Ultra16k();

NewPart partOf(const fruit::TriangleMesh& m, const std::string& name, fruit::Vec2 at = {0, 0}) {
    fruit::Placement p;
    p.offset = at;
    return NewPart{name, "", makePartModel(fruit::prepareModel(m)), p};
}

std::shared_ptr<Document> jobOf(std::vector<NewPart> parts) {
    auto doc = std::make_shared<Document>();
    doc->name = "parts";
    doc->printer = kPrinter;
    doc->replaceParts(std::move(parts));
    doc->clearHistory();
    return doc;
}

fruit::TriangleMesh box(double x, double y, double z) { return fruit::makeBox({0, 0, 0}, {x, y, z}); }

// Footprint rectangles clear of each other by `gap`, and inside the plate.
void checkLaidOut(const Document& d, double gap) {
    const auto& ps = d.parts();
    for (std::size_t i = 0; i < ps.size(); ++i) {
        const fruit::BBox2 a = ps[i].footprintBox();
        CHECK(a.min.x >= -d.printer.build_x / 2 - 1e-6);
        CHECK(a.max.x <= d.printer.build_x / 2 + 1e-6);
        CHECK(a.min.y >= -d.printer.build_y / 2 - 1e-6);
        CHECK(a.max.y <= d.printer.build_y / 2 + 1e-6);
        CHECK(ps[i].fit().fits);
        for (std::size_t j = i + 1; j < ps.size(); ++j) CHECK_FALSE(a.overlaps(ps[j].footprintBox(), gap - 1e-6));
    }
}

// Two boxes far apart in one mesh: a part with two shells.
fruit::TriangleMesh twoShells() {
    fruit::TriangleMesh m = fruit::makeBox({0, 0, 0}, {10, 10, 10});
    m.append(fruit::makeBox({30, 0, 0}, {45, 12, 8}));
    return m;
}

}  // namespace

TEST_CASE("Duplicate: copies beside the others, nothing overlaps, undo takes them back", "[gui][parts-tools]") {
    const auto doc = jobOf({partOf(box(20, 20, 10), "bracket", {10, 5})});
    const int src = doc->selectedId();
    const int rev = doc->revision();
    const fruit::BBox2 before = doc->part().footprintBox();
    ToolReport rep;
    REQUIRE(doc->editParts([&](PartsEdit& e) { rep = duplicatePart(*doc, e, 3, kPartGap); }));
    CHECK(rep.done == 3);
    CHECK(rep.wanted == 3);
    CHECK_THAT(rep.message, ContainsSubstring("Made 3 copies"));
    REQUIRE(doc->partCount() == 4);
    CHECK(doc->revision() != rev);  // supports and layers made for the old one are out of date
    // The source did not move; its copies have their own names and the same shared mesh.
    const fruit::BBox2 after = doc->findPart(src)->footprintBox();
    CHECK(after.min.x == Approx(before.min.x));
    CHECK(after.min.y == Approx(before.min.y));
    CHECK(doc->parts()[1].name() == "bracket 2");
    CHECK(doc->parts()[3].name() == "bracket 4");
    CHECK(doc->parts()[1].shared() == doc->parts()[0].shared());
    CHECK(doc->selectedId() == doc->parts()[3].id());
    checkLaidOut(*doc, kPartGap);
    for (const Part& p : doc->parts()) CHECK(p.fit().size.x == Approx(20));
    doc->undo();
    CHECK(doc->partCount() == 1);
    CHECK(doc->revision() == rev);
    CHECK(doc->selectedId() == src);
}

TEST_CASE("Duplicate: says how many fitted when not all did", "[gui][parts-tools]") {
    // 90 x 90 mm on a 218 x 122 mm plate: one copy at most beside it.
    const auto doc = jobOf({partOf(box(90, 90, 10), "slab")});
    ToolReport rep;
    doc->editParts([&](PartsEdit& e) { rep = duplicatePart(*doc, e, 5, kPartGap); });
    CHECK(rep.wanted == 5);
    CHECK(rep.done < 5);
    CHECK(doc->partCount() == static_cast<std::size_t>(1 + rep.done));
    CHECK_THAT(rep.message, ContainsSubstring("of 5") || ContainsSubstring("No copy fitted"));
    checkLaidOut(*doc, kPartGap);

    // No room at all: nothing is made, and nothing is recorded to undo.
    const auto full = jobOf({partOf(box(200, 110, 10), "all")});
    ToolReport none;
    const bool changed = full->editParts([&](PartsEdit& e) { none = duplicatePart(*full, e, 2, kPartGap); });
    CHECK_FALSE(changed);
    CHECK(none.done == 0);
    CHECK_THAT(none.message, ContainsSubstring("No copy fitted"));
    CHECK(full->partCount() == 1);
    CHECK_FALSE(full->canUndo());
}

TEST_CASE("Duplicate: more copies are placed around several parts without touching them", "[gui][parts-tools]") {
    const auto doc = jobOf({partOf(box(30, 20, 10), "a", {-60, 10}), partOf(box(20, 40, 10), "b", {20, -10})});
    doc->select(doc->parts()[1].id());
    ToolReport rep;
    doc->editParts([&](PartsEdit& e) { rep = duplicatePart(*doc, e, 4, 5); });
    CHECK(rep.done >= 1);
    CHECK(doc->partCount() == static_cast<std::size_t>(2 + rep.done));
    checkLaidOut(*doc, 5);
    CHECK(doc->parts()[0].footprintBox().min.x == Approx(-75));  // a did not move
    CHECK(doc->parts()[1].footprintBox().min.x == Approx(10));   // nor did b
}

TEST_CASE("Arrange: every part laid out with the gap, centred, undoable", "[gui][parts-tools]") {
    std::vector<NewPart> parts;
    for (int i = 0; i < 5; ++i) parts.push_back(partOf(box(30 + 5 * i, 25, 10), "p" + std::to_string(i), {0, 0}));  // all on top of each other
    const auto doc = jobOf(std::move(parts));
    const int rev = doc->revision();
    ToolReport rep;
    REQUIRE(doc->editParts([&](PartsEdit& e) { rep = arrangeParts(*doc, e, 6); }));
    CHECK(rep.done == 5);
    CHECK_THAT(rep.message, ContainsSubstring("All 5 parts fit"));
    checkLaidOut(*doc, 6);
    // Centred on the plate.
    fruit::BBox2 all;
    for (const Part& p : doc->parts()) all.extend(p.footprintBox());
    CHECK(all.center().x == Approx(0).margin(1e-6));
    CHECK(all.center().y == Approx(0).margin(1e-6));
    CHECK(doc->revision() != rev);
    // Arranging again with the same gap changes nothing.
    CHECK_FALSE(doc->editParts([&](PartsEdit& e) { rep = arrangeParts(*doc, e, 6); }));
    CHECK_THAT(rep.message, ContainsSubstring("already"));
    doc->undo();
    for (const Part& p : doc->parts()) CHECK(p.placement().offset.x == 0);
    CHECK(doc->revision() == rev);
}

TEST_CASE("Arrange: a part with no room stays where it is, and the report says so", "[gui][parts-tools]") {
    const auto doc = jobOf({partOf(box(40, 40, 10), "small", {-50, 0}), partOf(box(300, 50, 10), "huge", {5, 5})});
    ToolReport rep;
    doc->editParts([&](PartsEdit& e) { rep = arrangeParts(*doc, e, 5); });
    CHECK(rep.done == 1);
    CHECK(rep.wanted == 2);
    CHECK_THAT(rep.message, ContainsSubstring("1 of 2 parts fit"));
    CHECK_THAT(rep.message, ContainsSubstring("huge"));
    CHECK(doc->parts()[1].placement().offset.x == 5);  // untouched
}

TEST_CASE("Split: each shell its own part, standing where it was", "[gui][parts-tools]") {
    const auto doc = jobOf({partOf(twoShells(), "pair", {20, 10}), partOf(box(10, 10, 10), "other", {-60, 0})});
    doc->select(doc->parts()[0].id());
    const int id = doc->selectedId();
    CHECK(shellsOf(doc->part()) == 2);
    const fruit::BBox3 box_before = doc->part().box();
    const int rev = doc->revision();
    ToolReport rep;
    REQUIRE(doc->editParts([&](PartsEdit& e) { rep = splitPart(*doc, e, id); }));
    CHECK(rep.done == 2);
    REQUIRE(doc->partCount() == 3);
    CHECK(doc->findPart(id) == nullptr);
    CHECK(doc->revision() != rev);
    // Names, and the other part untouched.
    std::vector<std::string> names;
    for (const Part& p : doc->parts()) names.push_back(p.name());
    CHECK(std::count(names.begin(), names.end(), "pair 1") == 1);
    CHECK(std::count(names.begin(), names.end(), "pair 2") == 1);
    CHECK(doc->parts()[0].name() == "other");
    // Together they cover what the part covered, at the same height.
    fruit::BBox3 together;
    for (const Part& p : doc->parts())
        if (p.name() != "other") together.extend(p.box());
    CHECK(together.min.x == Approx(box_before.min.x));
    CHECK(together.max.x == Approx(box_before.max.x));
    CHECK(together.min.y >= box_before.min.y - 1e-6);
    CHECK(together.min.z == Approx(box_before.min.z));
    for (const Part& p : doc->parts()) CHECK(shellsOf(p) == 1);
    CHECK(doc->selectedId() == doc->parts()[1].id());
    doc->undo();
    REQUIRE(doc->partCount() == 2);
    CHECK(doc->findPart(id) != nullptr);
    CHECK(doc->revision() == rev);
}

TEST_CASE("Split: one shell is left alone and says why", "[gui][parts-tools]") {
    const auto doc = jobOf({partOf(box(10, 10, 10), "one")});
    ToolReport rep;
    const bool changed = doc->editParts([&](PartsEdit& e) { rep = splitPart(*doc, e, doc->selectedId()); });
    CHECK_FALSE(changed);
    CHECK_FALSE(rep.changed);
    CHECK_THAT(rep.message, ContainsSubstring("one shell"));
    CHECK_FALSE(doc->canUndo());
}

TEST_CASE("Merge: Join as one part keeps the shells and the places", "[gui][parts-tools]") {
    const auto doc = jobOf({partOf(box(10, 10, 10), "a", {-30, 0}), partOf(box(15, 12, 8), "b", {0, 20}),
                            partOf(box(8, 8, 20), "c", {40, -10})});
    const fruit::BBox3 before = doc->placedBox();
    const std::size_t faces = doc->placed().faceCount();
    std::vector<int> ids;
    for (const Part& p : doc->parts()) ids.push_back(p.id());
    const int rev = doc->revision();
    ToolReport rep;
    REQUIRE(doc->editParts([&](PartsEdit& e) { rep = mergeParts(*doc, e, ids, false); }));
    CHECK(rep.done == 3);
    REQUIRE(doc->partCount() == 1);
    CHECK(doc->part().name() == "a");
    CHECK(doc->part().mesh().faceCount() == faces);
    CHECK(shellsOf(doc->part()) == 3);
    CHECK(doc->part().box().min.x == Approx(before.min.x));
    CHECK(doc->part().box().max.x == Approx(before.max.x));
    CHECK(doc->part().box().max.y == Approx(before.max.y));
    CHECK(doc->part().box().min.z == Approx(before.min.z));
    CHECK(doc->part().box().max.z == Approx(before.max.z));
    CHECK(doc->revision() != rev);
    doc->undo();
    CHECK(doc->partCount() == 3);
}

TEST_CASE("Merge: only the chosen parts are joined", "[gui][parts-tools]") {
    const auto doc = jobOf({partOf(box(10, 10, 10), "a", {-30, 0}), partOf(box(10, 10, 10), "b", {0, 0}),
                            partOf(box(10, 10, 10), "c", {30, 0})});
    const std::vector<int> two = {doc->parts()[0].id(), doc->parts()[2].id()};
    ToolReport rep;
    doc->editParts([&](PartsEdit& e) { rep = mergeParts(*doc, e, two, false); });
    CHECK(doc->partCount() == 2);
    CHECK(doc->parts()[0].name() == "b");
    CHECK(doc->parts()[0].placement().offset.x == 0);  // untouched
    // One part is not enough.
    const std::vector<int> one = {doc->parts()[0].id()};
    CHECK_FALSE(doc->editParts([&](PartsEdit& e) { rep = mergeParts(*doc, e, one, false); }));
    CHECK_THAT(rep.message, ContainsSubstring("at least two"));
}

TEST_CASE("Merge: Fuse where they overlap makes one solid", "[gui][parts-tools]") {
    // Two 10 mm cubes, 5 mm into each other: 1500 mm3 as one.
    const auto doc = jobOf({partOf(box(10, 10, 10), "a", {0, 0}), partOf(box(10, 10, 10), "b", {5, 0})});
    std::vector<int> ids = {doc->parts()[0].id(), doc->parts()[1].id()};
    ToolReport rep;
    REQUIRE(doc->editParts([&](PartsEdit& e) { rep = mergeParts(*doc, e, ids, true); }));
    REQUIRE(doc->partCount() == 1);
    CHECK_THAT(rep.message, ContainsSubstring("Fused 2 parts"));
    CHECK(shellsOf(doc->part()) == 1);
    CHECK(doc->part().mesh().volume() == Approx(1500).epsilon(1e-6));
    CHECK(doc->part().box().min.x == Approx(-5));
    CHECK(doc->part().box().max.x == Approx(10));
    CHECK(doc->part().box().min.z == Approx(doc->placement().lift()));
}

TEST_CASE("Merge: Fuse says it fused when a part with a loose bit of its own overlaps another", "[gui][parts-tools]") {
    // a has two shells (its outline from x -22.5 to 22.5, the first box -22.5 to -12.5); b overlaps that first box
    // by 7.5 mm. Fused: that box and b are one shell, a's other box another, so two shells for two parts.
    const auto doc = jobOf({partOf(twoShells(), "a", {0, 0}), partOf(box(10, 10, 10), "b", {-15, -1})});
    REQUIRE(shellsOf(doc->parts()[0]) == 2);
    std::vector<int> ids = {doc->parts()[0].id(), doc->parts()[1].id()};
    ToolReport rep;
    REQUIRE(doc->editParts([&](PartsEdit& e) { rep = mergeParts(*doc, e, ids, true); }));
    REQUIRE(doc->partCount() == 1);
    CHECK(shellsOf(doc->part()) == 2);
    CHECK(doc->part().mesh().volume() == Approx(1000 + 15 * 12 * 8 + 1000 - 750).epsilon(1e-6));
    CHECK_THAT(rep.message, ContainsSubstring("Fused 2 parts"));
    CHECK_THAT(rep.message, !ContainsSubstring("nothing was fused"));

    // The same part far from b: nothing meets, and it says so with all three shells.
    const auto apart = jobOf({partOf(twoShells(), "a", {0, 0}), partOf(box(10, 10, 10), "b", {0, 60})});
    ids = {apart->parts()[0].id(), apart->parts()[1].id()};
    REQUIRE(apart->editParts([&](PartsEdit& e) { rep = mergeParts(*apart, e, ids, true); }));
    CHECK_THAT(rep.message, ContainsSubstring("nothing was fused"));
    CHECK_THAT(rep.message, ContainsSubstring("3 shells"));
}

TEST_CASE("Merge: fusing parts that do not overlap says nothing was fused; an open mesh stops it", "[gui][parts-tools]") {
    const auto apart = jobOf({partOf(box(10, 10, 10), "a", {-30, 0}), partOf(box(10, 10, 10), "b", {30, 0})});
    std::vector<int> ids = {apart->parts()[0].id(), apart->parts()[1].id()};
    ToolReport rep;
    REQUIRE(apart->editParts([&](PartsEdit& e) { rep = mergeParts(*apart, e, ids, true); }));
    CHECK_THAT(rep.message, ContainsSubstring("nothing was fused"));
    CHECK(shellsOf(apart->part()) == 2);

    fruit::TriangleMesh open = box(10, 10, 10);
    open.faces.pop_back();  // a hole
    fruit::LoadedModel lm = fruit::prepareModel(open, false);
    fruit::Placement p;
    p.offset = {30, 0};
    const auto bad = jobOf({partOf(box(10, 10, 10), "a", {-30, 0})});
    bad->addPart(NewPart{"holey", "", makePartModel(lm), p});
    ids = {bad->parts()[0].id(), bad->parts()[1].id()};
    const int rev = bad->revision();
    CHECK_FALSE(bad->editParts([&](PartsEdit& e) { rep = mergeParts(*bad, e, ids, true); }));
    CHECK_THAT(rep.message, ContainsSubstring("Can't fuse"));
    CHECK(bad->partCount() == 2);
    CHECK(bad->revision() == rev);
    // Join still works with it.
    CHECK(bad->editParts([&](PartsEdit& e) { rep = mergeParts(*bad, e, ids, false); }));
    CHECK(bad->partCount() == 1);
}

TEST_CASE("Names for copies", "[gui][parts-tools]") {
    CHECK(numberedName("bracket", {"bracket"}) == "bracket 2");
    CHECK(numberedName("bracket 2", {"bracket", "bracket 2"}) == "bracket 3");
    CHECK(numberedName("bracket", {"bracket", "bracket 2", "bracket 3"}) == "bracket 4");
    CHECK(numberedName("part A", {"part A"}) == "part A 2");
}

TEST_CASE("Collisions: overlapping, close and clear pairs", "[gui][collisions]") {
    // a: x -10..10; b overlaps it; c is 1 mm from b; d is far.
    const auto doc = jobOf({partOf(box(20, 20, 10), "a", {0, 0}), partOf(box(20, 20, 10), "b", {15, 0}),
                            partOf(box(20, 20, 10), "c", {36, 0}), partOf(box(20, 20, 10), "d", {-80, 0})});
    const CollisionReport r = checkCollisions(*doc, 2.0, fruit::JobControl::none());
    CHECK(r.revision == doc->revision());
    CHECK(r.parts == 4);
    CHECK(r.not_checked.empty());
    REQUIRE(r.problems.size() == 2);
    const int a = doc->parts()[0].id(), b = doc->parts()[1].id(), c = doc->parts()[2].id();
    CHECK(r.problems[0].a == a);
    CHECK(r.problems[0].b == b);
    CHECK(r.problems[0].overlaps);
    CHECK(r.problems[0].overlap_mm3 == Approx(5 * 20 * 10).epsilon(1e-3));
    CHECK(r.problems[1].a == b);
    CHECK(r.problems[1].b == c);
    CHECK_FALSE(r.problems[1].overlaps);
    CHECK(r.problems[1].gap == Approx(1.0).margin(1e-3));
    CHECK(r.overlapping() == 1);
    CHECK_FALSE(r.clear());
    CHECK_THAT(describePair(r.problems[0]), ContainsSubstring("overlap"));
    CHECK_THAT(describePair(r.problems[1]), ContainsSubstring("1.0 mm apart"));
    // A smaller least gap lets the close pair go.
    const CollisionReport r2 = checkCollisions(*doc, 0.5, fruit::JobControl::none());
    CHECK(r2.problems.size() == 1);
}

TEST_CASE("Collisions: parts that touch, a mirrored part, one part, an open mesh", "[gui][collisions]") {
    const auto touch = jobOf({partOf(box(20, 20, 10), "a", {0, 0}), partOf(box(20, 20, 10), "b", {20, 0})});
    const CollisionReport t = checkCollisions(*touch, 2.0, fruit::JobControl::none());
    REQUIRE(t.problems.size() == 1);
    CHECK_FALSE(t.problems[0].overlaps);
    CHECK(t.problems[0].gap == Approx(0).margin(1e-6));
    CHECK_THAT(describePair(t.problems[0]), ContainsSubstring("touch"));

    // Mirroring moves nothing but turns the mesh inside out in its own frame: still found.
    touch->select(touch->parts()[1].id());
    touch->mirror(0);
    CHECK(checkCollisions(*touch, 2.0, fruit::JobControl::none()).problems.size() == 1);
    touch->undo();

    const auto one = jobOf({partOf(box(20, 20, 10), "a")});
    const CollisionReport o = checkCollisions(*one, 2.0, fruit::JobControl::none());
    CHECK(o.problems.empty());
    CHECK(o.pairs_checked == 0);

    fruit::TriangleMesh open = box(10, 10, 10);
    open.faces.pop_back();
    const auto bad = jobOf({partOf(box(20, 20, 10), "a", {-30, 0})});
    fruit::Placement p;
    p.offset = {30, 0};
    bad->addPart(NewPart{"holey", "", makePartModel(fruit::prepareModel(open, false)), p});
    const CollisionReport b = checkCollisions(*bad, 2.0, fruit::JobControl::none());
    REQUIRE(b.not_checked.size() == 1);
    CHECK(b.not_checked[0].part == bad->parts()[1].id());
    CHECK_FALSE(b.not_checked[0].why.empty());
    CHECK_FALSE(b.clear());
}

TEST_CASE("Collisions: can be cancelled", "[gui][collisions]") {
    const auto doc = jobOf({partOf(box(20, 20, 10), "a", {0, 0}), partOf(box(20, 20, 10), "b", {15, 0})});
    fruit::JobControl ctl;
    ctl.cancel();
    CHECK_THROWS_AS(checkCollisions(*doc, 2.0, ctl), fruit::Cancelled);
}
