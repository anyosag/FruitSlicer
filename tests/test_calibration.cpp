#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <set>

#include "calibration/CalibrationPlates.h"
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
