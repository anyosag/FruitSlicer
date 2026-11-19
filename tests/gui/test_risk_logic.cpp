// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// What may not print: the replay's findings as the screens word, colour and paint them.
#include <catch2/catch_approx.hpp>
#include <set>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <filesystem>
#include <memory>

#include "Document.h"
#include "Layers.h"
#include "Risk.h"
#include "Supports.h"
#include "layers/LayerStack.h"
#include "mesh/Primitives.h"
#include "mesh/StlIO.h"

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using namespace gui;

namespace {

constexpr double kH = 0.05;

// A cap on a thin stem that stands on the plate: the stem is the weak place.
fruit::TriangleMesh capOnStem(double cap) {
    fruit::TriangleMesh m = fruit::makeBox({-0.5, -0.5, 0}, {0.5, 0.5, 2});
    m.append(fruit::makeBox({-cap / 2, -cap / 2, 2}, {cap / 2, cap / 2, 3}));
    return m;
}

fruit::StabilityResult replay(const fruit::TriangleMesh& m, int replays = 400) {
    const fruit::LayerStack layers = fruit::sliceModel(m, kH);
    fruit::StabilityInput in;
    in.print = &layers;
    in.model = &layers;
    fruit::StabilityConfig cfg;
    cfg.replays = replays;
    return fruit::analyzeStability(in, cfg);
}

double surface(const fruit::TriangleMesh& m) { return m.surfaceArea(); }

std::shared_ptr<Document> bracket() {
    const auto path = std::filesystem::temp_directory_path() / "risk_test_bracket.stl";
    fruit::writeStl(path.string(), demoModel());
    auto doc = loadDocument(path.string(), fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    std::filesystem::remove(path);
    return doc;
}

}  // namespace

TEST_CASE("Risk: nothing is shown under 1 in 20, then yellow, red and dark red", "[gui][risk]") {
    CHECK(riskBand(0.0) == RiskBand::None);
    CHECK(riskBand(0.049) == RiskBand::None);
    CHECK(riskBand(0.05) == RiskBand::May);
    CHECK(riskBand(0.199) == RiskBand::May);
    CHECK(riskBand(0.2) == RiskBand::Likely);
    CHECK(riskBand(0.5) == RiskBand::VeryLikely);
    CHECK(riskBand(1.0) == RiskBand::VeryLikely);
    CHECK(riskColour(RiskBand::None) == 0u);
    CHECK(riskColour(RiskBand::May) == 0xF2D34Bu);
    CHECK(riskColour(RiskBand::Likely) == 0xFF6B6Bu);
    CHECK(riskColour(RiskBand::VeryLikely) == 0xB3214Au);
}

TEST_CASE("Risk: a plain column has nothing to show, and says how it was checked", "[gui][risk]") {
    const fruit::StabilityResult res = replay(fruit::makeBox({0, 0, 0}, {5, 5, 4}));
    RiskReport r;
    findRiskPlaces(res, 0.7, r);
    CHECK(r.places.empty());
    CHECK(r.tips.empty());
    CHECK(r.closeness.size() == res.in_layer.size());
    const RiskHeadline h = riskHeadline(r);
    CHECK(h.ok);
    CHECK(h.title == "Nothing likely to be lost in printing");
    CHECK(h.detail == "The print was replayed layer by layer, 400 times.");
    fruit::TriangleMesh paint[3];
    paintRisk(fruit::makeBox({0, 0, 0}, {5, 5, 4}), res, paint, fruit::JobControl::none());
    for (const auto& m : paint) CHECK(m.empty());
}

TEST_CASE("Risk: a wide cap on a thin stem is a place, in plain words, with what goes with it", "[gui][risk]") {
    const fruit::TriangleMesh mesh = capOnStem(12);  // 144 mm² on 1 mm²
    const fruit::StabilityResult res = replay(mesh);
    RiskReport r;
    findRiskPlaces(res, 0.7, r);
    REQUIRE(!r.places.empty());
    const RiskPlace* thin = nullptr;
    for (const RiskPlace& p : r.places)
        if (p.kind == RiskPlace::Kind::ThinPlace && !thin) thin = &p;
    REQUIRE(thin != nullptr);
    CHECK(thin->band == RiskBand::VeryLikely);
    CHECK_THAT(thin->title, ContainsSubstring("Thin place, "));
    CHECK_THAT(thin->title, ContainsSubstring(" mm up"));
    CHECK_THAT(thin->brief, ContainsSubstring("Very likely to break · "));
    CHECK_THAT(thin->brief, ContainsSubstring(" mm³ would be lost"));
    CHECK(thin->holds == "144 mm² of layer on 1.0 mm² of resin");
    CHECK(thin->when == "while layer 41 prints, 2.05 mm up");  // the cap's first layer
    CHECK(thin->layer == 40);
    CHECK(thin->why == "all that is above it hangs on this one thin place");
    // The cap is 144 mm³; some of the stem goes with it.
    CHECK(thin->lost_mm3 >= 144.0);
    CHECK(thin->lost_mm3 <= 146.0);
    CHECK_THAT(thin->if_goes, ContainsSubstring("mm³ above it is lost"));
    CHECK(thin->note.empty());  // pulled straight: no bending to be unsure of
    // The place that loses the most of the model is first.
    CHECK(r.places.front().lost_mm3 >= r.places.back().lost_mm3);
    const RiskHeadline h = riskHeadline(r);
    CHECK_FALSE(h.ok);
    CHECK_FALSE(h.tips_only);
    CHECK_THAT(h.title, ContainsSubstring("may not print"));
    CHECK_THAT(h.detail, ContainsSubstring("Pick a place to see it on the model."));
    // The riskiest layer is one of the cap's: each pulls the stem as hard.
    CHECK(r.riskiest_layer >= 40);
    CHECK(r.closeness[static_cast<std::size_t>(r.riskiest_layer)] > 1.0f);
    CHECK(r.lost_mm3[2] >= 144.0);
}

TEST_CASE("Risk: the tips that may let go are one place; one that only grazes the model is left out", "[gui][risk]") {
    fruit::StabilityResult res;
    res.layer_height = kH;
    res.stats.replays = 500;
    auto tip = [](double x, double area, double chance, double load, double holds, int layer) {
        fruit::TipRisk t;
        t.at = {x, 0, 6};
        t.area = area;
        t.chance = chance;
        t.load = load;
        t.holds = holds;
        t.at_layer = layer;
        return t;
    };
    const double holds = 33.3, usable = 1.0 / 3;  // a 0.4 mm tip with the borrowed table: trusted with 11.1 mm²
    res.tips.push_back(tip(0, 0.126, 0.30, 30.0, holds, 160));  // likely
    res.tips.push_back(tip(2, 0.126, 0.72, 36.0, holds, 200));  // very likely
    res.tips.push_back(tip(4, 0.126, 0.01, 12.0, holds, 120));  // not worth showing
    res.tips.push_back(tip(6, 0.002, 1.00, 4.0, 0.5, 300));     // only grazes the model, and nothing goes with it
    res.tips.push_back(tip(8, 0.126, 0.25, 28.0, holds, 180));  // likely
    RiskReport r;
    findRiskPlaces(res, usable, r);
    // Coloured in the view: the three. Not the grazing one, nor the one under 1 in 20.
    REQUIRE(r.tips.size() == 3);
    CHECK(r.tips[0].band == RiskBand::Likely);
    CHECK(r.tips[1].band == RiskBand::VeryLikely);
    CHECK(r.tips[2].band == RiskBand::Likely);
    CHECK(r.tips[0].diameter == Approx(0.4));
    // One place for all of them; the model stays held without them.
    REQUIRE(r.places.size() == 1);
    const RiskPlace& p = r.places.front();
    CHECK(p.kind == RiskPlace::Kind::Tips);
    CHECK(p.tips == 3);
    CHECK(p.band == RiskBand::VeryLikely);
    CHECK(p.title == "3 tips may let go, some very likely");
    CHECK(p.brief == "Some are very likely to let go · the model stays held");
    CHECK(p.holds == "each carries 2.5 to 3.2 times what a tip of its size is trusted with");
    CHECK(p.when == "while layers 161 to 201 print");
    CHECK(p.if_goes == "the tips around them take their load, and the model stays held");
    CHECK(p.at.x == Approx(2));  // at the likeliest
    CHECK(p.layer == 200);
    CHECK(p.lost_mm3 == 0);
    const RiskHeadline h = riskHeadline(r);
    CHECK_FALSE(h.ok);
    CHECK(h.tips_only);
    CHECK(h.title == "3 tips may let go, some very likely");
    CHECK(h.detail == "They carry 2.5 to 3.2 times what they are trusted with. The model stays held without them.");

    // In 2 replays of 5 part of the model went with them, most often with the last of the three: the place
    // says so, stands at that tip, and is no longer "only tips".
    res.stats.tips_cost_chance = 0.4;
    res.stats.tips_cost_volume = 16;
    res.tips[4].lost_per_print = 5.5;
    res.tips[0].lost_per_print = 0.9;
    RiskReport costly;
    findRiskPlaces(res, usable, costly);
    REQUIRE(costly.places.size() == 1);
    const RiskPlace& c = costly.places.front();
    CHECK(c.tips == 3);
    CHECK(c.lost_mm3 == Approx(16));
    CHECK(c.brief == "Some are very likely to let go · 16 mm³ may go with them");
    CHECK(c.if_goes == "in 200 of 500 replays part of the model went with them: about 16 mm³");
    CHECK(c.at.x == Approx(8));
    CHECK(c.layer == 180);
    const RiskHeadline ch = riskHeadline(costly);
    CHECK_FALSE(ch.tips_only);
    CHECK(ch.title == "Part of the model is likely to come off its tips");
    CHECK(ch.detail == "The print was replayed layer by layer, 500 times. Pick it to see it on the model.");
    // When what goes is all of the model, it is said so.
    res.stats.model_volume = 17;
    res.stats.tips_cost_chance = 0.9;
    RiskReport all;
    findRiskPlaces(res, usable, all);
    CHECK(riskHeadline(all).title == "The whole model is very likely to come off its tips");
    res.stats.model_volume = 0;

    // One alone is said as one.
    res.stats.tips_cost_chance = 0;
    res.stats.tips_cost_volume = 0;
    res.tips.erase(res.tips.begin() + 1, res.tips.end());
    res.tips[0].lost_per_print = 0;
    RiskReport one;
    findRiskPlaces(res, usable, one);
    REQUIRE(one.places.size() == 1);
    CHECK(one.places.front().title == "1 tip is likely to let go");
    CHECK(one.places.front().brief == "Likely to let go · the model stays held");
    CHECK(riskHeadline(one).detail == "It carries about 2.7 times what it is trusted with. The model stays held without it.");
}

TEST_CASE("Risk: with a measured table, a tip at just what it is trusted with is not news", "[gui][risk]") {
    // A fitted table trusts a tip with 0.7 of the load at which half let go: there about 1 in 18 already does.
    fruit::StabilityResult res;
    res.layer_height = kH;
    res.stats.replays = 500;
    const double holds = 7.5, usable = 0.7;  // trusted with 5.25 mm²
    auto tip = [&](double x, double times_trusted) {
        fruit::TipRisk t;
        t.at = {x, 0, 6};
        t.area = 0.126;
        t.holds = holds;
        t.load = times_trusted * holds * usable;
        t.closeness = t.load / holds;
        t.chance = fruit::breakChance(t.closeness, 7);
        t.at_layer = 100;
        return t;
    };
    res.tips.push_back(tip(0, 1.0));   // at its rated load: 5.6 %
    res.tips.push_back(tip(2, 1.05));  // a little over: still not shown
    CHECK(res.tips[0].chance > kRiskShownFrom);
    RiskReport quiet;
    findRiskPlaces(res, usable, quiet);
    CHECK(quiet.tips.empty());
    CHECK(quiet.places.empty());
    CHECK(riskHeadline(quiet).ok);
    // Clearly over: shown.
    res.tips.push_back(tip(4, 1.3));
    RiskReport over;
    findRiskPlaces(res, usable, over);
    REQUIRE(over.tips.size() == 1);
    REQUIRE(over.places.size() == 1);
    CHECK(over.places.front().tips == 1);
    CHECK(over.places.front().holds == "it carries about 1.3 times what a tip of its size is trusted with");
    // But when the model goes with tips that are all at their rated loads, they are shown: all of them.
    res.tips.pop_back();
    res.stats.tips_cost_chance = 0.3;
    res.stats.tips_cost_volume = 40;
    RiskReport falls;
    findRiskPlaces(res, usable, falls);
    REQUIRE(falls.places.size() == 1);
    CHECK(falls.places.front().tips == 2);
    CHECK(falls.places.front().lost_mm3 == Approx(40));
    CHECK_FALSE(riskHeadline(falls).tips_only);
}

TEST_CASE("Risk: the paint covers the surface of what would be lost, and nothing else", "[gui][risk]") {
    const fruit::TriangleMesh mesh = capOnStem(12);
    const fruit::StabilityResult res = replay(mesh);
    fruit::TriangleMesh paint[3];
    paintRisk(mesh, res, paint, fruit::JobControl::none());
    // The cap is very likely lost: its top, its underside and its four sides.
    const double cap = 2 * 144.0 + 4 * 12.0 * 1.0;
    const double stem = 4 * 1.0 * 2.0 + 1.0;  // all of the stem's own faces, at most
    CHECK(surface(paint[2]) >= cap - 1e-6);
    CHECK(surface(paint[2]) <= cap + stem + 1e-6);
    CHECK(paint[0].empty());
    CHECK(paint[1].empty());
    // All of it lies on the model.
    const fruit::BBox3 box = mesh.bbox();
    for (const fruit::Vec3& v : paint[2].vertices) {
        CHECK(v.x >= box.min.x - 1e-9);
        CHECK(v.x <= box.max.x + 1e-9);
        CHECK(v.z >= box.min.z - 1e-9);
        CHECK(v.z <= box.max.z + 1e-9);
    }
    // The foot of the stem stays on the plate: nothing is painted there.
    double lowest = 1e9;
    for (const fruit::Vec3& v : paint[2].vertices) lowest = std::min(lowest, v.z);
    CHECK(lowest > 0.0);
}

TEST_CASE("Risk: a layer's picture is painted where its print would be lost", "[gui][risk]") {
    const fruit::StabilityResult res = replay(capOnStem(12));
    LayerImage grid;
    grid.w = 160;
    grid.h = 160;
    grid.px = grid.py = 0.1;
    grid.x_left = -8;
    grid.y_top = 8;
    // A layer of the cap: all of it dark red.
    const RiskLayerPaint cap = paintRiskLayer(res, 50, grid);
    REQUIRE(cap.any);
    REQUIRE(cap.cover[2].size() == 160u * 160u);
    double lit = 0;
    for (std::uint8_t c : cap.cover[2]) lit += c / 255.0;
    CHECK(lit * 0.01 == Approx(144.0).epsilon(0.02));  // mm²
    CHECK(cap.cover[2][80 * 160 + 80] == 255);          // its middle
    CHECK(cap.cover[2][2 * 160 + 2] == 0);              // off the print
    // The stem's first layer stands on the plate.
    const RiskLayerPaint foot = paintRiskLayer(res, 0, grid);
    CHECK_FALSE(foot.any);
    // Off the print's layers: nothing.
    CHECK_FALSE(paintRiskLayer(res, -1, grid).any);
    CHECK_FALSE(paintRiskLayer(res, 100000, grid).any);
}

TEST_CASE("Heatmap: the scale runs fine, 1 in 100, 1 in 20, 1 in 5, 1 in 2, in its colours", "[gui][risk][heat]") {
    CHECK(heatPosition(0) == 0);
    CHECK(heatPosition(0.0005) == 0);
    CHECK(heatPosition(0.01) == Approx(0.22));
    CHECK(heatPosition(0.05) == Approx(0.45));
    CHECK(heatPosition(0.2) == Approx(0.68));
    CHECK(heatPosition(0.5) == Approx(0.88));
    CHECK(heatPosition(1) == 1);
    double last = -1;
    for (double c = 1e-4; c <= 1.0; c *= 1.07) {
        const double p = heatPosition(c);
        CHECK(p >= last);
        last = p;
    }
    CHECK(heatStep(0) == 0);
    CHECK(heatStep(0.001) == 0);  // fine
    CHECK(heatStep(0.01) > 0);
    CHECK(heatStep(1) == kHeatSteps - 1);
    CHECK(heatColour(0) == 0x3E5C8Au);
    CHECK(heatColour(0.45) == 0xE2C255u);
    CHECK(heatColour(1) == 0x9E1838u);
    // Every mark on the legend lies where the scale puts its chance.
    for (const HeatMark& m : heatMarks()) CHECK(heatPosition(m.chance) > 0);
}

TEST_CASE("Heatmap: a plain column is fine all over, and nothing is painted on it", "[gui][risk][heat]") {
    const fruit::TriangleMesh mesh = fruit::makeBox({0, 0, 0}, {5, 5, 4});
    const fruit::StabilityResult res = replay(mesh);
    for (HeatOf of : {HeatOf::Lost, HeatOf::Breaks}) {
        HeatPaint heat;
        paintHeat(mesh, res, of, heat, fruit::JobControl::none());
        CHECK_FALSE(heat.any());
        for (float c : heatByLayer(res, of)) CHECK(heatStep(c) == 0);
    }
}

TEST_CASE("Heatmap: the wide cap on a thin stem is hot all over, its stem's foot fine", "[gui][risk][heat]") {
    const fruit::TriangleMesh mesh = capOnStem(12);
    const fruit::StabilityResult res = replay(mesh);
    HeatPaint heat;
    paintHeat(mesh, res, HeatOf::Lost, heat, fruit::JobControl::none());
    REQUIRE(heat.any());
    // The cap is very likely lost: its whole surface is painted, in the steps of 1 in 2 or more.
    const double cap = 2 * 144.0 + 4 * 12.0 * 1.0;
    double hot = 0, all = 0;
    for (int s = 0; s < kHeatSteps; ++s) {
        CHECK((s > 0 || heat.steps[s].empty()));  // step 0 is the model's own colour, never painted
        all += surface(heat.steps[s]);
        if (s >= heatStep(0.5)) hot += surface(heat.steps[s]);
    }
    CHECK(hot >= cap - 1e-6);
    CHECK(all <= surface(mesh) + 1e-6);  // never more than the model's surface
    // All of it lies on the model, and the stem's foot on the plate is not painted.
    const fruit::BBox3 box = mesh.bbox();
    double lowest = 1e9;
    for (const fruit::TriangleMesh& m : heat.steps)
        for (const fruit::Vec3& v : m.vertices) {
            CHECK(v.x >= box.min.x - 1e-9);
            CHECK(v.x <= box.max.x + 1e-9);
            CHECK(v.z <= box.max.z + 1e-9);
            lowest = std::min(lowest, v.z);
        }
    CHECK(lowest > 0.0);
    // Its layers say so too, layer by layer: the cap's hot, the stem's first fine.
    const std::vector<float> by = heatByLayer(res, HeatOf::Lost);
    REQUIRE(by.size() == res.in_layer.size());
    CHECK(heatStep(by[50]) >= heatStep(0.5));
    CHECK(heatStep(by[0]) == 0);
    // Where a break would start is somewhere under the cap, in the stem.
    HeatPaint breaks;
    paintHeat(mesh, res, HeatOf::Breaks, breaks, fruit::JobControl::none());
    REQUIRE(breaks.any());
}

TEST_CASE("Heatmap: a ledge's underside warms along its skin, not all one colour", "[gui][risk][heat]") {
    // A wide column with a ledge reaching out from its top: the ledge is held, its first layer a skin
    // reaching past what holds it.
    fruit::TriangleMesh mesh = fruit::makeBox({0, 0, 0}, {10, 10, 3});
    mesh.append(fruit::makeBox({0, 0, 3}, {15, 10, 4}));
    const fruit::StabilityResult res = replay(mesh);
    const int first = static_cast<int>(std::lround(3.0 / kH));
    REQUIRE(static_cast<std::size_t>(first) < res.skins.size());
    INFO("skins in the ledge's first layer: " << res.skins[static_cast<std::size_t>(first)].size());
    HeatPaint heat;
    paintHeat(mesh, res, HeatOf::Lost, heat, fruit::JobControl::none());
    // The steps painted on the ledge's underside (z = 3, beyond the column).
    std::set<int> under;
    for (int st = 1; st < kHeatSteps; ++st) {
        const fruit::TriangleMesh& m = heat.steps[st];
        for (const fruit::Face& f : m.faces) {
            const fruit::Vec3 c = (m.vertices[static_cast<std::size_t>(f[0])] + m.vertices[static_cast<std::size_t>(f[1])] +
                                   m.vertices[static_cast<std::size_t>(f[2])]) / 3.0;
            if (std::abs(c.z - 3.0) < 1e-6 && c.x > 10.0) under.insert(st);
        }
    }
    REQUIRE_FALSE(res.skins[static_cast<std::size_t>(first)].empty());
    CHECK(under.size() >= 3);
}

TEST_CASE("Heatmap: a layer's picture shows the print in its step, fine included", "[gui][risk][heat]") {
    const fruit::StabilityResult res = replay(capOnStem(12));
    LayerImage grid;
    grid.w = 160;
    grid.h = 160;
    grid.px = grid.py = 0.1;
    grid.x_left = -8;
    grid.y_top = 8;
    const RiskLayerHeat cap = heatRiskLayer(res, 50, grid, HeatOf::Lost);
    REQUIRE(cap.any);
    const int st = heatStep(res.chanceAt(50, {0, 0}));
    REQUIRE(cap.cover[st].size() == 160u * 160u);
    double lit = 0;
    for (std::uint8_t c : cap.cover[st]) lit += c / 255.0;
    CHECK(lit * 0.01 == Approx(144.0).epsilon(0.02));  // mm²
    // The stem's first layer is fine, and shown as fine (not left out).
    const RiskLayerHeat foot = heatRiskLayer(res, 0, grid, HeatOf::Lost);
    REQUIRE(foot.any);
    CHECK_FALSE(foot.cover[0].empty());
    CHECK_FALSE(heatRiskLayer(res, -1, grid, HeatOf::Lost).any);
    CHECK_FALSE(heatRiskLayer(res, 100000, grid, HeatOf::Lost).any);
}

TEST_CASE("No raft: a separate piece that starts inside the bottom layers stands on the plate", "[gui][risk][plate]") {
    // A block and, apart from it, a small piece whose underside is `up` mm above the plate.
    auto job = [](double up) {
        fruit::TriangleMesh m = fruit::makeBox({0, 0, 0}, {20, 10, 8});
        m.append(fruit::makeBox({24, 2, up}, {30, 8, 6}));
        const auto path = std::filesystem::temp_directory_path() / "risk_test_plate_piece.stl";
        fruit::writeStl(path.string(), m);
        auto doc = loadDocument(path.string(), fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
        std::filesystem::remove(path);
        fruit::Placement p = doc->placement();
        p.on_plate = true;
        doc->edit(p);
        return doc;
    };
    auto midAirRow = [](const SupportResult& s) {
        for (const SupportCheck& c : s.checks)
            if (c.title == "No part starts in mid-air") return c.ok;
        FAIL("no mid-air row");
        return false;
    };
    // 0.05 mm up, in the second layer: the printer's four bottom layers print two of the model's (0.1 mm).
    {
        const auto doc = job(0.05);
        const SupportResult sup = generate(*doc, SupportChoices{}, {}, fruit::JobControl::none());
        CHECK(sup.cfg.plate_hold_height == Approx(0.1));
        CHECK(sup.tips == 0);  // the planner: nothing for a tip to hold
        CHECK(sup.validation.floating.empty());  // the mid-air check
        CHECK(sup.validation.plate_held == 1);
        CHECK(midAirRow(sup));
        const RiskReport r = checkRisk(*doc, sup, fruit::JobControl::none());  // the replay
        REQUIRE(r.result != nullptr);
        CHECK(r.result->stats.unheld == 0);
        for (const RiskPlace& p : r.places) CHECK(p.kind != RiskPlace::Kind::MidAir);
        CHECK(riskHeadline(r).ok);
    }
    // 0.2 mm up, above them: tips under it, and with them nothing in mid-air.
    {
        const auto doc = job(0.2);
        const SupportResult sup = generate(*doc, SupportChoices{}, {}, fruit::JobControl::none());
        CHECK(sup.tips > 0);
        CHECK(sup.validation.plate_held == 0);
        CHECK(sup.validation.floating.empty());
        CHECK(midAirRow(sup));
        const RiskReport r = checkRisk(*doc, sup, fruit::JobControl::none());
        REQUIRE(r.result != nullptr);
        CHECK(r.result->stats.unheld == 0);
        CHECK(r.result->stats.tips > 0);
    }
}

TEST_CASE("Risk: the demo bracket on its own supports is replayed, the same every time", "[gui][risk][slow]") {
    const auto doc = bracket();
    const SupportResult sup = generate(*doc, SupportChoices{}, {}, fruit::JobControl::none());
    const RiskReport a = checkRisk(*doc, sup, fruit::JobControl::none());
    REQUIRE(a.result != nullptr);
    CHECK(a.borrowed);  // FruitSlicer's own tip table
    CHECK(a.replays == 500);
    CHECK(a.result->stats.tips > 0);
    CHECK(a.closeness.size() == a.result->in_layer.size());
    CHECK(a.result->stats.unheld == 0);  // as the supports' own check says: nothing starts in mid-air
    const RiskHeadline h = riskHeadline(a);
    CHECK_FALSE(h.title.empty());
    CHECK_FALSE(h.detail.empty());
    CHECK_FALSE(a.cups);
    CHECK(riskStrengthsNote(a) == "Strengths are borrowed, not measured for this resin. How far a layer's pull "
                                  "spreads through thin model is a guess too: with another, a tip can come out many "
                                  "times likelier to let go.");
    // Measured tips, and a cup in the print: the suction is named too.
    RiskReport measured_cup;
    measured_cup.borrowed = false;
    measured_cup.cups = true;
    CHECK(riskStrengthsNote(measured_cup) ==
          "Tip strengths are this resin's, from its tips plate. How strong the model is between its own layers is "
          "taken to be the same: not measured. How far a layer's pull spreads through thin model is a guess too: "
          "with another, a tip can come out many times likelier to let go. So is how hard the suction in a cup "
          "pulls.");
    // Each place has its words, and a layer to go to.
    for (const RiskPlace& p : a.places) {
        CHECK_FALSE(p.title.empty());
        CHECK_FALSE(p.brief.empty());
        CHECK_FALSE(p.holds.empty());
        CHECK_FALSE(p.when.empty());
        CHECK_FALSE(p.why.empty());
        CHECK_FALSE(p.if_goes.empty());
        CHECK(p.band != RiskBand::None);
        CHECK(p.layer >= 0);
        CHECK(p.layer < static_cast<int>(a.closeness.size()));
    }
    // The same again.
    const RiskReport b = checkRisk(*doc, sup, fruit::JobControl::none());
    REQUIRE(a.places.size() == b.places.size());
    for (std::size_t i = 0; i < a.places.size(); ++i) {
        CHECK(a.places[i].title == b.places[i].title);
        CHECK(a.places[i].brief == b.places[i].brief);
    }
    CHECK(a.tips.size() == b.tips.size());
    // The Hold slider asks more or less of each tip; it does not make a tip stronger or weaker.
    SupportChoices less = sup.choices;
    less.hold = 1;
    SupportResult trusted_more = sup;
    trusted_more.choices = less;
    const RiskReport c = checkRisk(*doc, trusted_more, fruit::JobControl::none());
    REQUIRE(c.result != nullptr);
    CHECK(c.result->stats.worst_closeness == Approx(a.result->stats.worst_closeness));
}

TEST_CASE("Risk: replaying the print and strengthening it makes losing part of the model less likely", "[gui][risk][slow]") {
    // A plate 20 x 20 mm, 1 mm thick, 6 mm up, its supports made as usual and then thinned to every fourth tip:
    // what is left is pulled far harder than its tips are trusted with.
    const auto path = std::filesystem::temp_directory_path() / "risk_test_plate.stl";
    fruit::writeStl(path.string(), fruit::makeBox({0, 0, 6}, {20, 20, 7}));
    auto doc = loadDocument(path.string(), fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    std::filesystem::remove(path);
    REQUIRE(doc);
    const SupportResult full = generate(*doc, SupportChoices{}, {}, fruit::JobControl::none());
    JobPlan thin = full.plans;
    for (auto& [part, plan] : thin) {
        std::vector<fruit::SupportPoint> kept;
        for (std::size_t i = 0; i < plan.points.size(); i += 4) kept.push_back(plan.points[i]);
        plan.points = kept;
    }
    const SupportResult weak = finishWithEdits(*doc, full.setup, thin, {}, fruit::JobControl::none());
    const RiskReport as_is = checkRisk(*doc, weak, fruit::JobControl::none(), false);
    const RiskReport before = checkRisk(*doc, weak, fruit::JobControl::none(), false, kPeakWeaker);
    const SupportResult strong = lowerPeak(*doc, full.setup, thin, {}, weak, fruit::JobControl::none());
    const PeakFix& f = strong.peak;
    CAPTURE(f.lost_before, f.lost_after, f.mm3_before, f.mm3_after, f.rounds, f.enlarged, f.added, f.seconds);
    REQUIRE(f.ran);
    CHECK(f.weaker == kPeakWeaker);
    CHECK(f.replays == before.result->stats.replays);
    // Its check is the replay with the resin a step weaker: no less of the model lost than with it as it is.
    CHECK(f.lost_before == Approx(1 - before.result->stats.whole_chance).margin(1e-9));
    CHECK(f.mm3_before == Approx(before.result->stats.expected_loss).margin(1e-9));
    CHECK(before.result->stats.whole_chance <= as_is.result->stats.whole_chance);
    REQUIRE(f.lost_before > kPeakChance);  // the thinned plate is weak enough to need it
    // It made tips bigger or added some, kept the rounds that helped, and less of the model is to be expected lost.
    CHECK(f.rounds >= 1);
    CHECK(f.enlarged + f.added > 0);
    CHECK(f.lost_after <= f.lost_before);
    CHECK(f.mm3_after < f.mm3_before);
    // The result is what it says: replayed again, it is where the loop left it.
    const RiskReport again = checkRisk(*doc, strong, fruit::JobControl::none(), false, kPeakWeaker);
    CHECK(1 - again.result->stats.whole_chance == Approx(f.lost_after).margin(1e-9));
    CHECK(again.result->stats.expected_loss == Approx(f.mm3_after).margin(1e-9));
    // The tips it added are the plan's, so they are saved with the job.
    std::size_t planned = 0, thinned = 0;
    for (const auto& [part, plan] : strong.plans) planned += plan.points.size();
    for (const auto& [part, plan] : thin) thinned += plan.points.size();
    CHECK(planned == thinned + f.added);
    // Said on the Supports panel.
    CHECK_THAT(peakLine(f), ContainsSubstring("Replayed with the resin 1.41 times weaker than its values"));
    CHECK_THAT(peakLine(f), ContainsSubstring("The chance of losing part of the model went from 1 in "));
    CHECK(peakLine(PeakFix{}).empty());
}

TEST_CASE("Risk: the strengthening loop leaves tips whose letting go costs none of the model", "[gui][risk][slow]") {
    // The same plate with all its supports: some of its tips may let go, but their neighbours keep the plate.
    const auto path = std::filesystem::temp_directory_path() / "risk_test_plate_full.stl";
    fruit::writeStl(path.string(), fruit::makeBox({0, 0, 6}, {20, 20, 7}));
    auto doc = loadDocument(path.string(), fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    std::filesystem::remove(path);
    REQUIRE(doc);
    const SupportResult full = generate(*doc, SupportChoices{}, {}, fruit::JobControl::none());
    const RiskReport check = checkRisk(*doc, full, fruit::JobControl::none(), false, kPeakWeaker);
    double likeliest = 0;
    for (const fruit::TipRisk& t : check.result->tips) likeliest = std::max(likeliest, t.chance);
    CAPTURE(likeliest, check.result->stats.whole_chance);
    REQUIRE(1 - check.result->stats.whole_chance <= kPeakChance);
    const SupportResult same = lowerPeak(*doc, full.setup, full.plans, {}, full, fruit::JobControl::none());
    CHECK(same.peak.ran);
    CHECK(same.peak.rounds == 0);
    CHECK(same.peak.enlarged + same.peak.added == 0);
    CHECK_THAT(peakLine(same.peak), ContainsSubstring("so no tips were needed."));
}
