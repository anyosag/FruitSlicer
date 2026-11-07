// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The Supports step's logic: generating, checking and explaining.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>

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
