// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Supports for each part: its own settings, its own raft or one it shares,
// standing on the plate beside parts on rafts, and saved in the job.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <filesystem>
#include <map>

#include <miniz.h>
#include <nlohmann/json.hpp>

#include "Document.h"
#include "JobFile.h"
#include "Print.h"
#include "Supports.h"
#include "mesh/ModelLoad.h"
#include "mesh/Primitives.h"

using namespace gui;
using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

const fruit::PrinterProfile kPrinter = fruit::PrinterProfile::saturn4Ultra16k();

NewPart partOf(const fruit::TriangleMesh& m, const std::string& name, fruit::Vec2 at, bool on_plate = false) {
    fruit::Placement p;
    p.offset = at;
    p.on_plate = on_plate;
    return NewPart{name, "", makePartModel(fruit::prepareModel(m)), p};
}

std::shared_ptr<Document> jobOf(std::vector<NewPart> parts) {
    auto doc = std::make_shared<Document>();
    doc->name = "per part";
    doc->printer = kPrinter;
    doc->replaceParts(std::move(parts));
    doc->clearHistory();
    return doc;
}

fruit::TriangleMesh cube(double s) { return fruit::makeBox({0, 0, 0}, {s, s, s}); }

// Two 10 mm cubes `gap` mm apart along X.
std::shared_ptr<Document> twoCubes(double gap) {
    return jobOf({partOf(cube(10), "a", {-(5 + gap / 2), 0}), partOf(cube(10), "b", {5 + gap / 2, 0})});
}

SupportResult made(const Document& doc) { return generate(doc, doc.supports.setup(), {}, fruit::JobControl::none()); }

// The most common tip size among part `id`'s tips.
double usualSize(const SupportResult& r, int id) {
    const SupportResult::PartMade* pm = r.partMade(id);
    REQUIRE(pm);
    std::map<double, int> n;
    for (std::size_t i = pm->first; i < pm->first + pm->tips; ++i) ++n[std::round(r.points[i].tip_diameter * 100) / 100];
    double best = 0;
    int most = -1;
    for (const auto& [d, k] : n)
        if (k > most) most = k, best = d;
    return best;
}

const SupportCheck* check(const SupportResult& r, const std::string& title) {
    for (const SupportCheck& c : r.checks)
        if (c.title.find(title) != std::string::npos) return &c;
    return nullptr;
}

fs::path dir() {
    const auto d = fs::temp_directory_path() / "fruit_per_part_tests";
    fs::create_directories(d);
    return d;
}

}  // namespace

TEST_CASE("Supports per part: the job's values, a part's own over them, and tidy", "[gui][supports][parts]") {
    SupportSetup s;
    s.choices.how_many = 2;
    s.choices.advanced["head_length"] = 1.5;
    PartChoices& o = s.own[4];
    o.usual_tip = 3;
    o.advanced["head_length"] = 2.0;
    o.advanced["route"] = 20;
    const SupportChoices four = s.forPart(4), five = s.forPart(5);
    CHECK(four.how_many == 2);  // the job's
    CHECK(four.usual_tip == 3);  // its own
    CHECK(four.advanced.at("head_length") == Approx(2.0));
    CHECK(four.advanced.at("route") == Approx(20));
    CHECK(five.usual_tip == s.choices.usual_tip);
    CHECK(five.advanced.at("head_length") == Approx(1.5));
    // A value equal to the job's is not kept, and a part left with none goes.
    o.how_many = 2;
    o.advanced["head_length"] = 1.5;
    s.shared = {{4, 5, 4}, {6}};
    tidy(s);
    REQUIRE(s.own.count(4));
    CHECK_FALSE(s.own[4].how_many);
    CHECK_FALSE(s.own[4].advanced.count("head_length"));
    CHECK(s.own[4].usual_tip == 3);
    CHECK(s.shared == std::vector<std::vector<int>>{{4, 5}});  // each part once; a raft of one is no sharing
    s.own[4].usual_tip.reset();
    s.own[4].advanced.clear();
    tidy(s);
    CHECK(s.own.empty());
    // Tip counts are made for every stop: How many at the centre everywhere.
    s.own[5].how_many = 4;
    const SupportSetup c = centred(s);
    CHECK(c.choices.how_many == kHowManyCentre);
    CHECK(c.own.empty());
}

