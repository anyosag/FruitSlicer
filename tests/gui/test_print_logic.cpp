// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The Print step's logic: settings, the plan, saving and reading back.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <filesystem>
#include <string>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "Document.h"
#include "Layers.h"
#include "Print.h"
#include "Supports.h"
#include "layers/AdaptiveLayers.h"
#include "mesh/Primitives.h"
#include "mesh/StlIO.h"

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using namespace gui;

namespace {

// A folder of this test process's own: ctest runs the cases side by side,
// and a shared box.stl could be read while another case rewrites it.
std::filesystem::path tmp(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / ("fruit_print_tests_" + std::to_string(getpid()));
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
    doc->edit(p);
    doc->print.settings.height_correction = correction;  // the job's print setting, from its resin
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
    lifted->print.settings.height_correction = 0.3;
    const LayerResult raft = sliceLayers(*lifted, {}, 1.0, fruit::JobControl::none());
    CHECK(raft.layers->size() == 160);
    CHECK(raft.base_layers == 0);
    CHECK(raft.raft_layers == 20);
}

TEST_CASE("Height correction: a taller print still has to fit the build height", "[gui][print][correction]") {
    const auto doc = boxOnPlate(0.10);
    LayerResult lr = sliceLayers(*doc, {}, 0.0, fruit::JobControl::none());
    // Its layers are the print's height: 0.10 mm more than the model.
    CHECK(lr.bounds.max.z == Approx(3.1));
    CHECK(fruit::checkFit(doc->part().mesh(), doc->placement(), doc->printer).top == Approx(3.0));
    CHECK(planPrint(*doc, lr, 0.0, typed()).fits);
    lr.bounds.max.z = doc->printer.build_z + 0.01;
    CHECK_FALSE(planPrint(*doc, lr, 0.0, typed()).fits);
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

TEST_CASE("Height correction: a print setting; changing it cuts the layers again, not the supports",
          "[gui][print][correction]") {
    const auto doc = boxOnPlate(0.10);
    auto sup = std::make_shared<SupportResult>();
    sup->choices = doc->supports.choices;
    sup->edits = doc->supports.edits;
    doc->supports.result = sup;
    doc->supports.result_revision = doc->revision();
    doc->supports.result_settings = doc->supportSettings();
    doc->layers.result = std::make_shared<const LayerResult>(sliceLayers(*doc, {}, 0.0, fruit::JobControl::none()));
    doc->layers.for_supports = doc->supports.result;
    REQUIRE(doc->layersFresh());
    doc->saved = doc->stamp();
    const int rev = doc->revision();

    doc->print.settings.height_correction = 0.15;
    CHECK(doc->revision() == rev);  // the supports are still up to date
    CHECK(doc->supports.fresh(doc->revision(), doc->supportSettings()));
    CHECK_FALSE(doc->layersFresh());  // 3 layers now, the layers were cut with 2
    CHECK(doc->unsaved());

    doc->print.settings.height_correction = -1;  // not set: the typical 0.10 mm, 2 layers again
    CHECK(heightCorrection(doc->print.settings) == Approx(0.10));
    CHECK(doc->layersFresh());

    // With a raft nothing is added, so no setting puts the layers out of date.
    fruit::Placement p = doc->placement();
    p.on_plate = false;
    doc->edit(p);
    CHECK(baseLayers(*doc, kLayerHeight) == 0);
    doc->print.settings.height_correction = 0.4;
    CHECK(baseLayers(*doc, kLayerHeight) == 0);
}

TEST_CASE("Adaptive layers: saving writes merged layers and reads back good", "[gui][print][adaptive]") {
    const auto doc = box();
    const LayerResult lr = sliceLayers(*doc, {}, 0.0, fruit::JobControl::none());  // the box 5 mm up: 160 layers of 0.05 mm
    PrintSettings off = typed();
    const PrintPlan p_off = planPrint(*doc, lr, 0.0, off);
    REQUIRE(p_off.ready);
    CHECK(p_off.adaptive == 1);
    const SaveResult thin = savePrint(*doc, lr, {}, p_off, tmp("adaptive_thin.goo").string(), false, fruit::JobControl::none());
    REQUIRE(thin.good);
    CHECK(thin.written.merged_layers == 0);
    CHECK_FALSE(fruit::readGoo(thin.path).header.print.advance_mode);

    PrintSettings s = typed();
    s.adaptive = 4;      // up to 0.2 mm
    s.light_depth = 0.12;
    const PrintPlan plan = planPrint(*doc, lr, 0.0, s);
    CHECK(plan.adaptive == 4);
    CHECK(plan.light_depth == Approx(0.12));
    CHECK(plan.layers == 160);  // (the box stands 5 mm up on a raft) what is shown before saving: thin layers throughout
    const auto path = tmp("adaptive_box.goo").string();
    const SaveResult r = savePrint(*doc, lr, {}, plan, path, false, fruit::JobControl::none());
    CHECK(r.good);
    CHECK(r.volume_matches);
    CHECK(r.written.layers < 160);
    CHECK(r.written.merged_layers > 0);
    CHECK(r.written.thin_layers == 160);
    CHECK(r.check.layers == r.written.layers);
    const fruit::GooFileInfo f = fruit::readGoo(path);
    CHECK(f.header.print.advance_mode);
    CHECK(f.layers.back().z == Approx(8.0).margin(1e-4));
    CHECK(f.header.volume == Approx(thin.written.volume_mm3).epsilon(0.005));
    CHECK(r.check.volume_mm3 == Approx(thin.check.volume_mm3).epsilon(0.005));
    CHECK(r.written.print_time_s < thin.written.print_time_s);
    // A merged layer's exposure is from the formula with this light depth.
    bool seen = false;
    for (std::size_t k = 1; k < f.layers.size(); ++k) {
        const double thick = f.layers[k].z - f.layers[k - 1].z;
        if (thick < 0.06) continue;
        seen = true;
        CHECK(f.layers[k].exposure == Approx(fruit::adaptiveExposure(2.5, 0.05, thick, 0.12)).margin(0.011));
    }
    CHECK(seen);
}

TEST_CASE("Adaptive layers: on the plate, the height correction's layers stay thin", "[gui][print][adaptive]") {
    const auto doc = boxOnPlate(0.15);  // 3 layers of 0.05 mm under the box
    const LayerResult lr = sliceLayers(*doc, {}, 0.0, fruit::JobControl::none());
    REQUIRE(lr.base_layers == 3);
    PrintSettings s = typed();
    s.bottom_layers = 1;
    s.transition_layers = 0;  // so only the height correction protects layers here
    s.adaptive = 4;
    const PrintPlan plan = planPrint(*doc, lr, 0.0, s);
    REQUIRE(plan.ready);
    const auto path = tmp("adaptive_plate.goo").string();
    const SaveResult r = savePrint(*doc, lr, {}, plan, path, false, fruit::JobControl::none());
    CHECK(r.good);
    const fruit::GooFileInfo f = fruit::readGoo(path);
    REQUIRE(f.layers.size() > 4);
    for (std::size_t i = 0; i < 3; ++i) CHECK(f.layers[i].z == Approx(0.05 * static_cast<double>(i + 1)).margin(1e-4));
    CHECK(f.layers[3].z - f.layers[2].z == Approx(0.10).margin(1e-4));  // merging starts after them, 0.05 -> 0.1
    CHECK(f.layers.back().z == Approx(3.15).margin(1e-4));
    CHECK(r.written.merged_layers > 0);
    // The model's footprint is the same all the way up: the first merged layer is as lit as a thin one.
    CHECK(r.check.volume_mm3 == Approx(10 * 8 * 3.15).epsilon(0.02));
}

TEST_CASE("Adaptive layers: a sample at another layer height cuts the layers again and merges them",
          "[gui][print][adaptive]") {
    const auto doc = boxOnPlate(0.10);
    const LayerResult lr = sliceLayers(*doc, {}, 0.0, fruit::JobControl::none());
    PrintSettings s;
    s.sample_path = sample003();
    s.sample = loadSample(s.sample_path);
    s.adaptive = 8;  // 0.24 mm is over the limit: 0.12 mm is the thickest
    const PrintPlan p = planPrint(*doc, lr, 0.0, s);
    REQUIRE(p.reslice);
    CHECK(p.adaptive == 4);
    CHECK(p.base_layers == 3);
    const auto path = tmp("adaptive_003.goo").string();
    const SaveResult r = savePrint(*doc, lr, {}, p, path, false, fruit::JobControl::none());
    CHECK(r.good);
    CHECK(r.written.thin_layers == 103);
    CHECK(r.written.layers < 103);
    const fruit::GooFileInfo f = fruit::readGoo(path);
    CHECK(f.header.layer_height == Approx(0.03f));
    CHECK(f.layers.back().z == Approx(3.09).margin(1e-4));
    const auto counts = fruit::gooThicknessCounts(f.header, f.layers);
    CHECK(counts.back().first == Approx(0.12).margin(1e-4));
    // The base layers (and the sample's own 6 bottom and 6 transition layers) are thin.
    for (std::size_t i = 0; i < 12; ++i) CHECK(f.layers[i].z == Approx(0.03 * static_cast<double>(i + 1)).margin(1e-4));
}

namespace {

// What the estimate says is what saving writes: layers, thin layers, merged layers and print time.
void checkEstimateIsWhatSaves(const Document& doc, const LayerResult& lr, const fruit::TriangleMesh& supports,
                              const PrintPlan& plan, const char* name) {
    const PrintEstimate e = estimatePrint(doc, lr, supports, plan, fruit::JobControl::none());
    const auto path = tmp(name).string();
    const SaveResult r = savePrint(doc, lr, supports, plan, path, false, fruit::JobControl::none());
    REQUIRE(r.good);
    CHECK(e.layers == r.written.layers);
    CHECK(e.thin_layers == r.written.thin_layers);
    CHECK(e.merged_layers == r.written.merged_layers);
    CHECK(static_cast<std::uint32_t>(e.print_time_s) == r.written.print_time_s);
    // And what the file itself holds.
    const fruit::GooFileInfo f = fruit::readGoo(path);
    CHECK(f.layers.size() == e.layers);
    CHECK(f.header.print_time == r.written.print_time_s);
}

}  // namespace

TEST_CASE("Adaptive estimate: the job's own layers give what saving writes", "[gui][print][adaptive][estimate]") {
    const auto doc = box();
    const LayerResult lr = sliceLayers(*doc, {}, 0.0, fruit::JobControl::none());
    PrintSettings s = typed();
    s.adaptive = 4;
    s.light_depth = 0.12;
    const PrintPlan plan = planPrint(*doc, lr, 0.0, s);
    REQUIRE(plan.ready);
    const PrintEstimate e = estimatePrint(*doc, lr, {}, plan, fruit::JobControl::none());
    CHECK(e.merged_layers > 0);
    CHECK(e.layers < e.thin_layers);
    CHECK(e.thin_layers == 160);
    CHECK(e.print_time_s < plan.print_time_s);
    checkEstimateIsWhatSaves(*doc, lr, {}, plan, "estimate_same.goo");
}

TEST_CASE("Adaptive estimate: a re-sliced job (a 0.03 mm sample)", "[gui][print][adaptive][estimate]") {
    const auto doc = boxOnPlate(0.10);
    const LayerResult lr = sliceLayers(*doc, {}, 0.0, fruit::JobControl::none());
    PrintSettings s;
    s.sample_path = sample003();
    s.sample = loadSample(s.sample_path);
    s.adaptive = 8;
    const PrintPlan plan = planPrint(*doc, lr, 0.0, s);
    REQUIRE(plan.reslice);
    checkEstimateIsWhatSaves(*doc, lr, {}, plan, "estimate_reslice.goo");
}

TEST_CASE("Adaptive estimate: height-correction layers on the plate stay thin", "[gui][print][adaptive][estimate]") {
    const auto doc = boxOnPlate(0.15);
    const LayerResult lr = sliceLayers(*doc, {}, 0.0, fruit::JobControl::none());
    PrintSettings s = typed();
    s.bottom_layers = 1;
    s.transition_layers = 0;
    s.adaptive = 4;
    const PrintPlan plan = planPrint(*doc, lr, 0.0, s);
    REQUIRE(plan.ready);
    const PrintEstimate e = estimatePrint(*doc, lr, {}, plan, fruit::JobControl::none());
    CHECK(e.thin_layers == 63);
    CHECK(e.merged_layers > 0);
    checkEstimateIsWhatSaves(*doc, lr, {}, plan, "estimate_plate.goo");
}

TEST_CASE("Adaptive estimate: off, it is the thin numbers and nothing merges", "[gui][print][adaptive][estimate]") {
    const auto doc = box();
    const LayerResult lr = sliceLayers(*doc, {}, 0.0, fruit::JobControl::none());
    const PrintPlan plan = planPrint(*doc, lr, 0.0, typed());
    REQUIRE(plan.adaptive == 1);
    const PrintEstimate e = estimatePrint(*doc, lr, {}, plan, fruit::JobControl::none());
    CHECK(e.layers == plan.layers);
    CHECK(e.thin_layers == plan.layers);
    CHECK(e.merged_layers == 0);
    CHECK(e.print_time_s == Approx(plan.print_time_s));
    checkEstimateIsWhatSaves(*doc, lr, {}, plan, "estimate_off.goo");
}

TEST_CASE("Adaptive estimate: it can be cancelled", "[gui][print][adaptive][estimate]") {
    const auto doc = box();
    const LayerResult lr = sliceLayers(*doc, {}, 0.0, fruit::JobControl::none());
    PrintSettings s = typed();
    s.adaptive = 4;
    const PrintPlan plan = planPrint(*doc, lr, 0.0, s);
    const fruit::JobControl ctl;
    ctl.cancel();
    CHECK_THROWS_AS(estimatePrint(*doc, lr, {}, plan, ctl), fruit::Cancelled);
}

TEST_CASE("Adaptive estimate: the document keeps one for the same layers and settings only",
          "[gui][print][adaptive][estimate]") {
    const auto doc = box();
    const LayerResult lr = sliceLayers(*doc, {}, 0.0, fruit::JobControl::none());
    doc->layers.result = std::make_shared<const LayerResult>(lr);
    doc->print.settings = typed();
    doc->print.settings.adaptive = 4;
    CHECK(doc->print.estimateFor(doc->layers.result) == nullptr);
    auto e = std::make_shared<AdaptiveEstimate>();
    e->from = doc->layers.result;
    e->edits = doc->print.settings;
    e->sample_path = doc->print.settings.sample_path;
    e->numbers.layers = 52;
    doc->print.estimate = e;
    REQUIRE(doc->print.estimateFor(doc->layers.result) != nullptr);
    CHECK(doc->print.estimateFor(doc->layers.result)->layers == 52);
    doc->print.settings.light_depth = 0.2;  // another setting: stale
    CHECK(doc->print.estimateFor(doc->layers.result) == nullptr);
    doc->print.settings.light_depth = -1;
    CHECK(doc->print.estimateFor(doc->layers.result) != nullptr);
    doc->print.settings.sample_path = "other.goo";
    CHECK(doc->print.estimateFor(doc->layers.result) == nullptr);
    doc->print.settings.sample_path.clear();
    doc->layers.result = std::make_shared<const LayerResult>(lr);  // cut again: stale
    CHECK(doc->print.estimateFor(doc->layers.result) == nullptr);
}

TEST_CASE("Adaptive estimate: the bands say where each layer height is used", "[gui][print][adaptive][estimate]") {
    const auto doc = box();  // lifted 5 mm: only the model's box is above that
    const LayerResult lr = sliceLayers(*doc, {}, 0.0, fruit::JobControl::none());
    PrintSettings s = typed();
    s.adaptive = 4;
    const PrintPlan plan = planPrint(*doc, lr, 0.0, s);
    REQUIRE(plan.ready);
    const PrintEstimate e = estimatePrint(*doc, lr, {}, plan, fruit::JobControl::none());
    REQUIRE(e.bands.size() > 1);
    CHECK(e.thin_height == Approx(plan.layer_height));
    // Bottom to top with no gaps, from the plate to the top of the print.
    CHECK(e.bands.front().z0 == Approx(0.0));
    CHECK(e.bands.back().z1 == Approx(static_cast<double>(e.thin_layers) * e.thin_height));
    std::size_t layers = 0;
    bool thick = false;
    for (std::size_t k = 0; k < e.bands.size(); ++k) {
        const LayerBand& b = e.bands[k];
        CHECK((b.count == 1 || b.count == 2 || b.count == 4));
        CHECK(b.z1 > b.z0);
        if (k > 0) {
            CHECK(b.z0 == Approx(e.bands[k - 1].z1));
            CHECK(b.count != e.bands[k - 1].count);  // neighbours of one height are one band
        }
        layers += static_cast<std::size_t>(std::lround((b.z1 - b.z0) / (b.count * e.thin_height)));
        thick = thick || b.count == 4;
    }
    CHECK(layers == e.layers);
    CHECK(thick);
    CHECK(e.bands.front().count == 1);  // the bottom layers are never merged
    // Without adaptive layers there are no bands.
    PrintSettings off = typed();
    const PrintPlan plain = planPrint(*doc, lr, 0.0, off);
    CHECK(estimatePrint(*doc, lr, {}, plain, fruit::JobControl::none()).bands.empty());
}
