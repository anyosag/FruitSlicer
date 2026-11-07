// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "calibration/ABPlate.h"
#include "calibration/CalibrationPlates.h"
#include "mesh/Primitives.h"
#include "layers/IslandGraph.h"
#include "support/SupportGeometry.h"

using namespace fruit;
using Catch::Approx;

namespace {

// Every coupon must be held by its supports: after slicing, the only islands
// are the rafts on the first layer (and nothing else floats).
void requireOnlyRaftIslands(const CalibrationPlate& plate) {
    const LayerStack ls = sliceModel(plate.mesh, 0.1);
    const IslandGraph g(ls, 0.0);
    for (int id : g.islands()) CHECK(g.part(id).layer == 0);
    // One raft per coupon plus the marker raft.
    size_t first_layer = 0;
    for (int id : g.islands()) first_layer += g.part(id).layer == 0;
    CHECK(first_layer == plate.coupons.size() + 1);
}

void requireFitsAndUnique(const CalibrationPlate& plate, const PrinterProfile& p) {
    const BBox3 b = plate.mesh.bbox();
    CHECK(p.fitsXY(b.min.x, b.min.y));
    CHECK(p.fitsXY(b.max.x, b.max.y));
    std::set<std::pair<int, int>> cells;
    for (const auto& c : plate.coupons) {
        CHECK(cells.insert({c.row, c.col}).second);
        CHECK_FALSE((c.row == 0 && c.col == 0));  // marker cell
    }
}

}  // namespace

TEST_CASE("head direction clamps to 45 degrees from vertical", "[support]") {
    const Vec3 d = headDirection({0, 1, 0});  // vertical wall
    CHECK(d.z == Approx(-std::cos(deg2rad(45))));
    CHECK(d.y == Approx(std::sin(deg2rad(45))));
    const Vec3 down = headDirection({0, 0, -1});
    CHECK(down.z == Approx(-1));
    const Vec3 mild = headDirection(normalized(Vec3{0, 0.3, -1}));
    CHECK(mild.y == Approx(normalized(Vec3{0, 0.3, -1}).y));
}

TEST_CASE("tips plate: all combinations, fits, connected, deterministic", "[calibration]") {
    const PrinterProfile p = PrinterProfile::saturn4Ultra16k();
    TipsPlateOptions o;
    const CalibrationPlate a = tipsPlate(o, p);
    CHECK(a.coupons.size() == 4 * 2 * 3 * 2);
    requireFitsAndUnique(a, p);
    requireOnlyRaftIslands(a);
    const CalibrationPlate b = tipsPlate(o, p);
    REQUIRE(b.coupons.size() == a.coupons.size());
    for (size_t i = 0; i < a.coupons.size(); ++i) CHECK(a.coupons[i].params == b.coupons[i].params);
    o.common.seed = 7;
    const CalibrationPlate c = tipsPlate(o, p);
    bool differs = false;
    for (size_t i = 0; i < a.coupons.size(); ++i) differs |= a.coupons[i].params != c.coupons[i].params;
    CHECK(differs);
    CHECK(a.csvTemplate().find("tip_diameter") != std::string::npos);
    CHECK(a.manifest()["coupons"].size() == a.coupons.size());
}

TEST_CASE("spacing plate: tilted plates and wedges are held by their tips", "[calibration]") {
    const PrinterProfile p = PrinterProfile::saturn4Ultra16k();
    const CalibrationPlate plate = spacingPlate(SpacingPlateOptions{}, p);
    CHECK(plate.coupons.size() == 5 * 6 + 7);
    requireFitsAndUnique(plate, p);
    requireOnlyRaftIslands(plate);
}

TEST_CASE("islands plate: columns start in mid-air on 1-3 tips", "[calibration]") {
    const PrinterProfile p = PrinterProfile::saturn4Ultra16k();
    const CalibrationPlate plate = islandsPlate(IslandsPlateOptions{}, p);
    const size_t skipped = plate.settings["skipped_combinations"].size();
    CHECK(skipped == 0);
    CHECK(plate.coupons.size() == (6 * 3 - skipped) * 2);
    requireFitsAndUnique(plate, p);
    requireOnlyRaftIslands(plate);
    for (const auto& c : plate.coupons) CHECK(c.params["tip_diameter"].get<double>() >= 0.1 - 1e-9);
}

