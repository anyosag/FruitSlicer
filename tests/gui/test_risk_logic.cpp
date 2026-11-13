// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// What may not print: the replay's findings as the screens word, colour and paint them.
#include <catch2/catch_approx.hpp>
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

TEST_CASE("Risk: tips that may let go are one place while the model stays held; one that only grazes it is left out",
          "[gui][risk]") {
    fruit::StabilityResult res;
    res.layer_height = kH;
    res.stats.replays = 500;
    auto tip = [](double x, double area, double chance, double load, double holds, double lost, int layer) {
        fruit::TipRisk t;
        t.at = {x, 0, 6};
        t.area = area;
        t.chance = chance;
        t.load = load;
        t.holds = holds;
        t.lost_volume = lost;
        t.at_layer = layer;
        return t;
    };
    const double holds = 33.3, usable = 1.0 / 3;  // a 0.4 mm tip with the borrowed table: trusted with 11.1 mm²
    res.tips.push_back(tip(0, 0.126, 0.30, 30.0, holds, 0, 160));  // may let go; the model stays held
    res.tips.push_back(tip(2, 0.126, 0.72, 36.0, holds, 0, 200));  // very likely; the model stays held
    res.tips.push_back(tip(4, 0.126, 0.01, 12.0, holds, 0, 120));  // not worth showing
    res.tips.push_back(tip(6, 0.002, 1.00, 4.0, 0.5, 0, 300));     // only grazes the model, and nothing goes with it
    res.tips.push_back(tip(8, 0.126, 0.25, 28.0, holds, 3.0, 180));  // takes 3 mm³ of the model with it
    RiskReport r;
    findRiskPlaces(res, usable, r);
    // Coloured in the view: the two, and the one that takes model with it. Not the grazing one.
    REQUIRE(r.tips.size() == 3);
    CHECK(r.tips[0].band == RiskBand::Likely);
    CHECK(r.tips[1].band == RiskBand::VeryLikely);
    CHECK(r.tips[2].band == RiskBand::Likely);
    CHECK(r.tips[0].diameter == Approx(0.4));
    // One place for the two the model stays held without. (The one that takes model with it is a place of
    // its own, from the replay's list of weak places: none was given here.)
    REQUIRE(r.places.size() == 1);
    const RiskPlace& p = r.places.front();
    CHECK(p.kind == RiskPlace::Kind::Tips);
    CHECK(p.tips == 2);
    CHECK(p.band == RiskBand::VeryLikely);
    CHECK(p.title == "2 tips may let go");
    CHECK(p.brief == "Some are very likely to let go · the model stays held");
    CHECK(p.holds == "each carries 2.7 to 3.2 times what a tip of its size is trusted with");
    CHECK(p.when == "while layers 161 to 201 print");
    CHECK(p.at.x == Approx(2));  // at the likeliest
    CHECK(p.layer == 200);
    const RiskHeadline h = riskHeadline(r);
    CHECK_FALSE(h.ok);
    CHECK(h.tips_only);
    CHECK(h.title == "2 tips may let go");
    CHECK(h.detail == "They carry 2.7 to 3.2 times what they are trusted with. The model stays held without them.");
    // One alone is said as one.
    res.tips.erase(res.tips.begin() + 1, res.tips.end());
    RiskReport one;
    findRiskPlaces(res, usable, one);
    REQUIRE(one.places.size() == 1);
    CHECK(one.places.front().title == "1 tip may let go");
    CHECK(riskHeadline(one).detail == "It carries about 2.7 times what it is trusted with. The model stays held without it.");
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
    CHECK(riskStrengthsNote(a) == "Strengths are borrowed, not measured for this resin.");
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
