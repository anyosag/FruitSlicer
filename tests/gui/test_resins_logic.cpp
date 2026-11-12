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
#include "Layers.h"
#include "Resins.h"
#include "Supports.h"
#include "calibration/CalibrationFit.h"
#include "calibration/CalibrationPlates.h"
#include "formats/GooExport.h"
#include "formats/GooSetup.h"
#include "mesh/Primitives.h"
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
    a.print.exposure = 1.5;
    a.print.bottom_exposure = 27.5;
    // Every value of your own is kept with it.
    a.print.layer_height = 0.04;
    a.print.wait_before_lift = 0.5;
    a.print.wait_after_retract = 3;
    a.print.light_power = 200;
    a.print.density = 1.1;
    a.profile = {{kResinOwnKey, {{"critical_angle", 50.0}}}};
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
    own.print.exposure = 2;
    own.print.bottom_exposure = 30;
    own.profile = r.profile;
    saveResin(lib, own);
    const Resin back = resinFromProfileFile((fs::path(lib) / "siraya.json").string(), {own});
    CHECK(back.name == "Siraya (copy)");
    CHECK(back.print.exposure == 2);
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
    CHECK(v[4].value == "1 tip ≤ 1.7 · 2 ≤ 6.5 mm");
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
    using PS = PrintRow::Source;
    const fruit::PrinterProfile pr = fruit::PrinterProfile::saturn4Ultra16k();
    fruit::GooPrintRequest req;
    req.layer_height = 0.03;
    req.exposure = 1.5;
    req.bottom_exposure = 27.5;
    req.bottom_layers = 6;
    req.transition_layers = 6;
    const fruit::GooHeader h = fruit::buildGooSetup(pr, req).header;
    Resin from_file;
    from_file.sample_path = "/kept/sample.goo";
    auto rows = printRows(from_file, &h, pr);
    REQUIRE(rows.size() == 9);
    CHECK(rows[0].value == "0.03 mm");
    CHECK(rows[1].value == "6 × 27.5 s");
    CHECK(rows[2].key == "height_correction");  // next to the bottom layers
    CHECK(rows[2].value == "0.1 mm");
    CHECK(rows[2].after.empty());
    CHECK_THAT(rows[2].note, ContainsSubstring("typical value, not measured on your printer"));
    CHECK(rows[3].value == "6, from 23.17 to 1.50 s");
    CHECK(rows[3].after == "from 23.17 to 1.50 s");
    CHECK(rows[4].value == "1.5 s");
    CHECK(rows[5].value == "0 · 5 s");
    CHECK(rows[6].key == "bottom_waits");  // the bottom layers' own, as the file has them
    CHECK(rows[6].value == "0 · 5 s");
    CHECK(rows[6].fields.size() == 2);
    CHECK(rows[6].note.empty());
    CHECK(rows[7].value == "255 of 255");
    CHECK(rows[8].value == "1.15 g/ml");
    for (std::size_t i : {0, 1, 3, 4, 5, 6, 7}) CHECK(rows[i].source == PS::File);
    CHECK(rows[2].source == PS::Default);  // no print file has a height correction
    CHECK(rows[8].source == PS::Default);  // or a density
    REQUIRE(rows[1].fields.size() == 2);
    CHECK(rows[1].fields[0].key == "bottom_layers");
    CHECK(rows[1].fields[1].value == Approx(27.5));
    REQUIRE(rows[2].fields.size() == 1);
    CHECK(rows[2].fields[0].key == "height_correction");
    CHECK(rows[2].fields[0].value == Approx(0.10));
    CHECK(rows[2].fields[0].hi == Approx(0.5));
    CHECK(rows[5].fields.size() == 2);
    // Typed in, without a file: the printer's own counts and FruitSlicer's defaults.
    Resin typed;
    typed.print.exposure = 2;
    typed.print.bottom_exposure = 30;
    rows = printRows(typed, nullptr, pr);
    REQUIRE(rows.size() == 9);
    CHECK(rows[0].value == "0.05 mm");
    CHECK(rows[0].source == PS::Default);
    CHECK(rows[1].value == "4 × 30 s");
    CHECK(rows[1].source == PS::Yours);
    CHECK(rows[1].base.empty());  // no file, so no exposure to go back to
    CHECK(rows[4].value == "2 s");
    CHECK(rows[5].source == PS::Default);
    // Nothing yet: every row there to type into, the exposures not set.
    rows = printRows(Resin{}, nullptr, pr);
    REQUIRE(rows.size() == 9);
    CHECK(rows[4].value == "not set");
    CHECK(rows[4].fields[0].value == 0);
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

