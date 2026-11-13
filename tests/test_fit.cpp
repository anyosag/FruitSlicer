// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "calibration/CalibrationFit.h"
#include "calibration/CalibrationPlates.h"
#include "mesh/Placement.h"
#include "mesh/Primitives.h"
#include "support/PointGenerator.h"

using namespace fruit;
using Catch::Approx;
using json = nlohmann::json;

namespace {

// Writes the manifest and a results CSV filled by `judge(params) -> {ok, scar}`.
template <class Judge>
std::string writePlate(const CalibrationPlate& plate, const std::string& name, Judge judge) {
    const auto dir = std::filesystem::temp_directory_path() / "fruit_test_fit";
    std::filesystem::create_directories(dir);
    const std::string stl = (dir / (name + ".stl")).string();
    std::ofstream(stl + ".json") << plate.manifest().dump();
    // Start from the generated template and fill result + scar columns.
    std::istringstream tmpl(plate.csvTemplate());
    std::ostringstream out;
    std::string line;
    std::getline(tmpl, line);
    out << line << "\n";
    size_t i = 0;
    while (std::getline(tmpl, line)) {
        const auto [ok, scar] = judge(plate.coupons[i++].params);
        line = line.substr(0, line.size() - 3);  // drop ",,," (result, scar, notes)
        out << line << "," << (ok ? "ok" : "failed") << "," << scar << ",\n";
    }
    std::ofstream(stl + ".results.csv") << out.str();
    return stl + ".results.csv";
}

}  // namespace

TEST_CASE("logistic fit: separable, censored and overlapping data", "[fit]") {
    auto f = fitLogistic({5, 10, 20, 5, 10, 20}, {1, 1, 0, 1, 1, 0});
    CHECK(f.separable);
    CHECK(f.l50 == Approx(15));
    f = fitLogistic({5, 10, 20}, {1, 1, 1});
    CHECK(std::isinf(f.l50));
    CHECK(f.l50 > 0);
    f = fitLogistic({5, 10, 20}, {0, 0, 0});
    CHECK(std::isinf(f.l50));
    CHECK(f.l50 < 0);
    // Overlap: success falls with load, 50 % somewhere between 8 and 14.
    f = fitLogistic({4, 6, 8, 10, 12, 14, 16, 18, 8, 12, 10, 14}, {1, 1, 1, 0, 1, 0, 0, 0, 1, 1, 1, 0});
    CHECK_FALSE(f.separable);
    CHECK(f.b < 0);
    CHECK(f.l50 > 8);
    CHECK(f.l50 < 14);
}