TEST_CASE("Supports per part: each part gets its own tips and its own raft, apart", "[gui][supports][parts][slow]") {
    const auto doc = twoCubes(40);
    const int a = doc->parts()[0].id(), b = doc->parts()[1].id();
    const SupportResult r = made(*doc);
    REQUIRE(r.rafts.size() == 2);
    CHECK(r.rafts[0].parts == std::vector<int>{a});
    CHECK(r.rafts[1].parts == std::vector<int>{b});
    for (const auto& g : r.rafts) {
        CHECK_FALSE(g.touching);
        CHECK(g.pieces == 1);
        CHECK(g.top > 0);
        CHECK_FALSE(g.pillars.empty());
    }
    // Each part's tips are on it, in the job's order.
    REQUIRE(r.parts.size() == 2);
    CHECK(r.parts[0].first == 0);
    CHECK(r.parts[1].first == r.parts[0].tips);
    CHECK(r.parts[0].tips + r.parts[1].tips == r.points.size());
    for (const auto& pm : r.parts) {
        CHECK(pm.tips > 0);
        CHECK(pm.trunks > 0);
        CHECK(pm.ml > 0);
        const fruit::BBox3 box = doc->findPart(pm.part)->box();
        for (std::size_t i = pm.first; i < pm.first + pm.tips; ++i) {
            CHECK(r.points[i].pos.x >= box.min.x - 0.5);
            CHECK(r.points[i].pos.x <= box.max.x + 0.5);
            CHECK(r.partOfTip(i) == &pm);
        }
    }
    CHECK(r.plans.size() == 2);
    CHECK(r.validation.floating.empty());
    CHECK(r.validation.first_layer_islands == 2);  // two rafts, each one piece
    const SupportCheck* c = check(r, "Each raft comes off in one piece");
    REQUIRE(c);
    CHECK(c->ok);
    CHECK(r.thinnestRaft() == Approx(r.rafts[0].top));
    // The Raft page's picture of one raft: its trunks only.
    CHECK(raftTreeOf(r, 1).pillars.size() == r.rafts[1].pillars.size());
}

TEST_CASE("Supports per part: parts that share a raft by choice stand on one, in one piece",
          "[gui][supports][parts][slow]") {
    const auto doc = twoCubes(12);
    const int a = doc->parts()[0].id(), b = doc->parts()[1].id();
    doc->supports.shared = {{a, b}};
    const SupportResult r = made(*doc);
    REQUIRE(r.rafts.size() == 1);
    CHECK(r.rafts[0].parts == std::vector<int>{a, b});
    CHECK_FALSE(r.rafts[0].touching);  // by choice
    CHECK(r.validation.first_layer_islands == 1);
    const SupportCheck* c = check(r, "Raft comes off in one piece");
    REQUIRE(c);
    CHECK(c->ok);
    // Both parts' trunks are under the one raft.
    CHECK(r.rafts[0].pillars.size() >= r.parts[0].trunks + r.parts[1].trunks);
    // Separate again: two rafts.
    doc->supports.shared.clear();
    CHECK(made(*doc).rafts.size() == 2);
}

TEST_CASE("Supports per part: rafts that would meet are one, and say why", "[gui][supports][parts][slow]") {
    const auto doc = twoCubes(1);
    const SupportResult r = made(*doc);
    REQUIRE(r.rafts.size() == 1);
    CHECK(r.rafts[0].parts.size() == 2);
    CHECK(r.rafts[0].touching);
    CHECK(r.validation.first_layer_islands == 1);
}

TEST_CASE("Supports per part: a part's own settings are its alone", "[gui][supports][parts][slow]") {
    const auto doc = twoCubes(40);
    const int a = doc->parts()[0].id(), b = doc->parts()[1].id();
    // A lifted cube's underside starts in mid-air: its tips are the size for that, 0.6 mm at the job's
    // Tip size and a size smaller with 0.3 mm.
    doc->supports.own[b].usual_tip = 1;  // 0.3 mm
    const SupportResult r = made(*doc);
    CHECK(usualSize(r, a) == Approx(0.6));
    CHECK(usualSize(r, b) == Approx(0.4));
    CHECK(r.partMade(a)->tree_cfg.max_route_length == Approx(r.partMade(b)->tree_cfg.max_route_length));
    // How many of its own: the same cube, more tips.
    doc->supports.own.clear();
    doc->supports.own[b].how_many = kQuickStops;
    const SupportResult more = made(*doc);
    CHECK(more.partMade(b)->tips > more.partMade(a)->tips);
    // The counts under the stops: each part's, and the job's with a part keeping its own stop.
    const auto counts = partTipCounts(*doc, doc->supports.setup(), {}, fruit::JobControl::none());
    REQUIRE(counts.size() == 2);
    CHECK(counts.at(a)[kHowManyCentre - 1] == more.partMade(a)->tips);
    CHECK(counts.at(b)[kQuickStops - 1] == more.partMade(b)->tips);
    const TipCounts job = jobTipCounts(counts, doc->supports.setup());
    for (std::size_t k = 0; k < kQuickStops; ++k) CHECK(job[k] == counts.at(a)[k] + counts.at(b)[kQuickStops - 1]);
    CHECK(job[kHowManyCentre - 1] == more.tips);
}

