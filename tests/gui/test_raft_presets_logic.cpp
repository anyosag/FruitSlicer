// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "ProcessTemp.h"
#include "RaftPresets.h"
#include "RaftPreview.h"

using namespace gui;
namespace fs = std::filesystem;

namespace {

const fruit::RaftSpec& builtIn(const char* name) {
    const fruit::RaftPreset* p = fruit::builtInRaftPreset(name);
    REQUIRE(p != nullptr);
    return p->spec;
}

}  // namespace

TEST_CASE("Raft presets: a copy is named after the preset it came from", "[gui][raft]") {
    RaftPresets p;
    CHECK(copyName("Least resin", p) == "Copy of Least resin");
    p.own.push_back({"Copy of Least resin", "Least resin", builtIn("Least resin")});
    CHECK(copyName("Least resin", p) == "Copy of Least resin 2");
    p.own.push_back({"Copy of Least resin 2", "Least resin", builtIn("Least resin")});
    CHECK(copyName("Least resin", p) == "Copy of Least resin 3");
    CHECK(copyName("Easy release", p) == "Copy of Easy release");
}

TEST_CASE("Raft presets: a name can't be empty, a built-in one's, or another of yours", "[gui][raft]") {
    RaftPresets p;
    p.own.push_back({"Thin parts", "Easy release", builtIn("Easy release")});
    CHECK_FALSE(nameProblem("", p).empty());
    CHECK_FALSE(nameProblem("   ", p).empty());
    CHECK_FALSE(nameProblem("Easy release", p).empty());
    CHECK_FALSE(nameProblem("Thin parts", p).empty());
    CHECK(nameProblem("Thin parts", p, "Thin parts").empty());  // renaming it to itself
    CHECK(nameProblem("Wide lip", p).empty());
}

TEST_CASE("Raft presets: built in or yours, by name, and where a copy came from", "[gui][raft]") {
    RaftPresets p;
    fruit::RaftSpec mine = builtIn("Easy release");
    mine.reach = 2.5;
    p.own.push_back({"Copy of Easy release", "Easy release", mine});
    CHECK(isBuiltInPreset("Pads with frame"));
    CHECK_FALSE(isBuiltInPreset("Copy of Easy release"));
    REQUIRE(presetSpec("Copy of Easy release", p) != nullptr);
    CHECK(*presetSpec("Copy of Easy release", p) == mine);
    CHECK(*presetSpec("Least resin", p) == fruit::leastResinSpec());
    CHECK(presetSpec("Nothing", p) == nullptr);
    CHECK(presetOrigin("Copy of Easy release", p) == "Easy release");
    CHECK(presetOrigin("Strong hold", p) == "Strong hold");
    CHECK(presetOrigin("Nothing", p).empty());
}

TEST_CASE("Raft presets: yours and the one new jobs start with are kept on this computer", "[gui][raft]") {
    const fs::path dir = testfiles::temp() / "fruit_raft_presets_tests";
    fs::remove_all(dir);
    const std::string file = (dir / "rafts.json").string();
    CHECK(loadRaftPresets(file) == RaftPresets{});  // none yet: Least resin starts new jobs
    RaftPresets p;
    fruit::RaftSpec mine = builtIn("Pads with frame");
    mine.frame_gap = 1.5;
    p.own.push_back({"Wide gap", "Pads with frame", mine});
    p.new_jobs = "Wide gap";
    saveRaftPresets(file, p);
    CHECK(loadRaftPresets(file) == p);
    // A file that can't be read, or names a preset that is gone, starts new jobs on Least resin.
    {
        std::ofstream out(file);
        out << "{\"presets\": [], \"new_jobs\": \"Wide gap\"}";
    }
    CHECK(loadRaftPresets(file).new_jobs == fruit::kDefaultRaftPreset);
    {
        std::ofstream out(file);
        out << "not json";
    }
    CHECK(loadRaftPresets(file) == RaftPresets{});
}

TEST_CASE("Raft presets: what a raft is, in a sentence, and one change in words", "[gui][raft]") {
    CHECK(raftSummary(builtIn("Least resin"), 1.0) == "A 1 mm pad under each support, joined by thin bars.");
    CHECK(raftSummary(builtIn("Plain base"), 1.0) == "A 1 mm raft, one base under all the supports, with a sloped edge for the scraper.");
    CHECK(raftSummary(builtIn("Easy release"), 2.0) ==
          "A 2 mm raft, one base under all the supports, with a tapered side, a rounded lip and windows for a screwdriver.");
    CHECK(raftSummary(builtIn("Pads with frame"), 1.0) ==
          "A 1 mm pad under each support, joined by stiff bars, inside a frame with windows for a screwdriver.");
    fruit::RaftSpec s = builtIn("Easy release");
    s.reach = 2.5;
    CHECK(raftChange(builtIn("Easy release"), s) == "Reach past the trunks 1.8 → 2.5 mm");
    s.windows = fruit::RaftWindows::EveryTip;
    CHECK(raftChange(builtIn("Easy release"), s).empty());  // two changes: no one line for them
    fruit::RaftSpec a = builtIn("Least resin");
    a.side_angle = 90;
    CHECK(raftChange(builtIn("Least resin"), a) == "Side angle 112° → 90°");
}

TEST_CASE("Raft preview: the raft from straight above, the same every time, its windows dark notches", "[gui][raft]") {
    fruit::SupportTree t;
    t.raft_top = 2.0;
    for (double x : {4.0, 9.0, 14.0, 19.0, 24.0})
        for (double y : {4.0, 9.0, 14.0}) {
            fruit::Pillar P;
            P.xy = {x, y};
            P.bottom = 3.0;
            P.top = 8;
            P.diameter = 1.0;
            t.pillars.push_back(P);
        }
    fruit::RaftSpec s = builtIn("Easy release");
    s.windows = fruit::RaftWindows::EveryTip;
    const RaftPreviewPicture a = drawRaftPreview(t, fruit::TreeConfig{}, s, 0.4, 200, 120);
    REQUIRE_FALSE(a.picture.empty());
    CHECK(a.picture.w == 200);
    CHECK_FALSE(a.shape.windows.empty());
    CHECK(drawRaftPreview(t, fruit::TreeConfig{}, s, 0.4, 200, 120).picture.rgba == a.picture.rgba);
    s.windows = fruit::RaftWindows::None;
    const RaftPreviewPicture b = drawRaftPreview(t, fruit::TreeConfig{}, s, 0.4, 200, 120);
    // The same raft but for its windows: those pixels only are darker.
    int darker = 0, lighter = 0;
    for (std::size_t i = 0; i < a.picture.rgba.size(); i += 4) {
        const int da = a.picture.rgba[i] + a.picture.rgba[i + 1] + a.picture.rgba[i + 2];
        const int db = b.picture.rgba[i] + b.picture.rgba[i + 1] + b.picture.rgba[i + 2];
        darker += da + 30 < db;
        lighter += db + 30 < da;
    }
    CHECK(darker > 20);
    CHECK(lighter < darker / 4);  // (the windows' cut changes a little shading round them too)
    // Nothing made yet, or a raft too small to see: a picture of the ground alone, never a failure.
    fruit::SupportTree none;
    none.raft_top = 2.0;
    CHECK_FALSE(drawRaftPreview(none, fruit::TreeConfig{}, s, 0.4, 40, 30).picture.empty());
}