TEST_CASE("plates that do not fit are rejected; thin rafts are rejected", "[calibration]") {
    const PrinterProfile p = PrinterProfile::saturn4Ultra16k();
    TipsPlateOptions o;
    o.common.replicates = 10;
    CHECK_THROWS(tipsPlate(o, p));
    TipsPlateOptions thin;
    thin.common.raft_thickness = 0.2;
    CHECK_THROWS(tipsPlate(thin, p));
}

TEST_CASE("A/B plate: balanced shuffled arms that fit the plate", "[calibration][ab]") {
    const PrinterProfile p = PrinterProfile::saturn4Ultra16k();
    TriangleMesh model = makeBox({-5, -5, 5}, {5, 5, 8});
    TriangleMesh sup = makeBox({-6, -6, 0}, {6, 6, 5});  // stand-in for supports + raft
    ABPlateOptions o;
    o.copies = 8;
    const ABPlate a = makeABPlate(model, sup, p, o);
    size_t na = 0;
    std::set<std::pair<int, int>> cells;
    for (const auto& c : a.manifest["copies"]) {
        na += c["arm"] == "A";
        CHECK(cells.insert({c["row"].get<int>(), c["col"].get<int>()}).second);
    }
    CHECK(na == 4);
    for (const BBox3& b : {a.supported.bbox(), a.bare.bbox()}) {
        CHECK(p.fitsXY(b.min.x, b.min.y));
        CHECK(p.fitsXY(b.max.x, b.max.y));
    }
    CHECK(a.bare.bbox().min.z == Approx(0));  // bare copies stand on the plate
    // Same seed, same layout; another seed shuffles differently.
    const ABPlate b = makeABPlate(model, sup, p, o);
    CHECK(a.manifest["copies"] == b.manifest["copies"]);
    o.seed = 3;
    CHECK(makeABPlate(model, sup, p, o).manifest["copies"] != a.manifest["copies"]);
    o.copies = 500;
    CHECK_THROWS(makeABPlate(model, sup, p, o));
}

TEST_CASE("A/B plate: as many copies as fit, and no more", "[calibration][ab]") {
    const PrinterProfile p = PrinterProfile::saturn4Ultra16k();
    const TriangleMesh model = makeBox({-20, -15, 5}, {20, 15, 30});
    const TriangleMesh sup = makeBox({-22, -17, 0}, {22, 17, 5});
    const ABPlateFit fit = abPlateFit(model, sup, p);
    CHECK(fit.cell_x == Approx(44 + 4));
    CHECK(fit.cell_y == Approx(34 + 4));
    CHECK(fit.capacity == fit.rows * fit.cols - 1);
    REQUIRE(fit.capacity >= 2);
    ABPlateOptions o;
    o.copies = fit.capacity;
    CHECK_NOTHROW(makeABPlate(model, sup, p, o));
    o.copies = fit.capacity + 1;
    CHECK_THROWS(makeABPlate(model, sup, p, o));
    // A model too big for two copies fits none beside the marker.
    const TriangleMesh huge = makeBox({-100, -55, 0}, {100, 55, 10});
    CHECK(abPlateFit(huge, TriangleMesh{}, p).capacity == 0);
}

TEST_CASE("A/B plate: the manifest names the model and its tips", "[calibration][ab]") {
    const PrinterProfile p = PrinterProfile::saturn4Ultra16k();
    ABPlateOptions o;
    o.copies = 4;
    o.model = "bracket";
    o.tips_per_copy = 213;
    const ABPlate a = makeABPlate(makeBox({-5, -5, 5}, {5, 5, 8}), makeBox({-6, -6, 0}, {6, 6, 5}), p, o);
    CHECK(a.manifest["model"] == "bracket");
    CHECK(a.manifest["tips_per_copy"] == 213);
    o.tips_per_copy = -1;
    CHECK(makeABPlate(makeBox({-5, -5, 5}, {5, 5, 8}), makeBox({-6, -6, 0}, {6, 6, 5}), p, o)
              .manifest["tips_per_copy"]
              .is_null());
}