TEST_CASE("fit turns filled plates into a resin profile", "[fit]") {
    const PrinterProfile printer = PrinterProfile::saturn4Ultra16k();
    // True capacity 12 mm² for a 0.4 mm tip, scaling with tip area.
    const std::string tips = writePlate(tipsPlate({}, printer), "tips", [](const json& p) {
        const double d = p["tip_diameter"].get<double>();
        const bool ok = p["load_per_tip_mm2"].get<double>() < 12.0 * (d / 0.4) * (d / 0.4);
        const int scar = std::clamp(static_cast<int>(std::lround(1 + 6 * d)), 1, 5);
        return std::pair{ok, scar};
    });
    // Plates hold up to 3 mm pitch at every angle; wedges print up to 50°.
    const std::string spacing = writePlate(spacingPlate({}, printer), "spacing", [](const json& p) {
        if (p["kind"] == "wedge") return std::pair{p["angle_deg"].get<double>() <= 50, 0};
        return std::pair{p["spacing_mm"].get<double>() <= 3.0, 0};
    });
    // Islands need 1 tip up to 1 mm, 2 up to 3 mm, 3 beyond.
    const std::string islands = writePlate(islandsPlate({}, printer), "islands", [](const json& p) {
        const double D = p["island_diameter"].get<double>();
        const int need = D <= 1.0 ? 1 : (D <= 3.0 ? 2 : 3);
        return std::pair{p["tips"].get<int>() >= need, 0};
    });

    std::vector<PlateResults> plates;
    for (const auto& csv : {tips, spacing, islands}) plates.push_back(loadPlateResults(manifestForResults(csv), csv));
    FitOptions fo;
    fo.resin = "test resin";
    fo.layer_height = 0.05;
    const json prof = fitProfile(plates, fo);

    // Tips: loads 5/10/20 → 0.4 mm holds 5 and 10, fails 20 → 50 % at 15 → 0.7 × 15.
    std::map<double, double> cap;
    for (const auto& e : prof["capacity_table"]) cap[e[0].get<double>()] = e[1].get<double>();
    CHECK(cap.at(0.4) == Approx(0.7 * 15));
    // Never failed: half would let go somewhere above 20, so 0.7 × 20 is a
    // true lower bound. (The truth here is 0.7 × 27 = 18.9; reporting 20, as
    // the fit once did, would overstate it.)
    CHECK(cap.at(0.6) == Approx(0.7 * 20));
    CHECK(cap.at(0.2) == Approx(2.5));     // always failed: half the smallest load
    CHECK(prof["minima_tip_diameter"].get<double>() == Approx(0.6));
    // Default: lowest scar among tips holding >= 4 mm² → 0.3 mm (capacity 0.7 × 7.5).
    CHECK(prof["default_tip_diameter"].get<double>() == Approx(0.3));

    // What each tip size's value rests on, for saying how it was found.
    std::map<double, json> tip;
    for (const auto& t : prof["fits"]["tips"]) tip[t["tip_diameter"].get<double>()] = t;
    CHECK(tip.at(0.4)["basis"] == "gap midpoint");
    CHECK(tip.at(0.4)["held"].get<int>() == 8);  // 5 and 10 mm², at 2 depths, twice
    CHECK(tip.at(0.4)["coupons"].get<int>() == 12);
    CHECK(tip.at(0.4)["max_held_mm2"].get<double>() == Approx(10));
    CHECK(tip.at(0.4)["min_failed_mm2"].get<double>() == Approx(20));
    CHECK(tip.at(0.6)["basis"] == "lower bound");
    CHECK(tip.at(0.6)["held"].get<int>() == 12);
    CHECK(tip.at(0.6)["min_failed_mm2"].is_null());
    CHECK(tip.at(0.2)["max_held_mm2"].is_null());
    REQUIRE(tip.at(0.4)["penetrations"].size() == 2);
    for (const auto& d : tip.at(0.4)["penetrations"]) {
        CHECK(d["held"].get<int>() == 4);
        CHECK(d["coupons"].get<int>() == 6);
        CHECK(d["mean_scar"].get<double>() > 0);
    }
    REQUIRE(prof["fits"]["spacing"]["wedges"].size() == 7);
    for (const auto& w : prof["fits"]["spacing"]["wedges"])
        CHECK(w["ok"].get<bool>() == (w["angle_deg"].get<double>() <= 50));

    // Spacing: 3 mm pitch → r0 = 3/√2, seed curve scaled.
    CHECK(prof["coverage_curve"][0][1].get<double>() == Approx(3.0 / std::sqrt(2.0)));
    CHECK(prof["critical_angle_deg"].get<double>() == Approx(50));

    // Islands.
    CHECK(prof["island_one_tip_length"].get<double>() == Approx(1.0));
    CHECK(prof["island_two_tip_length"].get<double>() == Approx(3.0));

    bool flagged = false;
    for (const auto& n : prof["notes"]) flagged |= n.get<std::string>().find("lower bound") != std::string::npos;
    CHECK(flagged);

    // Applying it changes the generator settings.
    SupportConfig cfg;
    TreeConfig tc;
    std::vector<std::string> notes;
    cfg.layer_height = 0.1;
    applyProfile(prof, cfg, &tc, &notes);
    CHECK(cfg.critical_angle_deg == Approx(50));
    CHECK(cfg.capacity.capacity(0.4) == Approx(10.5));
    CHECK(cfg.tip_classes.size() == 4);
    CHECK(cfg.tip_classes[cfg.default_tip].diameter == Approx(0.3));
    CHECK_FALSE(tc.penetration_table.empty());
    CHECK(notes.size() == 1);  // layer-height mismatch
    // Measured tips are trusted nearer to where they let go, so each one's own load is held further under it.
    CHECK(SupportConfig{}.tip_load_limit == Approx(1.0));
    CHECK(cfg.tip_load_limit == Approx(kMeasuredTipLoadLimit));

    // A tighter measured radius means more points on the same plate.
    TriangleMesh m = makeBox({0, 0, 0}, {20, 20, 2});
    placeOnPlate(m, {0, 0, 0}, 5);
    SupportConfig base;
    base.layer_height = 0.1;
    const size_t before = generateSupportPoints(m, base).points.size();
    const size_t after = generateSupportPoints(m, cfg).points.size();
    CHECK(after > before);
}

