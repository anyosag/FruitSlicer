// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Saved jobs: what comes back, what counts as unsaved, recent jobs.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <functional>
#include <fstream>
#include <miniz.h>
#include <nlohmann/json.hpp>

#include "Document.h"
#include "Hollows.h"
#include "JobFile.h"
#include "Print.h"
#include "Supports.h"
#include "formats/GooExport.h"
#include "formats/GooSetup.h"
#include "mesh/Primitives.h"
#include "mesh/StlIO.h"

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using namespace gui;
namespace fs = std::filesystem;

namespace {

fs::path dir() {
    const auto d = fs::temp_directory_path() / "fruit_job_tests";
    fs::create_directories(d);
    return d;
}

std::shared_ptr<Document> bracketAt(const fs::path& model) {
    fruit::writeStl(model.string(), demoModel());
    return loadDocument(model.string(), fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
}

}  // namespace

TEST_CASE("Jobs: what was decided comes back, supports included", "[gui][jobs][slow]") {
    const auto doc = bracketAt(dir() / "bracket.stl");
    fruit::Placement p = doc->placement();
    p.scale = 1.25;
    p.rotation_deg = {0, 0, 30};
    doc->edit(p);
    doc->orient().visible = {{0, 0, 1}};
    doc->orient().decided = true;
    doc->supports.choices.usual_tip = 3;
    doc->supports.choices.advanced["head_length"] = 1.4;
    auto r = std::make_shared<SupportResult>(generate(*doc, doc->supports.choices, doc->visibleInPrinter(),
                                                      fruit::JobControl::none()));
    doc->supports.result = r;
    doc->supports.result_revision = doc->revision();
    doc->supports.result_settings = doc->orient().settings;
    doc->print.settings.exposure = 2.5;
    doc->print.settings.bottom_exposure = 30;

    const auto job = (dir() / "bracket.fruitjob").string();
    saveJob(job, *doc);
    const OpenedJob o = openJob(job, fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    const Document& d = *o.doc;
    CHECK_FALSE(o.model_missing);
    CHECK(o.model_path == doc->part().path());
    CHECK(d.name == "bracket");
    CHECK(d.job_path == job);
    CHECK(meshHash(d.part().mesh()) == meshHash(doc->part().mesh()));
    CHECK(d.placement().scale == Approx(1.25));
    CHECK(d.placement().rotation_deg.z == Approx(30));
    CHECK(d.placed().bbox().max.x == Approx(doc->placed().bbox().max.x));
    REQUIRE(d.orient().visible.size() == 1);
    CHECK(d.orient().decided);
    CHECK(d.supports.choices == doc->supports.choices);
    REQUIRE(d.supports.result);
    CHECK(d.supports.fresh(d.revision(), d.orient().settings));
    CHECK(d.supports.result->tips == r->tips);
    CHECK(d.supports.result->pillars == r->pillars);
    CHECK(d.supports.result->tree_cfg.head_length == Approx(1.4));
    REQUIRE(d.supports.result->problems.size() == r->problems.size());
    for (std::size_t i = 0; i < r->problems.size(); ++i) CHECK(d.supports.result->problems[i].code == r->problems[i].code);
    CHECK(d.print.settings.exposure == 2.5);
    CHECK(d.print.settings.bottom_exposure == 30);
    CHECK_FALSE(d.unsaved());
}

TEST_CASE("Jobs: the SatelLite settings travel with the job; a moved model is noticed", "[gui][jobs]") {
    const auto model = dir() / "moving.stl";
    const auto doc = bracketAt(model);
    // A sample like SatelLite's, then gone from disk: the job keeps its header.
    fruit::GooPrintRequest req;
    req.layer_height = 0.03;
    req.exposure = 1.5;
    req.bottom_exposure = 27.5;
    const fruit::GooPrintSetup s = fruit::buildGooSetup(fruit::PrinterProfile::saturn4Ultra16k(), req);
    const auto sample = (dir() / "sample.goo").string();
    fruit::exportGoo(fruit::makeBox({-2, -2, 0}, {2, 2, 0.3}), s.header, s.grid, fruit::GooExportOptions{}, sample);
    doc->print.settings.sample_path = sample;
    doc->print.settings.sample = loadSample(sample);
    const auto job = (dir() / "moving.fruitjob").string();
    saveJob(job, *doc);
    fs::remove(sample);
    fs::remove(model);

    const OpenedJob o = openJob(job, fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    CHECK(o.model_missing);
    CHECK(o.model_path == model.string());
    REQUIRE(o.doc->print.settings.sample);
    CHECK(o.doc->print.settings.sample->print.exposure == Approx(1.5));
    CHECK(o.doc->print.settings.sample->layer_height == Approx(0.03));
    CHECK_FALSE(o.doc->supports.result);  // none were made
    CHECK(o.doc->part().mesh().faces.size() == demoModel().faces.size());
}

TEST_CASE("Jobs: the SatelLite settings are kept even when that file went before saving", "[gui][jobs]") {
    const auto doc = bracketAt(dir() / "gone_sample.stl");
    fruit::GooPrintRequest req;
    req.layer_height = 0.03;
    req.exposure = 1.5;
    req.bottom_exposure = 27.5;
    const fruit::GooPrintSetup s = fruit::buildGooSetup(fruit::PrinterProfile::saturn4Ultra16k(), req);
    const auto sample = (dir() / "gone_sample.goo").string();
    fruit::exportGoo(fruit::makeBox({-2, -2, 0}, {2, 2, 0.3}), s.header, s.grid, fruit::GooExportOptions{}, sample);
    doc->print.settings.sample_path = sample;
    doc->print.settings.sample = loadSample(sample);
    fs::remove(sample);  // moved away before the job is saved
    const auto job = (dir() / "gone_sample.fruitjob").string();
    saveJob(job, *doc);
    const OpenedJob o = openJob(job, fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    REQUIRE(o.doc->print.settings.sample);
    CHECK(o.doc->print.settings.sample->print.exposure == Approx(1.5));
    CHECK(o.doc->print.settings.sample->layer_height == Approx(0.03));
}

TEST_CASE("Jobs: damaged and newer files are refused plainly", "[gui][jobs]") {
    const auto junk = (dir() / "junk.fruitjob").string();
    std::ofstream(junk) << "not a zip";
    CHECK_THROWS_WITH(openJob(junk, fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none()),
                      ContainsSubstring("not a FruitSlicer job"));
    CHECK_THROWS(openJob((dir() / "missing.fruitjob").string(), fruit::PrinterProfile::saturn4Ultra16k(),
                         fruit::JobControl::none()));
}

TEST_CASE("Jobs: what counts as a change since saving", "[gui][jobs]") {
    const auto doc = bracketAt(dir() / "changes.stl");
    CHECK(doc->unsaved());  // never saved
    doc->saved = doc->stamp();
    CHECK_FALSE(doc->unsaved());
    fruit::Placement p = doc->placement();
    p.rotation_deg = {0, 0, 15};
    doc->edit(p);
    CHECK(doc->unsaved());
    auto changes = changesSince(*doc->saved, doc->stamp());
    REQUIRE(changes.size() == 1);
    CHECK_THAT(changes[0], ContainsSubstring("turned"));
    doc->saved = doc->stamp();
    doc->print.settings.exposure = 2;
    changes = changesSince(*doc->saved, doc->stamp());
    REQUIRE(changes.size() == 1);
    CHECK_THAT(changes[0], ContainsSubstring("print settings"));
}

TEST_CASE("Jobs: recent jobs, newest first, each once", "[gui][jobs]") {
    const auto file = (dir() / "data" / "recent.json").string();
    fs::remove_all(dir() / "data");
    CHECK(loadRecentJobs(file).empty());
    for (int i = 0; i < 10; ++i) rememberJob(file, {"/jobs/" + std::to_string(i) + ".fruitjob", "job " + std::to_string(i), "", ""});
    rememberJob(file, {"/jobs/5.fruitjob", "job 5", "supports ready", "now"});
    const auto jobs = loadRecentJobs(file);
    REQUIRE(jobs.size() == 8);
    CHECK(jobs[0].path == "/jobs/5.fruitjob");
    CHECK(jobs[0].state == "supports ready");
    CHECK(jobs[1].path == "/jobs/9.fruitjob");
    for (std::size_t i = 1; i < jobs.size(); ++i) CHECK(jobs[i].path != jobs[0].path);
}

TEST_CASE("Jobs: tip edits come back, applied to the planner's tips", "[gui][jobs][slow]") {
    const auto doc = bracketAt(dir() / "edited.stl");
    auto r = std::make_shared<SupportResult>(generate(*doc, doc->supports.choices, {}, fruit::JobControl::none()));
    const fruit::Transform to_model = doc->part().transform().inverse();
    TipEdit del;
    del.kind = TipEdit::Kind::Delete;
    del.at = to_model.apply(r->points[0].pos);
    del.radius = kDeletedZone;
    TipEdit add;
    add.kind = TipEdit::Kind::Add;
    add.at = to_model.apply(r->points[1].pos + fruit::Vec3{1.5, 0, 0});
    add.normal = {0, 0, -1};
    add.diameter = 0.3;
    doc->supports.edits = {del, add};
    // Planned again around the edits, as regenerating does: the job keeps both.
    r = std::make_shared<SupportResult>(
        generate(*doc, doc->supports.choices, {}, fruit::JobControl::none(), doc->supports.edits));
    REQUIRE(r->plan_edits == doc->supports.edits);
    doc->supports.result = r;
    doc->supports.result_revision = doc->revision();
    doc->supports.result_settings = doc->orient().settings;
    const auto job = (dir() / "edited.fruitjob").string();
    saveJob(job, *doc);
    const OpenedJob o = openJob(job, fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    const SupportState& s = o.doc->supports;
    CHECK(s.edits == doc->supports.edits);
    REQUIRE(s.result);
    CHECK(s.fresh(o.doc->revision(), o.doc->orient().settings));
    CHECK(s.result->points.size() == r->points.size());
    CHECK(s.result->planned.size() == r->planned.size());
    CHECK(s.result->plan_edits == r->plan_edits);
    std::size_t yours = 0;
    for (const auto& p : s.result->points) yours += p.category == fruit::PointCategory::Manual;
    CHECK(yours == 1);
    CHECK_FALSE(o.doc->unsaved());
}

TEST_CASE("Jobs: the hollows come back, and the supports made for the drilled model", "[gui][jobs][hollow]") {
    // A block with a sealed hollow, drained by two holes placed by hand.
    fruit::TriangleMesh m = fruit::makeBox({0, 0, 0}, {20, 20, 12});
    fruit::TriangleMesh v = fruit::makeBox({4, 4, 3}, {16, 16, 9});
    for (auto& f : v.faces) std::swap(f[1], f[2]);
    m.append(v);
    const auto model = dir() / "sealed.stl";
    fruit::writeStl(model.string(), m);
    const auto doc = loadDocument(model.string(), fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    doc->hollow.choices.auto_holes = false;
    doc->hollow.choices.hole_diameter = 2.5;
    doc->hollow.manual = {{{10, 10, 0}, {0, 0, 1}, 2.5, false}, {{10, 10, 12}, {0, 0, -1}, 2.5, false}};
    doc->hollow.applied = true;
    auto r = std::make_shared<SupportResult>(generate(*doc, doc->supports.choices, {}, fruit::JobControl::none()));
    REQUIRE(r->hollow);
    REQUIRE(r->hollow->drains == std::vector<char>{1});
    doc->supports.result = r;
    doc->supports.result_revision = doc->revision();
    doc->supports.result_settings = doc->supportSettings();

    const auto job = (dir() / "sealed.fruitjob").string();
    saveJob(job, *doc);
    const OpenedJob o = openJob(job, fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    const Document& d = *o.doc;
    CHECK(d.hollow.applied);
    CHECK(d.hollow.choices == doc->hollow.choices);
    CHECK(d.hollow.manual == doc->hollow.manual);
    REQUIRE(d.supports.result);
    CHECK(d.supports.fresh(d.revision(), d.supportSettings()));
    CHECK(d.supports.result->tips == r->tips);
    // Its layers and print file use the drilled model, as before saving.
    REQUIRE(d.supports.result->hollow);
    CHECK(d.supports.result->hollow->holes == doc->hollow.manual);
    CHECK(d.supports.result->hollow->drains == std::vector<char>{1});
    CHECK(d.hollow.planFresh(d.revision()));
    CHECK(hollowsToFix(d) == 0);
    CHECK_FALSE(d.unsaved());

    // A change to them is one to save.
    Document& e = *o.doc;
    e.hollow.choices.fix = HollowChoices::Fix::Fill;
    const auto changes = changesSince(*e.saved, e.stamp());
    REQUIRE(changes.size() == 1);
    CHECK_THAT(changes[0], ContainsSubstring("hollows"));
}

TEST_CASE("Jobs: the height correction is saved with the print settings; older jobs get the usual one",
          "[gui][jobs]") {
    const auto doc = bracketAt(dir() / "correction.stl");
    CHECK(doc->print.settings.height_correction < 0);  // not set: the typical value
    CHECK(heightCorrection(doc->print.settings) == Approx(0.10));
    fruit::Placement p = doc->placement();
    p.on_plate = true;
    doc->edit(p);
    doc->print.settings.height_correction = 0.15;
    const auto job = (dir() / "correction.fruitjob").string();
    saveJob(job, *doc);
    const OpenedJob o = openJob(job, fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    CHECK(o.doc->placement().on_plate);
    CHECK(o.doc->print.settings.height_correction == Approx(0.15));

    // Rewrites the saved job's job.json with `change`.
    auto rewritten = [&](const char* name, const std::function<void(nlohmann::json&)>& change) {
        mz_zip_archive in{};
        REQUIRE(mz_zip_reader_init_file(&in, job.c_str(), 0));
        std::vector<std::pair<std::string, std::string>> files;
        for (mz_uint k = 0; k < mz_zip_reader_get_num_files(&in); ++k) {
            char file[256];
            mz_zip_reader_get_filename(&in, k, file, sizeof file);
            std::size_t n = 0;
            void* data = mz_zip_reader_extract_to_heap(&in, k, &n, 0);
            REQUIRE(data);
            files.emplace_back(file, std::string(static_cast<const char*>(data), n));
            mz_free(data);
        }
        mz_zip_reader_end(&in);
        const auto out_path = (dir() / name).string();
        mz_zip_archive out{};
        REQUIRE(mz_zip_writer_init_file(&out, out_path.c_str(), 0));
        for (auto& [file, bytes] : files) {
            if (file == "job.json") {
                auto j = nlohmann::json::parse(bytes);
                change(j);
                bytes = j.dump();
            }
            REQUIRE(mz_zip_writer_add_mem(&out, file.c_str(), bytes.data(), bytes.size(), MZ_DEFAULT_COMPRESSION));
        }
        REQUIRE(mz_zip_writer_finalize_archive(&out));
        mz_zip_writer_end(&out);
        return openJob(out_path, fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    };
    // A job saved before it existed: not set, so the typical 0.10 mm.
    const OpenedJob older = rewritten("correction_old.fruitjob", [](nlohmann::json& j) {
        j.at("print").erase("height_correction");
    });
    CHECK(older.doc->placement().on_plate);
    CHECK(older.doc->print.settings.height_correction < 0);
    CHECK(heightCorrection(older.doc->print.settings) == Approx(0.10));
    // Saved while it sat with the placement: taken from there.
    const OpenedJob moved = rewritten("correction_placement.fruitjob", [](nlohmann::json& j) {
        j.at("print").erase("height_correction");
        nlohmann::json& pl = j.contains("parts") ? j["parts"][0]["placement"] : j["placement"];
        pl["height_correction"] = 0.2;
    });
    CHECK(moved.doc->print.settings.height_correction == Approx(0.2));
}
