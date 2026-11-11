// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Several parts on one plate: the list and its undo, what prints, the tip
// edits and holes that belong to a part, and saved jobs old and new.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstring>
#include <filesystem>

#include <miniz.h>
#include <nlohmann/json.hpp>

#include "Document.h"
#include "Hollows.h"
#include "JobFile.h"
#include "Layers.h"
#include "Navigation.h"
#include "Supports.h"
#include "geometry/Polygon.h"
#include "mesh/ModelLoad.h"
#include "mesh/Primitives.h"
#include "mesh/StlIO.h"
#include "support/SupportIO.h"

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using namespace gui;
namespace fs = std::filesystem;

namespace {

const fruit::PrinterProfile kPrinter = fruit::PrinterProfile::saturn4Ultra16k();

fs::path dir() {
    const auto d = fs::temp_directory_path() / "fruit_parts_tests";
    fs::create_directories(d);
    return d;
}

NewPart partOf(const fruit::TriangleMesh& m, const std::string& name, fruit::Vec2 at = {0, 0}) {
    fruit::Placement p;
    p.offset = at;
    return NewPart{name, "", makePartModel(fruit::prepareModel(m)), p};
}

// A job with these parts, as loading leaves it: nothing to undo.
std::shared_ptr<Document> jobOf(std::vector<NewPart> parts) {
    auto doc = std::make_shared<Document>();
    doc->name = "parts";
    doc->printer = kPrinter;
    doc->replaceParts(std::move(parts));
    doc->clearHistory();
    return doc;
}

fruit::TriangleMesh cube(double s) { return fruit::makeBox({0, 0, 0}, {s, s, s}); }

// The saved job `from` written again as `to`, with `file` in it holding `bytes`.
void rewriteJob(const std::string& from, const std::string& to, const std::string& file,
                const std::vector<std::uint8_t>& bytes) {
    mz_zip_archive in{};
    REQUIRE(mz_zip_reader_init_file(&in, from.c_str(), 0));
    fs::remove(to);
    mz_zip_archive out{};
    REQUIRE(mz_zip_writer_init_file(&out, to.c_str(), 0));
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&in); ++i) {
        char name[256] = {};
        mz_zip_reader_get_filename(&in, i, name, sizeof name);
        if (file == name) continue;
        size_t n = 0;
        void* data = mz_zip_reader_extract_to_heap(&in, i, &n, 0);
        REQUIRE(data);
        REQUIRE(mz_zip_writer_add_mem(&out, name, data, n, MZ_DEFAULT_COMPRESSION));
        mz_free(data);
    }
    REQUIRE(mz_zip_writer_add_mem(&out, file.c_str(), bytes.data(), bytes.size(), MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_finalize_archive(&out));
    REQUIRE(mz_zip_writer_end(&out));
    mz_zip_reader_end(&in);
}

}  // namespace

TEST_CASE("Parts: adding, selecting, removing, and the job's placed mesh", "[gui][parts]") {
    const auto doc = jobOf({partOf(cube(10), "a", {-30, 0})});
    REQUIRE(doc->partCount() == 1);
    const int a = doc->selectedId();
    CHECK_FALSE(doc->canUndo());
    CHECK_FALSE(doc->removePart(a));  // a job keeps one part

    const int r0 = doc->revision();
    const int b = doc->addPart(partOf(fruit::makeBox({0, 0, 0}, {20, 10, 5}), "b", {30, 0}));
    REQUIRE(b >= 0);
    CHECK(b != a);
    CHECK(doc->partCount() == 2);
    CHECK(doc->selectedId() == b);  // the new part is selected
    CHECK(doc->revision() != r0);   // what was made for one part is out of date
    CHECK(doc->canUndo());
    // Every part in the job's mesh, in order.
    const fruit::TriangleMesh& all = doc->placed();
    CHECK(all.faceCount() == doc->parts()[0].mesh().faceCount() + doc->parts()[1].mesh().faceCount());
    CHECK(doc->partOfFace(0) == 0);
    CHECK(doc->partOfFace(all.faceCount() - 1) == 1);
    CHECK(doc->placedBox().min.x == Approx(-35));
    CHECK(doc->placedBox().max.x == Approx(40));
    CHECK(doc->partNear({-30, 0, 10}) == a);
    CHECK(doc->partNear({30, 0, 7}) == b);
    CHECK(doc->fits());

    // The tools act on the selected part only.
    doc->select(a);
    fruit::Placement p = doc->placement();
    p.scale = 2;
    doc->edit(p);
    CHECK(doc->findPart(a)->placement().scale == 2);
    CHECK(doc->findPart(b)->placement().scale == 1);
    CHECK(doc->findPart(a)->fit().size.x == Approx(20));
    // The lift is the job's: flat on the plate goes for every part.
    p = doc->placement();
    p.on_plate = true;
    doc->edit(p);
    CHECK(doc->findPart(b)->placement().on_plate);
    CHECK(doc->placed().bbox().min.z == Approx(0));

    CHECK(doc->removePart(b));
    CHECK(doc->partCount() == 1);
    CHECK(doc->selectedId() == a);
    CHECK(doc->findPart(b) == nullptr);
    doc->undo();  // the removal
    REQUIRE(doc->partCount() == 2);
    CHECK(doc->findPart(b)->placement().on_plate);
    // A part added to a job flat on the plate stands flat too.
    const int c = doc->addPart(partOf(cube(5), "c", {0, 40}));
    CHECK(doc->findPart(c)->placement().on_plate);
}

