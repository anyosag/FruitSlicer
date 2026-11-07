// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The Print step's logic: settings, the plan, saving and reading back.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <filesystem>

#include "Document.h"
#include "Layers.h"
#include "Print.h"
#include "mesh/Primitives.h"
#include "mesh/StlIO.h"

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using namespace gui;

namespace {

std::filesystem::path tmp(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / "fruit_print_tests";
    std::filesystem::create_directories(dir);
    return dir / name;
}

std::shared_ptr<Document> box() {
    const auto path = tmp("print_box.stl");
    fruit::writeStl(path.string(), fruit::makeBox({0, 0, 0}, {10, 8, 3}));
    return loadDocument(path.string(), fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
}

// A SatelLite-like sample: 0.03 mm layers, its own exposures.
std::string sample003() {
    fruit::GooPrintRequest r;
    r.layer_height = 0.03;
    r.exposure = 1.5;
    r.bottom_exposure = 27.5;
    r.bottom_layers = 6;
    r.transition_layers = 6;
    const fruit::GooPrintSetup s = fruit::buildGooSetup(fruit::PrinterProfile::saturn4Ultra16k(), r);
    const auto path = tmp("sample_003.goo").string();
    fruit::exportGoo(fruit::makeBox({-2, -2, 0}, {2, 2, 1}), s.header, s.grid, fruit::GooExportOptions{}, path);
    return path;
}

}  // namespace

TEST_CASE("Print: no exposures, no print file", "[gui][print]") {
    const auto doc = box();
    const LayerResult lr = sliceLayers(*doc, {}, 0.0, fruit::JobControl::none());
    const PrintPlan p = planPrint(*doc, lr, 0.0, PrintSettings{});
    CHECK_FALSE(p.ready);
    CHECK_THAT(p.why_not, ContainsSubstring("SatelLite"));
}

TEST_CASE("Print: typed exposures plan, save and read back", "[gui][print]") {
    const auto doc = box();  // raised 5 mm: 8 mm tall in all
    const LayerResult lr = sliceLayers(*doc, {}, 0.0, fruit::JobControl::none());
    PrintSettings s;
    s.exposure = 2.5;
    s.bottom_exposure = 30;
    const PrintPlan p = planPrint(*doc, lr, 1.0, s);
    REQUIRE(p.ready);
    CHECK_FALSE(p.reslice);
    CHECK(p.layer_height == Approx(0.05));
    CHECK(p.layers == 160);
    CHECK(p.print_time_s > 160 * 2.5);
    CHECK(p.raft_thick_enough);  // 1 mm raft against 8 × 0.05 mm
    CHECK(p.resin_g == Approx(p.resin_ml * 1.15));
    CHECK(p.fits);
    // Supports reaching past the plate's edge (it is 211.68 mm wide) block the print.
    LayerResult wide = lr;
    wide.bounds.max.x = 106;
    CHECK_FALSE(planPrint(*doc, wide, 1.0, s).fits);

    const auto path = tmp("box.goo").string();
    const SaveResult r = savePrint(*doc, lr, {}, p, path, false, fruit::JobControl::none());
    CHECK(std::filesystem::exists(path));
    CHECK(r.written.layers == 160);
    CHECK(r.check.layers == 160);
    CHECK(r.check.checksums_ok);
    CHECK(r.check.grey_values == 0);
    CHECK(r.volume_matches);
    CHECK(r.check.volume_mm3 == Approx(240.0).epsilon(0.02));  // 10 × 8 × 3 mm
    CHECK(r.good);
}

TEST_CASE("Print: a 0.03 mm sample cuts the layers again at its height", "[gui][print]") {
    const auto doc = box();
    const LayerResult lr = sliceLayers(*doc, {}, 0.0, fruit::JobControl::none());
    PrintSettings s;
    s.sample_path = sample003();
    s.sample = loadSample(s.sample_path);
    const PrintPlan p = planPrint(*doc, lr, 1.0, s);
    REQUIRE(p.ready);
    CHECK(p.reslice);
    CHECK(p.layer_height == Approx(0.03));
    CHECK(p.layers == static_cast<std::size_t>(std::ceil(8.0 / 0.03 - 1e-9)));
    // 6 bottom + 6 transition layers of 0.03 mm need 0.36 mm of raft.
    CHECK(p.raft_needed == Approx(0.36));
    const auto path = tmp("box_003.goo").string();
    const SaveResult r = savePrint(*doc, lr, {}, p, path, false, fruit::JobControl::none());
    CHECK(r.written.layers == p.layers);
    CHECK(r.check.checksums_ok);
    CHECK(r.volume_matches);
    // A raft thinner than the bottom layers is flagged.
    CHECK_FALSE(planPrint(*doc, lr, 0.2, s).raft_thick_enough);
}

TEST_CASE("Print: cancelling while the file is read back leaves no file", "[gui][print]") {
    const auto doc = box();
    const LayerResult lr = sliceLayers(*doc, {}, 0.0, fruit::JobControl::none());
    PrintSettings s;
    s.exposure = 2.5;
    s.bottom_exposure = 30;
    const PrintPlan p = planPrint(*doc, lr, 1.0, s);
    REQUIRE(p.ready);
    const auto path = tmp("cancelled.goo").string();
    std::filesystem::remove(path);
    // Cancel as soon as the read-back starts: the file is complete by then.
    std::shared_ptr<fruit::JobControl> ctl;
    bool verifying = false;
    ctl = std::make_shared<fruit::JobControl>([&](std::string_view stage, double) {
        if (stage == "verify" && !verifying) {
            verifying = true;
            ctl->cancel();
        }
    });
    CHECK_THROWS_AS(savePrint(*doc, lr, {}, p, path, false, *ctl), fruit::Cancelled);
    CHECK(verifying);
    CHECK_FALSE(std::filesystem::exists(path));
}
