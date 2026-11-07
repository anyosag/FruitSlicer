// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Progress, cancellation, worker-thread exceptions and checked file output:
// what a GUI needs to run engine work on a background thread.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/JobControl.h"
#include "core/OutFile.h"
#include "core/Parallel.h"
#include "formats/GooExport.h"
#include "io/PngWriter.h"
#include "layers/LayerStack.h"
#include "mesh/Placement.h"
#include "mesh/Primitives.h"
#include "mesh/StlIO.h"
#include "printer/PrinterProfile.h"
#include "support/OrientationSearch.h"
#include "support/PointGenerator.h"
#include "support/SupportTree.h"
#include "support/SupportValidator.h"

using namespace fruit;
using Catch::Approx;

namespace {

std::filesystem::path tmpDir() {
    const auto dir = std::filesystem::temp_directory_path() / "fruit_test_jobs";
    std::filesystem::create_directories(dir);
    return dir;
}

// Records every report; never-decreasing is checked by the tests.
struct Recorder {
    std::mutex mu;
    std::vector<std::pair<std::string, double>> seen;
    JobControl::Sink sink() {
        return [this](std::string_view stage, double f) {
            std::lock_guard<std::mutex> lock(mu);
            seen.emplace_back(std::string(stage), f);
        };
    }
    bool monotonic() {
        for (size_t i = 1; i < seen.size(); ++i)
            if (seen[i].second < seen[i - 1].second) return false;
        return true;
    }
    std::set<std::string> stages() {
        std::set<std::string> s;
        for (const auto& [k, v] : seen) s.insert(k);
        return s;
    }
};

TriangleMesh testModel() {
    // A box with an overhanging shelf: islands, overhangs and some layers.
    TriangleMesh m = makeBox({-4, -4, 0}, {4, 4, 6});
    m.append(makeBox({-4, 4, 4}, {4, 10, 6}));
    placeOnPlate(m, {0, 0, 0}, 5.0);
    return m;
}

}  // namespace

TEST_CASE("job control: sub-ranges map into the parent and never go backwards", "[jobs]") {
    Recorder rec;
    JobControl ctl(rec.sink());
    ctl.report("a", 0.5);
    const JobControl half = ctl.sub(0.5, 1.0);
    half.report("b", 0.0);  // maps to 0.5
    half.report("b", 0.5);  // 0.75
    ctl.report("a", 0.2);   // older stage reporting less: clamped
    half.sub(0.5, 1.0).report("c", 1.0);
    REQUIRE(rec.seen.size() == 5);
    CHECK(rec.seen[1].second == Approx(0.5));
    CHECK(rec.seen[2].second == Approx(0.75));
    CHECK(rec.seen[3].second == Approx(0.75));
    CHECK(rec.seen[4].second == Approx(1.0));
    CHECK(rec.monotonic());
}

TEST_CASE("job control: cancel is shared by children; none() is inert", "[jobs]") {
    JobControl ctl;
    const JobControl child = ctl.sub(0.2, 0.4);
    const JobControl quiet = ctl.silent();
    CHECK_NOTHROW(child.check());
    // Code that polls an atomic sees the same flag through every child.
    const std::atomic<bool>* flag = child.cancelFlag();
    REQUIRE(flag != nullptr);
    CHECK(flag == quiet.cancelFlag());
    CHECK_FALSE(flag->load());
    CHECK(JobControl::none().cancelFlag() == nullptr);
    ctl.cancel();
    CHECK(flag->load());
    CHECK(child.cancelled());
    CHECK(quiet.cancelled());
    CHECK_THROWS_AS(child.check(), Cancelled);
    CHECK_THROWS_AS(quiet.step("x", 0.5), Cancelled);
    JobControl::none().cancel();
    CHECK_FALSE(JobControl::none().cancelled());
    CHECK_NOTHROW(JobControl::none().check());
}

TEST_CASE("worker threads: the first exception reaches the caller, nothing terminates", "[jobs]") {
    std::atomic<int> started{0};
    CHECK_THROWS_WITH(runThreads(6,
                                 [&](unsigned t, const std::atomic<bool>&) {
                                     ++started;
                                     if (t == 3) throw std::runtime_error("worker 3 failed");
                                 }),
                      Catch::Matchers::Equals("worker 3 failed"));
    CHECK(started == 6);

    std::vector<std::atomic<int>> hits(500);
    parallelFor(hits.size(), 4, [&](size_t i) { ++hits[i]; });
    for (const auto& h : hits) CHECK(h == 1);
    CHECK_THROWS_AS(parallelFor(100, 4, [&](size_t i) {
                        if (i == 40) throw std::out_of_range("index 40");
                    }),
                    std::out_of_range);
}

TEST_CASE("threads: 0 means the default, all cores unless set lower", "[jobs]") {
    const unsigned all = resolveThreads(0);
    CHECK(all >= 1);
    CHECK(resolveThreads(3) == 3);  // an explicit count is kept
    setDefaultThreads(1);
    CHECK(resolveThreads(0) == 1);
    CHECK(resolveThreads(5) == 5);
    setDefaultThreads(all + 100);  // never more than there are cores
    CHECK(resolveThreads(0) == all);
    setDefaultThreads(0);
    CHECK(resolveThreads(0) == all);
}