TEST_CASE("Parts: undo and redo put the list back as it was, revisions too", "[gui][parts]") {
    const auto doc = jobOf({partOf(cube(10), "a")});
    const int a = doc->selectedId();
    const int r0 = doc->revision(), a0 = doc->part().revision();
    const int b = doc->addPart(partOf(cube(8), "b", {30, 0}));
    const int r1 = doc->revision();
    fruit::Placement p = doc->placement();  // b's
    p.offset = {40, 10};
    doc->edit(p);
    const int r2 = doc->revision();
    REQUIRE(doc->replaceMesh(a, fruit::prepareModel(cube(12))));
    const int r3 = doc->revision();
    CHECK(doc->findPart(a)->mesh().bbox().size().x == Approx(12));
    CHECK(doc->findPart(a)->revision() != a0);
    CHECK(r0 != r1);
    CHECK(r1 != r2);
    CHECK(r2 != r3);

    doc->undo();  // the new mesh
    CHECK(doc->revision() == r2);
    CHECK(doc->findPart(a)->mesh().bbox().size().x == Approx(10));
    CHECK(doc->findPart(a)->revision() == a0);
    doc->undo();  // b moved
    CHECK(doc->revision() == r1);
    CHECK(doc->findPart(b)->placement().offset.x == Approx(30));
    doc->undo();  // b added
    CHECK(doc->revision() == r0);
    CHECK(doc->partCount() == 1);
    CHECK(doc->selectedId() == a);
    CHECK_FALSE(doc->canUndo());

    doc->redo();
    CHECK(doc->partCount() == 2);
    CHECK(doc->revision() == r1);
    doc->redo();
    doc->redo();
    CHECK(doc->revision() == r3);
    CHECK(doc->findPart(b)->placement().offset.x == Approx(40));
    CHECK(doc->findPart(a)->mesh().bbox().size().x == Approx(12));
    CHECK_FALSE(doc->canRedo());

    // Supports made for a revision count again when undo brings it back.
    doc->supports.result = std::make_shared<const SupportResult>();
    doc->supports.result_revision = doc->revision();
    doc->supports.result_settings = doc->supportSettings();
    CHECK(doc->supports.fresh(doc->revision(), doc->supportSettings()));
    doc->removePart(b);
    CHECK_FALSE(doc->supports.fresh(doc->revision(), doc->supportSettings()));
    doc->undo();
    CHECK(doc->supports.fresh(doc->revision(), doc->supportSettings()));
}

