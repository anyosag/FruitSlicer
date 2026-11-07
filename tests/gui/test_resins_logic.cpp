// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Resins: the library, a resin's measured values shaping a job's supports,
// and jobs keeping their resin.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>
#include <string>

#include "Document.h"
#include "JobFile.h"
#include "Resins.h"
#include "Supports.h"
#include "calibration/CalibrationFit.h"
#include "calibration/CalibrationPlates.h"
#include "formats/GooSetup.h"
#include "mesh/StlIO.h"

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using namespace gui;
namespace fs = std::filesystem;
using Source = ResinValue::Source;

namespace {

fs::path dir() {
    const auto d = fs::temp_directory_path() / "fruit_resin_tests";
    fs::create_directories(d);
    return d;
}

// A profile like `fruitslicer fit` writes for a tips plate scored as a resin
// a little weaker than the borrowed values.
nlohmann::json tipsProfile(const std::vector<double>& diameters = {0.2, 0.3, 0.4, 0.6}) {
    fruit::TipsPlateOptions o;
    o.tip_diameters = diameters;
    const fruit::CalibrationPlate plate = fruit::tipsPlate(o, fruit::PrinterProfile::saturn4Ultra16k());
    fruit::PlateResults res;
    res.kind = plate.kind;
    res.settings = plate.settings;
    for (const auto& c : plate.coupons) {
        fruit::CouponResult r;
        r.id = c.id;
        r.params = c.params;
        r.recorded = true;
        const double d = c.params["tip_diameter"].get<double>(), load = c.params["load_per_tip_mm2"].get<double>();
        r.ok = load <= 0.8 * fruit::CapacityModel{}.capacity(d);
        r.scar = r.ok ? (d >= 0.6 ? 4 : d >= 0.4 ? 2 : 1) : 0;
        res.coupons.push_back(r);
    }
    fruit::FitOptions f;
    f.resin = "Test resin";
    return fruit::fitProfile({res}, f);
}

const AdvancedSetting& row(const char* label) {
    for (const AdvancedSetting& s : advancedSettings())
        if (std::string(s.label) == label) return s;
    throw std::runtime_error(std::string("no row ") + label);
}

}  // namespace

TEST_CASE("Resins: kept, read back, named and removed", "[gui][resins]") {
    const auto d = (dir() / "lib").string();
    fs::remove_all(d);
    CHECK(loadResins(d).empty());
    Resin a;
    a.name = "My resin";
    a.id = newResinId({}, a.name);
    a.exposure = 1.5;
    a.bottom_exposure = 27.5;
    CHECK(a.id == "my-resin");
    saveResin(d, a);
    Resin b = a;
    b.name = freeResinName({a}, "My resin");
    b.id = newResinId({a}, b.name);
    b.profile = tipsProfile();
    CHECK(b.name == "My resin (copy)");
    CHECK(b.id == "my-resin-copy");
    saveResin(d, b);
    const auto all = loadResins(d);
    REQUIRE(all.size() == 2);
    CHECK(all[0] == a);
    CHECK(all[1] == b);
    CHECK(newResinId(all, "My resin") == "my-resin-2");
    CHECK(freeResinName(all, "My resin") == "My resin (copy 2)");
    CHECK(newResinId(all, "  ") == "resin");
    removeResin(d, a.id);
    CHECK(loadResins(d).size() == 1);
    std::ofstream(fs::path(d) / "junk.json") << "{ not a resin";
    CHECK(loadResins(d).size() == 1);  // left out
    // By name, ignoring case.
    Resin lower;
    lower.id = "alpha";
    lower.name = "alpha";
    saveResin(d, lower);
    const auto sorted = loadResins(d);
    REQUIRE(sorted.size() == 2);
    CHECK(sorted[0].name == "alpha");
    CHECK(sorted[1].name == "My resin (copy)");
}

TEST_CASE("Resins: the first launch makes My resin from print settings an earlier setup kept", "[gui][resins]") {
    const auto lib = (dir() / "first").string();
    fs::remove_all(lib);
    const auto settings = (dir() / "first_settings.json").string();
    std::ofstream(settings) << R"({"setup_done": true, "print_defaults": {"sample_path": "/kept/a.goo",
        "sample_name": "a.goo", "exposure": 0, "bottom_exposure": 0, "bottom_layers": -1, "transition_layers": -1}})";
    const auto first = firstResins(lib, settings);
    REQUIRE(first.size() == 1);
    CHECK(first[0].name == "My resin");
    CHECK(first[0].sample_path == "/kept/a.goo");
    CHECK(first[0].sample_name == "a.goo");
    CHECK(loadResins(lib).size() == 1);
    // Only once: later launches keep the library as it is.
    Resin other;
    other.id = "other";
    other.name = "Other";
    saveResin(lib, other);
    CHECK(firstResins(lib, settings).size() == 2);
    // No earlier settings: My resin with none.
    const auto empty_lib = (dir() / "first_none").string();
    fs::remove_all(empty_lib);
    const auto none = firstResins(empty_lib, (dir() / "missing.json").string());
    REQUIRE(none.size() == 1);
    CHECK(none[0].sample_path.empty());
}