TEST_CASE("Resins: print values you edit reach the print plan, its estimate and the file", "[gui][resins]") {
    using PS = PrintRow::Source;
    const fruit::PrinterProfile pr = fruit::PrinterProfile::saturn4Ultra16k();
    // A resin from a SatelLite-like file: 0.03 mm, 1.5 s, 6 × 27.5 s bottom, waits 0 · 5 s.
    fruit::GooPrintRequest req;
    req.layer_height = 0.03;
    req.exposure = 1.5;
    req.bottom_exposure = 27.5;
    req.bottom_layers = 6;
    req.transition_layers = 6;
    const fruit::GooPrintSetup ss = fruit::buildGooSetup(pr, req);
    const auto goo = (dir() / "edit_sample.goo").string();
    fruit::exportGoo(fruit::makeBox({-2, -2, 0}, {2, 2, 1}), ss.header, ss.grid, fruit::GooExportOptions{}, goo);
    const auto sample = loadSample(goo);
    Resin r;
    r.id = "edited";
    r.name = "Edited";
    r.sample_path = goo;
    setPrintValue(r, sample.get(), pr, "exposure", 1.8);
    setPrintValue(r, sample.get(), pr, "bottom_layers", 4);
    setPrintValue(r, sample.get(), pr, "wait_after_retract", 3);
    setPrintValue(r, sample.get(), pr, "light_power", 200);
    setPrintValue(r, sample.get(), pr, "density", 1.1);
    setPrintValue(r, sample.get(), pr, "height_correction", 0.10);  // the typical value: not an edit
    CHECK(r.print.height_correction < 0);
    setPrintValue(r, sample.get(), pr, "height_correction", 0.15);  // measured on the printer
    CHECK(r.print.height_correction == Approx(0.15));
    setPrintValue(r, sample.get(), pr, "light_power", 300);  // clamped to 255: the file's, so not kept
    CHECK(r.print.light_power == -1);
    setPrintValue(r, sample.get(), pr, "light_power", 200);
    setPrintValue(r, sample.get(), pr, "wait_before_lift", 0);  // the file's own value: not an edit
    CHECK(r.print.wait_before_lift < 0);
    CHECK_THROWS(setPrintValue(r, sample.get(), pr, "speed", 1));

    // The page: yours, with the file's value to go back to, and what isn't checked.
    auto rows = printRows(r, sample.get(), pr);
    CHECK(rows[0].source == PS::File);
    CHECK(rows[1].value == "4 × 27.5 s");
    CHECK(rows[1].source == PS::Yours);
    CHECK(rows[1].base == "6 × 27.5 s");
    CHECK(rows[2].value == "0.15 mm");
    CHECK(rows[2].source == PS::Yours);
    CHECK(rows[2].base == "0.1 mm");
    CHECK(rows[2].note.empty());  // yours: nothing to say about it
    CHECK(rows[4].value == "1.8 s");
    CHECK(rows[4].source == PS::Yours);
    CHECK(rows[4].base == "1.5 s");
    CHECK(rows[4].note.empty());
    CHECK(rows[5].value == "0 · 3 s");
    CHECK(rows[5].source == PS::Yours);
    // The bottom layers wait as the others do, so the 3 s is yours there too, not the file's.
    CHECK(rows[6].value == "0 · 3 s");
    CHECK(rows[6].source == PS::Yours);
    CHECK(rows[6].base.empty());  // the way back is the waits row's
    CHECK(rows[6].note.empty());
    CHECK_THAT(rows[7].note, ContainsSubstring("set at 255 and haven't been checked at 200"));
    CHECK(rows[8].value == "1.1 g/ml");
    CHECK(rows[8].base == "1.15 g/ml");

    // Kept with the resin.
    const auto lib = (dir() / "edited_lib").string();
    fs::remove_all(lib);
    saveResin(lib, r);
    const auto back = loadResins(lib);
    REQUIRE(back.size() == 1);
    CHECK(back[0] == r);

    // A job on it plans with them: the file, the estimate and the weight.
    const auto doc = loadDocument(
        [] {
            const auto stl = (dir() / "edited_box.stl").string();
            fruit::writeStl(stl, fruit::makeBox({0, 0, 0}, {10, 8, 3}));
            return stl;
        }(),
        pr, fruit::JobControl::none());
    const LayerResult lr = sliceLayers(*doc, {}, 0.0, fruit::JobControl::none());
    const PrintSettings s = printSettingsFor(r);
    const PrintPlan p = planPrint(*doc, lr, 1.0, s);
    REQUIRE(p.ready);
    const fruit::GooPrintSettings& hp = p.setup.header.print;
    CHECK(hp.exposure == Approx(1.8));
    CHECK(hp.bottom_exposure == Approx(27.5));  // the file's
    CHECK(hp.bottom_layers == 4);
    CHECK(hp.transition_layers == 6);
    CHECK(hp.wait_after_retract == Approx(3));
    CHECK(hp.bottom_wait_after_retract == Approx(3));
    CHECK(hp.lift_distance == Approx(sample->print.lift_distance));  // the file's, not a setting
    CHECK(hp.bottom_retract_distance == Approx(sample->print.bottom_retract_distance));
    CHECK(hp.pwm == 200);
    CHECK(hp.bottom_pwm == 200);
    CHECK(p.layer_height == Approx(0.03));
    CHECK(p.resin_g == Approx(p.resin_ml * 1.1));
    // The estimate: 2 s less waiting on every layer, 0.3 s more light on most.
    PrintSettings file_only;
    file_only.sample = sample;
    file_only.sample_path = goo;
    const PrintPlan q = planPrint(*doc, lr, 1.0, file_only);
    REQUIRE(q.ready);
    REQUIRE(q.layers == p.layers);
    CHECK(p.print_time_s < q.print_time_s - 1.5 * static_cast<double>(p.layers));
    CHECK(p.print_time_s == Approx(fruit::estimatePrintSeconds(p.setup.header, static_cast<uint32_t>(p.layers), {})));

    // A job keeps them in its file, and an edit is a change to it.
    const JobStamp before = doc->stamp();
    doc->print.settings = s;
    CHECK(changesSince(before, doc->stamp()) == std::vector<std::string>{"changed the print settings"});
    const auto job = (dir() / "edited.fruitjob").string();
    saveJob(job, *doc);
    const OpenedJob o = openJob(job, pr, fruit::JobControl::none());
    CHECK(static_cast<const PrintEdits&>(o.doc->print.settings) == r.print);

    // An edited layer height: the file is cut at it, and the page says the exposures weren't checked there.
    setPrintValue(r, sample.get(), pr, "layer_height", 0.05);
    rows = printRows(r, sample.get(), pr);
    CHECK(rows[0].value == "0.05 mm");
    CHECK(rows[0].source == PS::Yours);
    CHECK_THAT(rows[0].note, ContainsSubstring("set for 0.03 mm layers and haven't been checked at 0.05 mm"));
    const PrintPlan p5 = planPrint(*doc, lr, 1.0, printSettingsFor(r));
    REQUIRE(p5.ready);
    CHECK(p5.layer_height == Approx(0.05));
    CHECK_FALSE(p5.reslice);  // the job's own 0.05 mm layers
    const auto out = (dir() / "edited.goo").string();
    const SaveResult saved = savePrint(*doc, lr, {}, p5, out, false, fruit::JobControl::none());
    CHECK(saved.good);
    const fruit::GooHeader written = fruit::readGooHeader(out);
    CHECK(written.layer_height == Approx(0.05));
    CHECK(written.print.exposure == Approx(1.8));
    CHECK(written.print.wait_after_retract == Approx(3));
    CHECK(written.print.pwm == 200);
    CHECK(written.weight == Approx(written.volume / 1000.0 * 1.1).epsilon(0.001));

    // Back to the file's, a row at a time.
    resetPrintRow(r.print, "exposure");
    resetPrintRow(r.print, "bottom");
    resetPrintRow(r.print, "height_correction");
    CHECK(r.print.exposure == 0);
    CHECK(r.print.bottom_layers == -1);
    CHECK(r.print.height_correction < 0);
    rows = printRows(r, sample.get(), pr);
    CHECK(rows[4].source == PS::File);
    CHECK(rows[4].value == "1.5 s");
    CHECK(rows[1].source == PS::File);
    CHECK(rows[2].source == PS::Default);  // FruitSlicer's typical value again
    CHECK(rows[2].value == "0.1 mm");
    CHECK(rows[6].source == PS::Yours);  // the waits are still yours, and so the bottom layers'
    resetPrintRow(r.print, "waits");
    rows = printRows(r, sample.get(), pr);
    CHECK(rows[5].source == PS::File);
    CHECK(rows[6].source == PS::File);  // the file's again, with the other layers'
}