TEST_CASE("Parts: editParts is one undo step, and nothing when nothing changed", "[gui][parts]") {
    const auto doc = jobOf({partOf(cube(10), "a"), partOf(cube(6), "b", {30, 0})});
    const int r0 = doc->revision();
    CHECK_FALSE(doc->editParts([](PartsEdit&) {}));
    CHECK_FALSE(doc->canUndo());
    CHECK(doc->revision() == r0);

    // Split-like: one part goes, two come in its place; all one step.
    const int a = doc->parts()[0].id();
    int left = -1, right = -1;
    CHECK(doc->editParts([&](PartsEdit& e) {
        e.remove(a);
        left = e.add(partOf(cube(4), "a 1", {-20, 0}));
        right = e.add(partOf(cube(4), "a 2", {-10, 0}));
        e.selected = right;
    }));
    CHECK(doc->partCount() == 3);
    CHECK(doc->selectedId() == right);
    CHECK(doc->findPart(left)->name() == "a 1");
    CHECK(left != a);
    CHECK(right != a);
    doc->undo();
    CHECK(doc->partCount() == 2);
    CHECK(doc->findPart(a) != nullptr);
    CHECK(doc->revision() == r0);
    // A job is never left empty.
    CHECK_FALSE(doc->editParts([](PartsEdit& e) { e.parts().clear(); }));
    CHECK(doc->partCount() == 2);
    // Arrange-like: several placements at once; ids are never used twice.
    const int b = doc->parts()[1].id();
    CHECK(doc->editParts([&](PartsEdit& e) {
        e.find(a)->placement.offset = {-40, 0};
        e.find(b)->placement.offset = {40, 0};
    }));
    CHECK(doc->findPart(a)->placement().offset.x == Approx(-40));
    CHECK(doc->findPart(b)->placement().offset.x == Approx(40));
    const int d = doc->addPart(partOf(cube(3), "d"));
    CHECK(d != left);
    CHECK(d != right);
}

TEST_CASE("Parts: a drag moves the selected part, as one undo step", "[gui][parts]") {
    const auto doc = jobOf({partOf(cube(10), "a"), partOf(cube(6), "b", {30, 0})});
    const int b = doc->parts()[1].id();
    doc->select(b);
    const int r0 = doc->revision();
    doc->beginDrag();
    fruit::Placement p = doc->placement();
    for (int i = 1; i <= 3; ++i) {
        p.offset.y = 5.0 * i;
        doc->preview(p);
    }
    CHECK(doc->findPart(b)->box().center().y == Approx(15));
    CHECK(doc->placedBox().max.y == Approx(18));
    doc->endDrag();
    CHECK(doc->parts()[0].placement().offset.y == 0);
    doc->undo();
    CHECK(doc->revision() == r0);
    CHECK(doc->findPart(b)->placement().offset.y == 0);
    // Dragged back where it started: nothing to undo, the same revision.
    doc->beginDrag();
    p = doc->placement();
    p.offset.y = 7;
    doc->preview(p);
    p.offset.y = 0;
    doc->preview(p);
    doc->endDrag();
    CHECK(doc->revision() == r0);
    CHECK(doc->canRedo());  // what was undone before is still there to redo
}

TEST_CASE("Parts: a new part goes beside the others, clear of them", "[gui][parts]") {
    fruit::BBox2 a;
    a.extend(fruit::Vec2{-10, -10});
    a.extend(fruit::Vec2{10, 10});
    // Nothing on the plate: the middle.
    const fruit::Vec2 first = freeSpot({}, {20, 20}, kPrinter, 3);
    CHECK(first.x == 0);
    CHECK(first.y == 0);
    const fruit::Vec2 c = freeSpot({a}, {20, 20}, kPrinter, 3);
    fruit::BBox2 n;
    n.extend(c - fruit::Vec2{10, 10});
    n.extend(c + fruit::Vec2{10, 10});
    CHECK_FALSE(n.overlaps(a, kPartGap - 1e-6));
    CHECK(fruit::distance(c, {0, 0}) < 26.01);  // right beside it
    CHECK(std::abs(c.x) + 10 <= kPrinter.build_x / 2 - 3 + 1e-9);
    // No room left: off the plate to the right, where the list shows it.
    fruit::BBox2 big;
    big.extend(fruit::Vec2{-kPrinter.build_x / 2, -kPrinter.build_y / 2});
    big.extend(fruit::Vec2{kPrinter.build_x / 2, kPrinter.build_y / 2});
    const fruit::Vec2 off = freeSpot({big}, {20, 20}, kPrinter, 3);
    CHECK(off.x - 10 > kPrinter.build_x / 2);

    CHECK(partName("/models/bracket.stl", {}) == "bracket");
    CHECK(partName("/models/bracket.stl", {"bracket"}) == "bracket 2");
    CHECK(partName("/models/bracket.stl", {"bracket", "bracket 2"}) == "bracket 3");
    CHECK(partName("", {}) == "Part");
}