TEST_CASE("Resins: a fitted profile becomes a resin; other files are refused plainly", "[gui][resins]") {
    const auto file = (dir() / "profile.json").string();
    std::ofstream(file) << tipsProfile().dump(2);
    const Resin r = resinFromProfileFile(file, {});
    CHECK(r.name == "Test resin");
    CHECK(r.id == "test-resin");
    CHECK(r.profile.contains("capacity_table"));
    CHECK(resinState(r) == "Measured: tips");
    CHECK(resinSummary(r, nullptr) == "no print settings · tips measured");
    // A resin file from the Resins page comes back with its print settings.
    const auto lib = (dir() / "own").string();
    fs::remove_all(lib);
    Resin own;
    own.id = "siraya";
    own.name = "Siraya";
    own.exposure = 2;
    own.bottom_exposure = 30;
    own.profile = r.profile;
    saveResin(lib, own);
    const Resin back = resinFromProfileFile((fs::path(lib) / "siraya.json").string(), {own});
    CHECK(back.name == "Siraya (copy)");
    CHECK(back.exposure == 2);
    CHECK(back.profile == own.profile);
    const auto bad = (dir() / "bad.json").string();
    std::ofstream(bad) << R"({"hello": 1})";
    CHECK_THROWS_WITH(resinFromProfileFile(bad, {}), ContainsSubstring("none of the measured values"));
    std::ofstream(bad) << "not json";
    CHECK_THROWS_WITH(resinFromProfileFile(bad, {}), ContainsSubstring("couldn't be read"));
}

TEST_CASE("Resins: the values the page shows, and where each comes from", "[gui][resins]") {
    auto v = resinSupportValues(Resin{});
    REQUIRE(v.size() == 7);
    CHECK(v[0].label == "Usual tip size");
    CHECK(v[0].value == "0.4 mm");
    CHECK(v[0].source == Source::Default);
    CHECK(v[1].value == "11.1 mm²");
    CHECK(v[1].source == Source::Borrowed);
    CHECK(v[2].value == "3.2 → 6.0 mm");
    CHECK(v[3].value == "lean > 45°");
    CHECK(v[4].value == "1 tip ≤ 1.7 mm");
    CHECK(v[5].source == Source::Default);
    CHECK(v[6].value == "2.0 mm");
    Resin measured;
    measured.profile = tipsProfile();
    v = resinSupportValues(measured);
    CHECK(v[1].source == Source::Measured);
    CHECK(v[1].value != "11.1 mm²");
    CHECK(v[2].source == Source::Borrowed);  // the spacing plate isn't scored
    CHECK(v[5].source == Source::Measured);  // contact depth comes from the tips plate
    CHECK(resinLabel("", nlohmann::json()) == "My resin · Defaults");
    CHECK(resinLabel("Siraya", measured.profile) == "Siraya · Measured: tips");
}

TEST_CASE("Resins: print settings as the page lists them", "[gui][resins]") {
    const fruit::PrinterProfile pr = fruit::PrinterProfile::saturn4Ultra16k();
    fruit::GooPrintRequest req;
    req.layer_height = 0.03;
    req.exposure = 1.5;
    req.bottom_exposure = 27.5;
    req.bottom_layers = 6;
    req.transition_layers = 6;
    const fruit::GooHeader h = fruit::buildGooSetup(pr, req).header;
    auto rows = printRows(Resin{}, &h, pr);
    REQUIRE(rows.size() == 8);
    CHECK(rows[0].second == "0.03 mm");
    CHECK(rows[1].second == "6 × 27.5 s");
    CHECK(rows[2].second == "6, from 23.17 to 1.50 s");
    CHECK(rows[3].second == "1.5 s");
    CHECK(rows[4].second == "0 · 0 · 5 s");
    CHECK(rows[5].second == "0.05 mm");
    CHECK(rows[6].second == "255 of 255");
    CHECK(rows[7].second == "1.15 g/ml");
    Resin typed;
    typed.exposure = 2;
    typed.bottom_exposure = 30;
    rows = printRows(typed, nullptr, pr);
    REQUIRE(rows.size() == 5);
    CHECK(rows[0].second == "0.05 mm, FruitSlicer's own");
    CHECK_THAT(rows[1].second, ContainsSubstring("the printer's own count"));
    CHECK(rows[3].second == "2 s");
    CHECK(printRows(Resin{}, nullptr, pr).empty());
}