TEST_CASE("A/B score sheets: blind, and every score reads back", "[calibration][ab]") {
    using Catch::Matchers::ContainsSubstring;
    const PrinterProfile p = PrinterProfile::saturn4Ultra16k();
    ABPlateOptions o;
    o.copies = 4;
    const ABPlate a = makeABPlate(makeBox({-5, -5, 5}, {5, 5, 8}), makeBox({-6, -6, 0}, {6, 6, 5}), p, o);
    const auto dir = std::filesystem::temp_directory_path() / "fruit_ab_tests";
    std::filesystem::create_directories(dir);
    const std::string manifest = (dir / "ab.json").string(), sheet = (dir / "ab.results.csv").string();
    std::ofstream(manifest) << a.manifest.dump(2);

    std::vector<ABCopyResult> scores(2);
    scores[0].id = 1;
    scores[0].recorded = scores[0].ok = true;
    scores[0].scar = 2;
    scores[0].removal_s = 41.5;
    scores[0].notes = "clean, one tip, left a dot";
    scores[1].id = 3;
    scores[1].recorded = scores[1].partial = true;
    scores[1].removal_s = 95;
    const std::string csv = blindABCsv(a.manifest, scores);
    CHECK(csv.substr(0, csv.find('\n')) == "id,row,col,result,scar,removal_s,notes");
    CHECK(csv.find(",A,") == std::string::npos);  // no arms
    CHECK(csv.find(",B,") == std::string::npos);
    std::ofstream(sheet) << csv;
    const std::vector<ABCopyResult> back = loadABResults(manifest, sheet);
    REQUIRE(back.size() == 4);
    CHECK(back[0].recorded);
    CHECK(back[0].ok);
    CHECK(back[0].scar == 2);
    CHECK(back[0].removal_s == Approx(41.5));
    CHECK(back[0].notes == "clean, one tip, left a dot");
    CHECK_FALSE(back[1].recorded);
    CHECK(back[1].removal_s < 0);
    CHECK(back[2].partial);
    CHECK_FALSE(back[2].ok);
    CHECK(back[2].scar == 0);

    // The sheet makeABPlate writes (arms and all) reads too.
    std::ofstream(sheet) << a.csv;
    CHECK(loadABResults(manifest, sheet).size() == 4);
    // An arm that disagrees with the manifest is refused.
    std::string swapped = a.csv;
    const std::string arm1 = a.manifest["copies"][0]["arm"].get<std::string>();
    const std::string row1 = "1,0,1," + arm1 + ",";
    REQUIRE(swapped.find(row1) != std::string::npos);
    swapped.replace(swapped.find(row1), row1.size(), "1,0,1," + std::string(arm1 == "A" ? "B" : "A") + ",");
    std::ofstream(sheet) << swapped;
    CHECK_THROWS_WITH(loadABResults(manifest, sheet), ContainsSubstring("in its manifest"));
    std::ofstream(sheet) << "id,row,col,result,scar,removal_s,notes\n9,0,0,ok,,,\n";
    CHECK_THROWS_WITH(loadABResults(manifest, sheet), ContainsSubstring("isn't on the plate"));
    std::ofstream(sheet) << "id,row,col,result,scar,removal_s,notes\n1,0,1,maybe,,,\n";
    CHECK_THROWS_WITH(loadABResults(manifest, sheet), ContainsSubstring("ok, partial or failed"));
    std::ofstream(sheet) << "id,row,col,result,scar,removal_s,notes\n1,0,1,ok,7,,\n";
    CHECK_THROWS_WITH(loadABResults(manifest, sheet), ContainsSubstring("scar must be 1 to 5"));
    std::ofstream(sheet) << "id,row,col,result,scar,removal_s,notes\n1,0,1,ok,2,soon,\n";
    CHECK_THROWS_WITH(loadABResults(manifest, sheet), ContainsSubstring("removal_s must be seconds"));
}