TEST_CASE("Parts: footprints and frames", "[gui][parts]") {
    const auto doc = jobOf({partOf(cube(10), "a", {-20, 0}), partOf(cube(6), "b", {20, 5})});
    const Part& b = doc->parts()[1];
    const fruit::BBox2 fb = b.footprintBox();
    CHECK(fb.min.x == Approx(17));
    CHECK(fb.max.y == Approx(8));
    const auto& hull = b.footprint();
    REQUIRE(hull.size() == 4);
    double area = 0;
    for (std::size_t i = 0; i < hull.size(); ++i) area += fruit::cross(hull[i], hull[(i + 1) % hull.size()]) / 2;
    CHECK(area == Approx(36));  // counter-clockwise
    const PartFrames f = doc->frames();
    REQUIRE(f.find(b.id()));
    CHECK(f.find(b.id())->apply({0, 0, 0}).x == Approx(17));
    CHECK(f.find(12345) == nullptr);
}

TEST_CASE("Parts: tip edits and holes stay on their part", "[gui][parts]") {
    const auto doc = jobOf({partOf(cube(10), "a", {-20, 0}), partOf(cube(10), "b", {20, 0})});
    const int a = doc->parts()[0].id(), b = doc->parts()[1].id();
    // A tip added by hand under b, in b's own coordinates.
    TipEdit e;
    e.kind = TipEdit::Kind::Add;
    e.part = b;
    e.at = {5, 5, 0};
    e.normal = {0, 0, -1};
    const auto tipX = [&] {
        const auto pts = applyEdits({}, {e}, doc->frames());
        return pts.empty() ? -1e9 : pts[0].pos.x;
    };
    CHECK(tipX() == Approx(20));
    // It follows b when b moves, and stays when a moves.
    doc->select(b);
    fruit::Placement p = doc->placement();
    p.offset.x = 35;
    doc->edit(p);
    CHECK(tipX() == Approx(35));
    doc->select(a);
    p = doc->placement();
    p.offset.x = -40;
    doc->edit(p);
    CHECK(tipX() == Approx(35));
    // Left out while b is off the job; back with it.
    doc->removePart(b);
    CHECK(applyEdits({}, {e}, doc->frames()).empty());
    doc->undo();
    CHECK(tipX() == Approx(35));

    // Recording a change to a planner's tip on b keeps the edit on b.
    fruit::SupportPoint tip;
    tip.pos = {35, 0, 5};
    tip.normal = {0, 0, -1};
    std::vector<TipEdit> edits;
    REQUIRE(recordDelete(edits, tip, doc->frames(), b));
    CHECK(edits[0].part == b);
    CHECK(edits[0].at.x == Approx(5));  // b's own coordinates: its middle

    // Holes: each drilled with its own part's transform.
    fruit::DrainHole h;
    h.at = {5, 5, 10};
    h.axis = {0, 0, -1};
    h.part = b;
    fruit::DrainHole g = h;
    g.part = a;
    const fruit::LayerStack solid = fruit::sliceModel(doc->placed(), 0.05, nullptr, 0, fruit::JobControl::none());
    const std::vector<fruit::PlacedHole> placed = placeHoles({h, g}, doc->frames(), solid, fruit::HollowResult{});
    REQUIRE(placed.size() == 2);
    CHECK(placed[0].to.x == Approx(35));
    CHECK(placed[1].to.x == Approx(-40));
}

TEST_CASE("Parts: two parts slice into layers that hold both", "[gui][parts][slow]") {
    // Flat on the plate, no supports: a 10 mm cube and a 20 × 10 × 5 mm slab.
    const auto doc = jobOf({partOf(cube(10), "cube", {-25, 0}), partOf(fruit::makeBox({0, 0, 0}, {20, 10, 5}), "slab", {25, 0})});
    fruit::Placement p = doc->placement();
    p.on_plate = true;
    doc->edit(p);
    const LayerResult r = sliceLayers(*doc, fruit::TriangleMesh{}, 0.0, fruit::JobControl::none());
    REQUIRE(r.layers);
    const fruit::LayerStack& ls = *r.layers;
    CHECK(r.top == Approx(10).margin(0.06));
    const std::size_t low = ls.layerAt(2.5), high = ls.layerAt(7.5);
    CHECK(fruit::area(ls.slices[low]) == Approx(300).epsilon(0.01));  // both
    CHECK(fruit::area(ls.slices[high]) == Approx(100).epsilon(0.01));  // the cube alone
    const fruit::BBox2 b = fruit::bbox(ls.slices[low]);
    CHECK(b.min.x == Approx(-30).margin(0.01));
    CHECK(b.max.x == Approx(35).margin(0.01));
    CHECK(r.bounds.min.x == Approx(-30));
    CHECK(r.bounds.max.x == Approx(35));
}

