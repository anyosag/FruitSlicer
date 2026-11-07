// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Settings, first-run setup and About: what is kept, what a SatelLite file
// says, and the licences built in.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>
#include <string>

#include "Calibration.h"
#include "Licences.h"
#include "Preferences.h"
#include "Print.h"
#include "Resins.h"
#include "Setup.h"
#include "formats/GooExport.h"
#include "formats/GooFile.h"
#include "formats/GooSetup.h"
#include "mesh/Primitives.h"

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using namespace gui;
namespace fs = std::filesystem;

namespace {

fs::path dir() {
    const auto d = fs::temp_directory_path() / "fruit_settings_tests";
    fs::create_directories(d);
    return d;
}

// A print file like one SatelLite saves for the 16K: 0.03 mm layers,
// 6 bottom layers of 27.5 s, a 6-layer ramp, then 1.5 s.
std::string satelliteLike(const std::string& name) {
    fruit::GooPrintRequest r;
    r.layer_height = 0.03;
    r.exposure = 1.5;
    r.bottom_exposure = 27.5;
    r.bottom_layers = 6;
    r.transition_layers = 6;
    const fruit::GooPrintSetup s = fruit::buildGooSetup(fruit::PrinterProfile::saturn4Ultra16k(), r);
    const auto p = (dir() / name).string();
    fruit::exportGoo(fruit::makeBox({-2, -2, 0}, {2, 2, 0.3}), s.header, s.grid, fruit::GooExportOptions{}, p);
    return p;
}

}  // namespace

TEST_CASE("Preferences: kept and read back; missing or broken files read as the defaults", "[gui][settings]") {
    const auto file = (dir() / "prefs" / "preferences.json").string();
    fs::remove_all(dir() / "prefs");
    CHECK(loadPreferences(file) == Preferences{});
    Preferences p;
    p.smooth_view = false;
    p.cores = 3;
    p.goo_to_drive = false;
    p.goo_folder = "/Users/me/Prints";
    savePreferences(file, p);
    CHECK(loadPreferences(file) == p);
    std::ofstream(file) << "{ not json";
    CHECK(loadPreferences(file) == Preferences{});
}

TEST_CASE("Preferences: the core counts offered", "[gui][settings]") {
    CHECK(coreChoices(10) == std::vector<unsigned>{10, 9, 5, 1});
    CHECK(coreChoices(4) == std::vector<unsigned>{4, 3, 2, 1});
    CHECK(coreChoices(3) == std::vector<unsigned>{3, 2, 1});
    CHECK(coreChoices(2) == std::vector<unsigned>{2, 1});
    CHECK(coreChoices(1) == std::vector<unsigned>{1});
}

TEST_CASE("Printer setup: setup and the resin for new jobs are kept; older files read as not set up",
          "[gui][settings]") {
    const auto file = (dir() / "setup" / "settings.json").string();
    fs::remove_all(dir() / "setup");
    fs::create_directories(dir() / "setup");
    std::ofstream(file) << R"({"orientation": {"confirmed": true, "flip_x": false, "flip_y": true}})";
    PrinterSetup s = loadPrinterSetup(file);
    CHECK(s.orientation_confirmed);
    CHECK_FALSE(s.setup_done);
    CHECK(s.resin.empty());
    s.setup_done = true;
    s.resin = "siraya-fast";
    savePrinterSetup(file, s);
    CHECK(loadPrinterSetup(file) == s);
}

TEST_CASE("Setup: a SatelLite file's settings in words", "[gui][settings]") {
    const fruit::GooHeader h = fruit::readGooHeader(satelliteLike("facts.goo"));
    const auto f = sampleFacts(h);
    REQUIRE(f.size() == 8);
    CHECK(f[0].label == "Layer height");
    CHECK(f[0].value == "0.03 mm");
    CHECK(f[1].value == "6 × 27.5 s");
    CHECK(f[2].value == "6 layers");
    CHECK(f[2].note == "23.17 → 18.83 → 14.50 → 10.17 → 5.83 → 1.50 s");  // SatelLite's own ramp
    CHECK(f[3].value == "1.5 s");
    CHECK(f[4].label == "Wait after retract");
    CHECK(f[4].value == "5 s");
    CHECK(f[5].value == "0.05 mm");
    CHECK(f[6].value == "Off");
    CHECK(f[7].value == "Y");  // the 16K's rows are stored flipped
    // A long ramp is shortened in the middle.
    fruit::GooHeader longer = h;
    longer.print.transition_layers = 10;
    const auto g = sampleFacts(longer);
    CHECK_THAT(g[2].note, ContainsSubstring(" → … → "));
    CHECK(g[2].value == "10 layers");
}

