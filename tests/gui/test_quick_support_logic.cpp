// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Quick support: How many, Tip size and Look further against the planner's
// real values and against what they do to real parts; jobs saved with the
// three sliders they replaced; the job file, reset, and the raft and lift edits.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <miniz.h>
#include <nlohmann/json.hpp>

#include "Document.h"
#include "JobFile.h"
#include "Parts.h"
#include "mesh/ModelLoad.h"
#include "Supports.h"
#include "mesh/Primitives.h"
#include "mesh/StlIO.h"

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using namespace gui;
namespace fs = std::filesystem;

namespace {

SupportChoices at(int how_many, int tip = 2, bool look_further = false) {
    SupportChoices c;
    c.how_many = how_many;
    c.usual_tip = tip;
    c.look_further = look_further;
    return c;
}

// The three tip sizes a job's choices come to: usual, where a part starts in mid-air, near edges.
std::array<double, 3> tipSizes(const SupportChoices& c) {
    const fruit::SupportConfig cfg = supportConfig(fruit::Placement{}, c);
    const auto d = [&](int cls) { return cfg.tip_classes[static_cast<std::size_t>(cls)].diameter; };
    return {d(cfg.default_tip), d(cfg.minima_tip), d(cfg.edge_tip)};
}

double tipSize(const SupportChoices& c) { return tipSizes(c)[0]; }

std::shared_ptr<Document> docOf(const fruit::TriangleMesh& m, const char* name, const fruit::Vec3& turned = {0, 0, 0},
                                bool on_plate = false) {
    const auto path = fs::temp_directory_path() / name;
    fruit::writeStl(path.string(), m);
    auto doc = loadDocument(path.string(), fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    fs::remove(path);
    fruit::Placement p = doc->placement();
    p.rotation_deg = turned;
    p.on_plate = on_plate;
    doc->edit(p);
    return doc;
}

std::shared_ptr<Document> bracket(const char* name, const fruit::Vec3& turned = {0, 0, 0}) {
    return docOf(demoModel(), name, turned);
}

SupportResult made(const Document& doc, const SupportChoices& c) {
    return generate(doc, c, doc.visibleInPrinter(), fruit::JobControl::none());
}

nlohmann::json jobJson(const std::string& path) {
    mz_zip_archive z{};
    REQUIRE(mz_zip_reader_init_file(&z, path.c_str(), 0));
    size_t n = 0;
    void* data = mz_zip_reader_extract_file_to_heap(&z, "job.json", &n, 0);
    REQUIRE(data);
    const nlohmann::json j = nlohmann::json::parse(std::string(static_cast<const char*>(data), n));
    mz_free(data);
    mz_zip_reader_end(&z);
    return j;
}

// The saved job `job` written again as `out`, its job.json changed by `change`.
void rewriteJob(const std::string& job, const std::string& out, const std::function<void(nlohmann::json&)>& change) {
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
    mz_zip_archive z{};
    REQUIRE(mz_zip_writer_init_file(&z, out.c_str(), 0));
    for (auto& [file, bytes] : files) {
        if (file == "job.json") {
            auto j = nlohmann::json::parse(bytes);
            change(j);
            bytes = j.dump();
        }
        REQUIRE(mz_zip_writer_add_mem(&z, file.c_str(), bytes.data(), bytes.size(), MZ_DEFAULT_COMPRESSION));
    }
    REQUIRE(mz_zip_writer_finalize_archive(&z));
    mz_zip_writer_end(&z);
}

}  // namespace

TEST_CASE("Quick support: the middle of How many plans what FruitSlicer always planned", "[gui][supports][quick]") {
    const SupportChoices c;  // every control at the resin's own
    CHECK(c.how_many == 3);
    CHECK(c.usual_tip == 2);
    CHECK_FALSE(c.look_further);
    CHECK(c.hold == 3);
    CHECK_FALSE(quickChanged(c));
    const fruit::SupportConfig cfg = supportConfig(fruit::Placement{}, c);
    const fruit::TreeConfig tc = treeConfig(c);
    const fruit::SupportConfig own;
    const fruit::TreeConfig tree;
    CHECK(cfg.default_tip == own.default_tip);  // 0.4 mm
    CHECK(cfg.minima_tip == own.minima_tip);
    CHECK(cfg.edge_tip == own.edge_tip);
    CHECK(cfg.coverage.density == 1.0);
    CHECK(cfg.critical_angle_deg == own.critical_angle_deg);
    CHECK(cfg.self_supported_reach == own.self_supported_reach);
    CHECK(cfg.skin_reach == own.skin_reach);
    CHECK(cfg.capacity.reference_capacity == own.capacity.reference_capacity);
    CHECK(tc.max_route_length == tree.max_route_length);
    CHECK(tc.merge_pillars == tree.merge_pillars);
    CHECK(tc.max_merge_distance == tree.max_merge_distance);
    CHECK(tc.max_tips_per_trunk == tree.max_tips_per_trunk);
    CHECK(tc.max_trunk_diameter == tree.max_trunk_diameter);
    CHECK(tc.brace_min_height == tree.brace_min_height);
    CHECK(tc.brace_step == tree.brace_step);
    CHECK(tc.brace_diameter == tree.brace_diameter);
    CHECK(tc.brace_max_distance == tree.brace_max_distance);
    CHECK(std::string(howManyWord(3, c)) == "Starting values");
    CHECK_FALSE(howManyMeasured(c));
}

TEST_CASE("Quick support: a resin's measured values are the middle, and the stops move from them",
          "[gui][supports][quick]") {
    SupportChoices c;
    c.profile = {{"default_tip_diameter", 0.6}, {"critical_angle_deg", 52.0}};
    c.usual_tip = usualTipFor(c.profile);
    CHECK(c.usual_tip == 3);
    CHECK_FALSE(quickChanged(c));
    CHECK(howManyMeasured(c));
    CHECK(std::string(howManyWord(3, c)) == "As measured");
    CHECK(supportConfig(fruit::Placement{}, c).critical_angle_deg == Approx(52));
    CHECK(tipSize(c) == Approx(0.6));
    // How many moves the angle from the resin's, not from FruitSlicer's own.
    SupportChoices fewest = c;
    fewest.how_many = 1;
    CHECK(supportConfig(fruit::Placement{}, fewest).critical_angle_deg == Approx(62));
    // Tip size is the size itself; the other two sit as far from it as with the resin's usual tip.
    const auto resins = tipSizes(c);
    SupportChoices smaller = c;
    smaller.usual_tip = 2;
    CHECK(quickChanged(smaller));
    CHECK(tipSize(smaller) == Approx(0.4));
    CHECK(tipSizes(smaller)[1] < resins[1] + 1e-9);
    CHECK(tipSizes(smaller)[2] < resins[2]);
}

TEST_CASE("Quick support: every position's planner values", "[gui][supports][quick]") {
    // How many: the lean angle; towards More the spacing and the ledge rule with the angle; towards
    // Fewer how close a tip sits to a free fresh edge.
    const fruit::SupportConfig own;
    const double angle[] = {55, 50, 45, 40, 35};
    const double density[] = {1, 1, 1, 1.25, 1.5};
    const double edge[] = {1.5, 1.25, 1, 1, 1};
    const double rad = 3.14159265358979323846 / 180;
    for (int s = 1; s <= 5; ++s) {
        const SupportChoices c = at(s);
        const fruit::SupportConfig cfg = supportConfig(fruit::Placement{}, c);
        CHECK(cfg.critical_angle_deg == Approx(angle[s - 1]));
        CHECK(cfg.coverage.density == Approx(density[s - 1]));
        CHECK(cfg.skin_reach == Approx(own.skin_reach * edge[s - 1]));
        const double ledge = s > 3 ? own.self_supported_reach * std::tan(angle[s - 1] * rad) / std::tan(45 * rad)
                                   : own.self_supported_reach;
        CHECK(cfg.self_supported_reach == Approx(ledge));
        CHECK(cfg.capacity.reference_capacity == Approx(own.capacity.reference_capacity));  // no hold scale
        CHECK(quickChanged(c) == (s != 3));
        CHECK(howManyAngleShift(s) == Approx(angle[s - 1] - 45));
        CHECK(howManyDensity(s) == Approx(density[s - 1]));
        CHECK(howManyEdgeReach(s) == Approx(edge[s - 1]));
    }
    CHECK(supportConfig(fruit::Placement{}, at(4)).self_supported_reach == Approx(0.839).margin(0.001));
    CHECK(supportConfig(fruit::Placement{}, at(5)).self_supported_reach == Approx(0.700).margin(0.001));
    // Out of range stops are held to the ends.
    CHECK(supportConfig(fruit::Placement{}, at(9)).critical_angle_deg == Approx(35));
    CHECK(supportConfig(fruit::Placement{}, at(-4)).critical_angle_deg == Approx(55));

    // Tip size: the usual tip, and the other two with it, as far as the sizes go.
    CHECK(tipSizes(at(3, 1)) == std::array<double, 3>{0.3, 0.4, 0.2});
    CHECK(tipSizes(at(3, 2)) == std::array<double, 3>{0.4, 0.6, 0.3});
    CHECK(tipSizes(at(3, 3)) == std::array<double, 3>{0.6, 0.6, 0.4});
    CHECK(tipSizes(at(3, 0)) == std::array<double, 3>{0.2, 0.3, 0.2});  // an older job's 0.2 mm
    CHECK(quickChanged(at(3, 1)));
    CHECK(quickChanged(at(3, 3)));

    // Look further: a longer way down before a support stands on the model.
    const fruit::TreeConfig tree;
    CHECK(treeConfig(at(3, 2, false)).max_route_length == Approx(tree.max_route_length));
    CHECK(treeConfig(at(3, 2, true)).max_route_length == Approx(kLookFurtherRoute));
    CHECK(kLookFurtherRoute > tree.max_route_length);
    CHECK(quickChanged(at(3, 2, true)));

    // Trunks and braces, a row on the Advanced page with five stops.
    const bool merge[] = {false, true, true, true, true};
    const double dist[] = {8, 8, 10, 12, 15};
    const double brace[] = {1000, 15, 10, 8, 5};
    const double trunk[] = {1.8, 1.8, 1.8, 2.2, 2.6};
    for (int s = 1; s <= 5; ++s) {
        SupportChoices c;
        c.advanced[kSturdyKey] = s;
        const fruit::TreeConfig tc = treeConfig(c);
        CHECK(tc.merge_pillars == merge[s - 1]);
        CHECK(tc.max_merge_distance == Approx(dist[s - 1]));
        CHECK(tc.brace_min_height == Approx(brace[s - 1]));
        CHECK(tc.max_trunk_diameter == Approx(trunk[s - 1]));
        CHECK(sturdinessOf(tc) == s);
        CHECK_FALSE(quickChanged(c));  // the Advanced page's, not a quick control
    }
    CHECK(sturdinessOf(treeConfig(SupportChoices{})) == kSturdyDefault);
}

TEST_CASE("Quick support: the advanced page's values win over the controls", "[gui][supports][quick]") {
    SupportChoices c = at(5, 3, true);
    c.advanced[kSturdyKey] = 5;
    c.advanced["critical_angle"] = 47;
    c.advanced["ledge"] = 0.5;
    c.advanced["skin_reach"] = 1.2;
    c.advanced["spacing"] = 0.8;
    c.advanced["minima_tip"] = 1;
    c.advanced["edge_tip"] = 0;
    c.advanced["route"] = 20;
    c.advanced["merge_distance"] = 6;
    c.advanced["brace_height"] = 12;
    c.advanced["capacity"] = 11;
    const fruit::SupportConfig cfg = supportConfig(fruit::Placement{}, c);
    const fruit::TreeConfig tc = treeConfig(c);
    CHECK(cfg.critical_angle_deg == Approx(47));
    CHECK(cfg.self_supported_reach == Approx(0.5));
    CHECK(cfg.skin_reach == Approx(1.2));
    CHECK(cfg.coverage.density == Approx(0.8));  // wider apart than the borrowed reach: only this row can
    CHECK(tipSizes(c) == std::array<double, 3>{0.6, 0.3, 0.2});
    CHECK(tc.max_route_length == Approx(20));
    CHECK(tc.max_merge_distance == Approx(6));
    CHECK(tc.brace_min_height == Approx(12));
    CHECK(cfg.capacity.capacity(0.4) == Approx(11));
    CHECK(tc.max_trunk_diameter == Approx(2.6));  // no row of its own: the stop's
    // With the angle set there, How many leaves the ledge rule alone too.
    SupportChoices pinned = at(5);
    pinned.advanced["critical_angle"] = 47;
    CHECK(supportConfig(fruit::Placement{}, pinned).self_supported_reach ==
          Approx(fruit::SupportConfig{}.self_supported_reach));
    CHECK(supportConfig(fruit::Placement{}, pinned).coverage.density == Approx(1.5));
}

TEST_CASE("Quick support: captions and words state the real values", "[gui][supports][quick]") {
    const SupportChoices c;
    const char* words[] = {"Fewest", "Fewer", "Starting values", "More", "Most"};
    for (int s = 1; s <= 5; ++s) CHECK(std::string(howManyWord(s, c)) == words[s - 1]);
    // Every number in a caption is the value in use.
    for (int s = 1; s <= 5; ++s) {
        const fruit::SupportConfig cfg = supportConfig(fruit::Placement{}, at(s));
        const std::string cap = howManyCaption(s, c);
        CAPTURE(s, cap);
        char num[32];
        std::snprintf(num, sizeof num, "%.0f°", cfg.critical_angle_deg);
        CHECK_THAT(cap, ContainsSubstring(num));
        if (s >= 3) {
            std::snprintf(num, sizeof num, "%.1f mm", cfg.coverage.radius(0));
            CHECK_THAT(cap, ContainsSubstring(num));
        } else {
            std::snprintf(num, sizeof num, "within %.1f mm", cfg.skin_reach);
            CHECK_THAT(cap, ContainsSubstring(num));
        }
    }
    CHECK_THAT(howManyCaption(3, c), ContainsSubstring("not measured for this resin"));
    CHECK_THAT(howManyCaption(4, c), ContainsSubstring("11% closer"));
    CHECK_THAT(howManyCaption(5, c), ContainsSubstring("18% closer"));
    CHECK_THAT(howManyCaption(1, c), ContainsSubstring("Only faces leaning more than 55°"));
    SupportChoices measured;
    measured.profile = {{"critical_angle_deg", 50.0}};
    CHECK_THAT(howManyCaption(3, measured), ContainsSubstring("As measured for this resin"));
    CHECK_THAT(howManyCaption(3, measured), ContainsSubstring("50°"));
    // The advanced page's angle is the real one, and the caption says where it is set.
    SupportChoices own = at(1);
    own.advanced["critical_angle"] = 50;
    CHECK_THAT(howManyCaption(1, own), ContainsSubstring("50°"));
    CHECK_THAT(howManyCaption(1, own), ContainsSubstring("Advanced"));
    // ...and in the middle it no longer calls them starting values.
    CHECK_THAT(howManyCaption(3, own), ContainsSubstring("50°"));
    CHECK_THAT(howManyCaption(3, own), ContainsSubstring("Advanced"));
    CHECK_THAT(howManyCaption(3, own), !ContainsSubstring("Starting values"));

    CHECK_THAT(tipSizeCaption(at(3, 2)), ContainsSubstring("0.6 mm where a part starts in mid-air"));
    CHECK_THAT(tipSizeCaption(at(3, 2)), ContainsSubstring("0.3 mm near edges"));
    CHECK_THAT(tipSizeCaption(at(3, 1)), ContainsSubstring("0.4 mm where a part starts in mid-air"));
    CHECK_THAT(tipSizeCaption(at(3, 1)), ContainsSubstring("0.2 mm near edges"));
    CHECK_THAT(tipSizeCaption(at(3, 3)), ContainsSubstring("0.4 mm near edges"));

    CHECK_THAT(lookFurtherCaption(at(3, 2, false)), ContainsSubstring("within 15 mm"));
    CHECK_THAT(lookFurtherCaption(at(3, 2, false)), ContainsSubstring("stands on the model"));
    CHECK_THAT(lookFurtherCaption(at(3, 2, true)), ContainsSubstring("up to 30 mm"));
    CHECK_THAT(lookFurtherCaption(at(3, 2, true)), ContainsSubstring("not printed yet"));
    SupportChoices routed = at(3, 2, true);
    routed.advanced["route"] = 20;
    CHECK_THAT(lookFurtherCaption(routed), ContainsSubstring("up to 20 mm"));
    CHECK_THAT(lookFurtherCaption(routed), ContainsSubstring("Advanced"));

    CHECK_THAT(sturdinessCaption(c), ContainsSubstring("within 8 mm"));
    CHECK_THAT(sturdinessCaption(c), ContainsSubstring("taller than 15 mm"));
    SupportChoices single;
    single.advanced[kSturdyKey] = 1;
    CHECK_THAT(sturdinessCaption(single), ContainsSubstring("no braces"));
    SupportChoices most;
    most.advanced[kSturdyKey] = 5;
    CHECK_THAT(sturdinessCaption(most), ContainsSubstring("2.6 mm"));
}

TEST_CASE("Quick support: reset puts the controls back", "[gui][supports][quick]") {
    SupportChoices c = at(1, 3, true);
    c.advanced["head_length"] = 1.4;
    c.advanced[kSturdyKey] = 4;
    c.density = 1.5;
    c.merge = false;
    c.hold = 5;
    CHECK(quickChanged(c));
    resetQuick(c);
    CHECK_FALSE(quickChanged(c));
    CHECK(c.how_many == 3);
    CHECK(c.usual_tip == 2);
    CHECK_FALSE(c.look_further);
    CHECK(c.hold == 3);
    CHECK(c.density == 1.0);
    CHECK(c.merge);
    CHECK(c.advanced.count("head_length") == 1);  // the advanced page's values are its own
    CHECK(c.advanced.count(kSturdyKey) == 1);
    // On a resin with its own usual tip, that is where Tip size goes back to.
    SupportChoices r;
    r.profile = {{"default_tip_diameter", 0.6}};
    r.usual_tip = 1;
    resetQuick(r);
    CHECK(r.usual_tip == 3);
}

TEST_CASE("Quick support: a job saved with the three sliders plans as it did", "[gui][supports][quick]") {
    // What Hold, Amount and Sturdiness set at each of their stops.
    const int tip_shift[] = {-1, -1, 0, 1, 1};
    const double hold_scale[] = {1.25, 1.0, 1.0, 1.0, 0.8};
    const double angle_shift[] = {15, 5, 0, -5, -10};
    const double density[] = {1, 1, 1, 1.25, 1.5};
    const bool merge[] = {false, true, true, true, true};
    const double dist[] = {8, 8, 10, 12, 15}, brace[] = {1000, 15, 10, 8, 5}, trunk[] = {1.8, 1.8, 1.8, 2.2, 2.6};
    const int per_trunk[] = {6, 6, 6, 8, 10};
    const double step[] = {6, 6, 6, 5, 4}, thick[] = {0.6, 0.6, 0.6, 0.8, 1.0};
    const double std_tip[] = {0.2, 0.3, 0.4, 0.6};

    struct Job {
        const char* what;
        SupportChoices c;
    };
    std::vector<Job> jobs;
    jobs.push_back({"FruitSlicer's own values", SupportChoices{}});
    {
        SupportChoices c;
        c.profile = {{"default_tip_diameter", 0.6}, {"critical_angle_deg", 52.0}};
        c.usual_tip = usualTipFor(c.profile);
        jobs.push_back({"a resin with a 0.6 mm usual tip and its own angle", c});
    }
    {
        SupportChoices c;
        c.profile = {{"default_tip_diameter", 0.3}};
        c.usual_tip = usualTipFor(c.profile);
        jobs.push_back({"a resin with a 0.3 mm usual tip", c});
    }
    {
        SupportChoices c;
        c.usual_tip = 1;  // set on the Advanced page: Hold left it alone
        jobs.push_back({"a usual tip of the job's own", c});
    }
    {
        SupportChoices c;
        c.advanced["critical_angle"] = 47;
        c.advanced["capacity"] = 9;
        c.advanced["merge_distance"] = 6;
        c.density = 1.2;
        jobs.push_back({"values of the job's own on the Advanced page", c});
    }
    for (const Job& job : jobs) {
        // As the job read with every slider at the resin's stop.
        const fruit::SupportConfig plain = supportConfig(fruit::Placement{}, job.c);
        const double plain_angle = job.c.advanced.count("critical_angle") ? 0 : plain.critical_angle_deg;
        // The sliders never moved the mid-air and edge sizes: those were the resin's, whatever the usual tip.
        SupportChoices resins = job.c;
        resins.usual_tip = usualTipFor(job.c.profile);
        const auto base = tipSizes(resins);
        const bool own_tip = job.c.usual_tip != usualTipFor(job.c.profile);
        for (int h = 1; h <= 5; ++h)
            for (int a = 1; a <= 5; ++a)
                for (int s = 1; s <= 5; ++s) {
                    CAPTURE(job.what, h, a, s);
                    SupportChoices c = job.c;
                    convertOldQuick(c, h, a, s);
                    const fruit::SupportConfig cfg = supportConfig(fruit::Placement{}, c);
                    const fruit::TreeConfig tc = treeConfig(c);
                    // Hold: the usual tip a size either way (not one of the job's own), the others where they were.
                    const double usual =
                        std_tip[own_tip ? job.c.usual_tip : std::clamp(job.c.usual_tip + tip_shift[h - 1], 0, 3)];
                    REQUIRE(tipSizes(c) == std::array<double, 3>{usual, base[1], base[2]});
                    // ...and its ends, what a tip is counted on to hold (a value of the job's own wins).
                    const double cap = plain.capacity.capacity(0.4) * (job.c.advanced.count("capacity") ? 1 : hold_scale[h - 1]);
                    REQUIRE(cfg.capacity.capacity(0.4) == Approx(cap));
                    // Amount: the angle (not one of the job's own) and the spacing; nothing else.
                    if (plain_angle > 0)
                        REQUIRE(cfg.critical_angle_deg == Approx(std::clamp(plain_angle + angle_shift[a - 1], 20.0, 70.0)));
                    else
                        REQUIRE(cfg.critical_angle_deg == Approx(47));
                    REQUIRE(cfg.coverage.density == Approx(plain.coverage.density * density[a - 1]));
                    REQUIRE(cfg.self_supported_reach == Approx(plain.self_supported_reach));
                    REQUIRE(cfg.skin_reach == Approx(plain.skin_reach));
                    // Sturdiness: the trunks and braces (a merge distance of the job's own wins).
                    REQUIRE(tc.merge_pillars == merge[s - 1]);
                    REQUIRE(tc.max_merge_distance == Approx(job.c.advanced.count("merge_distance") ? 6 : dist[s - 1]));
                    REQUIRE(tc.brace_min_height == Approx(brace[s - 1]));
                    REQUIRE(tc.max_trunk_diameter == Approx(trunk[s - 1]));
                    REQUIRE(tc.max_tips_per_trunk == per_trunk[s - 1]);
                    REQUIRE(tc.brace_step == Approx(step[s - 1]));
                    REQUIRE(tc.brace_diameter == Approx(thick[s - 1]));
                    REQUIRE(tc.max_route_length == Approx(fruit::TreeConfig{}.max_route_length));
                    // The controls of now are at rest, apart from the tip size.
                    REQUIRE(c.how_many == 3);
                    REQUIRE_FALSE(c.look_further);
                }
    }
    // Left at 3, 3, 2 with nothing of its own, a job reads exactly as a new one.
    SupportChoices untouched;
    convertOldQuick(untouched, 3, 3, 2);
    CHECK(untouched == SupportChoices{});
}

TEST_CASE("Quick support: the controls are saved with the job, and jobs with the old sliders convert",
          "[gui][supports][quick][jobs]") {
    const fs::path dir = fs::temp_directory_path() / "fruit_quick_tests";
    fs::create_directories(dir);
    const auto doc = bracket("quick_bracket.stl");
    const auto job = (dir / "plain.fruitjob").string();
    saveJob(job, *doc);
    // At the resin's own only the marker is written.
    const nlohmann::json plain = jobJson(job).at("supports").at("choices");
    CHECK(plain.at("controls") == 2);
    CHECK_FALSE(plain.contains("how_many"));
    CHECK_FALSE(plain.contains("look_further"));
    CHECK_FALSE(plain.contains("quick"));
    const OpenedJob a = openJob(job, fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    CHECK(a.doc->supports.choices == SupportChoices{});

    doc->supports.choices.how_many = 5;
    doc->supports.choices.usual_tip = 3;
    doc->supports.choices.look_further = true;
    doc->supports.choices.advanced[kSturdyKey] = 4;
    const auto job2 = (dir / "quick.fruitjob").string();
    saveJob(job2, *doc);
    const OpenedJob b = openJob(job2, fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    CHECK(b.doc->supports.choices.how_many == 5);
    CHECK(b.doc->supports.choices.usual_tip == 3);
    CHECK(b.doc->supports.choices.look_further);
    CHECK(b.doc->supports.choices == doc->supports.choices);
    CHECK(jobJson(job2).at("supports").at("choices").at("how_many") == 5);

    // A job from before these controls: Hold 5, Amount 2, Sturdiness 4. It opens with the same
    // settings, as the tip size and values of its own.
    const auto old_job = (dir / "old_sliders.fruitjob").string();
    rewriteJob(job, old_job, [](nlohmann::json& j) {
        nlohmann::json& c = j.at("supports").at("choices");
        c.erase("controls");
        c["quick"] = {{"hold", 5}, {"amount", 2}, {"sturdy", 4}};
    });
    const OpenedJob o = openJob(old_job, fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    const SupportChoices& oc = o.doc->supports.choices;
    CHECK(oc.how_many == 3);
    CHECK(oc.usual_tip == 3);  // 0.6 mm
    CHECK(oc.hold == 5);       // still asks 20% less of each tip
    CHECK(tipSizes(oc) == std::array<double, 3>{0.6, 0.6, 0.3});
    CHECK(supportConfig(fruit::Placement{}, oc).critical_angle_deg == Approx(50));
    CHECK(supportConfig(fruit::Placement{}, oc).capacity.reference_capacity ==
          Approx(fruit::SupportConfig{}.capacity.reference_capacity * 0.8));
    CHECK(treeConfig(oc).max_trunk_diameter == Approx(2.2));
    CHECK_FALSE(o.doc->unsaved());
    // One from before the sliders reads as a new job.
    const auto older = (dir / "older.fruitjob").string();
    rewriteJob(job, older, [](nlohmann::json& j) { j.at("supports").at("choices").erase("controls"); });
    CHECK(openJob(older, fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none()).doc->supports.choices ==
          SupportChoices{});

    // The raft and the lift are the placement's, saved as before.
    fruit::Placement p = doc->placement();
    p.on_plate = true;
    p.elevation = 8;
    doc->edit(p);
    const auto job3 = (dir / "plate.fruitjob").string();
    saveJob(job3, *doc);
    const OpenedJob c = openJob(job3, fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    CHECK(c.doc->placement().on_plate);
    CHECK(c.doc->placement().elevation == Approx(8));
}

TEST_CASE("Quick support: what the controls do to real parts", "[gui][supports][quick][slow]") {
    // The demo bracket as it lies and tilted 30° and 45°, a ball, and a block on the plate with a low shelf.
    struct Model {
        const char* name;
        std::shared_ptr<Document> doc;
    };
    fruit::TriangleMesh shelf = fruit::makeBox({0, 0, 0}, {30, 8, 10});
    shelf.append(fruit::makeBox({5, -6, 4}, {25, 0.5, 6}));
    const std::vector<Model> models = {
        {"bracket", bracket("quick_out_flat.stl")},
        {"bracket at 30°", bracket("quick_out_30.stl", {30, 0, 0})},
        {"bracket at 45°", bracket("quick_out_45.stl", {45, 0, 0})},
        {"ball", docOf(fruit::makeSphere({0, 0, 12}, 12, 64), "quick_out_ball.stl")},
        {"shelf on the plate", docOf(shelf, "quick_out_shelf.stl", {0, 0, 0}, true)},
    };
    std::array<bool, 4> differs{};  // each pair of neighbouring stops: does it change the tips on some model?
    for (const Model& m : models) {
        CAPTURE(m.name);
        std::array<std::size_t, 5> tips{};
        for (int s = 1; s <= 5; ++s) {
            const SupportResult r = made(*m.doc, at(s));
            tips[static_cast<std::size_t>(s - 1)] = r.tips;
            // No stop makes supports that can't be saved.
            CHECK(r.count(SupportProblem::Level::Blocks) == 0);
        }
        CAPTURE(tips);
        // More never gives fewer: Fewest to Most, the count does not go down. Below the middle that is
        // exact (a lighter stop keeps the fewer of two plans). Above it each stop is planned afresh, and the
        // tips added so that none carries too much come out a few either way: 1 tip in 30 of slack.
        for (std::size_t i = 0; i + 1 < tips.size(); ++i) {
            CHECK(tips[i] <= tips[i + 1] + (i >= 2 ? std::max<std::size_t>(1, tips[i] / 30) : 0));
            if (tips[i] != tips[i + 1]) differs[i] = true;
        }
        // What the panel shows under the stops is what making them gives.
        const auto counted = tipCounts(*m.doc, SupportChoices{}, m.doc->visibleInPrinter(), fruit::JobControl::none());
        CHECK(counted == tips);
    }
    for (const bool d : differs) CHECK(d);

    // The case the old Amount slider could not reach: tilted 45°, the faces lean exactly as much as the
    // starting angle, and only a lower angle with the ledge rule moved along gives them tips of their own
    // (tips that cover a face, as against the ones added for hold).
    const Document& tilted = *models[2].doc;
    const auto covering = [](const SupportResult& r) {
        return std::count_if(r.points.begin(), r.points.end(),
                             [](const fruit::SupportPoint& p) { return p.category == fruit::PointCategory::Overhang; });
    };
    const auto middle = covering(made(tilted, at(3)));
    CHECK(covering(made(tilted, at(4))) >= 5 * middle);
    SupportChoices angle_alone;
    angle_alone.advanced["critical_angle"] = 40;  // what "More" used to be
    CHECK(covering(made(tilted, angle_alone)) <= 2 * middle + 2);

    // Tip size reaches every tip the planner places: none stays at its size between two sizes next to each
    // other, apart from where the sizes run out (0.2 and 0.6 mm). (With "Each tip's own load" on, as it
    // is unless turned off, a tip that would carry too much is then made a size bigger: so that is off
    // here, and on only for the check that every size still makes supports that can be saved.)
    for (const Model& m : models) {
        CAPTURE(m.name);
        const auto sizes = [&](int tip) {
            CHECK(made(*m.doc, at(3, tip)).count(SupportProblem::Level::Blocks) == 0);
            SupportChoices placed = at(3, tip);
            placed.advanced["tip_load_limit"] = 0;
            const SupportResult r = made(*m.doc, placed);
            double lo = 1e9, hi = 0;
            for (const auto& [d, n] : r.tip_sizes) lo = std::min(lo, d), hi = std::max(hi, d);
            return std::pair<double, double>{lo, hi};
        };
        const auto small = sizes(1), usual = sizes(2), big = sizes(3);
        if (usual.second <= 0) continue;  // no tips at all
        CHECK(small.second <= 0.4 + 1e-9);  // mid-air tips came down from 0.6 with it
        CHECK(small.second < usual.second + 1e-9);
        CHECK(big.first >= 0.4 - 1e-9);     // edge tips went up from 0.3 with it
        CHECK(big.first > usual.first - 1e-9);
    }
}

TEST_CASE("Quick support: raft and lift are the job's, in every part, and undo", "[gui][supports][quick]") {
    const auto doc = bracket("quick_bracket2.stl");
    CHECK_FALSE(doc->placement().on_plate);
    CHECK(doc->placement().elevation == 5);  // the default lift
    fruit::Placement p = doc->placement();
    p.elevation = 8;
    doc->edit(p);
    CHECK(doc->placement().lift() == 8);
    doc->undo();
    CHECK(doc->placement().elevation == 5);
    doc->redo();
    // A second part takes the same lift, and the same raft choice.
    fruit::Placement other;
    other.offset = {40, 0};
    other.elevation = 3;
    const int b = doc->addPart(NewPart{"b", "", makePartModel(fruit::prepareModel(fruit::makeBox({0, 0, 0}, {10, 10, 10}))), other});
    REQUIRE(b >= 0);
    for (const Part& part : doc->parts()) CHECK(part.placement().elevation == 8);
    p = doc->placement();
    p.elevation = 6;
    doc->edit(p);
    for (const Part& part : doc->parts()) CHECK(part.placement().elevation == 6);
    doc->undo();
    for (const Part& part : doc->parts()) CHECK(part.placement().elevation == 8);
    doc->undo();  // the part's addition
    doc->redo();
    doc->redo();
    doc->undo();
    CHECK(doc->placement().elevation == 8);
    // Raft off: no lift at all, but the number is kept for when it is back on.
    p = doc->placement();
    p.on_plate = true;
    doc->edit(p);
    CHECK(doc->placement().lift() == 0);
    CHECK(doc->placement().elevation == 8);
    doc->undo();
    CHECK_FALSE(doc->placement().on_plate);
    CHECK(doc->placement().lift() == 8);

    // Dragging the slider is one undo step, and a change marks supports out of date.
    const int before = doc->revision();
    doc->beginDrag();
    for (const double mm : {7.0, 6.0, 3.0, 2.0}) {
        p = doc->placement();
        p.elevation = mm;
        doc->preview(p);
    }
    doc->endDrag();
    CHECK(doc->placement().elevation == 2);
    CHECK(doc->revision() != before);
    doc->undo();
    CHECK(doc->placement().elevation == 8);
    CHECK(doc->revision() == before);
}

TEST_CASE("Quick support: the stops plan and the far ends still make supports", "[gui][supports][quick][slow]") {
    const auto doc = bracket("quick_bracket3.stl");
    const auto run = [&](const SupportChoices& c) {
        return generate(*doc, c, {}, fruit::JobControl::none());
    };
    const SupportResult normal = run(SupportChoices{});
    // A job left at its resin stops plans the same supports as the same choices with the sliders ignored.
    CHECK(normal.cfg.default_tip == 2);
    CHECK(normal.tree_cfg.merge_pillars);
    SupportChoices single = at(1);
    single.advanced[kSturdyKey] = 1;
    SupportChoices sturdiest = at(5);
    sturdiest.advanced[kSturdyKey] = 5;
    const SupportResult fewest = run(single);
    const SupportResult most = run(sturdiest);
    CHECK(fewest.tips > 0);
    CHECK(most.tips >= fewest.tips);
    CHECK(fewest.tree_cfg.merge_pillars == false);
    CHECK(fewest.braces == 0);
    CHECK(most.tree_cfg.max_trunk_diameter == Approx(2.6));
    CHECK(run(at(3, 1)).tips > 0);
    CHECK(run(at(3, 3)).tips > 0);
    CHECK(run(at(3, 2, true)).tips > 0);
}

TEST_CASE("Full raft: the choice reaches the engine, is saved with the job and makes supports out of date",
          "[gui][supports][jobs]") {
    SupportChoices c;
    CHECK(treeConfig(c).raft == fruit::RaftStyle::Pads);  // pads stay the default
    c.full_raft = true;
    CHECK(treeConfig(c).raft == fruit::RaftStyle::Full);
    CHECK_FALSE(c == SupportChoices{});  // differs from what the supports were made with
    fruit::Placement flat;
    flat.on_plate = true;  // no raft at all, whatever the choice
    fruit::SupportConfig cfg;
    fruit::TreeConfig tc;
    fruit::SupportChoices ch;
    ch.on_plate = true;
    fruit::resolveSettings(nullptr, ch, cfg, &tc);
    CHECK(tc.raft == fruit::RaftStyle::None);
    // The edge and gap rows set the engine's values.
    c.advanced["full_raft_margin"] = 2.0;
    c.advanced["full_raft_gap"] = 6.0;
    CHECK(treeConfig(c).full_raft_margin == Approx(2.0));
    CHECK(treeConfig(c).full_raft_gap == Approx(6.0));

    const fs::path dir = fs::temp_directory_path() / "fruit_full_raft_tests";
    fs::create_directories(dir);
    const auto doc = bracket("full_raft_bracket.stl");
    const auto pads = (dir / "pads.fruitjob").string();
    saveJob(pads, *doc);
    CHECK_FALSE(openJob(pads, fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none())
                    .doc->supports.choices.full_raft);
    doc->supports.choices.full_raft = true;
    const auto full = (dir / "full.fruitjob").string();
    saveJob(full, *doc);
    const OpenedJob o = openJob(full, fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    CHECK(o.doc->supports.choices.full_raft);
    CHECK(o.doc->supports.choices == doc->supports.choices);
}