TEST_CASE("Parts: supports and auto drain holes cover every part", "[gui][parts][slow]") {
    // Two balls with a sealed hollow each, raised on supports.
    auto ball = [] {
        fruit::TriangleMesh m = fruit::makeSphere({0, 0, 10}, 10, 48);
        fruit::TriangleMesh v = fruit::makeSphere({0, 0, 10}, 8, 48);
        for (auto& f : v.faces) std::swap(f[1], f[2]);
        m.append(v);
        return m;
    };
    const auto doc = jobOf({partOf(ball(), "left", {-30, 0}), partOf(ball(), "right", {30, 0})});
    const int left = doc->parts()[0].id(), right = doc->parts()[1].id();
    CHECK(hollowsToFix(*doc) == 2);
    const HollowPlan plan = planHollow(*doc, HollowChoices{}, {}, kLayerHeight, fruit::JobControl::none());
    REQUIRE(plan.closed.cavities.size() == 2);
    REQUIRE(plan.holes.size() == 4);  // a drain and an air hole for each
    std::size_t on_left = 0, on_right = 0;
    for (std::size_t i = 0; i < plan.holes.size(); ++i) {
        on_left += plan.holes[i].part == left;
        on_right += plan.holes[i].part == right;
        CHECK(plan.placed[i].cavity >= 0);
        // In its part's own coordinates: near that ball's middle.
        CHECK(std::abs(plan.holes[i].at.x) < 10.5);
    }
    CHECK(on_left == 2);
    CHECK(on_right == 2);
    CHECK(plan.undrained() == 0);

    const SupportResult s = generate(*doc, SupportChoices{}, doc->visibleInPrinter(), fruit::JobControl::none());
    std::size_t tips_left = 0, tips_right = 0;
    for (const auto& t : s.points) (t.pos.x < 0 ? tips_left : tips_right) += 1;
    CHECK(tips_left > 0);
    CHECK(tips_right > 0);
}