TEST_CASE("slicing: progress reaches 1; a failing progress sink and cancel both surface", "[jobs]") {
    const TriangleMesh m = testModel();
    Recorder rec;
    const LayerStack ok = sliceModel(m, 0.05, nullptr, 4, JobControl(rec.sink()));
    CHECK(ok.size() > 100);
    REQUIRE_FALSE(rec.seen.empty());
    CHECK(rec.seen.back().second == Approx(1.0));
    CHECK(rec.monotonic());
    CHECK(rec.stages() == std::set<std::string>{"slice"});

    // An exception thrown on a slicing worker thread is rethrown here.
    std::atomic<int> calls{0};
    JobControl throwing([&](std::string_view, double) {
        if (++calls == 40) throw std::runtime_error("sink failed");
    });
    CHECK_THROWS_WITH(sliceModel(m, 0.05, nullptr, 4, throwing), Catch::Matchers::Equals("sink failed"));

    // A control whose own sink cancels it halfway, as a Cancel button would.
    std::shared_ptr<JobControl> holder = std::make_shared<JobControl>();
    *holder = JobControl([holder_weak = std::weak_ptr<JobControl>(holder)](std::string_view, double f) {
        if (f > 0.5)
            if (auto h = holder_weak.lock()) h->cancel();
    });
    CHECK_THROWS_AS(sliceModel(m, 0.05, nullptr, 4, *holder), Cancelled);
}

TEST_CASE("point generation: stages, cancellation and returned layer analysis", "[jobs]") {
    const TriangleMesh m = testModel();
    SupportConfig cfg;
    cfg.layer_height = 0.1;
    Recorder rec;
    const PointGenResult res = generateSupportPoints(m, cfg, {}, JobControl(rec.sink()));
    CHECK(rec.monotonic());
    CHECK(rec.seen.back().second == Approx(1.0));
    const auto st = rec.stages();
    CHECK(st.count("slice"));
    CHECK(st.count("islands"));
    CHECK(st.count("points"));
    // The analysis is what the points came from.
    REQUIRE(res.analysis.layers);
    REQUIRE(res.analysis.islands);
    REQUIRE(res.analysis.hollows);
    CHECK(res.analysis.layers->size() == res.stats.layers);
    CHECK(res.analysis.islands->parts().size() == res.stats.parts);
    CHECK(res.analysis.islands->islands().size() == res.stats.islands);

    // Precomputed layers give the very same points.
    PointGenOptions reuse;
    reuse.layers = res.analysis.layers;
    const PointGenResult again = generateSupportPoints(m, cfg, reuse);
    REQUIRE(again.points.size() == res.points.size());
    for (size_t i = 0; i < res.points.size(); ++i) {
        CHECK(again.points[i].id == res.points[i].id);
        CHECK(again.points[i].pos.z == res.points[i].pos.z);
    }
    SupportConfig other = cfg;
    other.layer_height = 0.05;
    CHECK_THROWS_AS(generateSupportPoints(m, other, reuse), std::invalid_argument);

    // Cancelled while placing points.
    auto holder = std::make_shared<JobControl>();
    *holder = JobControl([w = std::weak_ptr<JobControl>(holder)](std::string_view stage, double) {
        if (stage == "points")
            if (auto h = w.lock()) h->cancel();
    });
    CHECK_THROWS_AS(generateSupportPoints(m, cfg, {}, *holder), Cancelled);
}

TEST_CASE("orientation search: same results on 1 or many threads; finalists keep their points", "[jobs]") {
    TriangleMesh t = makeBox({-1, -1, 0}, {1, 1, 8});
    t.append(makeBox({-8, -4, 8}, {8, 4, 9}));
    OrientOptions o;
    o.directions = 16;
    o.finalists = 3;
    o.cfg.layer_height = 0.1;
    o.threads = 1;
    const auto one = searchOrientations(t, o);
    o.threads = 4;
    Recorder rec;
    const auto many = searchOrientations(t, o, JobControl(rec.sink()));
    REQUIRE(one.size() == many.size());
    for (size_t i = 0; i < one.size(); ++i) {
        CHECK(one[i].evaluated == many[i].evaluated);
        CHECK(one[i].points == many[i].points);
        CHECK(one[i].score == many[i].score);
        CHECK(one[i].proxy == many[i].proxy);
        CHECK(one[i].euler_deg.x == many[i].euler_deg.x);
    }
    CHECK(rec.monotonic());
    CHECK(rec.stages() == std::set<std::string>{"orient.score", "orient.evaluate"});
    // The kept points are the generator's points for that orientation.
    const OrientationResult& best = many.front();
    REQUIRE(best.evaluated);
    CHECK(best.support_points.size() == best.points);
    TriangleMesh m = t;
    m.transform(best.rotation);
    placeOnPlate(m, {0, 0, 0}, o.elevation);
    const auto fresh = generateSupportPoints(m, o.cfg);
    REQUIRE(fresh.points.size() == best.support_points.size());
    for (size_t i = 0; i < fresh.points.size(); ++i) CHECK(fresh.points[i].id == best.support_points[i].id);

    JobControl cancelled;
    cancelled.cancel();
    CHECK_THROWS_AS(searchOrientations(t, o, cancelled), Cancelled);
}