TEST_CASE("results CSV errors are reported clearly", "[fit]") {
    const auto dir = std::filesystem::temp_directory_path() / "fruit_test_fit_bad";
    std::filesystem::create_directories(dir);
    const CalibrationPlate plate = islandsPlate({}, PrinterProfile::saturn4Ultra16k());
    std::ofstream((dir / "p.stl.json").string()) << plate.manifest().dump();
    std::ofstream((dir / "p.stl.results.csv").string()) << "id,row,col,result,scar,notes\n1,0,1,maybe,,\n";
    CHECK_THROWS(loadPlateResults((dir / "p.stl.json").string(), (dir / "p.stl.results.csv").string()));
    CHECK_THROWS(manifestForResults("plate.csv"));
}

TEST_CASE("blind score sheets: no parameters, and every score reads back", "[fit]") {
    const PrinterProfile pr = PrinterProfile::saturn4Ultra16k();
    const CalibrationPlate plate = tipsPlate(TipsPlateOptions{}, pr);
    const auto dir = std::filesystem::temp_directory_path() / "fruit_blind_sheet";
    std::filesystem::create_directories(dir);
    const std::string manifest = (dir / "tips.stl.json").string(), csv = (dir / "tips.stl.results.csv").string();
    std::ofstream(manifest) << plate.manifest().dump(2);
    // A blank sheet: only where each coupon is.
    const std::string blank = blindResultsCsv(plate.manifest(), {});
    CHECK(blank.rfind("id,row,col,result,scar,notes\n", 0) == 0);
    CHECK(blank.find("tip_diameter") == std::string::npos);
    CHECK(std::count(blank.begin(), blank.end(), '\n') == static_cast<long>(plate.coupons.size()) + 1);
    // Held, partly and fell off, with marks and notes (commas included).
    std::vector<CouponResult> scores(3);
    scores[0].id = plate.coupons[0].id;
    scores[0].recorded = scores[0].ok = true;
    scores[0].scar = 2;
    scores[1].id = plate.coupons[1].id;
    scores[1].recorded = scores[1].partial = true;
    scores[1].scar = 4;
    scores[1].notes = "one tip let go, the far one";
    scores[2].id = plate.coupons[2].id;
    scores[2].recorded = true;
    scores[2].notes = "gone \"completely\"\nline two";
    std::ofstream(csv) << blindResultsCsv(plate.manifest(), scores);
    const PlateResults back = loadPlateResults(manifest, csv);
    REQUIRE(back.coupons.size() == plate.coupons.size());
    const CouponResult& a = back.coupons[0];
    CHECK(a.recorded);
    CHECK(a.ok);
    CHECK(a.scar == 2);
    CHECK(a.params == plate.coupons[0].params);  // from the manifest
    const CouponResult& b = back.coupons[1];
    CHECK(b.recorded);
    CHECK_FALSE(b.ok);
    CHECK(b.partial);
    CHECK(b.notes == "one tip let go, the far one");
    const CouponResult& c = back.coupons[2];
    CHECK(c.recorded);
    CHECK_FALSE(c.ok);
    CHECK_FALSE(c.partial);
    CHECK(c.notes == "gone 'completely' line two");
    CHECK_FALSE(back.coupons[3].recorded);
}