TEST_CASE("Jobs: a job saved before parts (version 2) still opens the same", "[gui][parts][jobs][slow]") {
    // As FruitSlicer wrote a one-model job before parts (303da3c): job.json
    // with "model" and "placement", the mesh as model.bin, tip edits and
    // holes with no part named.
    const fruit::LoadedModel model = fruit::prepareModel(demoModel());
    const fruit::TriangleMesh& m = model.mesh;
    std::vector<std::uint8_t> bin(16 + m.vertices.size() * 24 + m.faces.size() * 12);
    {
        std::uint8_t* q = bin.data();
        const char magic[8] = {'F', 'S', 'M', 'E', 'S', 'H', '1', 0};
        std::memcpy(q, magic, 8);
        const auto nv = static_cast<std::uint32_t>(m.vertices.size()), nf = static_cast<std::uint32_t>(m.faces.size());
        std::memcpy(q + 8, &nv, 4);
        std::memcpy(q + 12, &nf, 4);
        q += 16;
        for (const fruit::Vec3& v : m.vertices) {
            const double xyz[3] = {v.x, v.y, v.z};
            std::memcpy(q, xyz, 24);
            q += 24;
        }
        for (const fruit::Face& f : m.faces) {
            const std::int32_t abc[3] = {f[0], f[1], f[2]};
            std::memcpy(q, abc, 12);
            q += 12;
        }
    }
    fruit::Placement p;
    p.scale = 1.25;
    p.rotation_deg = {0, 0, 30};
    p.offset = {12.5, -8};
    // The planner's tips for it, as that FruitSlicer saved them.
    const auto ref = jobOf({NewPart{"bracket", "", makePartModel(model), p}});
    ref->orient().visible = {{0, 0, 1}};
    SupportChoices c;
    c.usual_tip = 3;
    c.advanced["head_length"] = 1.4;
    TipEdit zone;
    zone.kind = TipEdit::Kind::Zone;
    zone.at = {15, 10, 4};
    zone.radius = kZoneRadius;
    const SupportResult r = generate(*ref, c, ref->visibleInPrinter(), fruit::JobControl::none(), {zone});

    const std::string stl = (dir() / "old bracket.stl").string();
    fruit::writeStl(stl, demoModel());
    using nlohmann::json;
    json j = {{"format", "fruitslicer-job"},
              {"version", 2},
              {"name", "old bracket"},
              {"model", {{"path", stl}, {"file", "model.bin"}, {"hash", std::to_string(meshHash(m))}}},
              {"placement", {{"scale", 1.25}, {"rotation", {0, 0, 30}}, {"elevation", 5.0}, {"on_plate", false},
                             {"offset", {12.5, -8}}}},
              {"orient", {{"visible", {{0, 0, 1}}}, {"decided", true}}},
              {"supports",
               {{"choices", {{"usual_tip", 3}, {"density", 1.0}, {"merge", true}, {"advanced", {{"head_length", 1.4}}}}},
                {"edits", {{{"kind", "zone"}, {"at", {15, 10, 4}}, {"to", {0, 0, 0}}, {"normal", {0, 0, 0}},
                            {"diameter", 0.4}, {"radius", kZoneRadius}}}},
                {"tips", fruit::pointsToJson(r.planned)},
                {"planned_around", json::array({{{"kind", "zone"}, {"at", {15, 10, 4}}, {"to", {0, 0, 0}},
                                                 {"normal", {0, 0, 0}}, {"diameter", 0.4}, {"radius", kZoneRadius}}})},
                {"warnings", json::array()}}},
              {"hollow", {{"applied", false}, {"fix", "holes"}, {"hollow", false}, {"wall", 2.0}, {"ceiling_lean", 45.0},
                          {"hole_diameter", 3.0}, {"auto_holes", false},
                          {"holes", {{{"at", {20, 10, 12}}, {"axis", {0, 0, -1}}, {"diameter", 3.0}}}}}},
              {"print", {{"exposure", 2.5}, {"bottom_exposure", 30.0}, {"bottom_layers", -1}, {"transition_layers", -1},
                         {"sample_path", ""}}}};
    const std::string path = (dir() / "old bracket.fruitjob").string();
    {
        fs::remove(path);
        mz_zip_archive z{};
        REQUIRE(mz_zip_writer_init_file(&z, path.c_str(), 0));
        const std::string text = j.dump(2);
        REQUIRE(mz_zip_writer_add_mem(&z, "job.json", text.data(), text.size(), MZ_DEFAULT_COMPRESSION));
        REQUIRE(mz_zip_writer_add_mem(&z, "model.bin", bin.data(), bin.size(), MZ_DEFAULT_COMPRESSION));
        REQUIRE(mz_zip_writer_finalize_archive(&z));
        REQUIRE(mz_zip_writer_end(&z));
    }

    const OpenedJob o = openJob(path, kPrinter, fruit::JobControl::none());
    const Document& d = *o.doc;
    CHECK_FALSE(o.model_missing);
    CHECK(o.model_path == stl);
    CHECK(d.name == "old bracket");
    REQUIRE(d.partCount() == 1);
    const Part& part = d.part();
    CHECK(part.name() == "old bracket");
    CHECK(part.path() == stl);
    CHECK(meshHash(part.mesh()) == meshHash(m));
    CHECK(part.placement() == p);
    // Placed where it was: the same as placing the model by hand.
    const fruit::BBox3 want = fruit::placeModel(m, p).bbox();
    CHECK(d.placed().bbox().min.x == Approx(want.min.x));
    CHECK(d.placed().bbox().max.y == Approx(want.max.y));
    CHECK(d.placed().bbox().min.z == Approx(5));
    CHECK(d.orient().visible.size() == 1);
    CHECK(d.orient().decided);
    CHECK(d.supports.choices.usual_tip == 3);
    REQUIRE(d.supports.edits.size() == 1);
    CHECK(d.supports.edits[0].part == part.id());
    REQUIRE(d.hollow.manual.size() == 1);
    CHECK(d.hollow.manual[0].part == part.id());
    CHECK(d.hollow.manual[0].at.x == Approx(20));
    // Its supports come back as they were made.
    REQUIRE(d.supports.result);
    CHECK(d.supports.fresh(d.revision(), d.supportSettings()));
    CHECK(d.supports.result->tips == r.tips);
    CHECK(d.supports.result->points.size() == r.points.size());
    CHECK(d.print.settings.exposure == 2.5);
    CHECK_FALSE(d.unsaved());

    // Saved again, it is still a version 2 job older FruitSlicers open.
    const std::string again = (dir() / "old bracket again.fruitjob").string();
    saveJob(again, d);
    {
        mz_zip_archive z{};
        REQUIRE(mz_zip_reader_init_file(&z, again.c_str(), 0));
        size_t n = 0;
        void* data = mz_zip_reader_extract_file_to_heap(&z, "job.json", &n, 0);
        REQUIRE(data);
        const json k = json::parse(static_cast<const char*>(data), static_cast<const char*>(data) + n);
        mz_free(data);
        CHECK(mz_zip_reader_locate_file(&z, "model.bin", nullptr, 0) >= 0);
        mz_zip_reader_end(&z);
        CHECK(k["version"] == 2);
        CHECK_FALSE(k.contains("parts"));
        CHECK(k["placement"] == j["placement"]);
        CHECK(k["orient"] == j["orient"]);
        CHECK(k["supports"]["edits"] == j["supports"]["edits"]);
        CHECK(k["hollow"]["holes"] == j["hollow"]["holes"]);
    }
}

