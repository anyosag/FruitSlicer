// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Home: what has been checked before a print and what hasn't, the open
// job's next step, recent jobs' cards and the pictures kept with jobs.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "Document.h"
#include "Home.h"
#include "JobFile.h"
#include "JobPicture.h"
#include "Supports.h"
#include "mesh/Primitives.h"
#include "mesh/StlIO.h"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::EndsWith;
using Catch::Matchers::StartsWith;
using namespace gui;
namespace fs = std::filesystem;

namespace {

fs::path dir() {
    const auto d = fs::temp_directory_path() / "fruit_home_tests";
    fs::create_directories(d);
    return d;
}

std::shared_ptr<Document> bracketAt(const fs::path& model) {
    fruit::writeStl(model.string(), demoModel());
    return loadDocument(model.string(), fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
}

// A SatelLite header as the user's own file gives it.
fruit::GooHeader satelliteHeader() {
    fruit::GooHeader h;
    h.layer_height = 0.03f;
    h.print.bottom_layers = 6;
    h.print.bottom_exposure = 27.5f;
    h.print.exposure = 1.5f;
    return h;
}

const SetupCheck& row(const SetupChecks& c, std::size_t i) {
    REQUIRE(c.rows.size() == 4);
    return c.rows[i];
}

// Noon on a given local day.
std::time_t day(int y, int m, int d, int h = 12, int min = 0) {
    std::tm t{};
    t.tm_year = y - 1900, t.tm_mon = m - 1, t.tm_mday = d, t.tm_hour = h, t.tm_min = min, t.tm_isdst = -1;
    return std::mktime(&t);
}

}  // namespace

TEST_CASE("Home: on a first launch only the printer counts as checked", "[gui][home]") {
    const auto printer = fruit::PrinterProfile::saturn4Ultra16k();
    const SetupChecks c = setupChecks(printer, PrinterSetup{}, Resin{}, nullptr);
    using S = SetupCheck::State;
    CHECK(row(c, 0).state == S::Ok);
    CHECK(row(c, 0).title == "Printer: " + printer.name);
    CHECK(row(c, 0).detail == "211.68 × 118.37 × 220 mm build volume");
    CHECK(row(c, 1).state == S::Review);
    CHECK(row(c, 1).title == "No print settings yet");
    CHECK(row(c, 1).go == SetupCheck::Go::Resins);
    CHECK(row(c, 2).state == S::Review);
    CHECK(row(c, 2).title == "Image orientation not checked");
    CHECK(row(c, 2).action == "Check now");
    CHECK(row(c, 2).go == SetupCheck::Go::OrientationCheck);
    // Starting values are not a fault, and not a check either.
    CHECK(row(c, 3).state == S::Note);
    CHECK(row(c, 3).title == "Supports use starting values");
    CHECK_THAT(row(c, 3).detail, ContainsSubstring("nothing is measured on your resin"));
    CHECK(row(c, 3).go == SetupCheck::Go::Calibrate);
    CHECK(c.checked == 1);
    REQUIRE(c.plates.size() == 3);
    CHECK(c.plates[0].label == "Tips");
    CHECK(c.plates[1].label == "Spacing");
    CHECK(c.plates[2].label == "Islands");
    for (const auto& p : c.plates) CHECK_FALSE(p.measured);
}

TEST_CASE("Home: the print settings say where they come from and what they are", "[gui][home]") {
    const auto printer = fruit::PrinterProfile::saturn4Ultra16k();
    const fruit::GooHeader h = satelliteHeader();
    Resin r;
    r.sample_path = "/resins/my resin.goo";
    r.sample_name = "DishRack3.goo";

    SECTION("a SatelLite file, as it is") {
        const SetupChecks all = setupChecks(printer, PrinterSetup{}, r, &h);
        const SetupCheck& s = row(all, 1);
        CHECK(s.state == SetupCheck::State::Ok);
        CHECK(s.title == "Print settings from your SatelLite file");
        CHECK(s.detail == "0.03 mm layers · 6 bottom layers at 27.5 s · 1.5 s normal");
        CHECK(s.action == "View");
    }
    SECTION("with values of the user's own over it") {
        r.print.exposure = 2;
        const SetupChecks all = setupChecks(printer, PrinterSetup{}, r, &h);
        const SetupCheck& s = row(all, 1);
        CHECK(s.title == "Print settings from your SatelLite file, with your changes");
        CHECK_THAT(s.detail, EndsWith("2 s normal"));
    }
    SECTION("a file that no longer reads counts as none") {
        const SetupChecks all = setupChecks(printer, PrinterSetup{}, r, nullptr);
        const SetupCheck& s = row(all, 1);
        CHECK(s.state == SetupCheck::State::Review);
        CHECK(s.title == "Print settings file doesn't read");
        CHECK_THAT(s.detail, StartsWith("DishRack3.goo has moved"));
    }
    SECTION("typed in") {
        Resin t;
        t.print.exposure = 2.5;
        t.print.bottom_exposure = 30;
        const SetupChecks all = setupChecks(printer, PrinterSetup{}, t, nullptr);
        const SetupCheck& s = row(all, 1);
        CHECK(s.state == SetupCheck::State::Ok);
        CHECK(s.title == "Print settings typed in");
        CHECK_THAT(s.detail, ContainsSubstring("at 30 s · 2.5 s normal"));
    }
}

TEST_CASE("Home: the orientation check and measured plates count once done", "[gui][home]") {
    const auto printer = fruit::PrinterProfile::saturn4Ultra16k();
    const fruit::GooHeader h = satelliteHeader();
    Resin r;
    r.sample_path = "/resins/my resin.goo";
    PrinterSetup ps;
    ps.orientation_confirmed = true;
    ps.checked_with = "f.goo";

    r.profile = nlohmann::json{{"capacity_table", nlohmann::json::array({1, 2})}};
    SetupChecks c = setupChecks(printer, ps, r, &h);
    CHECK(row(c, 2).state == SetupCheck::State::Ok);
    CHECK(row(c, 2).title == "Image orientation checked");
    CHECK_THAT(row(c, 2).detail, StartsWith("Checked with f.goo"));
    CHECK(row(c, 2).action.empty());
    CHECK(row(c, 3).state == SetupCheck::State::Note);
    CHECK(row(c, 3).title == "Some support values measured");
    CHECK_THAT(row(c, 3).detail, StartsWith("Tips measured on your resin; spacing and islands still use"));
    CHECK(c.checked == 3);
    CHECK(c.plates[0].measured);
    CHECK_FALSE(c.plates[1].measured);

    r.profile["coverage_curve"] = nlohmann::json::array({1});
    r.profile["island_one_tip_length"] = 2.0;
    c = setupChecks(printer, ps, r, &h);
    CHECK(row(c, 3).state == SetupCheck::State::Ok);
    CHECK(row(c, 3).title == "Support values measured on your resin");
    CHECK(c.checked == 4);
}

TEST_CASE("Home: Continue takes the open job to what it needs next", "[gui][home][slow]") {
    const auto doc = bracketAt(dir() / "bracket.stl");
    ContinueTo c = continueTo(*doc);
    CHECK(c.label == "Continue: make supports");
    CHECK(c.step == Step::Supports);
    CHECK(c.make_supports);
    std::vector<StepChip> chips = stepChips(*doc);
    REQUIRE(chips.size() == 5);
    CHECK(chips[0].state == StepChip::State::Done);   // placed, and it fits
    CHECK(chips[1].state == StepChip::State::Later);  // Orient: never decided, never needed
    CHECK(chips[2].state == StepChip::State::Next);
    CHECK(chips[2].detail == " · next");
    CHECK(chips[3].state == StepChip::State::Later);
    CHECK_THAT(openJobLine(*doc), StartsWith("Not saved yet · 1 part · "));
    CHECK_THAT(openJobLine(*doc), EndsWith("My resin · Defaults"));
    CHECK(openJobSupports(*doc).empty());

    auto r = std::make_shared<SupportResult>(
        generate(*doc, doc->supports.choices, doc->visibleInPrinter(), fruit::JobControl::none()));
    doc->supports.result = r;
    doc->supports.result_revision = doc->revision();
    doc->supports.result_settings = doc->supportSettings();
    c = continueTo(*doc);
    CHECK(c.label == "Continue: cut the layers");
    CHECK(c.step == Step::Layers);
    CHECK_FALSE(c.make_supports);
    chips = stepChips(*doc);
    CHECK(chips[2].state == StepChip::State::Done);
    CHECK(chips[2].detail == " · " + std::to_string(r->tips) + " tips");
    CHECK(chips[3].state == StepChip::State::Next);
    CHECK_THAT(openJobSupports(*doc), StartsWith("Supports are made: " + std::to_string(r->tips) + " tips, "));

    // Moved since: the supports are for how it was.
    fruit::Placement p = doc->placement();
    p.rotation_deg = {0, 0, 25};
    doc->edit(p);
    CHECK(continueTo(*doc).label == "Continue: remake supports");
    CHECK_THAT(openJobSupports(*doc), StartsWith("Supports are out of date"));
}

TEST_CASE("Home: a recent job's card says what it held when saved", "[gui][home]") {
    RecentJob j{"/jobs/rack.fruitjob", "DishRack3", "layers cut", "2026-11-12 15:32"};
    CHECK(jobState(j) == "Layers cut");
    CHECK(jobFacts(j).empty());  // remembered before the counts were kept
    j.parts = 2;
    j.on_plate = true;
    j.tips = 25;
    j.support_ml = 0.24;
    CHECK(jobFacts(j) == "2 parts on the plate · 25 tips · 0.2 ml supports");
    j.parts = 1;
    j.on_plate = false;
    j.tips = 0;
    CHECK(jobFacts(j) == "1 part · no tips needed");
    j.tips = -1;
    CHECK(jobFacts(j) == "1 part");
}

TEST_CASE("Home: when a job was saved, in words", "[gui][home]") {
    const std::time_t now = day(2026, 11, 13, 9, 30);
    CHECK(whenWords("2026-11-13 08:05", now) == "Today, 08:05");
    CHECK(whenWords("2026-11-12 15:32", now) == "Yesterday, 15:32");
    CHECK(whenWords("2026-11-11 18:40", now) == "11 Nov, 18:40");
    CHECK(whenWords("2025-02-03 09:10", now) == "3 Feb 2025, 09:10");
    CHECK(whenWords("2026-01-01 00:00", day(2026, 1, 1, 0, 10)) == "Today, 00:00");
    CHECK(whenWords("2025-12-31 23:59", day(2026, 1, 1, 0, 10)) == "Yesterday, 23:59");
    CHECK(whenWords("today", now) == "Today");  // as scripted runs keep it
    CHECK(whenWords("", now).empty());
}

TEST_CASE("Home: recent jobs keep their counts; older lists read without them", "[gui][home][jobs]") {
    const auto file = (dir() / "data" / "recent.json").string();
    fs::remove_all(dir() / "data");
    RecentJob j{"/jobs/cup.fruitjob", "eyecupv3", "supports ready", "2026-11-12 13:05"};
    j.parts = 1;
    j.tips = 371;
    j.support_ml = 12.5;
    rememberJob(file, j);
    const auto back = loadRecentJobs(file);
    REQUIRE(back.size() == 1);
    CHECK(back[0].parts == 1);
    CHECK_FALSE(back[0].on_plate);
    CHECK(back[0].tips == 371);
    CHECK(back[0].support_ml == 12.5);

    {
        std::ofstream out(file);
        out << R"({"jobs": [{"path": "/jobs/old.fruitjob", "name": "old", "state": "placed", "when": "x"}]})";
    }
    const auto old = loadRecentJobs(file);
    REQUIRE(old.size() == 1);
    CHECK(old[0].parts == 0);
    CHECK(old[0].tips == -1);
    CHECK(old[0].support_ml < 0);
}

TEST_CASE("Home: a job's picture shows it on the 3D view's ground, and is kept in its file", "[gui][home][jobs]") {
    const fruit::TriangleMesh box = fruit::makeBox({0, 0, 0}, {20, 10, 8});
    const JobPicture p = drawJob({{&box, 0xAEB6C4}}, 80, 60);
    REQUIRE(p.w == 80);
    REQUIRE(p.h == 60);
    REQUIRE(p.rgba.size() == 80u * 60u * 4u);
    auto px = [&](int x, int y) { return &p.rgba[4 * (static_cast<std::size_t>(y) * 80 + x)]; };
    // The corners are the ground; the middle is the box, lit.
    CHECK(px(0, 0)[0] == 0x0E);
    CHECK(px(0, 0)[1] == 0x10);
    CHECK(px(0, 0)[2] == 0x14);
    CHECK(px(79, 59)[0] == 0x0E);
    CHECK(px(40, 30)[0] > 0x40);
    CHECK(px(40, 30)[3] == 255);
    // The same job always draws the same.
    CHECK(drawJob({{&box, 0xAEB6C4}}, 80, 60).rgba == p.rgba);
    // Nothing to draw: the ground alone.
    const JobPicture none = drawJob({}, 8, 6);
    CHECK(none.rgba[0] == 0x0E);

    const JobPicture again = pictureFrom(pictureBytes(p));
    CHECK(again.w == 80);
    CHECK(again.h == 60);
    CHECK(again.rgba == p.rgba);
    CHECK(pictureFrom({'F', 'S', 'P', '1', 80, 0, 60, 0, 1, 2}).empty());  // short
    CHECK(pictureFrom({}).empty());

    const auto doc = bracketAt(dir() / "pictured.stl");
    const auto job = (dir() / "pictured.fruitjob").string();
    saveJob(job, *doc);
    const JobPicture kept = loadJobPicture(job);
    CHECK(kept.w == 400);
    CHECK(kept.h == 300);
    CHECK(kept.rgba == jobPicture(*doc).rgba);
    CHECK(loadJobPicture((dir() / "no such job.fruitjob").string()).empty());
}
