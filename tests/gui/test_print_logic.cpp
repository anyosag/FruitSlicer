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
#include "Supports.h"
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

TEST_CASE("Print: a bar flat on the plate with tips just above it fits the plate", "[gui][print][slow]") {
    // Like a dish rack's side bar: 10 mm tall, flat on the plate, with stubs
    // out to one side whose undersides are 1 mm up. The tips under them
    // stand on the plate, so model and supports stay inside it.
    fruit::TriangleMesh m = fruit::makeBox({0, 0, 0}, {150, 8, 10});
    for (int i = 0; i < 7; ++i) m.append(fruit::makeBox({5 + i * 21.0, -6, 1}, {9 + i * 21.0, 0.5, 4}));
    const auto path = tmp("print_bar.stl");
    fruit::writeStl(path.string(), m);
    const auto doc = loadDocument(path.string(), fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    fruit::Placement p = doc->placement();
    p.on_plate = true;
    doc->edit(p);
    const SupportResult sup = generate(*doc, SupportChoices{}, {}, fruit::JobControl::none());
    REQUIRE(sup.tips > 0);
    CHECK(sup.validation.floating.empty());
    CHECK(sup.size_with_supports.z == Approx(10.0).margin(1e-6));
    const LayerResult lr = sliceLayers(*doc, sup.mesh, 0.0, fruit::JobControl::none(), sup.hollow);
    CHECK(lr.bounds.min.z >= -1e-9);
    CHECK(lr.bounds.size().z == Approx(10.1).margin(1e-6));  // 10 mm and 0.10 mm of height correction
    CHECK(fitsPlate(doc->printer, lr.bounds));
    CHECK(planPrint(*doc, lr, 0.0, PrintSettings{}).fits);
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

namespace {

std::shared_ptr<Document> boxOnPlate(double correction) {
    const auto doc = box();
    fruit::Placement p = doc->placement();
    p.on_plate = true;
    p.height_correction = correction;
    doc->edit(p);
    return doc;
}

PrintSettings typed() {
    PrintSettings s;
    s.exposure = 2.5;
    s.bottom_exposure = 30;
    return s;
}

}  // namespace

TEST_CASE("Height correction: an on-plate job has n more layers, a raft job does not", "[gui][print][correction]") {
    const std::size_t flat = sliceLayers(*boxOnPlate(0.0), {}, 0.0, fruit::JobControl::none()).layers->size();
    CHECK(flat == 60);  // 3 mm at 0.05 mm
    const auto doc = boxOnPlate(0.10);
    const LayerResult lr = sliceLayers(*doc, {}, 0.0, fruit::JobControl::none());
    CHECK(lr.layers->size() == 62);
    CHECK(lr.base_layers == 2);
    CHECK(lr.top == Approx(3.1));
    CHECK(lr.area.front() == Approx(80.0).margin(0.5));  // the first layer, copied
    CHECK(lr.area[2] == Approx(80.0).margin(0.5));
    CHECK(lr.resin_ml == Approx(0.248).margin(0.002));
    CHECK(lr.bounds.max.z == Approx(3.1));
    CHECK(lr.raft_layers == 0);
    const PrintPlan plan = planPrint(*doc, lr, 0.0, typed());
    REQUIRE(plan.ready);
    CHECK(plan.layers == 62);
    CHECK(plan.height == Approx(3.1));
    // 0.15 mm is 3 layers; the placed model is the same height either way.
    const auto doc3 = boxOnPlate(0.15);
    CHECK(sliceLayers(*doc3, {}, 0.0, fruit::JobControl::none()).layers->size() == 63);
    CHECK(doc3->placed().bbox().max.z == Approx(3.0));
    // A raft job (the box lifted 5 mm) is not corrected, whatever the setting.
    const auto lifted = box();
    fruit::Placement p = lifted->placement();
    p.height_correction = 0.3;
    lifted->edit(p);
    const LayerResult raft = sliceLayers(*lifted, {}, 1.0, fruit::JobControl::none());
    CHECK(raft.layers->size() == 160);
    CHECK(raft.base_layers == 0);
    CHECK(raft.raft_layers == 20);
}

TEST_CASE("Height correction: a taller print still has to fit the build height", "[gui][print][correction]") {
    const auto doc = boxOnPlate(0.10);
    LayerResult lr = sliceLayers(*doc, {}, 0.0, fruit::JobControl::none());
    CHECK(planPrint(*doc, lr, 0.0, typed()).fits);
    lr.bounds.max.z = doc->printer.build_z + 0.01;
    CHECK_FALSE(planPrint(*doc, lr, 0.0, typed()).fits);
    const fruit::PlateFit f = fruit::checkFit(doc->part().mesh(), doc->placement(), doc->printer);
    CHECK(f.top == Approx(3.1));
    // A model 0.05 mm short of the build height fits as it is, but not with 0.10 mm added under it.
    const fruit::TriangleMesh tall = fruit::makeBox({0, 0, 0}, {10, 8, doc->printer.build_z - 0.05});
    fruit::Placement p = doc->placement();
    CHECK_FALSE(fruit::checkFit(tall, p, doc->printer).fits);
    p.height_correction = 0;
    CHECK(fruit::checkFit(tall, p, doc->printer).fits);
}

TEST_CASE("Height correction: the saved print file has the extra layers", "[gui][print][correction]") {
    const auto doc = boxOnPlate(0.10);
    const LayerResult lr = sliceLayers(*doc, {}, 0.0, fruit::JobControl::none());
    const PrintPlan p = planPrint(*doc, lr, 0.0, typed());
    REQUIRE(p.ready);
    const auto path = tmp("correct_box.goo").string();
    const SaveResult r = savePrint(*doc, lr, {}, p, path, false, fruit::JobControl::none());
    CHECK(r.written.layers == 62);
    CHECK(r.check.layers == 62);
    CHECK(r.check.checksums_ok);
    CHECK(r.volume_matches);
    CHECK(r.good);
    CHECK(r.check.volume_mm3 == Approx(248.0).epsilon(0.02));  // 10 × 8 × 3.1 mm
    const fruit::GooFileInfo back = fruit::readGoo(path);
    REQUIRE(back.layers.size() == 62);
    CHECK(back.rle[0] == back.rle[1]);
    CHECK(back.rle[1] == back.rle[2]);

    // Corrected by 0: the print is the model's own layers.
    const auto none = boxOnPlate(0.0);
    const LayerResult lr0 = sliceLayers(*none, {}, 0.0, fruit::JobControl::none());
    const PrintPlan p0 = planPrint(*none, lr0, 0.0, typed());
    const auto path0 = tmp("uncorrected_box.goo").string();
    const SaveResult r0 = savePrint(*none, lr0, {}, p0, path0, false, fruit::JobControl::none());
    CHECK(r0.written.layers == 60);
    CHECK(r0.check.volume_mm3 == Approx(240.0).epsilon(0.02));
}

TEST_CASE("Height correction: a sample at another layer height gets it in its own layers", "[gui][print][correction]") {
    const auto doc = boxOnPlate(0.10);
    const LayerResult lr = sliceLayers(*doc, {}, 0.0, fruit::JobControl::none());
    PrintSettings s;
    s.sample_path = sample003();
    s.sample = loadSample(s.sample_path);
    const PrintPlan p = planPrint(*doc, lr, 0.0, s);
    REQUIRE(p.ready);
    REQUIRE(p.reslice);
    // 3 mm at 0.03 mm is 100 layers, and 0.10 mm is 3 layers of 0.03 mm (0.09 mm).
    CHECK(p.base_layers == 3);
    CHECK(p.layers == 103);
    CHECK(p.height == Approx(3.09));
    const auto path = tmp("correct_box_003.goo").string();
    const SaveResult r = savePrint(*doc, lr, {}, p, path, false, fruit::JobControl::none());
    CHECK(r.written.layers == 103);
    CHECK(r.check.layers == 103);
    CHECK(r.check.checksums_ok);
    CHECK(r.volume_matches);
}