TEST_CASE("Supports per part: a part on the plate beside a part on a raft", "[gui][supports][parts][slow]") {
    const auto doc = jobOf({partOf(cube(10), "a", {-20, 0}), partOf(cube(10), "b", {20, 0}, true)});
    const int a = doc->parts()[0].id(), b = doc->parts()[1].id();
    REQUIRE(doc->findPart(b)->placement().on_plate);
    CHECK(doc->anyOnPlate());
    CHECK_FALSE(doc->allOnPlate());
    CHECK(baseLayers(*doc, kLayerHeight) > 0);  // the height correction, for the part on the plate
    const SupportResult r = made(*doc);
    REQUIRE(r.rafts.size() == 1);
    CHECK(r.rafts[0].parts == std::vector<int>{a});
    CHECK(r.partMade(b)->raft == -1);
    CHECK(r.partMade(b)->tips == 0);  // flat on the plate: nothing overhangs
    CHECK(r.partMade(b)->trunks == 0);
    const SupportCheck* c = check(r, "b stands on the plate");
    REQUIRE(c);
    CHECK(c->ok);
    CHECK(r.validation.floating.empty());
    CHECK(r.validation.first_layer_islands == 2);  // a's raft, and b itself
    // Far enough from the raft: nothing to report.
    for (const auto& p : r.problems) CHECK(p.code != "raft_meets_part");
}

TEST_CASE("Supports per part: a raft that meets a part on the plate is reported", "[gui][supports][parts][slow]") {
    const auto doc = jobOf({partOf(cube(10), "a", {-5.5, 0}), partOf(cube(10), "b", {5.5, 0}, true)});
    const SupportResult r = made(*doc);
    const SupportProblem* meets = nullptr;
    for (const auto& p : r.problems)
        if (p.code == "raft_meets_part") meets = &p;
    REQUIRE(meets);
    CHECK(meets->level == SupportProblem::Level::Review);
    CHECK_THAT(meets->title, ContainsSubstring("Raft 1 meets b"));
    CHECK(meets->has_where);
}

TEST_CASE("Supports per part: a shared raft has its first part's settings", "[gui][supports][parts]") {
    const auto doc = twoCubes(12);
    const int a = doc->parts()[0].id(), b = doc->parts()[1].id();
    doc->supports.own[b].raft = fruit::plainBaseSpec();
    doc->supports.own[b].raft_preset.name = "Plain base";
    CHECK(chosenRafts(*doc, doc->supports.setup()).size() == 2);
    doc->supports.shared = {{b, a}};
    const auto rafts = chosenRafts(*doc, doc->supports.setup());
    REQUIRE(rafts.size() == 1);
    CHECK(rafts[0].parts == std::vector<int>{a, b});  // in the job's order
    CHECK(rafts[0].spec == doc->supports.choices.raft);  // a's: the job's
    // A part on the plate is on no raft, even one it shares.
    doc->editParts([&](PartsEdit& e) { e.find(b)->placement.on_plate = true; });
    const auto now = chosenRafts(*doc, doc->supports.setup());
    REQUIRE(now.size() == 1);
    CHECK(now[0].parts == std::vector<int>{a});
}

TEST_CASE("Supports per part: a job of one part is made as one job always was", "[gui][supports][parts][slow]") {
    const auto doc = jobOf({partOf(demoModel(), "bracket", {0, 0})});
    const SupportResult a = generate(*doc, doc->supports.setup(), {}, fruit::JobControl::none());
    const SupportResult b = generate(*doc, SupportChoices{}, {}, fruit::JobControl::none());
    CHECK(a.rafts.size() == 1);
    CHECK(a.tips == b.tips);
    CHECK(a.mesh.faces.size() == b.mesh.faces.size());
    CHECK(a.mesh.vertices.size() == b.mesh.vertices.size());
    const SupportCheck* c = check(a, "Raft comes off in one piece");
    REQUIRE(c);
    CHECK(c->ok);
}

