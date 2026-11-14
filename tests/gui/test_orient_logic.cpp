// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The Orient step's logic: scores, the search, pictures and summaries.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <numeric>

#include "Document.h"
#include "Orient.h"
#include "mesh/Primitives.h"
#include "mesh/StlIO.h"

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using namespace gui;

namespace {

std::shared_ptr<Document> load(const fruit::TriangleMesh& m, const char* name) {
    const auto path = std::filesystem::temp_directory_path() / name;
    fruit::writeStl(path.string(), m);
    auto doc = loadDocument(path.string(), fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    std::filesystem::remove(path);
    return doc;
}

std::size_t covered(const Silhouette& s) {
    return static_cast<std::size_t>(std::count_if(s.alpha.begin(), s.alpha.end(), [](auto a) { return a > 127; }));
}

}  // namespace

TEST_CASE("Orient: a box seen from below fills its footprint, mirrored", "[gui][orient]") {
    // 20 x 10 mm footprint at 1 mm per pixel: 200 pixels.
    fruit::TriangleMesh box = fruit::makeBox({0, 0, 0}, {20, 10, 5});
    const Silhouette s = silhouetteBelow(box, 1.0, 40, 30);
    CHECK(covered(s) == Approx(200).margin(30));
    // An L in plan view: a 10 mm square at -X and a 4 mm wide arm towards
    // +X. From underneath +X runs to the left, so the leftmost covered
    // column is the thin arm and the rightmost the square.
    fruit::TriangleMesh l = fruit::makeBox({0, 0, 0}, {10, 10, 5});
    l.append(fruit::makeBox({10, 0, 0}, {30, 4, 5}));
    const Silhouette ls = silhouetteBelow(l, 1.0, 40, 30);
    auto column = [&](int x) {
        int n = 0;
        for (int y = 0; y < ls.h; ++y) n += ls.alpha[static_cast<std::size_t>(y) * ls.w + x] > 127;
        return n;
    };
    int first = -1, last = -1;
    for (int x = 0; x < ls.w; ++x)
        if (column(x) > 0) {
            if (first < 0) first = x;
            last = x;
        }
    REQUIRE(first >= 0);
    CHECK(column(first + 1) == Approx(4).margin(1));
    CHECK(column(last - 1) == Approx(10).margin(1));
}

TEST_CASE("Orient: summaries name the gains first and the costs after", "[gui][orient]") {
    OrientMetrics now;
    now.tips = 298, now.island_tips = 36, now.short_of_hold = 14, now.biggest_layer = 1035, now.top = 51.2,
    now.layers = 1024;
    OrientMetrics o = now;
    o.tips = 215, o.island_tips = 6, o.short_of_hold = 2, o.biggest_layer = 571, o.top = 70.0, o.layers = 1400;
    const std::string s = summarize(o, now);
    CHECK_THAT(s, ContainsSubstring("30 fewer spots start in mid-air"));
    CHECK_THAT(s, ContainsSubstring("12 fewer layers are short of hold"));
    CHECK_THAT(s, ContainsSubstring("But it stands 18.8 mm taller (376 more layers)"));
    CHECK(summarize(now, now) == "Much like now.");
}

TEST_CASE("Orient: picked sides follow the model when it turns", "[gui][orient]") {
    fruit::Placement p;
    p.rotation_deg = {0, 0, 90};  // a quarter turn about the vertical
    // A side picked as "Front" (-Y in the printer) is +X... in model terms it
    // is whatever the turn brought to the front; check the round trip.
    const fruit::Vec3 model_front{1, 0, 0};
    const auto printer = visibleInPrinter({model_front}, p);
    REQUIRE(printer.size() == 1);
    CHECK(printer[0].x == Approx(0).margin(1e-9));
    CHECK(printer[0].y == Approx(1));  // +X of the model now faces the back
}

TEST_CASE("Orient: scoring as it sits and searching for better", "[gui][orient][slow]") {
    // The demo bracket: its shelf overhangs, so lying it down helps.
    const auto doc = load(demoModel(), "orient_test_bracket.stl");
    const OrientMetrics now = measure(*doc, {}, fruit::JobControl::none());
    CHECK(now.tips > 0);
    CHECK(now.top == Approx(doc->fit().top));
    CHECK(now.layers == static_cast<std::size_t>(std::ceil(now.top / kLayerHeight - 1e-9)));

    const OrientSearch s = search(*doc, {}, fruit::JobControl::none());
    CHECK(s.directions == 65);
    CHECK(s.finalists == 6);  // OrientOptions' default, how it sits now not counted
    // How it sits now and every finalist that fits got supports and a replay:
    // the tips are the supports' own (the planner alone leaves out those it
    // adds where one tip would carry too much).
    CHECK(s.now.m.risk.checked);
    CHECK(s.now.m.tips > 0);
    CHECK(s.replayed >= 1);
    CHECK(s.replayed <= 1 + s.finalists);
    CHECK(s.options.size() <= 3);
    CHECK(static_cast<int>(s.options.size()) + s.riskier <= s.finalists);
    for (const OrientOption& o : s.options) {
        CHECK(o.fits);
        CHECK(o.m.risk.checked);
        CHECK_FALSE(saferThan(s.now.m.risk, o.m.risk));  // never one that may lose more than now
        CHECK(o.m.tips > 0);
        CHECK(o.m.biggest_layer > 0);
        CHECK_FALSE(o.summary.empty());
        CHECK(o.below.w == 568);
        CHECK(covered(o.below) > 100);
    }
    if (s.options.empty() && s.riskier > 0) CHECK_THAT(s.now.summary, ContainsSubstring("may not print"));
    CHECK(covered(s.now.below) > 100);
    // The same run for how it sits now, alone, gives the same numbers.
    OrientMetrics again = now;
    const OrientRisk r = riskFor(*doc, doc->placement(), {}, again, fruit::JobControl::none());
    CHECK(again.tips == s.now.m.tips);
    CHECK(r.places == s.now.m.risk.places);
    CHECK(r.lost_mm3 == Approx(s.now.m.risk.lost_mm3));
}

TEST_CASE("Orient: what the replay finds ranks first", "[gui][orient]") {
    auto risk = [](RiskBand worst, std::size_t places, double lost) {
        OrientRisk r;
        r.checked = true;
        r.worst = worst;
        r.places = places;
        r.lost_mm3 = lost;
        return r;
    };
    const OrientRisk none = risk(RiskBand::None, 0, 0.002), may = risk(RiskBand::May, 1, 0.4),
                     may_less = risk(RiskBand::May, 2, 0.2), likely = risk(RiskBand::Likely, 1, 0.1);
    // A lower band is safer, whatever else.
    CHECK(saferThan(none, may));
    CHECK(saferThan(may, likely));
    CHECK_FALSE(saferThan(likely, may));
    // In the same band, clearly less of the model lost is safer; a little less is not.
    CHECK(saferThan(may_less, may));
    CHECK_FALSE(saferThan(risk(RiskBand::May, 1, 0.35), may));
    CHECK_FALSE(saferThan(risk(RiskBand::None, 0, 0), none));  // nothing listed: as safe as each other
    // Not replayed: neither is safer.
    CHECK_FALSE(saferThan(OrientRisk{}, may));
    CHECK_FALSE(saferThan(may, OrientRisk{}));
    CHECK(riskWords(none) == "nothing likely to be lost");
    CHECK(riskWords(may) == "1 place may not print");
    CHECK(riskWords(may_less) == "2 places may not print");
    CHECK(riskWords(OrientRisk{}).empty());

    // The summary leads with it.
    OrientMetrics now, o;
    now.tips = o.tips = 100, now.top = o.top = 30;
    now.risk = may;
    o.risk = none;
    CHECK_THAT(summarize(o, now), StartsWith("Nothing is likely to be lost in printing"));
    o.risk = may_less;
    CHECK_THAT(summarize(o, now), StartsWith("Less of the model is likely to be lost"));
    now.risk = likely;
    o.risk = may;
    CHECK_THAT(summarize(o, now), StartsWith("What may not print is less likely to go"));
    CHECK_THAT(summarize(now, o), ContainsSubstring("But 1 place may not print"));
}