TEST_CASE("Resins: a job's measured values shape its supports; tip sizes go by diameter", "[gui][resins]") {
    SupportChoices c;
    c.profile = tipsProfile();
    const fruit::SupportConfig cfg = supportConfig(fruit::Placement{}, c);
    CHECK_FALSE(cfg.capacity.table.empty());
    REQUIRE(cfg.tip_classes.size() == 4);
    CHECK(cfg.tip_classes[static_cast<std::size_t>(cfg.default_tip)].diameter == Approx(0.4));
    CHECK(supportConfig(fruit::Placement{}, SupportChoices{}).capacity.table.empty());
    // A profile that measured other sizes: a choice takes the nearest one.
    SupportChoices odd;
    odd.profile = tipsProfile({0.22, 0.5});
    fruit::SupportConfig o = supportConfig(fruit::Placement{}, odd);
    REQUIRE(o.tip_classes.size() == 2);
    CHECK(o.tip_classes[static_cast<std::size_t>(o.default_tip)].diameter == Approx(0.5));  // usual 0.4 mm
    odd.advanced["minima_tip"] = 0;                                                         // 0.2 mm
    o = supportConfig(fruit::Placement{}, odd);
    CHECK(o.tip_classes[static_cast<std::size_t>(o.minima_tip)].diameter == Approx(0.22));
    CHECK(standardTipClass(o, 0) == 0);  // 0.22 mm shows as 0.2
    CHECK(nearestTipClass(o, 3) == 1);   // 0.6 mm is nearest 0.5
    // Rows the resin measured say so, until the job changes them.
    CHECK(measuredByResin(row("How much a tip holds"), c));
    CHECK(measuredByResin(row("Contact depth"), c));
    CHECK_FALSE(measuredByResin(row("Tip reach"), c));
    CHECK_FALSE(measuredByResin(row("How much a tip holds"), SupportChoices{}));
    SupportChoices mine = c;
    mine.advanced["capacity"] = 20;
    CHECK_FALSE(measuredByResin(row("How much a tip holds"), mine));
    int mapped = 0;
    for (const AdvancedSetting& s : advancedSettings()) mapped += std::string(measuredKey(s)).empty() ? 0 : 1;
    CHECK(mapped == 8);  // every row the map names exists
    // The capacity row shows the value in use; setting it scales the measured table.
    const AdvancedField& cap = row("How much a tip holds").fields[0];
    const fruit::SupportConfig sc = supportConfig(fruit::Placement{}, c);
    const fruit::TreeConfig tc = treeConfig(c);
    const double now = cap.get(sc, tc);
    CHECK(now == Approx(sc.capacity.capacity(0.4)));
    CHECK(now < 11.0);  // the resin's, not the borrowed 11.1
    SupportChoices scaled = c;
    scaled.advanced["capacity"] = 2 * now;
    const fruit::SupportConfig sc2 = supportConfig(fruit::Placement{}, scaled);
    CHECK(sc2.capacity.capacity(0.4) == Approx(2 * now));
    CHECK(sc2.capacity.capacity(0.3) == Approx(2 * sc.capacity.capacity(0.3)));
    CHECK(usualTipFor(nlohmann::json()) == 2);
    CHECK(usualTipFor(nlohmann::json{{"default_tip_diameter", 0.3}}) == 1);
    CHECK(usualTipFor(nlohmann::json{{"default_tip_diameter", 0.2}}) == 1);  // the smallest the step offers
}

TEST_CASE("Resins: a job keeps its resin's values, and changing it is a change", "[gui][resins][slow]") {
    const auto model = dir() / "resin_job.stl";
    fruit::writeStl(model.string(), demoModel());
    const auto doc = loadDocument(model.string(), fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    const JobStamp before = doc->stamp();
    doc->supports.choices.resin = "Test resin";
    doc->supports.choices.profile = tipsProfile();
    const auto changes = changesSince(before, doc->stamp());
    REQUIRE(changes.size() == 1);
    CHECK(changes[0] == "changed the resin to Test resin");
    const auto job = (dir() / "resin_job.fruitjob").string();
    saveJob(job, *doc);
    const OpenedJob o = openJob(job, fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    CHECK(o.doc->supports.choices.resin == "Test resin");
    CHECK(o.doc->supports.choices.profile == doc->supports.choices.profile);
    CHECK(o.doc->supports.choices == doc->supports.choices);
}