TEST_CASE("Jobs: each part's own values, shared rafts, the job's lift and each part's tips come back",
          "[gui][supports][parts][jobs][slow]") {
    const auto doc = jobOf({partOf(cube(10), "a", {-30, 0}), partOf(cube(10), "b", {-12, 0}),
                            partOf(demoModel(), "c", {30, 0})});
    const int a = doc->parts()[0].id(), b = doc->parts()[1].id(), c = doc->parts()[2].id();
    doc->editParts([&](PartsEdit& e) {
        e.job.elevation = 4;
        e.find(a)->placement.elevation = 4;
        e.find(b)->placement.elevation = 4;
        e.find(c)->placement.elevation = 7;  // its own
    });
    doc->supports.shared = {{a, b}};
    doc->supports.own[c].usual_tip = 3;
    doc->supports.own[c].advanced["route"] = 20;
    doc->supports.own[c].raft = fruit::plainBaseSpec();
    doc->supports.own[c].raft_preset.name = "Plain base";
    auto r = std::make_shared<SupportResult>(made(*doc));
    doc->supports.result = r;
    doc->supports.result_revision = doc->revision();
    doc->supports.result_settings = doc->supportSettings();
    REQUIRE(doc->supports.fresh(doc->revision(), doc->supportSettings()));

    const std::string path = (dir() / "per part.fruitjob").string();
    saveJob(path, *doc);
    const OpenedJob o = openJob(path, kPrinter, fruit::JobControl::none());
    const Document& d = *o.doc;
    REQUIRE(d.partCount() == 3);
    const int a2 = d.parts()[0].id(), b2 = d.parts()[1].id(), c2 = d.parts()[2].id();
    CHECK(d.jobLift().elevation == Approx(4));
    CHECK(d.parts()[2].placement().elevation == Approx(7));
    CHECK(d.supports.shared == std::vector<std::vector<int>>{{a2, b2}});
    REQUIRE(d.supports.own.count(c2));
    const PartChoices& own = d.supports.own.at(c2);
    CHECK(own.usual_tip == 3);
    CHECK(own.advanced.at("route") == Approx(20));
    REQUIRE(own.raft);
    CHECK(*own.raft == fruit::plainBaseSpec());
    CHECK(own.raft_preset.name == "Plain base");
    REQUIRE(d.supports.result);
    CHECK(d.supports.fresh(d.revision(), d.supportSettings()));
    CHECK(d.supports.result->tips == r->tips);
    CHECK(d.supports.result->rafts.size() == 2);
    CHECK(d.supports.result->mesh.faces.size() == r->mesh.faces.size());
    CHECK_FALSE(d.unsaved());
}

TEST_CASE("Jobs: several parts saved before each had its own raft open on one raft, as they printed",
          "[gui][supports][parts][jobs][slow]") {
    const auto doc = twoCubes(40);
    const std::string path = (dir() / "older.fruitjob").string();
    saveJob(path, *doc);
    // As an older FruitSlicer wrote it: no own values, shared rafts or job lift.
    mz_zip_archive in{};
    REQUIRE(mz_zip_reader_init_file(&in, path.c_str(), 0));
    std::vector<std::pair<std::string, std::string>> files;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&in); ++i) {
        char name[256] = {};
        mz_zip_reader_get_filename(&in, i, name, sizeof name);
        size_t n = 0;
        void* data = mz_zip_reader_extract_to_heap(&in, i, &n, 0);
        REQUIRE(data);
        files.emplace_back(name, std::string(static_cast<char*>(data), n));
        mz_free(data);
    }
    mz_zip_reader_end(&in);
    for (auto& [name, bytes] : files)
        if (name == "job.json") {
            nlohmann::json j = nlohmann::json::parse(bytes);
            j["supports"].erase("own");
            j["supports"].erase("shared_rafts");
            j["supports"].erase("job_lift");
            bytes = j.dump();
        }
    const std::string older = (dir() / "older2.fruitjob").string();
    fs::remove(older);
    mz_zip_archive out{};
    REQUIRE(mz_zip_writer_init_file(&out, older.c_str(), 0));
    for (const auto& [name, bytes] : files)
        REQUIRE(mz_zip_writer_add_mem(&out, name.c_str(), bytes.data(), bytes.size(), MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_finalize_archive(&out));
    REQUIRE(mz_zip_writer_end(&out));
    const OpenedJob o = openJob(older, kPrinter, fruit::JobControl::none());
    const Document& d = *o.doc;
    REQUIRE(d.partCount() == 2);
    REQUIRE(d.supports.shared.size() == 1);
    CHECK(d.supports.shared[0].size() == 2);
    CHECK(chosenRafts(d, d.supports.setup()).size() == 1);
}
