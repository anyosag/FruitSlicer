// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Slicing settings and the Layer height page: where each value comes from,
// the light each height needs, and what a height changes in the print.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <filesystem>
#include <string>

#include "Calibration.h"
#include "Document.h"
#include "Preferences.h"
#include "Print.h"
#include "ProcessTemp.h"
#include "Resins.h"
#include "SlicingSettings.h"
#include "Supports.h"
#include "formats/GooExport.h"
#include "formats/GooSetup.h"
#include "mesh/Primitives.h"
#include "mesh/StlIO.h"

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using namespace gui;

namespace {

std::filesystem::path tmp(const char* name) { return testfiles::temp() / name; }

// A 10 × 8 × 3 mm box, lifted 5 mm onto supports: its top is at 8 mm.
std::shared_ptr<Document> box() {
    const auto path = tmp("slicing_box.stl");
    fruit::writeStl(path.string(), fruit::makeBox({0, 0, 0}, {10, 8, 3}));
    return loadDocument(path.string(), fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
}

// A SatelLite-like file: 0.03 mm layers, 1.5 s, 6 × 27.5 s bottom layers, 6 transition layers.
std::shared_ptr<const fruit::GooHeader> sample003(std::string* path_out = nullptr) {
    fruit::GooPrintRequest r;
    r.layer_height = 0.03;
    r.exposure = 1.5;
    r.bottom_exposure = 27.5;
    r.bottom_layers = 6;
    r.transition_layers = 6;
    const fruit::GooPrintSetup s = fruit::buildGooSetup(fruit::PrinterProfile::saturn4Ultra16k(), r);
    const auto path = tmp("slicing_sample_003.goo").string();
    fruit::exportGoo(fruit::makeBox({-2, -2, 0}, {2, 2, 1}), s.header, s.grid, fruit::GooExportOptions{}, path);
    if (path_out) *path_out = path;
    return loadSample(path);
}

void withSample(Document& doc) {
    std::string path;
    doc.print.settings.sample = sample003(&path);
    doc.print.settings.sample_path = path;
}

Resin myResin() {
    Resin r;
    r.id = "my-resin";
    r.name = "My resin";
    r.tested_light = {{0.05, 2.5, "published profile"}, {0.10, 2.0, "your print"}};
    return r;
}

const SettingRow* row(const SliceSettingsView& v, const std::string& group, const std::string& label) {
    for (const SettingGroup& g : v.groups)
        if (g.title == group)
            for (const SettingRow& r : g.rows)
                if (r.label == label) return &r;
    return nullptr;
}

}  // namespace

TEST_CASE("Light: tested heights, and the curve through the thinnest and thickest", "[gui][slicing]") {
    const auto doc = box();
    withSample(*doc);
    const Resin r = myResin();
    const std::vector<TestedLight> t = testedLight(doc->print.settings, &r);
    REQUIRE(t.size() == 3);
    CHECK(t[0].layer_height == Approx(0.03));
    CHECK(t[0].exposure == Approx(1.5));
    CHECK(t[0].note == "your SatelLite file");
    CHECK(t[2].layer_height == Approx(0.10));

    const LightCurve c = lightCurve(t);
    REQUIRE(c.ok);
    CHECK(c.dp == Approx(0.07 / std::log(2.0 / 1.5)));  // about 0.243 mm
    CHECK(c.at(0.03) == Approx(1.5));
    CHECK(c.at(0.10) == Approx(2.0));

    const LightAt at05 = lightAt(t, 0.05);
    CHECK(at05.kind == LightAt::Kind::Tested);
    CHECK(at05.exposure == Approx(2.5));  // tested wins over the curve's 1.63 s
    CHECK(at05.note == "published profile");
    const LightAt at08 = lightAt(t, 0.08);
    CHECK(at08.kind == LightAt::Kind::Modeled);
    CHECK(at08.exposure == Approx(1.84));
    CHECK(lightAt(t, 0.01).exposure == Approx(1.38));
    CHECK(lightAt(t, 0.02).exposure == Approx(1.44));

    // One tested height: no curve, so the others are unknown.
    const std::vector<TestedLight> one = testedLight(doc->print.settings, nullptr);
    REQUIRE(one.size() == 1);
    CHECK_FALSE(lightCurve(one).ok);
    CHECK(lightAt(one, 0.05).kind == LightAt::Kind::Unknown);
    // The resin's own point at the file's height doesn't replace the file's.
    Resin dup = r;
    dup.tested_light.push_back({0.03, 9.0, "typo"});
    const std::vector<TestedLight> t2 = testedLight(doc->print.settings, &dup);
    CHECK(t2.size() == 3);
    CHECK(lightAt(t2, 0.03).exposure == Approx(1.5));
}

TEST_CASE("Light: a thicker point needing less light makes no curve", "[gui][slicing]") {
    const std::vector<TestedLight> t = {{0.03, 2.0, "a"}, {0.10, 1.5, "b"}};
    CHECK_FALSE(lightCurve(t).ok);
    CHECK(lightAt(t, 0.05).kind == LightAt::Kind::Unknown);
    CHECK(lightAt(t, 0.10).kind == LightAt::Kind::Tested);
}

TEST_CASE("Layer height: a box's flat faces, and the time at each height", "[gui][slicing]") {
    const auto doc = box();
    withSample(*doc);
    const Resin r = myResin();
    const auto tested = testedLight(doc->print.settings, &r);
    const PrintShape shape = printShape(doc->placed(), doc->placed().bbox().max.z);
    CHECK(shape.top == Approx(8.0));
    CHECK(shape.steepest == 0);  // walls and flat faces only
    CHECK(shape.walls == Approx(2 * (10 + 8) * 3).margin(0.01));
    CHECK(shape.flat == Approx(2 * 10 * 8).margin(0.01));
    REQUIRE(shape.flat_levels.size() == 2);
    CHECK(shape.flat_levels[0] == Approx(5.0));
    CHECK(shape.flat_levels[1] == Approx(8.0));

    // 0.03 mm: 5 and 8 mm fall between layers (by 10 µm each); 0.05 mm: on layers.
    const HeightChoice c3 = heightChoice(*doc, shape, tested, 0.03);
    CHECK(c3.layers == 267);
    CHECK(c3.flat_off == 2);
    CHECK(c3.flat_off_um == Approx(10).margin(0.5));
    CHECK(c3.step_um == 0);
    CHECK(c3.light.kind == LightAt::Kind::Tested);
    const HeightChoice c5 = heightChoice(*doc, shape, tested, 0.05);
    CHECK(c5.layers == 160);
    CHECK(c5.flat_off == 0);
    CHECK(c5.light.exposure == Approx(2.5));
    // The time is the print file's own estimate with that height's light.
    fruit::GooPrintRequest q = printRequest(withLayerHeight(doc->print.settings, 0.05, c5.light), 0.05);
    const fruit::GooHeader h = fruit::printHeader(doc->printer, q);
    CHECK(h.print.exposure == Approx(2.5));
    CHECK(c5.seconds == Approx(fruit::estimatePrintSeconds(h, 160, fruit::GooExportOptions{})));
    CHECK(c5.seconds < c3.seconds);
    CHECK(c5.long_layers == 12);
    CHECK(c5.long_mm == Approx(0.60));
}

TEST_CASE("Layer height: a raft thinner than the long-exposure layers at that height", "[gui][slicing]") {
    const auto doc = box();
    withSample(*doc);
    auto sup = std::make_shared<SupportResult>();
    SupportResult::RaftMade raft;
    raft.top = 1.0;
    sup->rafts.push_back(raft);
    raft.top = 2.0;  // the thinnest raft is the one that counts
    sup->rafts.push_back(raft);
    doc->supports.result = sup;
    const PrintShape shape = printShape(doc->placed(), 8.0);
    const auto tested = testedLight(doc->print.settings, nullptr);
    CHECK_FALSE(heightChoice(*doc, shape, tested, 0.08).raft_thin);  // 12 × 0.08 = 0.96 mm
    const HeightChoice c = heightChoice(*doc, shape, tested, 0.10);
    CHECK(c.raft == Approx(1.0));
    CHECK(c.long_mm == Approx(1.2));
    CHECK(c.raft_thin);
}

TEST_CASE("Layer height: on the plate, the long exposures hold more of the part at thicker layers", "[gui][slicing]") {
    const auto doc = box();
    withSample(*doc);
    fruit::Placement p = doc->placement();
    p.on_plate = true;
    doc->place(p);
    const PrintShape shape = printShape(doc->placed(), doc->placed().bbox().max.z);
    const auto tested = testedLight(doc->print.settings, nullptr);
    // 6 bottom layers less the 0.10 mm height correction's: 3 of 0.03 mm, 1 of 0.10 mm.
    CHECK(heightChoice(*doc, shape, tested, 0.03).held_mm == Approx(0.09));
    const HeightChoice c = heightChoice(*doc, shape, tested, 0.10);
    CHECK(c.held_mm == Approx(0.50));
    CHECK(c.layers == 31);  // 3 mm and the correction's one layer
    CHECK(c.raft == 0);
    CHECK_FALSE(c.raft_thin);
}

TEST_CASE("Layer height: using one sets the job's own values, or clears them back to the file's", "[gui][slicing]") {
    const auto doc = box();
    withSample(*doc);
    const Resin r = myResin();
    const auto tested = testedLight(doc->print.settings, &r);
    const PrintSettings s5 = withLayerHeight(doc->print.settings, 0.05, lightAt(tested, 0.05));
    CHECK(s5.layer_height == Approx(0.05));
    CHECK(s5.exposure == Approx(2.5));
    const PrintSettings back = withLayerHeight(s5, 0.03, lightAt(tested, 0.03));
    CHECK(back.layer_height == 0);
    CHECK(back.exposure == 0);
    // Without a known light, the exposure stays as it is.
    const PrintSettings unknown = withLayerHeight(doc->print.settings, 0.08, LightAt{});
    CHECK(unknown.layer_height == Approx(0.08));
    CHECK(unknown.exposure == 0);
}

TEST_CASE("Slicing settings: every group, each value's source, and what slicing waits for", "[gui][slicing]") {
    const auto doc = box();
    withSample(*doc);
    doc->print.settings.wait_after_retract = 3.0;
    PrinterSetup setup;
    const Resin r = myResin();
    const SliceSettingsView v = sliceSettingsView(*doc, setup, &r);
    REQUIRE(v.groups.size() == 4);
    CHECK(v.groups[0].title == "PRINTER");
    CHECK(v.groups[1].title == "RESIN");
    CHECK(v.groups[2].title == "LAYERS AND LIGHT");
    CHECK(v.groups[3].title == "MODEL, SUPPORTS AND RAFT");

    const SettingRow* lh = row(v, "LAYERS AND LIGHT", "Layer height");
    REQUIRE(lh);
    CHECK(lh->value == "0.03 mm");
    CHECK(lh->from == ValueFrom::File);
    CHECK_THAT(lh->help, ContainsSubstring("Saving cuts them again at 0.03 mm"));
    const SettingRow* exp = row(v, "LAYERS AND LIGHT", "Normal exposure");
    REQUIRE(exp);
    CHECK(exp->value == "1.5 s");
    CHECK_THAT(exp->help, ContainsSubstring("Tested with your resin"));
    const SettingRow* waits = row(v, "LAYERS AND LIGHT", "Waits");
    REQUIRE(waits);
    CHECK(waits->from == ValueFrom::Yours);
    CHECK(waits->value == "0 s before lift · 3 s after retract");
    CHECK(row(v, "PRINTER", "Image orientation")->from == ValueFrom::NotChecked);
    CHECK(row(v, "RESIN", "Print settings from")->from == ValueFrom::File);
    CHECK(row(v, "MODEL, SUPPORTS AND RAFT", "Supports")->from == ValueFrom::Missing);

    // No supports: slicing waits for them; the exposures are there.
    CHECK(v.fix == "Make supports");
    CHECK_FALSE(v.waits_for.empty());
    CHECK(v.missing.empty());
    bool not_flip_checked = false;
    for (const SliceCheck& c : v.not_checked) not_flip_checked |= c.text.find("flips") != std::string::npos;
    CHECK(not_flip_checked);

    // Confirmed orientation moves to Checked.
    setup.orientation_confirmed = true;
    const SliceSettingsView v2 = sliceSettingsView(*doc, setup, &r);
    CHECK(row(v2, "PRINTER", "Image orientation")->from == ValueFrom::Checked);
    CHECK(row(v2, "PRINTER", "Image orientation")->value == "No flip");
}

TEST_CASE("Slicing settings: no exposures stop saving, not slicing", "[gui][slicing]") {
    const auto doc = box();
    const SliceSettingsView v = sliceSettingsView(*doc, PrinterSetup{}, nullptr);
    CHECK_FALSE(v.missing.empty());
    CHECK(row(v, "RESIN", "Print settings from")->from == ValueFrom::Missing);
    CHECK(row(v, "LAYERS AND LIGHT", "Normal exposure")->from == ValueFrom::Missing);
    CHECK(row(v, "LAYERS AND LIGHT", "Normal exposure")->value == "No exposure yet");
}

TEST_CASE("Slicing settings: a value changed since the layers were cut shows what it was", "[gui][slicing]") {
    const auto doc = box();
    withSample(*doc);
    const PrintSettings cut = doc->print.settings;
    doc->print.settings.wait_after_retract = 4.0;
    const SliceSettingsView v = sliceSettingsView(*doc, PrinterSetup{}, nullptr, &cut);
    const SettingRow* waits = row(v, "LAYERS AND LIGHT", "Waits");
    REQUIRE(waits);
    CHECK(waits->was == "0 s before lift · 5 s after retract");
    CHECK(waits->value == "0 s before lift · 4 s after retract");
    CHECK(row(v, "LAYERS AND LIGHT", "Normal exposure")->was.empty());
}

TEST_CASE("Slicing settings: the Job panel's line and the time words", "[gui][slicing]") {
    const auto doc = box();
    withSample(*doc);
    CHECK(slicingLine(*doc) == "My resin · 0.03 mm layers · 1.5 s");
    CHECK(hoursMinutes(37 * 60 + 10) == "37 min");
    CHECK(hoursMinutes(62 * 60) == "1 h 02 min");
    CHECK(hoursMinutes(4 * 3600 + 15 * 60) == "4 h 15 min");
}

TEST_CASE("Resins keep their tested light; preferences keep the before-slicing switch", "[gui][slicing]") {
    const auto dir = tmp("slicing_resins").string();
    std::filesystem::remove_all(dir);
    saveResin(dir, myResin());
    const std::vector<Resin> back = loadResins(dir);
    REQUIRE(back.size() == 1);
    CHECK(back[0].tested_light == myResin().tested_light);

    const auto file = tmp("slicing_prefs.json").string();
    Preferences p;
    CHECK(p.settings_before_slice);
    p.settings_before_slice = false;
    savePreferences(file, p);
    CHECK_FALSE(loadPreferences(file).settings_before_slice);
}