TEST_CASE("Resins: support values you set are the resin's, and a job starts with them", "[gui][resins]") {
    Resin r;
    r.profile = tipsProfile();
    const nlohmann::json measured = r.profile;
    r.profile = withResinValue(r.profile, "capacity", 14);
    r.profile = withResinValue(r.profile, "critical_angle", 50);
    r.profile = withResinValue(r.profile, kUsualTipKey, 1);  // 0.3 mm
    r.profile = withResinValue(r.profile, "snap_zone", 3);
    CHECK_THROWS(withResinValue(r.profile, "tip_reach", 1));
    auto v = resinSupportValues(r);
    REQUIRE(v.size() == 7);
    CHECK(v[0].value == "0.3 mm");
    CHECK(v[0].source == Source::Yours);
    CHECK(v[1].value == "14.0 mm²");
    CHECK(v[1].source == Source::Yours);
    CHECK(v[3].value == "lean > 50°");
    CHECK(v[3].source == Source::Yours);
    CHECK(v[5].source == Source::Measured);  // contact depth: still the tips plate's
    CHECK(v[6].value == "3.0 mm");
    CHECK(resinState(r) == "Measured: tips");  // your values don't count as measured

    // A job on it starts with them, and they read as yours there too.
    SupportChoices c = resinChoices(r.profile);
    const fruit::SupportConfig cfg = supportConfig(fruit::Placement{}, c);
    CHECK(cfg.capacity.capacity(0.4) == Approx(14));
    CHECK(cfg.critical_angle_deg == Approx(50));
    CHECK(cfg.tip_classes[static_cast<std::size_t>(cfg.default_tip)].diameter == Approx(0.3));
    CHECK(treeConfig(c).flex_zone == Approx(3));
    CHECK(resinChanged(row("How much a tip holds"), c.profile));
    CHECK_FALSE(advancedChanged(row("How much a tip holds"), c));  // the job's Reset all comes back here
    CHECK_FALSE(measuredByResin(row("How much a tip holds"), c));
    CHECK_FALSE(advancedChanged(row("Usual tip size"), c));
    // The quick sliders work from them; a job's own value wins.
    c.hold = 1;
    CHECK(supportConfig(fruit::Placement{}, c).capacity.capacity(0.4) == Approx(14 * quickCapacityScale(1)));
    c.hold = kHoldDefault;
    c.advanced["capacity"] = 9;
    CHECK(supportConfig(fruit::Placement{}, c).capacity.capacity(0.4) == Approx(9));

    // Back to the measured value, a row at a time.
    r.profile = withoutResinRow(r.profile, row("How much a tip holds"));
    v = resinSupportValues(r);
    CHECK(v[1].source == Source::Measured);
    r.profile = withoutResinRow(r.profile, row("Usual tip size"));
    r.profile = withoutResinRow(r.profile, row("Faces that need tips"));
    r.profile = withoutResinRow(r.profile, row("Snap-off zone"));
    CHECK(r.profile == measured);
    // Without plates: only your values, gone again once you take them back;
    // FruitSlicer's own value is not kept as yours.
    nlohmann::json own = withResinValue(nullptr, "critical_angle", 50);
    CHECK(profileState(own) == "Defaults");
    CHECK(withoutResinRow(own, row("Faces that need tips")).is_null());
    CHECK(withResinValue(nullptr, "critical_angle", 45).is_null());
}

