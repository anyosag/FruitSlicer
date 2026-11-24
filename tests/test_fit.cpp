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

#include "ProcessTemp.h"
#include "calibration/CalibrationFit.h"
#include "support/SupportGeometry.h"
#include "calibration/CalibrationPlates.h"
#include "mesh/Placement.h"
#include "mesh/Primitives.h"
#include "support/PointGenerator.h"
#include "support/SupportTree.h"

using namespace fruit;
using Catch::Approx;
using json = nlohmann::json;

namespace {

// Writes the manifest and a results CSV filled by `judge(params) -> {ok, scar}`.
template <class Judge>
std::string writePlate(const CalibrationPlate& plate, const std::string& name, Judge judge) {
    const auto dir = testfiles::temp() / "fruit_test_fit";
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
    // A resin whose tips hold 60 mm² of layer for each mm² of the part they touch (contactDiameter, as the
    // plate draws them); loads 5/10/20 for every size and depth. Marks grow with tip size, not depth.
    const auto contact = [](double d, double pen) {
        PinheadSpec sp;
        sp.tip_diameter = d;
        sp.penetration = pen;
        sp.head_length = 1.0;
        sp.shaft_diameter = shaftDiameterFor(d);
        const double cd = contactDiameter(sp);
        return 3.14159265358979 * cd * cd / 4;
    };
    // The plate printed until 2026-11-24, discs on three tips: it alone tries two depths.
    DiscTipsPlateOptions to;
    to.ladders.clear();
    for (double d : {0.2, 0.3, 0.4, 0.6})
        for (double pen : {0.1, 0.2}) to.ladders.push_back({d, pen, {5, 10, 20}});
    to.neck_sets.clear();
    for (double d : {0.2, 0.3, 0.4, 0.6}) to.neck_sets.push_back({d, {5, 10, 20}});
    to.neck_copies = 2;
    const std::string tips = writePlate(discTipsPlate(to, printer), "tips", [&](const json& p) {
        const double d = p["tip_diameter"].get<double>();
        const bool ok = p["load_per_tip_mm2"].get<double>() < 60 * contact(d, p["penetration"].get<double>());
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

    // Tips: one strength over every coupon. A disc held when all three of its tips did, so its load is read as
    // the one a tip alone lets go at as often: 3^(1/7) = 1.17 times its load per tip. The heaviest that held is
    // 0.4 mm 0.1 mm deep at 5 mm² per tip (52 mm² of layer per mm² of contact, 61 as one tip), the lightest that
    // let go 0.6 mm 0.1 mm deep at 10 (63, 74): the strength found lies between, around the true 60 × 1.17 = 70.
    const json& model = prof["fits"]["tips_model"];
    CHECK(model["basis"] == "gap midpoint");
    const double s50 = model["strength_50pct"].get<double>();
    const double one = std::pow(3.0, 1 / 7.0);
    CHECK(s50 > 52 * one);
    CHECK(s50 < 63 * one);
    CHECK(prof["capacity_measured"] == true);
    // Each size's hold: 70 % of that strength times what it touches at its depth. Marks don't differ by depth
    // here, so the deeper one is kept, which touches more.
    std::map<double, double> cap;
    for (const auto& e : prof["capacity_table"]) cap[e[0].get<double>()] = e[1].get<double>();
    for (double d : {0.2, 0.3, 0.4, 0.6}) CHECK(cap.at(d) == Approx(0.7 * s50 * contact(d, 0.2)));
    for (const auto& e : prof["penetration_table"]) CHECK(e[1].get<double>() == Approx(0.2));
    CHECK(prof["minima_tip_diameter"].get<double>() == Approx(0.6));
    // Default: lowest mark among tips holding at least 1 mm² (a grid cell's layer). All do, but 0.2 and 0.3 mm
    // discs never held, so their marks are unknown: 0.4 mm, which marks less than 0.6.
    CHECK(prof["default_tip_diameter"].get<double>() == Approx(0.4));

    // What each tip size's value rests on, for saying how it was found.
    std::map<double, json> tip;
    for (const auto& t : prof["fits"]["tips"]) tip[t["tip_diameter"].get<double>()] = t;
    for (double d : {0.2, 0.3, 0.4, 0.6}) CHECK(tip.at(d)["basis"] == "contact model");
    CHECK(tip.at(0.4)["held"].get<int>() == 4);  // 5 mm², at 2 depths, twice
    CHECK(tip.at(0.4)["coupons"].get<int>() == 12);
    CHECK(tip.at(0.4)["max_held_mm2"].get<double>() == Approx(5 * one));
    CHECK(tip.at(0.4)["min_failed_mm2"].get<double>() == Approx(10 * one));
    CHECK(tip.at(0.6)["held"].get<int>() == 6);  // 5 at both depths, 10 at 0.2 mm
    CHECK(tip.at(0.2)["max_held_mm2"].is_null());  // never held, yet its hold is measured through the others
    CHECK(tip.at(0.2)["contact_mm2"].get<double>() == Approx(contact(0.2, 0.2)));
    REQUIRE(tip.at(0.4)["penetrations"].size() == 2);
    for (const auto& d : tip.at(0.4)["penetrations"]) {
        CHECK(d["held"].get<int>() == 2);
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


    // Applying it changes the generator settings.
    SupportConfig cfg;
    TreeConfig tc;
    std::vector<std::string> notes;
    cfg.layer_height = 0.1;
    applyProfile(prof, cfg, &tc, &notes);
    CHECK(cfg.critical_angle_deg == Approx(50));
    CHECK(cfg.capacity.capacity(0.4) == Approx(cap.at(0.4)));
    CHECK(cfg.tip_classes.size() == 4);
    CHECK(cfg.tip_classes[cfg.default_tip].diameter == Approx(0.4));
    // Near edges, FruitSlicer's own edge tip (0.3 mm), not the smallest measured.
    CHECK(cfg.tip_classes[cfg.edge_tip].diameter == Approx(0.3));
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

TEST_CASE("fit reads a tile's loads from the replay, and keeps the planner's depths", "[fit][tiles]") {
    const PrinterProfile printer = PrinterProfile::saturn4Ultra16k();
    const auto contact = [](double d, double pen) {
        PinheadSpec sp;
        sp.tip_diameter = d;
        sp.penetration = pen;
        sp.head_length = 1.0;
        sp.shaft_diameter = shaftDiameterFor(d);
        const double cd = contactDiameter(sp);
        return 3.14159265358979 * cd * cd / 4;
    };
    // One set (no hinge): a resin whose tips hold 100 mm² of layer per mm² they touch, one tip alone. A tile held
    // while the load one tip alone would let go at as often as one of its tips (load_mm2) is under that.
    const CalibrationPlate plate = tipsPlate(TipsPlateOptions{}, printer);
    REQUIRE(plate.coupons.size() == 96);
    double held_max = 0, failed_min = 1e9;
    const std::string csv = writePlate(plate, "tiles", [&](const json& p) {
        const double st = p["load_mm2"].get<double>() / contact(p["tip_diameter"].get<double>(), p["penetration"].get<double>());
        const bool ok = st < 100;
        if (ok) held_max = std::max(held_max, st);
        else failed_min = std::min(failed_min, st);
        return std::pair{ok, ok ? 2 : 0};
    });
    REQUIRE(held_max > 0);
    REQUIRE(failed_min < 1e9);
    FitOptions fo;
    fo.resin = "test resin";
    const json prof = fitProfile({loadPlateResults(manifestForResults(csv), csv)}, fo);
    const json& model = prof["fits"]["tips_model"];
    CHECK(model["coupons"].get<int>() == 96);
    const double s50 = model["strength_50pct"].get<double>();
    CHECK(s50 > held_max);
    CHECK(s50 < failed_min);
    CHECK(s50 == Approx(100).epsilon(0.1));
    CHECK(prof["capacity_measured"] == true);
    std::map<double, double> cap;
    for (const auto& e : prof["capacity_table"]) cap[e[0].get<double>()] = e[1].get<double>();
    REQUIRE(cap.size() == 4);
    for (const auto& [d, c] : cap) CHECK(c == Approx(0.7 * s50 * contact(d, tipPenetration(d, TreeConfig{}))));
    // Each size at the depth a job uses, and only that one: no depth is chosen, the planner's own rule stays.
    CHECK_FALSE(prof.contains("penetration_table"));
    SupportConfig cfg;
    TreeConfig tc;
    applyProfile(prof, cfg, &tc, nullptr);
    CHECK(tc.penetration_table.empty());
    CHECK(cfg.tip_load_limit == Approx(kMeasuredTipLoadLimit));

    // A tile that partly held says how many of its tips let go: up to all but one of its own, not three.
    const PlateCoupon& big = *std::max_element(plate.coupons.begin(), plate.coupons.end(), [](const auto& a, const auto& b) {
        return a.params["tips"].template get<int>() < b.params["tips"].template get<int>();
    });
    const int n = big.params["tips"].get<int>();
    REQUIRE(n > 3);
    const auto dir = testfiles::temp() / "fruit_test_fit";  // where writePlate put the manifest
    const std::string m = (dir / "tiles.stl.json").string(), sheet = (dir / "tiles_partly.stl.results.csv").string();
    std::ofstream(sheet) << "id,row,col,result,scar,tips_let_go,notes\n" << big.id << ",0,0,partial,3," << n - 1 << ",\n";
    const PlateResults back = loadPlateResults(m, sheet);
    REQUIRE(back.coupons.size() == 1);
    CHECK(back.coupons[0].tips_let_go == n - 1);
    std::ofstream(sheet) << "id,row,col,result,scar,tips_let_go,notes\n" << big.id << ",0,0,partial,3," << n + 1 << ",\n";
    CHECK_THROWS(loadPlateResults(m, sheet));
}

TEST_CASE("results CSV errors are reported clearly", "[fit]") {
    const auto dir = testfiles::temp() / "fruit_test_fit_bad";
    std::filesystem::create_directories(dir);
    const CalibrationPlate plate = islandsPlate({}, PrinterProfile::saturn4Ultra16k());
    std::ofstream((dir / "p.stl.json").string()) << plate.manifest().dump();
    std::ofstream((dir / "p.stl.results.csv").string()) << "id,row,col,result,scar,notes\n1,0,1,maybe,,\n";
    CHECK_THROWS(loadPlateResults((dir / "p.stl.json").string(), (dir / "p.stl.results.csv").string()));
    CHECK_THROWS(manifestForResults("plate.csv"));
}

TEST_CASE("blind score sheets: no parameters, and every score reads back", "[fit]") {
    const PrinterProfile pr = PrinterProfile::saturn4Ultra16k();
    TipsPlateOptions small;  // the sheet, not the plate, is what this checks: four tiles do
    small.sizes = {{0.3, 8, {0.5, 1}}};
    small.common.replicates = 1;
    const CalibrationPlate plate = tipsPlate(small, pr);
    REQUIRE(plate.coupons.size() == 4);
    const auto dir = testfiles::temp() / "fruit_blind_sheet";
    std::filesystem::create_directories(dir);
    const std::string manifest = (dir / "tips.stl.json").string(), csv = (dir / "tips.stl.results.csv").string();
    std::ofstream(manifest) << plate.manifest().dump(2);
    // A blank sheet: only where each coupon is.
    const std::string blank = blindResultsCsv(plate.manifest(), {});
    CHECK(blank.rfind("id,row,col,result,scar,tips_let_go,notes\n", 0) == 0);
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
    scores[1].tips_let_go = 1;
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
    CHECK(b.tips_let_go == 1);
    CHECK(b.notes == "one tip let go, the far one");
    CHECK(back.coupons[0].tips_let_go == -1);  // held: nothing to count
    // A sheet from before the column: read as it was, notes whole.
    std::ofstream(csv) << "id,row,col,result,scar,notes\n" << plate.coupons[1].id << ",0,0,partial,3,\"two let go, I think\"\n";
    const PlateResults old = loadPlateResults(manifest, csv);
    REQUIRE(old.coupons.size() == 1);
    CHECK(old.coupons[0].partial);
    CHECK(old.coupons[0].tips_let_go == -1);
    CHECK(old.coupons[0].notes == "two let go, I think");
    const CouponResult& c = back.coupons[2];
    CHECK(c.recorded);
    CHECK_FALSE(c.ok);
    CHECK_FALSE(c.partial);
    CHECK(c.notes == "gone 'completely' line two");
    CHECK_FALSE(back.coupons[3].recorded);
}
