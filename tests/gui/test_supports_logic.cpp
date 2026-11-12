// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The Supports step's logic: generating, checking and explaining.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <filesystem>
#include <memory>
#include <set>

#include "Document.h"
#include "Supports.h"
#include "mesh/Primitives.h"
#include "mesh/StlIO.h"

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using namespace gui;

namespace {

std::shared_ptr<Document> bracket() {
    const auto path = std::filesystem::temp_directory_path() / "supports_test_bracket.stl";
    fruit::writeStl(path.string(), demoModel());
    auto doc = loadDocument(path.string(), fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    std::filesystem::remove(path);
    return doc;
}

// A box open on one side, stood so the opening faces sideways, like a lid on
// its edge: no support can reach the ceiling deep inside it.
std::shared_ptr<Document> sidewaysCap() {
    fruit::TriangleMesh m = fruit::makeBox({0, 0, 0}, {30, 2, 30});
    m.append(fruit::makeBox({0, 0, 0}, {30, 30, 2}));
    m.append(fruit::makeBox({0, 0, 28}, {30, 30, 30}));
    m.append(fruit::makeBox({0, 0, 0}, {2, 30, 30}));
    m.append(fruit::makeBox({28, 0, 0}, {30, 30, 30}));
    const auto path = std::filesystem::temp_directory_path() / "supports_test_sideways_cap.stl";
    fruit::writeStl(path.string(), m);
    auto doc = loadDocument(path.string(), fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    std::filesystem::remove(path);
    return doc;
}

}  // namespace

TEST_CASE("Supports: a ceiling out of the raft's reach is held from the model, and says so",
          "[gui][supports][slow]") {
    const auto doc = sidewaysCap();
    const SupportResult r = generate(*doc, SupportChoices{}, {}, fruit::JobControl::none());
    CHECK(r.tree.dropped_points == 0);
    CHECK(r.on_model > 0);
    // Nothing blocks saving; the marks on the model are something to review.
    CHECK(r.count(SupportProblem::Level::Blocks) == 0);
    const SupportProblem* p = nullptr;
    for (const auto& q : r.problems)
        if (q.code == "anchor_on_model") p = &q;
    REQUIRE(p != nullptr);
    CHECK(p->level == SupportProblem::Level::Review);
    CHECK(p->count == r.on_model);
    bool said = false;
    for (const auto& c : r.checks)
        if (c.title == "No support stands on the model") {
            said = true;
            CHECK_FALSE(c.ok);
        }
    CHECK(said);
}

TEST_CASE("Supports: faces only dropped tips held block saving and show where", "[gui][supports][slow]") {
    const auto doc = sidewaysCap();
    // With standing on the model turned off, as the advanced page allows.
    SupportChoices off;
    off.advanced["anchor_drop"] = 0;
    const SupportResult r = generate(*doc, off, {}, fruit::JobControl::none());
    REQUIRE(r.tree.dropped_points > 0);
    const SupportProblem* p = nullptr;
    for (const auto& q : r.problems)
        if (q.code == "unsupported") p = &q;
    REQUIRE(p != nullptr);
    CHECK(p->level == SupportProblem::Level::Blocks);
    CHECK(p->count > 0);
    CHECK(p->count <= r.tree.dropped_points);
    CHECK(p->has_where);
    // On the ceiling inside the cap.
    CHECK(p->where.z == Approx(doc->placed().bbox().max.z - 2).margin(0.2));
    CHECK(r.count(SupportProblem::Level::Blocks) >= 1);
}

TEST_CASE("Supports: on the plate, the job's own bottom layers hold what is lower", "[gui][supports][slow]") {
    // A block with a shelf whose underside is 0.25 mm above the plate.
    fruit::TriangleMesh m = fruit::makeBox({0, 0, 0}, {30, 8, 10});
    m.append(fruit::makeBox({5, -5, 0.25}, {25, 0.5, 3}));
    const auto path = std::filesystem::temp_directory_path() / "supports_test_low_shelf.stl";
    fruit::writeStl(path.string(), m);
    auto doc = loadDocument(path.string(), fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    std::filesystem::remove(path);
    fruit::Placement p = doc->placement();
    p.on_plate = true;
    doc->edit(p);
    auto tipsUnderShelf = [&] {
        std::size_t n = 0;
        for (const auto& t : planTips(*doc, SupportChoices{}, {}, fruit::JobControl::none(), {}).points) n += t.pos.z < 0.5;
        return n;
    };
    // The printer's own four bottom layers at 0.05 mm reach 0.2 mm: the shelf
    // starts above them and gets tips.
    CHECK(bottomLayersHeight(*doc) == Approx(4 * 0.05));
    CHECK(tipsUnderShelf() > 0);
    // A resin with six bottom layers reaches 0.3 mm: the plate holds the shelf.
    doc->print.settings.bottom_layers = 6;
    CHECK(bottomLayersHeight(*doc) == Approx(6 * 0.05));
    CHECK(tipsUnderShelf() == 0);
    // The same six at the resin's own 0.03 mm layers reach only 0.18 mm.
    doc->print.settings.layer_height = 0.03;
    CHECK(bottomLayersHeight(*doc) == Approx(6 * 0.03));
    CHECK(tipsUnderShelf() > 0);
    // Without values of the job's own, a sample file's count and layer height.
    doc->print.settings.bottom_layers = -1;
    doc->print.settings.layer_height = 0;
    auto sample = std::make_shared<fruit::GooHeader>();
    sample->print.bottom_layers = 8;
    sample->layer_height = 0.05f;
    doc->print.settings.sample = sample;
    CHECK(bottomLayersHeight(*doc) == Approx(8 * fruit::gooLayerHeight(*sample)));
    CHECK(tipsUnderShelf() == 0);
}

TEST_CASE("Supports: the demo bracket gets a raft, tips and a clean check", "[gui][supports][slow]") {
    const auto doc = bracket();
    const SupportResult r = generate(*doc, SupportChoices{}, {}, fruit::JobControl::none());
    CHECK(r.tips > 0);
    CHECK(r.tips == r.points.size());
    std::size_t by_size = 0;
    for (const auto& [d, n] : r.tip_sizes) by_size += n;
    CHECK(by_size == r.tips);
    CHECK(r.pillars > 0);
    CHECK(r.trunks >= 1);
    CHECK(r.support_ml > 0);
    CHECK_FALSE(r.mesh.empty());
    // Model and supports together reach down to the plate.
    CHECK(r.size_with_supports.z == Approx(doc->placed().bbox().max.z).margin(0.01));
    REQUIRE(r.checks.size() == 4);
    CHECK(r.checks[0].title == "No part starts in mid-air");
    CHECK(r.checks[0].ok);
    CHECK(r.checks[1].title == "Raft comes off in one piece");
    CHECK(r.checks[1].ok);
    // Problems come blocking first, then to review, then notes.
    for (std::size_t i = 1; i < r.problems.size(); ++i) CHECK(r.problems[i - 1].level <= r.problems[i].level);
    CHECK(r.count(SupportProblem::Level::Blocks) == 0);
}

TEST_CASE("Supports: bigger usual tips and denser spacing change the plan", "[gui][supports][slow]") {
    const auto doc = bracket();
    SupportChoices dense;
    dense.density = 2.0;
    const SupportResult a = generate(*doc, SupportChoices{}, {}, fruit::JobControl::none());
    const SupportResult b = generate(*doc, dense, {}, fruit::JobControl::none());
    CHECK(b.tips >= a.tips);
    SupportChoices big;
    big.usual_tip = 3;  // 0.6 mm
    const SupportResult c = generate(*doc, big, {}, fruit::JobControl::none());
    bool has_06 = false;
    for (const auto& [d, n] : c.tip_sizes) has_06 = has_06 || std::abs(d - 0.6) < 1e-6;
    CHECK(has_06);
}

TEST_CASE("Supports: stages follow the progress ranges", "[gui][supports]") {
    CHECK(supportStage("slice", 0.1) == 0);
    CHECK(supportStage("islands", 0.2) == 1);
    CHECK(supportStage("points", 0.4) == 2);
    CHECK(supportStage("tree", 0.6) == 3);
    CHECK(supportStage("mesh", 0.8) == 4);
    CHECK(supportStage("slice", 0.9) == 5);  // the check slices again: the range tells it apart
    CHECK(supportStages().size() == 6);
}

TEST_CASE("Supports: every kind of tip explains itself", "[gui][supports]") {
    fruit::SupportPoint p;
    p.tip_diameter = 0.3;
    for (auto c : {fruit::PointCategory::Island, fruit::PointCategory::Overhang, fruit::PointCategory::Capacity,
                   fruit::PointCategory::Stabilization, fruit::PointCategory::Manual}) {
        p.category = c;
        CHECK_FALSE(tipWhy(p).empty());
        CHECK_THAT(tipKind(p), ContainsSubstring("0.3 mm"));
    }
    fruit::TreeConfig tc;
    fruit::SupportConfig cfg;
    // Default penetration: half the diameter within 0.08–0.2 mm.
    CHECK_THAT(tipNumbers(p, cfg, tc), ContainsSubstring("Contact depth 0.15 mm"));
}

TEST_CASE("Advanced settings: unique keys, ranges that hold the values in use, and values that reach the engine",
          "[gui][supports]") {
    const fruit::Placement p;
    const SupportChoices none;
    const fruit::SupportConfig cfg = supportConfig(p, none);
    const fruit::TreeConfig tc = treeConfig(none);
    std::set<std::string> keys;
    std::size_t rows = 0;
    for (const AdvancedSetting& s : advancedSettings()) {
        INFO(s.label);
        ++rows;
        REQUIRE(s.help != nullptr);
        CHECK_FALSE(advancedChanged(s, none));
        if (s.kind == AdvancedSetting::Kind::Fixed) {
            CHECK(s.text != nullptr);
            CHECK(s.fields.empty());
        } else {
            CHECK_FALSE(s.fields.empty());
        }
        for (const AdvancedField& f : s.fields) {
            INFO(f.key);
            CHECK(keys.insert(f.key).second);
            REQUIRE(f.get != nullptr);
            REQUIRE(f.set != nullptr);
            const double now = f.get(cfg, tc);
            CHECK(now >= f.lo);
            CHECK(now <= f.hi);
            if (std::string(f.key) == kUsualTipKey) continue;
            // A value of the user's own reaches the planner or the tree builder.
            SupportChoices c;
            const double v = f.decimals == 0 ? std::round((f.lo + f.hi) / 2) : (f.lo + f.hi) / 2;
            c.advanced[f.key] = v;
            CHECK(f.get(supportConfig(p, c), treeConfig(c)) == Approx(v));
            CHECK_FALSE(c == none);
            CHECK(advancedChanged(s, c));
            // Out of range: held to the nearest limit.
            c.advanced[f.key] = f.hi + 1000;
            CHECK(f.get(supportConfig(p, c), treeConfig(c)) == Approx(f.hi));
        }
    }
    CHECK(rows == 32);  // the 31 on the canvas board "Supports · advanced", and "Where a part starts"
    // The usual tip size lives in the choices themselves.
    SupportChoices big;
    big.usual_tip = 3;
    CHECK(supportConfig(p, big).default_tip == 3);
    CHECK(advancedChanged(advancedSettings().front(), big));
    // Tiny specks: left alone (FruitSlicer's way) or given tips.
    SupportChoices specks;
    specks.advanced["tiny_left_alone"] = 0;
    CHECK(supportConfig(p, specks).tiny_island_area == 0);
    CHECK(supportConfig(p, specks).absorb_island_area == 0);
    CHECK(cfg.tiny_island_area > 0);
}

TEST_CASE("Advanced settings: a value of your own makes the supports out of date", "[gui][supports]") {
    SupportState s;
    auto r = std::make_shared<SupportResult>();
    s.result = r;
    s.result_revision = 1;
    s.result_settings = 0;
    CHECK(s.fresh(1, 0));
    s.choices.advanced["head_length"] = 1.5;
    CHECK_FALSE(s.fresh(1, 0));
    r->choices = s.choices;
    CHECK(s.fresh(1, 0));
}

TEST_CASE("Advanced settings: every value at its lower limit, then its upper, still makes supports",
          "[gui][supports][slow]") {
    const auto doc = bracket();
    for (const bool high : {false, true}) {
        INFO((high ? "upper limits" : "lower limits"));
        SupportChoices c;
        for (const AdvancedSetting& s : advancedSettings())
            for (const AdvancedField& f : s.fields)
                if (std::string(f.key) != kUsualTipKey) c.advanced[f.key] = high ? f.hi : f.lo;
        const SupportResult r = generate(*doc, c, {}, fruit::JobControl::none());
        CHECK(r.tips > 0);
        CHECK(r.pillars > 0);
        CHECK_FALSE(r.mesh.empty());
        CHECK(r.checks.size() == 4);
        CHECK(r.tree_cfg.head_length == Approx(high ? 3.0 : 0.4));
    }
}

TEST_CASE("Supports: a hinge side adds hold away from the hinge", "[gui][supports]") {
    const fruit::Placement p;
    const SupportChoices none;
    CHECK(supportConfig(p, none).peel_bias == 0);
    SupportChoices left;
    left.hinge = fruit::HingeSide::MinusX;  // hinged on the left: peel pulls hardest on the right
    const fruit::SupportConfig cfg = supportConfig(p, left);
    CHECK(cfg.peel_bias == Approx(0.25));
    CHECK(cfg.peel_axis.x > 0.99);
    CHECK_FALSE(left == none);  // so supports made without it read out of date
}

namespace {

double dist(const fruit::Vec3& a, const fruit::Vec3& b) {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Tips within `r` of `at`: all of them, or only the planner's.
std::size_t tipsNear(const std::vector<fruit::SupportPoint>& pts, const fruit::Vec3& at, double r, bool planners_only) {
    std::size_t n = 0;
    for (const auto& p : pts)
        n += dist(p.pos, at) <= r && (!planners_only || p.category != fruit::PointCategory::Manual);
    return n;
}

}  // namespace

TEST_CASE("Tip edits: add, move, delete, resize and pin apply on top of the planner's tips", "[gui][supports]") {
    using K = TipEdit::Kind;
    const fruit::Transform t = fruit::Transform::translation({10, 0, 5});
    std::vector<fruit::SupportPoint> planned(2);
    planned[0].pos = {10, 0, 5};
    planned[1].pos = {14, 0, 5};
    // Model coordinates: the first planned tip is at the model's origin.
    TipEdit del{K::Delete, {0, 0, 0}, {}, {0, 0, -1}, 0.4, kDeletedZone};
    TipEdit add{K::Add, {2, 1, 0}, {}, {0, 0, -1}, 0.3, 0};
    TipEdit move{K::Move, {4, 0, 0}, {4, 2, 0}, {0, 0, -1}, 0.6, kDeletedZone};
    auto pts = applyEdits(planned, {del}, t);
    REQUIRE(pts.size() == 1);
    CHECK(pts[0].pos.x == 14);
    pts = applyEdits(planned, {del, add, move}, t);
    REQUIRE(pts.size() == 2);
    CHECK(pts[0].category == fruit::PointCategory::Manual);
    CHECK(pts[0].pinned);
    CHECK(pts[0].pos.x == 12);  // the added tip, placed by the transform
    CHECK(pts[0].tip_diameter == 0.3);
    CHECK(pts[1].pos.y == 2);   // the moved one
    CHECK(pts[1].tip_diameter == 0.6);
    // Resizing or pinning replaces the planner's tip with one of your own, once.
    TipEdit resize{K::Resize, {4, 0, 0}, {}, {0, 0, -1}, 0.6, 0};
    pts = applyEdits(planned, {resize}, t);
    REQUIRE(pts.size() == 2);
    CHECK(pts[1].tip_diameter == 0.6);
    CHECK(pts[1].category == fruit::PointCategory::Manual);
    // A zone takes away every tip of the planner's within it, and none of yours.
    const TipEdit zone{K::Zone, {2, 0, 0}, {}, {}, 0.4, 2.5};
    CHECK(applyEdits(planned, {zone}, t).empty());
    pts = applyEdits(planned, {add, zone}, t);
    REQUIRE(pts.size() == 1);
    CHECK(pts[0].category == fruit::PointCategory::Manual);
    // A deleted tip's zone keeps the neighbour one sample (1 mm) away.
    std::vector<fruit::SupportPoint> close = planned;
    close[1].pos = {11, 0, 5};
    CHECK(applyEdits(close, {del}, t).size() == 1);
    const auto [title, detail] = describeEdit(del, t);
    CHECK(title == "Deleted a tip");
    CHECK_THAT(detail, ContainsSubstring("1.8 mm no-tip zone"));
    const EditCounts n = countEdits({del, add, move, resize, zone, zone});
    CHECK(n.added == 1);
    CHECK(n.moved == 1);
    CHECK(n.deleted == 1);
    CHECK(n.resized == 1);
    CHECK(n.pinned == 0);
    CHECK(n.zones == 2);
    CHECK(describeCounts(n) == "1 added, 1 moved, 1 deleted, 1 resized, 2 no-tip zones");
    CHECK(describeCounts(countEdits({resize})) == "1 resized");
}

TEST_CASE("Tip edits: changing a tip of yours changes the edit that made it", "[gui][supports]") {
    using K = TipEdit::Kind;
    const fruit::Transform t = fruit::Transform::translation({10, 0, 5});
    fruit::SupportPoint planner;
    planner.pos = {14, 0, 5};
    planner.normal = {0, 0, -1};
    const std::vector<fruit::SupportPoint> planned{planner};
    std::vector<TipEdit> edits;
    // Resized, then moved, then deleted: one edit all along, about the planner's tip.
    REQUIRE(recordResize(edits, planner, 0.6, t));
    CHECK_FALSE(recordResize(edits, applyEdits(planned, edits, t)[0], 0.6, t));  // the size it has
    REQUIRE(recordMove(edits, applyEdits(planned, edits, t)[0], {14, 2, 5}, {0, 0, -1}, t));
    REQUIRE(edits.size() == 1);
    CHECK(edits[0].kind == K::Move);
    CHECK(edits[0].at.x == Approx(4));
    CHECK(edits[0].to.y == Approx(2));
    CHECK(edits[0].diameter == 0.6);
    auto pts = applyEdits(planned, edits, t);
    REQUIRE(pts.size() == 1);
    CHECK(pts[0].pos.y == Approx(2));
    CHECK_FALSE(recordPin(edits, pts[0], t));  // yours are pinned already
    REQUIRE(recordDelete(edits, pts[0], t));
    REQUIRE(edits.size() == 1);
    CHECK(edits[0].kind == K::Delete);
    CHECK(edits[0].at.x == Approx(4));
    CHECK(applyEdits(planned, edits, t).empty());
    // An added tip: moving it moves the addition; deleting it undoes it.
    std::vector<TipEdit> adds{TipEdit{K::Add, {2, 1, 0}, {}, {0, 0, -1}, 0.3, 0}};
    REQUIRE(recordMove(adds, applyEdits({}, adds, t)[0], {12, 3, 5}, {0, 0, -1}, t));
    REQUIRE(adds.size() == 1);
    CHECK(adds[0].kind == K::Add);
    CHECK(adds[0].at.y == Approx(3));
    REQUIRE(recordDelete(adds, applyEdits({}, adds, t)[0], t));
    CHECK(adds.empty());
    // A changed edit goes to the end of the list.
    std::vector<TipEdit> two{TipEdit{K::Add, {2, 1, 0}, {}, {0, 0, -1}, 0.3, 0},
                             TipEdit{K::Zone, {9, 9, 0}, {}, {}, 0.4, kZoneRadius}};
    REQUIRE(recordResize(two, applyEdits({}, two, t)[0], 0.6, t));
    CHECK(two[0].kind == K::Zone);
    CHECK(two[1].diameter == 0.6);
}

TEST_CASE("Tip edits: when the tips are planned again, the planner works around them", "[gui][supports][slow]") {
    const auto doc = bracket();
    fruit::Placement p = doc->placement();
    p.rotation_deg = {0, 0, 90};
    doc->edit(p);
    const auto none = fruit::JobControl::none();
    const SupportResult plain = generate(*doc, SupportChoices{}, {}, none);
    std::vector<fruit::SupportPoint> overhang;
    for (const auto& q : plain.points)
        if (q.category == fruit::PointCategory::Overhang) overhang.push_back(q);
    REQUIRE(overhang.size() > 6);
    const fruit::Transform t = doc->part().transform();
    // Delete one of the planner's tips, resize another, and add a tip of your own
    // where the planner has a third: edits in model coordinates.
    const fruit::SupportPoint gone = overhang[0], bigger = overhang[overhang.size() / 2], yours = overhang.back();
    std::vector<TipEdit> edits;
    REQUIRE(recordDelete(edits, gone, t));
    REQUIRE(recordResize(edits, bigger, 0.6, t));
    TipEdit add;
    add.kind = TipEdit::Kind::Add;
    add.at = t.inverse().apply(yours.pos);
    add.normal = t.inverse().applyLinear(yours.normal);
    add.diameter = 0.3;
    edits.push_back(add);
    const SupportResult r = generate(*doc, SupportChoices{}, {}, none, edits);
    // The planner itself kept out of the deleted tip's zone and off your tips
    // (on its own it puts a tip at each of these places).
    CHECK(tipsNear(plain.planned, gone.pos, kDeletedZone, true) == 1);
    CHECK(tipsNear(r.planned, gone.pos, kDeletedZone, true) == 0);
    CHECK(tipsNear(r.planned, bigger.pos, 0.45, true) == 0);
    CHECK(tipsNear(r.planned, yours.pos, 0.45, true) == 0);
    // Where the planner had a tip, the deleted one's zone now has none of its tips.
    CHECK(tipsNear(r.points, gone.pos, kDeletedZone, true) == 0);
    // Your tips are there once, with no tip of the planner's on top of them.
    CHECK(tipsNear(r.points, bigger.pos, 0.45, false) == 1);
    CHECK(tipsNear(r.points, bigger.pos, 0.45, true) == 0);
    CHECK(tipsNear(r.points, yours.pos, 0.45, false) == 1);
    CHECK(tipsNear(r.points, yours.pos, 0.45, true) == 0);
    // Applied again on top of these tips (as a saved job comes back), nothing changes.
    CHECK(r.plan_edits == edits);
    CHECK(planStillFits(r.plan_edits, edits));
    const SupportResult again =
        finishWithEdits(*doc, SupportChoices{}, SupportPlan{r.planned, r.plan_warnings, r.plan_edits, r.hollow}, edits, none);
    REQUIRE(again.points.size() == r.points.size());
    for (std::size_t i = 0; i < r.points.size(); ++i) CHECK(dist(again.points[i].pos, r.points[i].pos) < 1e-9);
    // A new edit goes on top; taking one back means planning again.
    std::vector<TipEdit> more = edits;
    more.push_back(TipEdit{TipEdit::Kind::Zone, add.at, {}, {}, 0.4, kZoneRadius});
    CHECK(planStillFits(r.plan_edits, more));
    CHECK_FALSE(planStillFits(r.plan_edits, std::vector<TipEdit>(edits.begin() + 1, edits.end())));
    // Turned back, the same edits are kept to where those places now are.
    p.rotation_deg = {0, 0, 0};
    doc->edit(p);
    const fruit::Transform t0 = doc->part().transform();
    const SupportResult back = generate(*doc, SupportChoices{}, {}, none, edits);
    CHECK(tipsNear(back.planned, t0.apply(edits[0].at), kDeletedZone, true) == 0);
    CHECK(tipsNear(back.points, t0.apply(edits[0].at), kDeletedZone, true) == 0);
    CHECK(tipsNear(back.points, t0.apply(edits[1].at), 0.45, false) == 1);
    CHECK(tipsNear(back.points, t0.apply(edits[2].at), 0.45, false) == 1);
}