TEST_CASE("Setup: a SatelLite file's settings are kept for new jobs; another printer's file is refused",
          "[gui][settings]") {
    const auto sample = satelliteLike("my sample.goo");
    const auto keep = dir() / "kept";
    fs::remove_all(keep);
    const std::string copy = keepSampleHeader(sample, keep.string());
    CHECK(fs::path(copy).filename() == "my sample.goo");
    CHECK(fs::file_size(copy) == fruit::kGooHeaderSize);
    // Jobs in a resin start from the copy, even once the file it came from is gone.
    fs::remove(sample);
    Resin s;
    s.sample_path = copy;
    s.sample_name = "my sample.goo";
    PrintSettings p = printSettingsFor(s);
    REQUIRE(p.sample);
    CHECK(p.sample->print.exposure == Approx(1.5));
    CHECK(p.sample_path == copy);
    CHECK(printSummary(s) == "From my sample.goo");
    // A copy that has gone leaves the job to ask; typed values pass through.
    s.sample_path = (keep / "gone.goo").string();
    s.exposure = 2;
    s.bottom_exposure = 30;
    p = printSettingsFor(s);
    CHECK_FALSE(p.sample);
    CHECK(p.exposure == 2);
    CHECK(p.bottom_exposure == 30);
    s.sample_path.clear();
    CHECK(printSummary(s) == "Typed in: 2 s, bottom 30 s");
    CHECK(printSummary(Resin{}) == "None yet: each job asks");

    // A file made for another printer: its exposures are for another light.
    fruit::GooHeader other = fruit::readGooHeader(copy);
    other.res_x = 11520;
    other.res_y = 5120;
    other.printer_name = "ELEGOO Saturn 4 Ultra";
    const auto bytes = fruit::gooHeaderBytes(other);
    const auto wrong = (dir() / "other printer.goo").string();
    std::ofstream(wrong, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                 static_cast<std::streamsize>(bytes.size()));
    CHECK_THROWS_WITH(loadSample(wrong), ContainsSubstring("made for ELEGOO Saturn 4 Ultra (11520 × 5120 pixels)"));
    CHECK_THROWS_WITH(keepSampleHeader(wrong, keep.string()), ContainsSubstring("Saturn 4 Ultra 16K (15120 × 6230)"));
    CHECK_FALSE(fs::exists(keep / "other printer.goo"));
    CHECK_THROWS(keepSampleHeader((dir() / "missing.goo").string(), keep.string()));
}

TEST_CASE("About: every library built in comes with its full licence text", "[gui][settings]") {
    const auto& all = licences();
    REQUIRE(all.size() == 11);
    CHECK(std::string(all[0].name) == "FruitSlicer");
    CHECK_THAT(std::string(all[0].text), ContainsSubstring("GNU AFFERO GENERAL PUBLIC LICENSE"));
    const std::pair<const char*, const char*> expect[] = {
        {"Clipper2", "Boost Software License"},
        {"nlohmann/json", "MIT License"},
        {"cpp-httplib", "MIT License"},
        {"miniz", "Permission is hereby granted"},
        {"pugixml", "MIT License"},
        {"SDL 3", "This software is provided 'as-is'"},
        {"SDL 3: YUV to RGB", "Redistributions in binary form must reproduce"},
        {"Dear ImGui", "MIT License"},
        {"glad", "Apache License"},
        {"Inter", "SIL Open Font License"},
    };
    for (const auto& [name, phrase] : expect) {
        const Licence* l = nullptr;
        for (const Licence& x : all)
            if (name == std::string(x.name)) l = &x;
        INFO(name);
        REQUIRE(l);
        CHECK(l->text.size() > 500);
        CHECK_THAT(std::string(l->text), ContainsSubstring(phrase));
    }
}