TEST_CASE("Jobs: several parts are saved as version 3 and come back", "[gui][parts][jobs][slow]") {
    const auto doc = jobOf({partOf(demoModel(), "bracket", {-40, 0}), partOf(cube(12), "cube", {40, 10})});
    const int a = doc->parts()[0].id(), b = doc->parts()[1].id();
    doc->removePart(a);  // ids go on from here: the saved ones are renumbered
    doc->undo();
    const int c = doc->addPart(partOf(cube(6), "small", {0, -30}));
    doc->orientOf(b).visible = {{1, 0, 0}};
    doc->orientOf(b).decided = true;
    doc->select(b);
    TipEdit e;
    e.kind = TipEdit::Kind::Zone;
    e.part = b;
    e.at = {6, 6, 0};
    e.radius = kZoneRadius;
    TipEdit gone = e;
    gone.part = 999;  // on no part: left out of the file
    doc->supports.edits = {e, gone};
    fruit::DrainHole h;
    h.at = {3, 3, 6};
    h.axis = {0, 0, -1};
    h.part = c;
    doc->hollow.manual = {h};
    auto r = std::make_shared<SupportResult>(
        generate(*doc, doc->supports.choices, doc->visibleInPrinter(), fruit::JobControl::none(), {e}));
    doc->supports.edits = {e};
    doc->supports.result = r;
    doc->supports.result_revision = doc->revision();
    doc->supports.result_settings = doc->supportSettings();

    const std::string path = (dir() / "three parts.fruitjob").string();
    saveJob(path, *doc);
    const OpenedJob o = openJob(path, kPrinter, fruit::JobControl::none());
    const Document& d = *o.doc;
    REQUIRE(d.partCount() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(d.parts()[i].name() == doc->parts()[i].name());
        CHECK(d.parts()[i].placement() == doc->parts()[i].placement());
        CHECK(meshHash(d.parts()[i].mesh()) == meshHash(doc->parts()[i].mesh()));
    }
    const int b2 = d.parts()[1].id(), c2 = d.parts()[2].id();
    CHECK(d.selectedId() == b2);
    CHECK(d.orientOf(b2).decided);
    CHECK(d.orientOf(b2).visible.size() == 1);
    CHECK_FALSE(d.orientDecided());  // not every part's
    REQUIRE(d.supports.edits.size() == 1);
    CHECK(d.supports.edits[0].part == b2);
    REQUIRE(d.hollow.manual.size() == 1);
    CHECK(d.hollow.manual[0].part == c2);
    REQUIRE(d.supports.result);
    CHECK(d.supports.fresh(d.revision(), d.supportSettings()));
    CHECK(d.supports.result->tips == r->tips);
    CHECK(d.placed().bbox().min.x == Approx(doc->placed().bbox().min.x));
    CHECK_FALSE(d.unsaved());
}