TEST_CASE("support tree and validation report progress and can be cancelled", "[jobs]") {
    const TriangleMesh m = testModel();
    SupportConfig cfg;
    cfg.layer_height = 0.1;
    const auto pts = generateSupportPoints(m, cfg).points;
    REQUIRE_FALSE(pts.empty());
    TreeConfig tc;
    Recorder rec;
    const SupportTree tree = buildSupportTree(m, pts, tc, 0.4, JobControl(rec.sink()));
    CHECK(rec.monotonic());
    CHECK(rec.seen.back().second == Approx(1.0));
    CHECK(rec.stages() == std::set<std::string>{"tree"});
    JobControl stop;
    stop.cancel();
    CHECK_THROWS_AS(buildSupportTree(m, pts, tc, 0.4, stop), Cancelled);
    CHECK_THROWS_AS(validateSupported(m, tree.mesh(tc), 0.1, cfg.link_grow, 0.12, 2.7e-4, stop), Cancelled);
}

TEST_CASE(".goo export: cancelled or failed exports leave no file and keep an older one", "[jobs]") {
    const TriangleMesh cube = makeBox({-5, -5, 0}, {5, 5, 2});
    GooHeader h;
    h.layer_height = 0.05f;
    h.print.exposure = 2.5f;
    h.print.bottom_exposure = 25.0f;
    const RasterGrid grid = RasterGrid::fromPrinter(PrinterProfile::saturn4Ultra16k());
    const auto path = (tmpDir() / "cancel.goo").string();
    std::filesystem::remove(path);
    Recorder rec;
    const GooExportResult ok = exportGoo(cube, h, grid, {}, path, JobControl(rec.sink()));
    CHECK(ok.layers == 40);
    CHECK(std::filesystem::exists(path));
    CHECK_FALSE(std::filesystem::exists(path + ".part"));
    CHECK(rec.stages() == std::set<std::string>{"slice", "rasterize"});
    CHECK(rec.monotonic());
    const auto size_before = std::filesystem::file_size(path);

    auto holder = std::make_shared<JobControl>();
    *holder = JobControl([w = std::weak_ptr<JobControl>(holder)](std::string_view stage, double) {
        if (stage == "rasterize")
            if (auto c = w.lock()) c->cancel();
    });
    CHECK_THROWS_AS(exportGoo(cube, h, grid, {}, path, *holder), Cancelled);
    CHECK(std::filesystem::file_size(path) == size_before);  // the older file is untouched
    CHECK_FALSE(std::filesystem::exists(path + ".part"));

    // Layers sliced at another height are refused.
    const LayerStack wrong = sliceModel(cube, 0.1);
    CHECK_THROWS_AS(exportGooLayers(wrong, &cube, h, grid, {}, path), std::invalid_argument);
}

TEST_CASE("checked output: unfinished atomic files vanish; write errors throw", "[jobs]") {
    const auto path = (tmpDir() / "atomic.bin").string();
    std::filesystem::remove(path);
    {
        OutFile f(path, true);
        f.write("abc", 3);
    }  // never committed
    CHECK_FALSE(std::filesystem::exists(path));
    CHECK_FALSE(std::filesystem::exists(path + ".part"));
    {
        OutFile f(path, true);
        f.write("abcdef", 6);
        f.commit();
    }
    CHECK(std::filesystem::file_size(path) == 6);
    // An STL writer abandoned midway (an exception) leaves nothing behind.
    const auto stl = (tmpDir() / "abandoned.stl").string();
    std::filesystem::remove(stl);
    try {
        StlWriter w(stl);
        w.add(makeBox({0, 0, 0}, {1, 1, 1}));
        throw std::runtime_error("interrupted");
    } catch (const std::runtime_error&) {
    }
    CHECK_FALSE(std::filesystem::exists(stl));
    CHECK_THROWS(OutFile((tmpDir() / "no_such_dir" / "x.bin").string()));
#if defined(__linux__)
    // /dev/full fails every write with ENOSPC: the error must surface.
    if (std::filesystem::exists("/dev/full")) {
        CHECK_THROWS([] {
            OutFile f("/dev/full");
            std::vector<char> big(1 << 20, 'x');
            f.write(big.data(), big.size());
            f.commit();
        }());
    }
#endif
}

TEST_CASE("PNG files written from many threads at once are all valid", "[jobs]") {
    std::vector<uint8_t> px(32 * 16, 128);
    parallelFor(16, 8, [&](size_t i) {
        writePngGray((tmpDir() / ("t" + std::to_string(i) + ".png")).string(), 32, 16, px);
    });
    for (int i = 0; i < 16; ++i) {
        std::ifstream in(tmpDir() / ("t" + std::to_string(i) + ".png"), std::ios::binary);
        char sig[8];
        in.read(sig, 8);
        CHECK(static_cast<unsigned char>(sig[1]) == 'P');
    }
}