TEST_CASE("Resins: the bottom layers can wait differently from the others", "[gui][resins]") {
    using PS = PrintRow::Source;
    const fruit::PrinterProfile pr = fruit::PrinterProfile::saturn4Ultra16k();
    Resin r;
    r.id = "bottom-waits";
    r.name = "Bottom waits";
    r.print.exposure = 2.5;
    r.print.bottom_exposure = 30;
    // Nothing set: the bottom layers wait as the others do.
    auto rows = printRows(r, nullptr, pr);
    REQUIRE(rows.size() == 9);
    CHECK(rows[6].key == "bottom_waits");
    CHECK(rows[6].label == "Bottom-layer waits (before lift, after retract)");
    CHECK(rows[6].value == rows[5].value);
    CHECK(rows[6].source == PS::Default);
    CHECK(rows[6].note.empty());
    // A wait typed for every layer is the bottom layers' too: the row says it is yours, and the way back
    // is the waits row's.
    setPrintValue(r, nullptr, pr, "wait_after_retract", 3);
    rows = printRows(r, nullptr, pr);
    CHECK(rows[5].value == "0 · 3 s");
    CHECK(rows[5].source == PS::Yours);
    CHECK(rows[6].value == "0 · 3 s");
    CHECK(rows[6].source == PS::Yours);
    CHECK(rows[6].base.empty());
    CHECK(rows[6].note.empty());
    // Their own: 1 s after the light (before the lift) and 10 s before it (after the retract).
    setPrintValue(r, nullptr, pr, "bottom_wait_before_lift", 1);
    setPrintValue(r, nullptr, pr, "bottom_wait_after_retract", 10);
    CHECK(r.print.bottom_wait_before_lift == Approx(1));
    CHECK(r.print.bottom_wait_after_retract == Approx(10));
    rows = printRows(r, nullptr, pr);
    CHECK(rows[5].value == "0 · 3 s");  // the other layers are as they were
    CHECK(rows[6].value == "1 · 10 s");
    CHECK(rows[6].source == PS::Yours);
    CHECK(rows[6].base == "0 · 3 s");
    CHECK_THAT(rows[6].note, ContainsSubstring("hasn't been tested"));

    // In the print file: the header's bottom-layer waits, and so each layer's own.
    const PrintSettings s = printSettingsFor(r);
    const fruit::GooHeader h = fruit::buildGooSetup(pr, printRequest(s, 0.05)).header;
    CHECK(h.print.bottom_wait_before_lift == Approx(1));
    CHECK(h.print.bottom_wait_after_lift == Approx(0));
    CHECK(h.print.bottom_wait_after_retract == Approx(10));
    CHECK(h.print.wait_before_lift == Approx(0));
    CHECK(h.print.wait_after_retract == Approx(3));
    const uint32_t bottoms = h.print.bottom_layers;
    REQUIRE(bottoms > 0);
    CHECK(fruit::layerDefFor(h, 0).wait_before_lift == Approx(1));
    CHECK(fruit::layerDefFor(h, bottoms - 1).wait_after_retract == Approx(10));
    CHECK(fruit::layerDefFor(h, bottoms).wait_after_retract == Approx(3));  // transition and normal layers
    CHECK(fruit::layerDefFor(h, bottoms + 20).wait_before_lift == Approx(0));
    // And in the time: 8 s more on each bottom layer.
    Resin plain = r;
    resetPrintRow(plain.print, "bottom_waits");
    CHECK(plain.print.bottom_wait_before_lift < 0);
    CHECK(plain.print.bottom_wait_after_retract < 0);
    const fruit::GooHeader hp = fruit::buildGooSetup(pr, printRequest(printSettingsFor(plain), 0.05)).header;
    const fruit::GooExportOptions o;
    CHECK(fruit::estimatePrintSeconds(h, 100, o) == Approx(fruit::estimatePrintSeconds(hp, 100, o) + 8.0 * bottoms));

    // Typing the other layers' value again is not an edit.
    setPrintValue(r, nullptr, pr, "bottom_wait_after_retract", 3);
    CHECK(r.print.bottom_wait_after_retract < 0);
    setPrintValue(r, nullptr, pr, "bottom_wait_after_retract", 10);

    // Kept with the resin.
    const auto lib = (dir() / "bottom_waits_lib").string();
    fs::remove_all(lib);
    saveResin(lib, r);
    const auto back = loadResins(lib);
    REQUIRE(back.size() == 1);
    CHECK(back[0] == r);
}

