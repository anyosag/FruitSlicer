// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Quick support: the three sliders' stops against the planner's real values,
// the job file, reset, and the raft and lift edits.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>
#include <miniz.h>
#include <nlohmann/json.hpp>

#include "Document.h"
#include "JobFile.h"
#include "Parts.h"
#include "mesh/ModelLoad.h"
#include "Supports.h"
#include "mesh/Primitives.h"
#include "mesh/StlIO.h"

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using namespace gui;
namespace fs = std::filesystem;

namespace {

SupportChoices at(int hold, int amount, int sturdy) {
    SupportChoices c;
    c.hold = hold;
    c.amount = amount;
    c.sturdy = sturdy;
    return c;
}

double tipSize(const SupportChoices& c) {
    const fruit::SupportConfig cfg = supportConfig(fruit::Placement{}, c);
    return cfg.tip_classes[static_cast<std::size_t>(cfg.default_tip)].diameter;
}

std::shared_ptr<Document> bracket(const char* name) {
    const auto path = fs::temp_directory_path() / name;
    fruit::writeStl(path.string(), demoModel());
    return loadDocument(path.string(), fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
}

}  // namespace

TEST_CASE("Quick support: the resin's stops plan what FruitSlicer always planned", "[gui][supports][quick]") {
    const SupportChoices c;  // every slider at its resin stop
    CHECK(c.hold == 3);
    CHECK(c.amount == 3);
    CHECK(c.sturdy == 2);
    CHECK_FALSE(quickChanged(c));
    const fruit::SupportConfig cfg = supportConfig(fruit::Placement{}, c);
    const fruit::TreeConfig tc = treeConfig(c);
    const fruit::SupportConfig own;
    const fruit::TreeConfig tree;
    CHECK(cfg.default_tip == 2);  // 0.4 mm
    CHECK(cfg.coverage.density == 1.0);
    CHECK(cfg.critical_angle_deg == own.critical_angle_deg);
    CHECK(cfg.capacity.reference_capacity == own.capacity.reference_capacity);
    CHECK(tc.merge_pillars == tree.merge_pillars);
    CHECK(tc.max_merge_distance == tree.max_merge_distance);
    CHECK(tc.max_tips_per_trunk == tree.max_tips_per_trunk);
    CHECK(tc.max_trunk_diameter == tree.max_trunk_diameter);
    CHECK(tc.brace_min_height == tree.brace_min_height);
    CHECK(tc.brace_step == tree.brace_step);
    CHECK(tc.brace_diameter == tree.brace_diameter);
    CHECK(tc.brace_max_distance == tree.brace_max_distance);
}

TEST_CASE("Quick support: the resin's own measured values stay at the middle stops", "[gui][supports][quick]") {
    SupportChoices c;
    c.profile = {{"default_tip_diameter", 0.6}, {"critical_angle_deg", 52.0}};
    c.usual_tip = usualTipFor(c.profile);
    CHECK(c.usual_tip == 3);
    const fruit::SupportConfig cfg = supportConfig(fruit::Placement{}, c);
    CHECK(cfg.critical_angle_deg == Approx(52));
    CHECK(tipSize(c) == Approx(0.6));
    // Amount moves the angle from the resin's, not from FruitSlicer's own.
    SupportChoices fewer = c;
    fewer.amount = 1;
    CHECK(supportConfig(fruit::Placement{}, fewer).critical_angle_deg == Approx(67));
    // Hold is relative to the resin's usual tip: one class down is 0.4 mm.
    SupportChoices light = c;
    light.hold = 2;
    CHECK(tipSize(light) == Approx(0.4));
}

TEST_CASE("Quick support: every stop's planner values", "[gui][supports][quick]") {
    // Hold: tip size, and how much each tip is counted on to hold.
    const double tip[] = {0.3, 0.3, 0.4, 0.6, 0.6};
    const double hold[] = {1.25, 1.0, 1.0, 1.0, 0.8};
    const double ref = fruit::SupportConfig{}.capacity.reference_capacity;
    for (int s = 1; s <= 5; ++s) {
        const SupportChoices c = at(s, 3, 2);
        CHECK(tipSize(c) == Approx(tip[s - 1]));
        CHECK(supportConfig(fruit::Placement{}, c).capacity.reference_capacity == Approx(ref * hold[s - 1]));
        CHECK(quickChanged(c) == (s != 3));
    }
    // Amount: the lean angle and the spacing.
    const double angle[] = {60, 50, 45, 40, 35};
    const double density[] = {1, 1, 1, 1.25, 1.5};
    for (int s = 1; s <= 5; ++s) {
        const fruit::SupportConfig cfg = supportConfig(fruit::Placement{}, at(3, s, 2));
        CHECK(cfg.critical_angle_deg == Approx(angle[s - 1]));
        CHECK(cfg.coverage.density == Approx(density[s - 1]));
    }
    // Sturdiness: trunks and braces.
    const bool merge[] = {false, true, true, true, true};
    const double dist[] = {8, 8, 10, 12, 15};
    const double brace[] = {1000, 15, 10, 8, 5};
    const double trunk[] = {1.8, 1.8, 1.8, 2.2, 2.6};
    for (int s = 1; s <= 5; ++s) {
        const fruit::TreeConfig tc = treeConfig(at(3, 3, s));
        CHECK(tc.merge_pillars == merge[s - 1]);
        CHECK(tc.max_merge_distance == Approx(dist[s - 1]));
        CHECK(tc.brace_min_height == Approx(brace[s - 1]));
        CHECK(tc.max_trunk_diameter == Approx(trunk[s - 1]));
    }
    // Out of range stops are held to the ends.
    CHECK(tipSize(at(9, 3, 2)) == Approx(0.6));
    CHECK(tipSize(at(-4, 3, 2)) == Approx(0.3));
}

TEST_CASE("Quick support: the advanced page's values win over the sliders", "[gui][supports][quick]") {
    SupportChoices c = at(1, 1, 5);
    c.advanced["critical_angle"] = 47;
    c.advanced["merge_distance"] = 6;
    c.advanced["brace_height"] = 12;
    c.advanced["capacity"] = 11;
    const fruit::SupportConfig cfg = supportConfig(fruit::Placement{}, c);
    const fruit::TreeConfig tc = treeConfig(c);
    CHECK(cfg.critical_angle_deg == Approx(47));
    CHECK(tc.max_merge_distance == Approx(6));
    CHECK(tc.brace_min_height == Approx(12));
    CHECK(cfg.capacity.capacity(0.4) == Approx(11));
    CHECK(tc.max_trunk_diameter == Approx(2.6));  // not on the page: the slider's
    // A usual tip chosen there stays whatever Hold says.
    SupportChoices big = at(1, 3, 2);
    big.usual_tip = 3;
    CHECK(tipSize(big) == Approx(0.6));
}

TEST_CASE("Quick support: captions and words state the real values", "[gui][supports][quick]") {
    const SupportChoices c;
    CHECK(std::string(quickWord(QuickSlider::Hold, 1)) == "Gentle");
    CHECK(std::string(quickWord(QuickSlider::Hold, 5)) == "Strong");
    CHECK(std::string(quickWord(QuickSlider::Amount, 1)) == "Fewest");
    CHECK(std::string(quickWord(QuickSlider::Amount, 5)) == "Most");
    CHECK(std::string(quickWord(QuickSlider::Sturdiness, 1)) == "Lightest");
    CHECK(std::string(quickWord(QuickSlider::Sturdiness, 5)) == "Sturdiest");
    CHECK_THAT(quickCaption(QuickSlider::Hold, 3, c), ContainsSubstring("0.4 mm"));
    CHECK_THAT(quickCaption(QuickSlider::Hold, 4, c), ContainsSubstring("0.6 mm"));
    CHECK_THAT(quickCaption(QuickSlider::Hold, 1, c), ContainsSubstring("25%"));
    CHECK_THAT(quickCaption(QuickSlider::Hold, 5, c), ContainsSubstring("20% less"));
    CHECK_THAT(quickCaption(QuickSlider::Amount, 3, c), ContainsSubstring("45°"));
    CHECK_THAT(quickCaption(QuickSlider::Amount, 1, c), ContainsSubstring("60°"));
    CHECK_THAT(quickCaption(QuickSlider::Amount, 5, c), ContainsSubstring("35°"));
    CHECK_THAT(quickCaption(QuickSlider::Amount, 5, c), ContainsSubstring("18% closer"));
    CHECK_THAT(quickCaption(QuickSlider::Sturdiness, 2, c), ContainsSubstring("within 8 mm"));
    CHECK_THAT(quickCaption(QuickSlider::Sturdiness, 2, c), ContainsSubstring("taller than 15 mm"));
    CHECK_THAT(quickCaption(QuickSlider::Sturdiness, 1, c), ContainsSubstring("no braces"));
    CHECK_THAT(quickCaption(QuickSlider::Sturdiness, 5, c), ContainsSubstring("2.6 mm"));
    const QuickInfo info = quickInfo(QuickSlider::Amount, at(3, 4, 2));
    CHECK(info.word == "More");
    CHECK(info.stop == 4);
    CHECK(info.normal == 3);
    // The advanced page's angle is the real one, and the caption says where it is set.
    SupportChoices own = at(3, 1, 2);
    own.advanced["critical_angle"] = 50;
    CHECK_THAT(quickCaption(QuickSlider::Amount, 1, own), ContainsSubstring("50°"));
    CHECK_THAT(quickCaption(QuickSlider::Amount, 1, own), ContainsSubstring("Advanced"));
}

TEST_CASE("Quick support: reset puts the sliders back", "[gui][supports][quick]") {
    SupportChoices c = at(5, 1, 4);
    c.advanced["head_length"] = 1.4;
    c.density = 1.5;
    c.merge = false;
    CHECK(quickChanged(c));
    resetQuick(c);
    CHECK_FALSE(quickChanged(c));
    CHECK(c.hold == 3);
    CHECK(c.amount == 3);
    CHECK(c.sturdy == 2);
    CHECK(c.density == 1.0);
    CHECK(c.merge);
    CHECK(c.advanced.count("head_length") == 1);  // the advanced page's values are its own
}

TEST_CASE("Quick support: the sliders are saved with the job and old jobs load the same", "[gui][supports][quick][jobs]") {
    const fs::path dir = fs::temp_directory_path() / "fruit_quick_tests";
    fs::create_directories(dir);
    const auto doc = bracket("quick_bracket.stl");
    const auto job = (dir / "plain.fruitjob").string();
    saveJob(job, *doc);
    // At their resin stops nothing is written: the file is as it was before the sliders.
    const auto jobJson = [](const std::string& path) {
        mz_zip_archive z{};
        REQUIRE(mz_zip_reader_init_file(&z, path.c_str(), 0));
        size_t n = 0;
        void* data = mz_zip_reader_extract_file_to_heap(&z, "job.json", &n, 0);
        REQUIRE(data);
        const nlohmann::json j = nlohmann::json::parse(std::string(static_cast<const char*>(data), n));
        mz_free(data);
        mz_zip_reader_end(&z);
        return j;
    };
    const nlohmann::json plain = jobJson(job);
    CHECK_FALSE(plain.at("supports").at("choices").contains("quick"));
    const OpenedJob a = openJob(job, fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    CHECK(a.doc->supports.choices == SupportChoices{});

    doc->supports.choices.hold = 5;
    doc->supports.choices.amount = 2;
    doc->supports.choices.sturdy = 4;
    const auto job2 = (dir / "quick.fruitjob").string();
    saveJob(job2, *doc);
    const OpenedJob b = openJob(job2, fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    CHECK(b.doc->supports.choices.hold == 5);
    CHECK(b.doc->supports.choices.amount == 2);
    CHECK(b.doc->supports.choices.sturdy == 4);
    CHECK(b.doc->supports.choices == doc->supports.choices);
    CHECK(jobJson(job2).at("supports").at("choices").at("quick").at("hold") == 5);
    // The raft and the lift are the placement's, saved as before.
    fruit::Placement p = doc->placement();
    p.on_plate = true;
    p.elevation = 8;
    doc->edit(p);
    const auto job3 = (dir / "plate.fruitjob").string();
    saveJob(job3, *doc);
    const OpenedJob c = openJob(job3, fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    CHECK(c.doc->placement().on_plate);
    CHECK(c.doc->placement().elevation == Approx(8));
}

TEST_CASE("Quick support: raft and lift are the job's, in every part, and undo", "[gui][supports][quick]") {
    const auto doc = bracket("quick_bracket2.stl");
    CHECK_FALSE(doc->placement().on_plate);
    CHECK(doc->placement().elevation == 5);  // the default lift
    fruit::Placement p = doc->placement();
    p.elevation = 8;
    doc->edit(p);
    CHECK(doc->placement().lift() == 8);
    doc->undo();
    CHECK(doc->placement().elevation == 5);
    doc->redo();
    // A second part takes the same lift, and the same raft choice.
    fruit::Placement other;
    other.offset = {40, 0};
    other.elevation = 3;
    const int b = doc->addPart(NewPart{"b", "", makePartModel(fruit::prepareModel(fruit::makeBox({0, 0, 0}, {10, 10, 10}))), other});
    REQUIRE(b >= 0);
    for (const Part& part : doc->parts()) CHECK(part.placement().elevation == 8);
    p = doc->placement();
    p.elevation = 6;
    doc->edit(p);
    for (const Part& part : doc->parts()) CHECK(part.placement().elevation == 6);
    doc->undo();
    for (const Part& part : doc->parts()) CHECK(part.placement().elevation == 8);
    doc->undo();  // the part's addition
    doc->redo();
    doc->redo();
    doc->undo();
    CHECK(doc->placement().elevation == 8);
    // Raft off: no lift at all, but the number is kept for when it is back on.
    p = doc->placement();
    p.on_plate = true;
    doc->edit(p);
    CHECK(doc->placement().lift() == 0);
    CHECK(doc->placement().elevation == 8);
    doc->undo();
    CHECK_FALSE(doc->placement().on_plate);
    CHECK(doc->placement().lift() == 8);

    // Dragging the slider is one undo step, and a change marks supports out of date.
    const int before = doc->revision();
    doc->beginDrag();
    for (const double mm : {7.0, 6.0, 3.0, 2.0}) {
        p = doc->placement();
        p.elevation = mm;
        doc->preview(p);
    }
    doc->endDrag();
    CHECK(doc->placement().elevation == 2);
    CHECK(doc->revision() != before);
    doc->undo();
    CHECK(doc->placement().elevation == 8);
    CHECK(doc->revision() == before);
}

TEST_CASE("Quick support: the stops plan and the far ends still make supports", "[gui][supports][quick][slow]") {
    const auto doc = bracket("quick_bracket3.stl");
    const auto run = [&](const SupportChoices& c) {
        return generate(*doc, c, {}, fruit::JobControl::none());
    };
    const SupportResult normal = run(SupportChoices{});
    // A job left at its resin stops plans the same supports as the same choices with the sliders ignored.
    CHECK(normal.cfg.default_tip == 2);
    CHECK(normal.tree_cfg.merge_pillars);
    const SupportResult fewest = run(at(3, 1, 1));
    const SupportResult most = run(at(3, 5, 5));
    CHECK(fewest.tips > 0);
    CHECK(most.tips >= fewest.tips);
    CHECK(fewest.tree_cfg.merge_pillars == false);
    CHECK(fewest.braces == 0);
    CHECK(most.tree_cfg.max_trunk_diameter == Approx(2.6));
    CHECK(run(at(1, 3, 2)).tips > 0);
    CHECK(run(at(5, 3, 2)).tips > 0);
}
