// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The Hollow screen's logic: a model's own hollows that hold resin, drilling,
// filling or leaving them, hollowing a solid model, and the words for it.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>

#include "Document.h"
#include "Hollows.h"
#include "Layers.h"
#include "Print.h"
#include "Supports.h"
#include "formats/GooSetup.h"
#include "mesh/Primitives.h"
#include "mesh/StlIO.h"

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using namespace gui;

namespace {

std::shared_ptr<Document> load(const fruit::TriangleMesh& m, const char* name) {
    const auto path = std::filesystem::temp_directory_path() / name;
    fruit::writeStl(path.string(), m);
    auto doc = loadDocument(path.string(), fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    std::filesystem::remove(path);
    return doc;
}

// A block with a sealed void inside, 12 × 12 × 6 mm under a 3 mm roof.
fruit::TriangleMesh sealed() {
    fruit::TriangleMesh m = fruit::makeBox({0, 0, 0}, {20, 20, 12});
    fruit::TriangleMesh v = fruit::makeBox({4, 4, 3}, {16, 16, 9});
    for (auto& f : v.faces) std::swap(f[1], f[2]);
    m.append(v);
    return m;
}

constexpr double kH = 0.1;  // layers: coarser than the printer's, for speed

bool inside(const fruit::Paths& p, fruit::Vec2 q) {
    for (const fruit::ExPolygon& e : fruit::toExPolygons(p))
        if (e.contains(q)) return true;
    return false;
}

}  // namespace

TEST_CASE("Hollow: a sealed hollow gets a drain and an air hole and then drains", "[gui][hollow]") {
    const auto doc = load(sealed(), "hollow_sealed.stl");
    REQUIRE(doc->part().check().cavities == 1);
    const HollowPlan p = planHollow(*doc, HollowChoices{}, {}, kH, fruit::JobControl::none());
    REQUIRE(p.closed.cavities.size() == 1);
    const fruit::Cavity& c = p.closed.cavities[0];
    CHECK(cavityKind(c, false) == "Resin trap");
    CHECK(cavityFacts(c, kH) == "0.9 ml · from 8.0 to 14.0 mm");  // placed 5 mm up
    CHECK_THAT(cavityMeaning(c, false), ContainsSubstring("could never be removed"));
    REQUIRE(p.holes.size() == 2);
    CHECK_FALSE(p.holes[0].air);
    CHECK(p.holes[1].air);
    CHECK(p.drains == std::vector<char>{1});
    CHECK(p.undrained() == 0);
    CHECK(p.missed() == 0);
    CHECK(p.changes());
    CHECK(applyLabel(p) == "Add 2 drain holes");
    // The hollow's resin drains out, and two 3 mm channels go through 3 mm
    // of floor and roof.
    CHECK(p.resin_saved == Approx(12 * 12 * 6 + 2 * 3.14159 * 1.5 * 1.5 * 3).margin(5));
    const auto out = hollowOutcomes(p);
    REQUIRE(out.size() >= 2);
    CHECK(out[0].ok);
    CHECK(out[0].title == "The hollow drains");
    CHECK(out[1].title == "Wash it out after printing");
}

TEST_CASE("Hollow: printing a sealed hollow solid, or leaving it", "[gui][hollow]") {
    const auto doc = load(sealed(), "hollow_fill.stl");
    HollowChoices c;
    c.fix = HollowChoices::Fix::Fill;
    const HollowPlan filled = planHollow(*doc, c, {}, kH, fruit::JobControl::none());
    CHECK(filled.holes.empty());
    CHECK(filled.drains == std::vector<char>{1});  // nothing holds resin
    CHECK(filled.resin_saved == Approx(0).margin(2));  // its resin cured instead of trapped
    CHECK(applyLabel(filled) == "Print it solid");
    CHECK(hollowOutcomes(filled)[0].title == "The hollow is printed solid");

    c.fix = HollowChoices::Fix::Leave;
    const HollowPlan left = planHollow(*doc, c, {}, kH, fruit::JobControl::none());
    CHECK_FALSE(left.changes());
    CHECK(left.undrained() == 1);
    CHECK(left.resin_saved == Approx(0).margin(2));
    CHECK(applyLabel(left) == "Leave it");
    CHECK_FALSE(hollowOutcomes(left)[0].ok);
}

TEST_CASE("Hollow: holes placed by hand, one of them missing", "[gui][hollow]") {
    const auto doc = load(sealed(), "hollow_manual.stl");
    HollowChoices c;
    c.auto_holes = false;
    // Up through the middle of the floor; and one on the side, into solid only.
    const std::vector<fruit::DrainHole> manual{{{10, 10, 0}, {0, 0, 1}, 2.0, false},
                                               {{20, 2, 6}, {-1, 0, 0}, 2.0, false}};
    const HollowPlan p = planHollow(*doc, c, manual, kH, fruit::JobControl::none());
    CHECK(p.holes == manual);
    REQUIRE(p.placed.size() == 2);
    CHECK(p.placed[0].cavity == 0);
    CHECK(p.placed[1].cavity == -1);
    CHECK(p.missed() == 1);
    CHECK(p.drains == std::vector<char>{1});
    bool told = false;
    for (const HollowOutcome& o : hollowOutcomes(p)) told = told || o.title == "A hole reaches no hollow";
    CHECK(told);
}

TEST_CASE("Hollow: hollowing a solid block, with holes into it", "[gui][hollow]") {
    const auto doc = load(fruit::makeBox({0, 0, 0}, {20, 20, 20}), "hollow_block.stl");
    CHECK(planHollow(*doc, HollowChoices{}, {}, kH, fruit::JobControl::none()).closed.cavities.empty());
    HollowChoices c;
    c.hollow = true;
    c.wall = 2.0;
    const HollowPlan p = planHollow(*doc, c, {}, kH, fruit::JobControl::none());
    REQUIRE(p.hollowed.cavities.size() == 1);
    CHECK(cavityKind(p.cavities().cavities[0], true) == "Hollowed");
    CHECK(p.holes.size() == 2);
    CHECK(p.drains == std::vector<char>{1});
    CHECK(p.resin_saved > 2000);  // a 16 mm cube under a hipped roof, less the holes
    CHECK(applyLabel(p) == "Hollow it");
    bool saving = false;
    for (const HollowOutcome& o : hollowOutcomes(p)) saving = saving || o.title.rfind("Saves ", 0) == 0;
    CHECK(saving);
}

TEST_CASE("Hollow: a sealed hollow inside a hollowed block goes with the rest", "[gui][hollow]") {
    // Hollowing fills the model's own hollow first: one cavity, not two.
    const auto doc = load(sealed(), "hollow_both.stl");
    HollowChoices c;
    c.hollow = true;
    c.wall = 1.5;
    const HollowPlan p = planHollow(*doc, c, {}, kH, fruit::JobControl::none());
    CHECK(p.closed.cavities.size() == 1);
    CHECK(p.hollowed.cavities.size() == 1);
    CHECK(p.drains == std::vector<char>{1});
}

TEST_CASE("Hollow: planning can be cancelled", "[gui][hollow]") {
    const auto doc = load(fruit::makeBox({0, 0, 0}, {20, 20, 20}), "hollow_cancel.stl");
    fruit::JobControl stop;
    stop.cancel();
    HollowChoices c;
    c.hollow = true;
    CHECK_THROWS_AS(planHollow(*doc, c, {}, kH, stop), fruit::Cancelled);
}

TEST_CASE("Hollow: applied, the supports, the layers and the print file use the drilled model", "[gui][hollow]") {
    const auto doc = load(sealed(), "hollow_pipeline.stl");  // placed 5 mm up, on supports
    doc->hollow.applied = true;                               // a drain and an air hole, suggested
    const SupportResult s = generate(*doc, SupportChoices{}, {}, fruit::JobControl::none());
    REQUIRE(s.hollow);
    REQUIRE(s.hollow->holes.size() == 2);
    CHECK(s.hollow->drains == std::vector<char>{1});
    CHECK(s.validation.floating.empty());
    const fruit::Vec3 mouth = doc->part().transform().apply(s.hollow->holes[0].at);  // underneath, among the tips
    CHECK(mouth.z == Approx(5.0).margin(1e-6));
    for (const auto& p : s.points) CHECK(fruit::distance(p.pos, mouth) > s.hollow->placed[0].radius);
    // None inside the hollow, though it drains now: they could never come
    // out. Its flat ceiling has to hold itself, and is there to review.
    for (const auto& p : s.points)
        CHECK(s.hollow->closed.cavityAt(s.hollow->solid->layerAt(p.pos.z - 0.05), p.pos.xy()) == -1);
    bool ceiling = false;
    for (const SupportProblem& q : s.problems) ceiling = ceiling || q.code == "cavity_ceiling";
    CHECK(ceiling);

    // The layers: the drain hole is open through the floor.
    const LayerResult lr = sliceLayers(*doc, s.mesh, 0.0, fruit::JobControl::none(), s.hollow);
    REQUIRE(lr.hollow == s.hollow);
    CHECK_FALSE(inside(lr.layers->slices[lr.layers->layerAt(mouth.z + 1.5)], mouth.xy()));
    CHECK(lr.traps() == 0);
    CHECK(lr.cups() == 0);
    // Not applied: the model as it is, its hollow a trap.
    const LayerResult plain = sliceLayers(*doc, s.mesh, 0.0, fruit::JobControl::none());
    CHECK(inside(plain.layers->slices[plain.layers->layerAt(mouth.z + 1.5)], mouth.xy()));
    CHECK(plain.traps() == 1);

    // Saved at a sample's 0.03 mm layers, it is planned again at that height
    // with the same holes.
    fruit::GooPrintRequest req;
    req.layer_height = 0.03;
    req.exposure = 1.5;
    req.bottom_exposure = 27.5;
    req.bottom_layers = 6;
    req.transition_layers = 6;
    const fruit::GooPrintSetup setup = fruit::buildGooSetup(fruit::PrinterProfile::saturn4Ultra16k(), req);
    const auto dir = std::filesystem::temp_directory_path() / "fruit_hollow_tests";
    std::filesystem::create_directories(dir);
    const std::string sample = (dir / "sample_003.goo").string();
    fruit::exportGoo(fruit::makeBox({-2, -2, 0}, {2, 2, 1}), setup.header, setup.grid, fruit::GooExportOptions{}, sample);
    PrintSettings ps;
    ps.sample_path = sample;
    ps.sample = loadSample(sample);
    const PrintPlan plan = planPrint(*doc, lr, s.tree.raft_top, ps);
    REQUIRE(plan.ready);
    REQUIRE(plan.reslice);
    const std::string path = (dir / "hollow_003.goo").string();
    const SaveResult saved = savePrint(*doc, lr, s.mesh, plan, path, false, fruit::JobControl::none());
    CHECK(saved.written.layers == plan.layers);
    CHECK(saved.check.checksums_ok);
    // Less resin than the model as it is cut at the same height, by the holes.
    fruit::TriangleMesh all = doc->placed();
    all.append(s.mesh);
    double solid = 0;
    for (const auto& sl : fruit::sliceModel(all, 0.03).slices) solid += fruit::area(sl) * 0.03;
    CHECK(saved.written.volume_mm3 < solid - 30);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Hollow: holes by hand, none placed yet, change nothing", "[gui][hollow]") {
    const auto doc = load(sealed(), "hollow_none_yet.stl");
    HollowChoices c;
    c.auto_holes = false;
    const HollowPlan p = planHollow(*doc, c, {}, kH, fruit::JobControl::none());
    CHECK(p.holes.empty());
    CHECK_FALSE(p.changes());
    CHECK(p.undrained() == 1);
    CHECK(applyLabel(p) == "Leave it");
    CHECK(p.resin_saved == Approx(0).margin(2));
}

TEST_CASE("Hollow: what the step bar, the Orient card and the 3D view say", "[gui][hollow]") {
    const auto doc = load(sealed(), "hollow_words.stl");
    // Before it is planned: the health check's hollow.
    CHECK(hollowsToFix(*doc) == 1);
    HollowOutcome s = hollowSummary(*doc);
    CHECK_FALSE(s.ok);
    CHECK(s.title == "1 hollow holds resin");
    CHECK(s.detail == "Drain it, fill it or leave it.");

    // Planned with holes, not yet applied: still to fix, labelled as it is.
    doc->hollow.plan = std::make_shared<const HollowPlan>(
        planHollow(*doc, doc->hollow.choices, doc->hollow.manual, kH, fruit::JobControl::none()));
    doc->hollow.plan_revision = doc->revision();
    CHECK(hollowsToFix(*doc) == 1);
    CHECK(cavityLabel(*doc->hollow.plan, 0, false).title == "0.9 ml of trapped resin");
    CHECK_FALSE(cavityLabel(*doc->hollow.plan, 0, false).ok);
    // In the middle of the hollow, half-way up (printer coordinates).
    const fruit::Vec3 at = cavityLabelAt(doc->hollow.plan->closed, 0, kH);
    const fruit::Vec3 mid = doc->part().transform().apply({10, 10, 6});
    CHECK(at.x == Approx(mid.x).margin(0.1));
    CHECK(at.y == Approx(mid.y).margin(0.1));
    CHECK(at.z == Approx(mid.z).margin(0.1));

    // Applied: nothing left to fix.
    doc->hollow.applied = true;
    CHECK(hollowsToFix(*doc) == 0);
    CHECK(cavityLabel(*doc->hollow.plan, 0, true).title == "0.9 ml · drains");
    s = hollowSummary(*doc);
    CHECK(s.ok);
    CHECK(s.title == "Drain holes added");
    CHECK(s.detail == "Placed automatically");

    // Left as it is: still to fix, though applied.
    doc->hollow.choices.fix = HollowChoices::Fix::Leave;
    CHECK(hollowsToFix(*doc) == 1);  // the plan is for other choices: the health check's count
    s = hollowSummary(*doc);
    CHECK_FALSE(s.ok);
    CHECK(s.detail == "Left as it is: shown as a problem before saving.");

    // Hollowed: the summary says so.
    doc->hollow.choices.hollow = true;
    s = hollowSummary(*doc);
    CHECK(s.ok);
    CHECK(s.title == "Hollowed · 2.0 mm wall");

    // A solid model: nothing to fix, an offer.
    const auto block = load(fruit::makeBox({0, 0, 0}, {20, 20, 20}), "hollow_block_words.stl");
    CHECK(hollowsToFix(*block) == 0);
    s = hollowSummary(*block);
    CHECK(s.ok);
    CHECK(s.title == "No hollows found");
}

TEST_CASE("Hollow: the cut through the middle shows the wall, the hollow and the holes", "[gui][hollow]") {
    const auto doc = load(sealed(), "hollow_cut.stl");
    const HollowPlan p = planHollow(*doc, HollowChoices{}, {}, kH, fruit::JobControl::none());
    REQUIRE(p.holes.size() == 2);
    // Straight through the middle of the block, along x (printer coordinates).
    const fruit::Vec3 mid = doc->part().transform().apply({10, 10, 6});
    const HollowSection s = hollowSection(p, mid.xy(), {1, 0});
    auto bands = [&](const std::vector<SectionBand>& v, double z) {
        std::vector<std::pair<double, double>> out;
        for (const SectionBand& b : v)
            if (b.layer == p.solid->layerAt(z)) out.emplace_back(b.t0, b.t1);
        return out;
    };
    // Half-way up the hollow: the block 20 mm across, the hollow its middle 12.
    const double z = mid.z;
    auto o = bands(s.outline, z), h = bands(s.hollow, z), pr = bands(s.prints, z);
    REQUIRE(o.size() == 1);
    CHECK(o[0].first == Approx(-10).margin(0.01));
    CHECK(o[0].second == Approx(10).margin(0.01));
    REQUIRE(h.size() == 1);
    CHECK(h[0].first == Approx(-6).margin(0.01));
    CHECK(h[0].second == Approx(6).margin(0.01));
    REQUIRE(pr.size() == 2);  // the two walls
    CHECK(pr[0].second == Approx(-6).margin(0.01));
    // In the floor, the drain hole: a gap in what prints, not in the outline.
    const double floor = mid.z - 4.5;
    o = bands(s.outline, floor);
    pr = bands(s.prints, floor);
    REQUIRE(o.size() == 1);
    REQUIRE(pr.size() == 2);
    CHECK(pr[0].second < 0);
    CHECK(pr[1].first > 0);
    CHECK(pr[1].first - pr[0].second == Approx(3).margin(0.3));  // the 3 mm hole
    CHECK(bands(s.hollow, floor).empty());

    // Printed solid: no hollow to show.
    HollowChoices fill;
    fill.fix = HollowChoices::Fix::Fill;
    const HollowSection f = hollowSection(planHollow(*doc, fill, {}, kH, fruit::JobControl::none()), mid.xy(), {1, 0});
    CHECK(f.hollow.empty());
    CHECK(bands(f.prints, z).size() == 1);
}
