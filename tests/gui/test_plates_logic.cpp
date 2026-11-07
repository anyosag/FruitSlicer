// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Resin test plates: what each plate and coupon is said to test, where the
// coupons sit, and the scores FruitSlicer keeps for a saved plate.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include "Plates.h"
#include "Resins.h"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using namespace gui;
namespace fs = std::filesystem;

namespace {

const fruit::PrinterProfile& printer() {
    static const fruit::PrinterProfile p = fruit::PrinterProfile::saturn4Ultra16k();
    return p;
}

// (A plain name: GCC takes a reference returned for a temporary string as dangling.)
const fruit::CalibrationPlate& plateOf(const char* kind) {
    static std::map<std::string, fruit::CalibrationPlate> made;
    auto it = made.find(kind);
    if (it == made.end()) it = made.emplace(kind, makePlate(kind, printer())).first;
    return it->second;
}

// A fresh, empty folder for one test.
fs::path freshDir(const std::string& name) {
    const auto d = fs::temp_directory_path() / "fruit_plate_tests" / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

PlateFiles keptAt(const fs::path& dir, const std::string& kind) {
    PlateFiles f;
    f.manifest = (dir / (kind + ".json")).string();
    f.results = (dir / (kind + ".results.csv")).string();
    return f;
}

std::string firstLine(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}

const fruit::PlateCoupon& couponWhere(const fruit::CalibrationPlate& p, const char* key, const char* value) {
    for (const auto& c : p.coupons)
        if (c.params.value(key, std::string()) == value) return c;
    throw std::runtime_error("no such coupon");
}

}  // namespace

TEST_CASE("each plate is described as it is made", "[plates]") {
    // The Calibrate page writes these without making the plates.
    CHECK(plateSubtitle(plateOf("tips")) == "48 coupons · 6 mm tall");
    CHECK(plateSubtitle(plateOf("spacing")) == "37 coupons · 19 mm tall");
    CHECK(plateSubtitle(plateOf("islands")) == "36 coupons · 7 mm tall");

    const auto tips = plateTests(plateOf("tips"));
    REQUIRE(tips.size() == 4);
    CHECK(tips[0] == "Tip sizes 0.2, 0.3, 0.4 and 0.6 mm");
    CHECK(tips[1] == "Contact depths 0.1 and 0.2 mm");
    CHECK(tips[2] == "Loads of 5, 10 and 20 mm² per tip");
    CHECK(tips[3] == "Each combination twice: 24 × 2 = 48");

    const auto spacing = plateTests(plateOf("spacing"));
    REQUIRE(spacing.size() == 4);
    CHECK(spacing[0] == "Plates tilted 0, 15, 30, 45 and 60° from flat");
    CHECK(spacing[1] == "On tips 1.5, 2, 3, 4, 5 and 6 mm apart");
    CHECK_THAT(spacing[2], StartsWith("Wedges with one side leaning 30, 40, 45, 50, 55, 60 and 65°"));
    CHECK(spacing[3] == "Each once: 30 plates and 7 wedges");

    const auto islands = plateTests(plateOf("islands"));
    REQUIRE(islands.size() == 4);
    CHECK(islands[0] == "Spots 0.3, 0.5, 1, 2, 3 and 5 mm across, starting in mid-air");
    CHECK(islands[1] == "On 1, 2 and 3 tips");
    CHECK(islands[2] == "Tips 0.3 mm, smaller where a spot is too small for them");
    CHECK(islands[3] == "Each combination twice: 18 × 2 = 36");

    for (const char* k : kPlateKinds) CHECK(isPlateKind(k));
    CHECK_FALSE(isPlateKind("ab"));
    CHECK_THROWS(makePlate("ab", printer()));
}

TEST_CASE("each coupon is put in words from its own settings", "[plates]") {
    const auto& t = plateOf("tips").coupons.front().params;
    const auto tf = couponFacts("tips", t);
    REQUIRE(tf.size() == 4);
    char size[32];
    std::snprintf(size, sizeof size, "%g mm", t["tip_diameter"].get<double>());
    CHECK(tf[0].label == "Tip size");
    CHECK(tf[0].value == size);
    std::snprintf(size, sizeof size, "%g mm²", t["load_per_tip_mm2"].get<double>());
    CHECK(tf[2].label == "Load per tip");
    CHECK(tf[2].value == size);
    std::ostringstream want;
    want << t["tips"].get<int>() << " tips";
    CHECK_THAT(tf[3].value, ContainsSubstring(want.str()));
    CHECK_THAT(couponMeaning("tips", t), StartsWith("A flat disc hangs from 3 tips"));

    // The wedge's base sits on tips; only its leaning side has none.
    const auto& w = couponWhere(plateOf("spacing"), "kind", "wedge").params;
    const auto wf = couponFacts("spacing", w);
    REQUIRE(wf.size() == 2);
    CHECK(wf[1].label == "Tips under that side");
    CHECK(wf[1].value == "None");
    CHECK_THAT(couponMeaning("spacing", w), ContainsSubstring("faces leaning this far or less need no tips"));

    const auto& pl = couponWhere(plateOf("spacing"), "kind", "plate").params;
    const auto pf = couponFacts("spacing", pl);
    REQUIRE(pf.size() == 2);
    CHECK(pf[0].label == "Plate, tilted");
    CHECK_THAT(couponMeaning("spacing", pl), ContainsSubstring("tilted at least this much"));

    nlohmann::json one = {{"island_diameter", 0.5}, {"tips", 1}, {"tip_diameter", 0.3}};
    CHECK(couponMeaning("islands", one) ==
          "A column 0.5 mm across that starts in mid-air, on 1 tip. If it prints, spots that size need no more tips "
          "than that.");
    one["tips"] = 2;
    CHECK_THAT(couponMeaning("islands", one), ContainsSubstring("on 2 tips."));
    CHECK(couponFacts("islands", one).at(0).value == "0.5 mm");
}

TEST_CASE("the map's layout is the plate's, from it or from its manifest", "[plates]") {
    for (const char* kind : kPlateKinds) {
        INFO(kind);
        const fruit::CalibrationPlate& p = plateOf(kind);
        const PlateLayout a = layoutOf(p), b = layoutOf(p.manifest());
        CHECK(a.rows == p.rows);
        CHECK(a.cols == p.cols);
        CHECK(a.cell_mm == p.cell);
        CHECK(b.rows == a.rows);
        CHECK(b.cols == a.cols);
        CHECK(b.cell_mm == a.cell_mm);
        REQUIRE(a.cells.size() == p.coupons.size());
        REQUIRE(b.cells.size() == a.cells.size());
        std::set<std::pair<int, int>> places;
        std::set<int> ids;
        for (std::size_t i = 0; i < a.cells.size(); ++i) {
            CHECK(a.cells[i].id == b.cells[i].id);
            CHECK(a.cells[i].row == b.cells[i].row);
            CHECK(a.cells[i].col == b.cells[i].col);
            places.insert({a.cells[i].row, a.cells[i].col});
            ids.insert(a.cells[i].id);
        }
        CHECK(places.size() == a.cells.size());  // one coupon per cell
        CHECK(ids.size() == a.cells.size());
        CHECK_FALSE(places.count({0, 0}));        // the marker's
    }
    nlohmann::json bad = plateOf("tips").manifest();
    bad["coupons"][0]["row"] = 99;
    CHECK_THROWS_WITH(layoutOf(bad), ContainsSubstring("outside the plate's grid"));
}

TEST_CASE("a kept plate's scores stay, blind, through another save", "[plates]") {
    const fs::path d = freshDir("keep");
    const fruit::CalibrationPlate& p = plateOf("tips");
    PlateFiles f = keptAt(d, "tips");
    keepPlate(p, f);
    CHECK(f.coupons == 48);
    CHECK(f.scored == 0);
    // No settings in the sheet, so they can't sway whoever scores.
    CHECK(firstLine(f.results) == "id,row,col,result,scar,notes");

    PlateScores s = loadScores(f);
    CHECK(s.kind == "tips");
    REQUIRE(s.coupons.size() == 48);
    CHECK(s.scored() == 0);
    CHECK(s.layout.cells.size() == 48);
    s.coupons[0].recorded = true;
    s.coupons[0].ok = true;
    s.coupons[0].scar = 2;
    s.coupons[1].recorded = true;
    s.coupons[1].partial = true;
    s.coupons[1].scar = 3;
    s.coupons[2].recorded = true;  // fell off
    s.coupons[2].notes = "gone, film has a disc";
    saveScores(f, s);
    CHECK(f.scored == 3);

    // Saved again (the same plate, maybe to another file): the scores stay.
    PlateFiles again = keptAt(d, "tips");
    again.file = "/somewhere/else/tips.stl";
    keepPlate(p, again);
    CHECK(again.scored == 3);
    const PlateScores back = loadScores(again);
    REQUIRE(back.find(s.coupons[0].id));
    CHECK(back.find(s.coupons[0].id)->ok);
    CHECK(back.find(s.coupons[0].id)->scar == 2);
    CHECK(back.find(s.coupons[1].id)->partial);
    CHECK_FALSE(back.find(s.coupons[1].id)->ok);
    CHECK(back.find(s.coupons[1].id)->scar == 3);
    CHECK(back.find(s.coupons[2].id)->recorded);
    CHECK_FALSE(back.find(s.coupons[2].id)->ok);
    CHECK_FALSE(back.find(s.coupons[2].id)->partial);
    CHECK(back.find(s.coupons[2].id)->notes == "gone, film has a disc");
    CHECK_FALSE(back.find(s.coupons[3].id)->recorded);
    CHECK_FALSE(fs::exists(d / "tips.earlier.results.csv"));
}

TEST_CASE("keeping another plate sets the earlier scores aside, never over them", "[plates]") {
    const fs::path d = freshDir("aside");
    PlateFiles f = keptAt(d, "tips");
    keepPlate(plateOf("tips"), f);
    // Nothing scored yet: replaced without a copy.
    keepPlate(plateOf("islands"), f);
    CHECK_FALSE(fs::exists(d / "tips.earlier.json"));
    CHECK(loadScores(f).kind == "islands");

    PlateScores s = loadScores(f);
    s.coupons[0].recorded = s.coupons[0].ok = true;
    s.coupons[5].recorded = true;
    saveScores(f, s);
    keepPlate(plateOf("tips"), f);
    REQUIRE(fs::exists(d / "tips.earlier.json"));
    REQUIRE(fs::exists(d / "tips.earlier.results.csv"));
    CHECK(loadScores(f).kind == "tips");
    CHECK(loadScores(f).scored() == 0);
    PlateFiles earlier = keptAt(d, "tips.earlier");
    CHECK(loadScores(earlier).kind == "islands");
    CHECK(loadScores(earlier).scored() == 2);
    // The earlier pair is a sheet and manifest like any other.
    CHECK(readScoreSheet(earlier.results).scored() == 2);

    s = loadScores(f);
    s.coupons[1].recorded = true;
    saveScores(f, s);
    keepPlate(plateOf("spacing"), f);
    CHECK(fs::exists(d / "tips.earlier-2.results.csv"));
    CHECK(loadScores(keptAt(d, "tips.earlier-2")).kind == "tips");
    CHECK(loadScores(earlier).kind == "islands");  // untouched

    // A sheet that can't be read is someone's work too: it's kept.
    std::ofstream(f.results) << "id,result\nnot,a,sheet\n";
    keepPlate(plateOf("tips"), f);
    CHECK(fs::exists(d / "tips.earlier-3.results.csv"));
    CHECK(loadScores(f).scored() == 0);
}

TEST_CASE("a sheet filled in by hand is read with the manifest beside it", "[plates]") {
    const fs::path d = freshDir("import");
    const fruit::CalibrationPlate& p = plateOf("tips");
    // As `fruitslicer calibrate` writes them: settings columns and all.
    const std::string base = (d / "by hand.stl").string();
    std::ofstream(base + ".json") << p.manifest().dump(2) << "\n";
    {
        std::istringstream in(p.csvTemplate());
        std::ofstream out(base + ".results.csv");
        std::string line;
        std::getline(in, line);
        out << line << "\n";
        for (int i = 1; std::getline(in, line); ++i) {
            line.resize(line.size() - 3);
            out << line << (i <= 10 ? ",ok,1," : i <= 12 ? ",partial,4,\"two tips let go, one held\"" : ",,,") << "\n";
        }
    }
    const PlateScores s = readScoreSheet(base + ".results.csv");
    CHECK(s.kind == "tips");
    CHECK(s.scored() == 12);
    CHECK(s.coupons[10].partial);
    CHECK(s.coupons[10].notes == "two tips let go, one held");

    PlateFiles f = keptAt(d / "kept", "tips");
    keepScores(s, f);
    CHECK(f.scored == 12);
    CHECK(firstLine(f.results) == "id,row,col,result,scar,notes");
    const PlateScores kept = loadScores(f);
    CHECK(kept.scored() == 12);
    CHECK(kept.coupons[10].notes == "two tips let go, one held");
    // Imported again over scores kept: those are set aside first.
    keepScores(s, f);
    CHECK(fs::exists(d / "kept" / "tips.earlier.results.csv"));

    CHECK_THROWS_WITH(readScoreSheet((d / "scores.csv").string()), ContainsSubstring("ends in .results.csv"));
    std::ofstream((d / "lonely.results.csv").string()) << "id,row,col,result,scar,notes\n";
    CHECK_THROWS_WITH(readScoreSheet((d / "lonely.results.csv").string()), ContainsSubstring("isn't beside it"));
    nlohmann::json ab = p.manifest();
    ab["kind"] = "ab";
    std::ofstream((d / "ab.json").string()) << ab.dump();
    std::ofstream((d / "ab.results.csv").string()) << "id,row,col,result,scar,notes\n";
    CHECK_THROWS_WITH(readScoreSheet((d / "ab.results.csv").string()), ContainsSubstring("isn't for a tips"));
    std::ofstream((d / "bad.json").string()) << p.manifest().dump();
    std::ofstream((d / "bad.results.csv").string()) << "id,row,col,result,scar,notes\n1,0,1,maybe,,\n";
    CHECK_THROWS_WITH(readScoreSheet((d / "bad.results.csv").string()), ContainsSubstring("couldn't be read"));
}

TEST_CASE("the next coupon to score skips those scored", "[plates]") {
    PlateScores s;
    for (int id = 1; id <= 5; ++id) {
        fruit::CouponResult r;
        r.id = id;
        s.coupons.push_back(r);
    }
    CHECK(nextToScore(s, 0) == 1);  // none chosen yet: the first
    CHECK(nextToScore(s, 2) == 3);
    s.coupons[2].recorded = s.coupons[3].recorded = true;
    CHECK(nextToScore(s, 2) == 5);
    CHECK(nextToScore(s, 5) == 1);  // round again
    for (auto& c : s.coupons) c.recorded = true;
    CHECK(nextToScore(s, 5) == 1);  // all scored: simply the next
    CHECK(nextToScore(s, 3) == 4);
    CHECK(nextToScore(PlateScores{}, 1) == 0);
}

TEST_CASE("manifests are the same plate when the coupons and settings are", "[plates]") {
    const nlohmann::json m = plateOf("tips").manifest();
    CHECK(samePlate(m, m));
    CHECK(samePlate(m, nlohmann::json::parse(m.dump())));
    nlohmann::json note = m;
    note["record"] = "other words";
    CHECK(samePlate(m, note));
    nlohmann::json moved = m;
    moved["coupons"][3]["params"]["load_per_tip_mm2"] = 99;
    CHECK_FALSE(samePlate(m, moved));
    nlohmann::json raft = m;
    raft["settings"]["raft_thickness"] = 2.0;
    CHECK_FALSE(samePlate(m, raft));
    CHECK_FALSE(samePlate(m, plateOf("islands").manifest()));
    CHECK_FALSE(samePlate(m, nlohmann::json::object()));
}

TEST_CASE("a resin keeps its saved plates, and which plates it has fitted", "[plates]") {
    const fs::path d = freshDir("resin");
    Resin r;
    r.id = "plate-resin";
    r.name = "Plate resin";
    PlateFiles f;
    f.file = "/Volumes/NO NAME/tips.goo";
    f.manifest = "/not/kept/tips.json";  // follows from the id instead
    f.scored = 12;
    f.coupons = 48;
    r.plates["tips"] = f;
    saveResin(d.string(), r);
    const auto back = loadResins(d.string());
    REQUIRE(back.size() == 1);
    REQUIRE(back[0].plates.count("tips"));
    const PlateFiles& b = back[0].plates.at("tips");
    CHECK(b.file == f.file);
    CHECK(b.scored == 12);
    CHECK(b.coupons == 48);
    CHECK(b.manifest.empty());

    nlohmann::json profile = {{"capacity_table", {{0.4, 8.0}}}, {"coverage_curve", nullptr}};
    CHECK(plateMeasured(profile, "tips"));
    CHECK_FALSE(plateMeasured(profile, "spacing"));
    CHECK_FALSE(plateMeasured(profile, "islands"));
    CHECK_FALSE(plateMeasured(nullptr, "tips"));
    profile["island_two_tip_length"] = 3.4;
    CHECK(plateMeasured(profile, "islands"));
    CHECK(profileState(profile) == "Measured: tips, islands");
}

TEST_CASE("the Calibrate page's next step follows the plates", "[plates]") {
    using Action = PlateStep::Action;
    Resin r;
    CHECK(nextPlateStep(r).action == Action::Print);
    CHECK(nextPlateStep(r).kind == "tips");
    CHECK(nextPlateStep(r).text == "Print the resin test plates, starting with the tips plate.");
    PlateFiles f;
    f.coupons = 48;
    r.plates["tips"] = f;
    CHECK(nextPlateStep(r).action == Action::Score);
    CHECK(nextPlateStep(r).text == "Score the tips plate once it's printed.");
    r.plates["tips"].scored = 20;
    CHECK(nextPlateStep(r).text == "Score the rest of the tips plate: 20 of 48 so far.");
    r.plates["tips"].scored = 48;
    CHECK(nextPlateStep(r).action == Action::Print);
    CHECK(nextPlateStep(r).kind == "spacing");
    CHECK(nextPlateStep(r).text == "Print the spacing plate next.");
    // A plate fitted already (an imported profile) needs nothing more.
    r.profile = {{"coverage_curve", {{0, 3.2}, {15, 5}}}};
    CHECK(nextPlateStep(r).kind == "islands");
    r.profile["island_one_tip_length"] = 1.7;
    // Every plate done, but the tips plate's scores aren't in the values yet.
    CHECK(nextPlateStep(r).action == Action::Fit);
    CHECK(nextPlateStep(r).text == "Turn the scores into this resin's support values.");
    r.profile["capacity_table"] = {{0.4, 8.0}};
    CHECK(nextPlateStep(r).action == Action::None);
    CHECK_THAT(nextPlateStep(r).text, StartsWith("This resin is calibrated"));
}
