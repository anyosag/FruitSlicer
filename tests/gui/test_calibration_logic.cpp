// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The printer's settings and the image orientation check.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>

#include "Calibration.h"
#include "calibration/Coupons.h"
#include "formats/GooExport.h"
#include "formats/GooFile.h"
#include "formats/GooSetup.h"
#include "mesh/StlIO.h"

using Catch::Matchers::ContainsSubstring;
using namespace gui;

namespace {

std::filesystem::path dir() {
    const auto d = std::filesystem::temp_directory_path() / "fruit_calibration_tests";
    std::filesystem::create_directories(d);
    return d;
}

std::string satelliteF(bool flip_x, bool flip_y) {
    const auto p = (dir() / (std::string("f_") + (flip_x ? "x" : "") + (flip_y ? "y" : "") + ".goo")).string();
    writeStandInSatelliteF(p, flip_x, flip_y);
    return p;
}

// Share of cells lit in one picture and not the other.
double mismatch(const LayerPicture& a, const LayerPicture& b) {
    if (a.w != b.w || a.h != b.h) return 1.0;
    std::size_t lit = 0, differ = 0;
    for (std::size_t i = 0; i < a.lit.size(); ++i) {
        const bool la = a.lit[i] > 127, lb = b.lit[i] > 127;
        lit += la || lb;
        differ += la != lb;
    }
    return lit ? static_cast<double>(differ) / static_cast<double>(lit) : 1.0;
}

}  // namespace

TEST_CASE("Orientation check: finds the flip that makes FruitSlicer's F match", "[gui][calibration]") {
    const auto printer = fruit::PrinterProfile::saturn4Ultra16k();
    struct Case {
        bool fx, fy;
        fruit::OrientationVerdict verdict;
    };
    for (const Case k : {Case{false, false, fruit::OrientationVerdict::Matches},
                         Case{false, true, fruit::OrientationVerdict::FlipY},
                         Case{true, false, fruit::OrientationVerdict::FlipX}}) {
        INFO("satellite flipped x " << k.fx << " y " << k.fy);
        const OrientationCheck c = checkOrientation(satelliteF(k.fx, k.fy), printer, fruit::JobControl::none());
        CHECK(c.verdict == k.verdict);
        CHECK(c.flip_x == k.fx);
        CHECK(c.flip_y == k.fy);
        CHECK(c.compare.best_iou > 0.9);
        CHECK(c.compare.scores.size() == 4);
        CHECK_THAT(verdictBody(c), ContainsSubstring("SatelLite's slice of the same F"));
        // With the fix applied, the two pictures show the same F.
        REQUIRE(c.ours.w > 50);
        REQUIRE(c.theirs.w > 50);
        CHECK(mismatch(c.ours, c.theirs) < 0.1);
        CHECK(c.ours.cell_mm > 0);
    }
    CHECK_THROWS(checkOrientation((dir() / "missing.goo").string(), printer, fruit::JobControl::none()));
}

TEST_CASE("Orientation check: the F test piece is the coupon, standing on the plate", "[gui][calibration]") {
    const auto p = (dir() / "f.stl").string();
    saveOrientationPiece(p);
    const fruit::TriangleMesh m = fruit::readStl(p);
    const fruit::BBox3 b = m.bbox();
    CHECK(b.min.z == 0.0);
    CHECK(b.min.x > 0);  // the +X, +Y quarter
    CHECK(b.min.y > 0);
    CHECK(m.faces.size() == fruit::orientationCoupon().mesh.faces.size());
}

TEST_CASE("Printer setup: kept between launches; the hinge as people pick it", "[gui][calibration]") {
    const auto file = (dir() / "data" / "settings.json").string();
    std::filesystem::remove_all(dir() / "data");
    CHECK(loadPrinterSetup(file) == PrinterSetup{});
    PrinterSetup s;
    s.orientation_confirmed = true;
    s.flip_y = true;
    s.checked_with = "f_satellite.goo";
    s.hinge = plateSide(HingeChoice::Left);
    savePrinterSetup(file, s);
    CHECK(loadPrinterSetup(file) == s);
    std::ofstream(file) << "{ broken";
    CHECK(loadPrinterSetup(file) == PrinterSetup{});

    CHECK(plateSide(HingeChoice::Left) == fruit::HingeSide::MinusX);
    CHECK(plateSide(HingeChoice::Right) == fruit::HingeSide::PlusX);
    CHECK(plateSide(HingeChoice::Back) == fruit::HingeSide::PlusY);
    CHECK(plateSide(HingeChoice::Front) == fruit::HingeSide::MinusY);
    for (HingeChoice c : {HingeChoice::Back, HingeChoice::Left, HingeChoice::Right, HingeChoice::Front})
        CHECK(hingeChoice(plateSide(c)) == c);
    CHECK_FALSE(hingeChoice(std::nullopt).has_value());
}