TEST_CASE("Resins: a resin file saved with adaptive layers, a light depth and a lift still loads, those keys ignored",
          "[gui][resins]") {
    Resin r;
    r.id = "from-before";
    r.name = "From before";
    r.print.exposure = 2.5;
    r.print.bottom_exposure = 30;
    r.print.bottom_wait_before_lift = 1;
    const auto lib = (dir() / "old_keys_lib").string();
    fs::remove_all(lib);
    saveResin(lib, r);
    // What an older version wrote: its two extra keys in every print object.
    for (auto& f : fs::directory_iterator(lib)) {
        nlohmann::json j;
        {
            std::ifstream in(f.path());
            in >> j;
        }
        for (auto& [k, v] : j.items())
            if (v.is_object() && v.contains("exposure")) {
                v["adaptive"] = 4;
                v["light_depth"] = 0.2;
                v["lift"] = 0.08;
            }
        std::ofstream(f.path()) << j.dump();
    }
    const auto back = loadResins(lib);
    REQUIRE(back.size() == 1);
    CHECK(back[0] == r);
    CHECK(back[0].print.exposure == Approx(2.5));
    CHECK(back[0].print.bottom_wait_before_lift == Approx(1));
    // The lift in the file made no difference to the print file's own.
    const fruit::GooPrintRequest plain = printRequest(printSettingsFor(back[0]), 0.05);
    CHECK(plain.lift_distance < 0);
}

