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

}  // namespace

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
    CHECK(rows == 29);  // as on the canvas board "Supports · advanced"
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

TEST_CASE("Tip edits: add, move, delete, resize and pin apply on top of the planner's tips", "[gui][supports]") {
    using K = TipEdit::Kind;
    const fruit::Transform t = fruit::Transform::translation({10, 0, 5});
    std::vector<fruit::SupportPoint> planned(2);
    planned[0].pos = {10, 0, 5};
    planned[1].pos = {14, 0, 5};
    // Model coordinates: the first planned tip is at the model's origin.
    TipEdit del{K::Delete, {0, 0, 0}, {}, {0, 0, -1}, 0.4, kDeletedZone};
    TipEdit add{K::Add, {2, 1, 0}, {}, {0, 0, -1}, 0.3, 0};
    TipEdit move{K::Move, {4, 0, 0}, {4, 2, 0}, {0, 0, -1}, 0.6, 0};
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
    // A zone takes away every tip within it.
    pts = applyEdits(planned, {TipEdit{K::Zone, {2, 0, 0}, {}, {}, 0.4, 2.5}}, t);
    CHECK(pts.empty());
    const auto [title, detail] = describeEdit(del, t);
    CHECK(title == "Deleted a tip");
    CHECK_THAT(detail, ContainsSubstring("1.0 mm no-tip zone"));
    const EditCounts n = countEdits({del, add, move, resize});
    CHECK(n.added == 1);
    CHECK(n.moved == 1);
    CHECK(n.deleted == 1);
    CHECK(n.changed == 1);
}

TEST_CASE("Tip edits: kept in model coordinates, they follow the model when it turns", "[gui][supports][slow]") {
    const auto doc = bracket();
    SupportResult r = generate(*doc, SupportChoices{}, {}, fruit::JobControl::none());
    REQUIRE(r.points.size() > 3);
    // Delete the first planned tip, by hand, where it is on the model.
    const fruit::Transform to_model = doc->transform().inverse();
    TipEdit del{TipEdit::Kind::Delete, to_model.apply(r.points[0].pos), {}, {0, 0, -1}, 0.4, kDeletedZone};
    SupportResult e = finishWithEdits(*doc, SupportChoices{}, SupportPlan{r.planned, r.plan_warnings}, {del},
                                      fruit::JobControl::none());
    CHECK(e.points.size() == r.points.size() - 1);
    CHECK(e.planned.size() == r.planned.size());
    CHECK(e.edits.size() == 1);
    // Turn the model: the deletion moves with it.
    fruit::Placement p = doc->placement();
    p.rotation_deg = {0, 0, 90};
    doc->edit(p);
    const fruit::Vec3 now = doc->transform().apply(del.at);
    const SupportResult turned = generate(*doc, SupportChoices{}, {}, fruit::JobControl::none(), {del});
    for (const auto& pt : turned.points) {
        const double dx = pt.pos.x - now.x, dy = pt.pos.y - now.y, dz = pt.pos.z - now.z;
        CHECK(std::sqrt(dx * dx + dy * dy + dz * dz) > kDeletedZone);
    }
}