TEST_CASE("Jobs: adding and removing parts count as changes", "[gui][parts][jobs]") {
    const auto doc = jobOf({partOf(cube(10), "a")});
    doc->saved = doc->stamp();
    CHECK_FALSE(doc->unsaved());
    const int b = doc->addPart(partOf(cube(5), "b", {30, 0}));
    CHECK(doc->unsaved());
    CHECK(changesSince(*doc->saved, doc->stamp()) == std::vector<std::string>{"added a part"});
    doc->saved = doc->stamp();
    fruit::Placement p = doc->placement();
    p.offset.y = 5;
    doc->edit(p);
    CHECK(changesSince(*doc->saved, doc->stamp()) == std::vector<std::string>{"moved, turned or resized parts"});
    doc->undo();
    CHECK_FALSE(doc->unsaved());
    doc->removePart(b);
    CHECK(changesSince(*doc->saved, doc->stamp()) == std::vector<std::string>{"removed a part"});
}

TEST_CASE("Parts: what each place needs looks at every part", "[gui][parts]") {
    const auto doc = jobOf({partOf(cube(10), "a"), partOf(cube(10), "b", {30, 0})});
    CHECK(needFor(*doc, Step::Supports) == Need::Nothing);
    // b over the edge, a selected: Supports needs every part on the plate.
    doc->select(doc->parts()[1].id());
    fruit::Placement p = doc->placement();
    p.offset.x = 200;
    doc->edit(p);
    doc->select(doc->parts()[0].id());
    CHECK(doc->fit().fits);
    CHECK_FALSE(doc->fits());
    CHECK(doc->partOff() == &doc->parts()[1]);
    CHECK(needFor(*doc, Step::Supports) == Need::Fit);
    CHECK(needFor(*doc, Step::Orient) == Need::Nothing);  // the search turns the selected part
    CHECK(markFor(*doc, Step::Model) == Mark::Problem);
    CHECK(mainButton(doc.get()).enabled == false);
}

TEST_CASE("Parts: after an undo the placed mesh is made again, at the earlier revision", "[gui][parts]") {
    // The pick tree points into the placed mesh. It was once kept by the job's revision alone: an undo
    // brings back an earlier revision, but not the mesh object that tree was made over (an undo step
    // keeps no meshes it can make again), so a click after an undo could read freed memory. It is now
    // kept by the mesh itself, which is the same object while nothing changes and another after.
    const auto doc = jobOf({partOf(cube(10), "a")});
    const auto m0 = doc->placedShared();
    CHECK(doc->placedShared() == m0);
    CHECK(&doc->placed() == m0.get());
    const int r0 = doc->revision();
    fruit::Placement p = doc->placement();
    p.offset = {20, 0};
    doc->edit(p);
    const auto m1 = doc->placedShared();
    CHECK(m1 != m0);
    doc->undo();
    CHECK(doc->revision() == r0);
    const auto m2 = doc->placedShared();
    CHECK(m2 != m0);  // the earlier revision, another mesh
    CHECK(m2 != m1);
    CHECK(m2->bbox().center().x == Approx(m0->bbox().center().x));
}

TEST_CASE("Jobs: a saved part with no triangles is refused as damaged, not opened a part short", "[gui][parts][jobs]") {
    // A mesh file in the job as FruitSlicer writes them, but with no vertices and no faces.
    std::vector<std::uint8_t> empty(16, 0);
    const char magic[8] = {'F', 'S', 'M', 'E', 'S', 'H', '1', 0};
    std::memcpy(empty.data(), magic, 8);

    const auto two = jobOf({partOf(cube(10), "a"), partOf(cube(6), "b", {30, 0})});
    const std::string job = (dir() / "empty part.fruitjob").string();
    saveJob(job, *two);
    for (const std::string file : {"part-2.bin", "part-1.bin"}) {
        const std::string bad = (dir() / ("empty " + file + ".fruitjob")).string();
        rewriteJob(job, bad, file, empty);
        CHECK_THROWS_WITH(openJob(bad, kPrinter, fruit::JobControl::none()), ContainsSubstring("damaged"));
    }
    // One part, and that one empty: no job with no part either.
    const auto one = jobOf({partOf(cube(10), "a")});
    const std::string single = (dir() / "empty model.fruitjob").string();
    saveJob(single, *one);
    const std::string bad = (dir() / "empty model.bin.fruitjob").string();
    rewriteJob(single, bad, "model.bin", empty);
    CHECK_THROWS_WITH(openJob(bad, kPrinter, fruit::JobControl::none()), ContainsSubstring("damaged"));
    // The jobs as written still open.
    CHECK(openJob(job, kPrinter, fruit::JobControl::none()).doc->partCount() == 2);
    CHECK(openJob(single, kPrinter, fruit::JobControl::none()).doc->partCount() == 1);
}