TEST_CASE("Resins: there is no setting for the lift and retract; the print file keeps the sample's, or the default",
          "[gui][resins]") {
    const fruit::PrinterProfile pr = fruit::PrinterProfile::saturn4Ultra16k();
    const auto doc = loadDocument(
        [] {
            const auto stl = (dir() / "lift_box.stl").string();
            fruit::writeStl(stl, fruit::makeBox({0, 0, 0}, {10, 8, 3}));
            return stl;
        }(),
        pr, fruit::JobControl::none());
    const LayerResult lr = sliceLayers(*doc, {}, 0.0, fruit::JobControl::none());
    // No row for it, and no key to set.
    Resin r;
    r.id = "lift";
    r.name = "Lift";
    for (const PrintRow& row : printRows(r, nullptr, pr)) CHECK(row.key != "lift");
    CHECK_THROWS(setPrintValue(r, nullptr, pr, "lift", 0.08));
    // A resin from a sample file whose lift and retract are 0.07 mm: its plan has those, with a typed exposure over it.
    fruit::GooPrintRequest req;
    req.layer_height = 0.03;
    req.exposure = 1.5;
    req.bottom_exposure = 27.5;
    req.bottom_layers = 6;
    req.transition_layers = 6;
    req.lift_distance = 0.07;
    const fruit::GooPrintSetup ss = fruit::buildGooSetup(pr, req);
    const auto goo = (dir() / "lift_sample.goo").string();
    fruit::exportGoo(fruit::makeBox({-2, -2, 0}, {2, 2, 1}), ss.header, ss.grid, fruit::GooExportOptions{}, goo);
    const auto sample = loadSample(goo);
    REQUIRE(sample);
    CHECK(sample->print.lift_distance == Approx(0.07));
    r.sample_path = goo;
    setPrintValue(r, sample.get(), pr, "exposure", 1.8);
    const PrintPlan p = planPrint(*doc, lr, 1.0, printSettingsFor(r));
    REQUIRE(p.ready);
    CHECK(p.setup.header.print.lift_distance == Approx(0.07));
    CHECK(p.setup.header.print.bottom_lift_distance == Approx(0.07));
    CHECK(p.setup.header.print.retract_distance == Approx(0.07));
    CHECK(p.setup.header.print.bottom_retract_distance == Approx(0.07));
    // A typed resin, with no sample: FruitSlicer's own 0.05 mm.
    Resin t;
    t.id = "typed";
    t.name = "Typed";
    t.print.exposure = 2.0;
    t.print.bottom_exposure = 30;
    const PrintPlan q = planPrint(*doc, lr, 1.0, printSettingsFor(t));
    REQUIRE(q.ready);
    CHECK(q.setup.header.print.lift_distance == Approx(0.05));
    CHECK(q.setup.header.print.bottom_lift_distance == Approx(0.05));
    CHECK(q.setup.header.print.retract_distance == Approx(0.05));
    CHECK(q.setup.header.print.bottom_retract_distance == Approx(0.05));
}
