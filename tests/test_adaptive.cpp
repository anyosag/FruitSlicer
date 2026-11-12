// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Adaptive layers: the allowed heights, the exposure formula, the planner,
// the merged layers, and the print file written from them.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "formats/GooExport.h"
#include "formats/GooFile.h"
#include "formats/GooSetup.h"
#include "layers/AdaptiveLayers.h"
#include "mesh/Primitives.h"

using namespace fruit;
using Catch::Approx;

namespace {

std::string tmp(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / "fruit_test_adaptive";
    std::filesystem::create_directories(dir);
    return (dir / name).string();
}

std::vector<char> bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::size_t countAt(const std::vector<LayerGroup>& g, std::size_t first) {
    for (const LayerGroup& x : g)
        if (x.first == first) return x.count;
    return 0;
}

// No group has the layers `i - 1` and `i` together: a boundary falls at `i`.
bool boundaryAt(const std::vector<LayerGroup>& g, std::size_t i) {
    for (const LayerGroup& x : g)
        if (x.first < i && i < x.first + x.count) return false;
    return true;
}

AdaptiveOptions opts(double h0, int mult, std::size_t start) {
    AdaptiveOptions o;
    o.thin_height = h0;
    o.multiplier = mult;
    o.first_mergeable = start;
    return o;
}

GooPrintSetup setup003(int transition = 2) {
    GooPrintRequest r;
    r.layer_height = 0.03;
    r.exposure = 1.5;
    r.bottom_exposure = 27.5;
    r.bottom_layers = 4;
    r.transition_layers = transition;
    return buildGooSetup(PrinterProfile::saturn4Ultra16k(), r);
}

}  // namespace

TEST_CASE("Adaptive: the heights a layer may have are doublings of the thin height", "[adaptive]") {
    auto heights = [](double h0, int m) { return adaptiveHeights(h0, m); };
    CHECK(heights(0.03, 8) == std::vector<double>{0.03, 0.06, 0.12});  // 0.24 is over the printer's limit
    CHECK(heights(0.05, 8) == std::vector<double>{0.05, 0.1, 0.2});
    const auto h4 = heights(0.04, 8);
    REQUIRE(h4.size() == 3);
    CHECK(h4[2] == Approx(0.16));  // 0.32 mm is capped away
    CHECK(heights(0.04, 2).size() == 2);   // never above the chosen thickest
    CHECK(heights(0.03, 1) == std::vector<double>{0.03});
    CHECK(heights(0.1, 8).size() == 2);    // 0.1, 0.2
    CHECK(heights(0.2, 8).size() == 1);
    CHECK(maxAdaptiveCount(0.03, 8) == 4);
    CHECK(maxAdaptiveCount(0.05, 8) == 4);
    CHECK(maxAdaptiveCount(0.01, 8) == 8);
    CHECK(maxAdaptiveCount(0.01, 3) == 2);  // a count that is not a doubling goes down to one
}

TEST_CASE("Adaptive: a typed height snaps to the nearest allowed one", "[adaptive]") {
    CHECK(snapAdaptiveMultiplier(0.03, 0.03) == 1);
    CHECK(snapAdaptiveMultiplier(0.03, 0.04) == 1);
    CHECK(snapAdaptiveMultiplier(0.03, 0.05) == 2);  // nearer 0.06 than 0.03
    CHECK(snapAdaptiveMultiplier(0.03, 0.06) == 2);
    CHECK(snapAdaptiveMultiplier(0.03, 0.1) == 4);
    CHECK(snapAdaptiveMultiplier(0.03, 0.2) == 4);   // 0.24 is not allowed
    CHECK(snapAdaptiveMultiplier(0.05, 0.2) == 4);
    CHECK(snapAdaptiveMultiplier(0.05, 0.08) == 2);
    CHECK(snapAdaptiveMultiplier(0.01, 0.2) == 8);   // 0.08 is the thickest of 0.01
    CHECK(snapAdaptiveMultiplier(0.2, 0.2) == 1);
}

TEST_CASE("Adaptive: the exposure of a thicker layer", "[adaptive]") {
    CHECK(adaptiveExposure(1.5, 0.03, 0.03, 0.10) == 1.5);  // the thin layer keeps the normal exposure
    CHECK(adaptiveExposure(1.5, 0.03, 0.06, 0.10) == Approx(2.02));   // 1.5 × e^0.3, to 0.01 s
    CHECK(adaptiveExposure(1.5, 0.03, 0.12, 0.10) == Approx(3.69));   // 1.5 × e^0.9
    CHECK(adaptiveExposure(1.5, 0.03, 0.12, 0.25) == Approx(2.15));   // a deeper light: a smaller rise
    CHECK(adaptiveExposure(1.5, 0.03, 0.12, 0.10) > adaptiveExposure(1.5, 0.03, 0.06, 0.10));
}

TEST_CASE("Adaptive: outlines match within a pixel, empty ones only each other", "[adaptive]") {
    const Paths a = sliceMeshAt(makeBox({0, 0, 0}, {10, 10, 1}), 0.5);
    const Paths near = sliceMeshAt(makeBox({0, 0, 0}, {10.015, 10, 1}), 0.5);
    const Paths far = sliceMeshAt(makeBox({0, 0, 0}, {10.05, 10, 1}), 0.5);
    CHECK(outlinesMatch(a, a, 0.02));
    CHECK(outlinesMatch(a, near, 0.02));
    CHECK_FALSE(outlinesMatch(a, far, 0.02));
    CHECK(outlinesMatch({}, {}, 0.02));
    CHECK_FALSE(outlinesMatch(a, {}, 0.02));
    CHECK_FALSE(outlinesMatch({}, a, 0.02));
}

TEST_CASE("Adaptive: a straight box merges to the thickest after the protected layers, a step at a time",
          "[adaptive][plan]") {
    const double h0 = 0.03;
    const LayerStack box = sliceModel(makeBox({0, 0, 0}, {10, 8, 3}), h0);
    REQUIRE(box.size() == 100);
    const auto g = planAdaptiveLayers(box, box.size(), opts(h0, 8, 12));
    std::size_t sum = 0;
    for (const LayerGroup& x : g) sum += x.count;
    CHECK(sum == 100);
    // Never merged up to layer 12; then 0.03 -> 0.06 -> 0.12 (at most twice the one before).
    for (std::size_t i = 0; i < 12; ++i) CHECK(countAt(g, i) == 1);
    CHECK(countAt(g, 12) == 2);
    CHECK(countAt(g, 14) == 4);
    for (std::size_t k = 1; k < g.size(); ++k) {
        CHECK(g[k].count <= 2 * g[k - 1].count);
        CHECK(g[k].count <= 4);  // 0.24 mm is over the printer's limit
        CHECK(g[k].first == g[k - 1].first + g[k - 1].count);
    }
    CHECK(g.back().count == 2);  // what is left at the top: 100 = 12 + 2 + 21 × 4 + 2
    CHECK(mergedCount(g) == g.size() - 12);
    // Off, or a thickest of one layer: nothing merges.
    CHECK(mergedCount(planAdaptiveLayers(box, box.size(), opts(h0, 1, 12))) == 0);
    // A thickest of 0.06 mm: pairs only.
    const auto pairs = planAdaptiveLayers(box, box.size(), opts(h0, 2, 12));
    for (const LayerGroup& x : pairs) CHECK(x.count <= 2);
    CHECK(countAt(pairs, 14) == 2);
}

TEST_CASE("Adaptive: protected layers are never merged", "[adaptive][plan]") {
    const double h0 = 0.05;
    const LayerStack box = sliceModel(makeBox({0, 0, 0}, {10, 8, 3}), h0);
    for (std::size_t start : {0u, 1u, 7u, 30u}) {
        const auto g = planAdaptiveLayers(box, box.size(), opts(h0, 4, start));
        for (const LayerGroup& x : g)
            if (x.first < start) CHECK(x.count == 1);
        CHECK(boundaryAt(g, start));
        if (start < 50) CHECK(mergedCount(g) > 0);
    }
}

TEST_CASE("Adaptive: slopes and curves stay thin, a gentle slope merges only within a pixel", "[adaptive][plan]") {
    const double h0 = 0.03;
    // A steep cone: the outline moves 0.06 mm per layer.
    const LayerStack cone = sliceModel(makeCone({0, 0, 0}, 10, {0, 0, 5}, 96), h0);
    const auto gc = planAdaptiveLayers(cone, cone.size(), opts(h0, 8, 6));
    CHECK(mergedCount(gc) == 0);
    // A sphere: thin on its slopes; only around the equator, where the wall is straight, may layers merge.
    const LayerStack sph = sliceModel(makeSphere({0, 0, 5}, 5, 192), h0);
    const auto gs = planAdaptiveLayers(sph, sph.size(), opts(h0, 8, 6));
    for (const LayerGroup& x : gs) {
        const double z = static_cast<double>(x.first) * h0;
        if (z < 1.5 || z > 8.5) CHECK(x.count == 1);
    }
    CHECK(gs.size() > sph.size() / 2);  // a good part of it is still thin
    // A gentle cone (0.015 mm a layer): pairs fit within 0.02 mm, four do not.
    const LayerStack gentle = sliceModel(makeCone({0, 0, 0}, 10, {0, 0, 20}, 96), h0);
    for (const LayerGroup& x : planAdaptiveLayers(gentle, gentle.size(), opts(h0, 8, 6))) CHECK(x.count <= 2);
}

TEST_CASE("Adaptive: a step in the outline keeps its exact height", "[adaptive][plan]") {
    const double h0 = 0.03;
    TriangleMesh m = makeBox({0, 0, 0}, {10, 10, 0.96});   // wide below
    m.append(makeBox({2, 2, 0.9}, {8, 8, 3}));             // narrower above, from layer 32
    const LayerStack ls = sliceModel(m, h0);
    const auto g = planAdaptiveLayers(ls, ls.size(), opts(h0, 8, 6));
    CHECK(boundaryAt(g, 32));
    CHECK(mergedCount(g) > 0);
    // The same with every start, so the grouping meets the step from either side.
    for (std::size_t start : {6u, 7u, 8u, 9u, 10u, 11u}) CHECK(boundaryAt(planAdaptiveLayers(ls, ls.size(), opts(h0, 8, start)), 32));
    // An appearing outline: nothing at first, a box later; and the other way round.
    LayerStack late = ls;
    for (std::size_t i = 0; i < 40; ++i) late.slices[i].clear();
    CHECK(boundaryAt(planAdaptiveLayers(late, late.size(), opts(h0, 8, 6)), 40));
}

TEST_CASE("Adaptive: layers are thin at a sudden change and step down towards it", "[adaptive][plan]") {
    const double h0 = 0.03;
    // Supports only (no model outline) up to layer 40, then a straight box: as under a part lifted on supports.
    LayerStack ls = sliceModel(makeBox({0, 0, 0}, {10, 10, 3}), h0);
    for (std::size_t i = 0; i < 40; ++i) ls.slices[i].clear();
    const auto g = planAdaptiveLayers(ls, ls.size(), opts(h0, 8, 6));
    // The last layer under the model and the model's first layer are thin ones.
    CHECK(countAt(g, 39) == 1);
    CHECK(countAt(g, 40) == 1);
    // Thick layers are used on both sides, away from the change.
    std::size_t before = 0, after = 0;
    for (const LayerGroup& x : g) {
        if (x.first + x.count <= 39) before = std::max(before, x.count);
        if (x.first > 40) after = std::max(after, x.count);
    }
    CHECK(before == 4);
    CHECK(after == 4);
    // Never more than twice the layer before or the layer after: 4, 2, 1 | 1, 2, 4.
    for (std::size_t k = 1; k < g.size(); ++k) {
        CHECK(g[k].count <= 2 * g[k - 1].count);
        CHECK(g[k - 1].count <= 2 * g[k].count);
        CHECK(g[k].first == g[k - 1].first + g[k - 1].count);
    }
    // A step in the middle of a model: thin on both sides of it too.
    TriangleMesh m = makeBox({0, 0, 0}, {10, 10, 0.96});
    m.append(makeBox({2, 2, 0.9}, {8, 8, 3}));
    const LayerStack st = sliceModel(m, h0);
    const auto s = planAdaptiveLayers(st, st.size(), opts(h0, 8, 6));
    CHECK(countAt(s, 31) == 1);
    CHECK(countAt(s, 32) == 1);
    for (std::size_t k = 1; k < s.size(); ++k) CHECK(s[k - 1].count <= 2 * s[k].count);
}

TEST_CASE("Adaptive: supports don't decide where layers merge", "[adaptive][plan]") {
    const double h0 = 0.03;
    const TriangleMesh box = makeBox({0, 0, 0}, {10, 8, 3});
    const TriangleMesh pin = makeCone({-4, -4, 0}, 1, {-4, -4, 2}, 24);  // a tapering support next to the box
    TriangleMesh both = box;
    both.append(pin);
    const LayerStack model = sliceModel(box, h0), full = sliceModel(both, h0);
    const auto g = planAdaptiveLayers(model, full.size(), opts(h0, 8, 12));
    CHECK(mergedCount(g) > 0);
    // Deciding on the whole print instead would have kept the tapering pin's layers thinner.
    CHECK(planAdaptiveLayers(full, full.size(), opts(h0, 8, 12)).size() > g.size());
    // The merged image still has the support: the union of its thin layers.
    const auto layers = buildAdaptiveLayers(full, g, h0);
    for (const AdaptiveLayer& l : layers) {
        for (std::size_t k = 0; k < l.group.count; ++k)
            CHECK(std::abs(area(difference(full.slices[l.group.first + k], l.slice))) < 1e-6);
    }
}

TEST_CASE("Adaptive: merged layers add up to the thin stack, with cumulative z and unioned images", "[adaptive][merge]") {
    const double h0 = 0.03;
    TriangleMesh m = makeBox({0, 0, 0}, {10, 8, 0.96});
    m.append(makeCone({0, 0, 0.9}, 4, {0, 0, 3}, 64));
    const LayerStack ls = sliceModel(m, h0);
    const auto g = planAdaptiveLayers(ls, ls.size(), opts(h0, 8, 6));
    const auto layers = buildAdaptiveLayers(ls, g, h0);
    REQUIRE(layers.size() == g.size());
    double sum = 0, z = 0;
    for (const AdaptiveLayer& l : layers) {
        sum += l.thickness;
        z += l.thickness;
        CHECK(l.thickness == Approx(static_cast<double>(l.group.count) * h0));
        CHECK(l.top_z == Approx(z));
    }
    CHECK(sum == Approx(static_cast<double>(ls.size()) * h0));
    CHECK(layers.back().top_z == Approx(ls.layerTop(ls.size() - 1)));
    // A merged layer is lit wherever any of its thin layers is (and nowhere else).
    bool merged = false;
    for (const AdaptiveLayer& l : layers) {
        if (l.group.count < 2) continue;
        merged = true;
        Paths all;
        for (std::size_t k = 0; k < l.group.count; ++k) all = unionOf(all, ls.slices[l.group.first + k]);
        CHECK(std::abs(area(difference(all, l.slice))) < 1e-6);
        CHECK(std::abs(area(difference(l.slice, all))) < 1e-6);
    }
    CHECK(merged);
}

TEST_CASE("Adaptive: the print file has each layer's own z and exposure and reads back", "[adaptive][goo]") {
    const GooPrintSetup s = setup003();
    const double h0 = 0.03;
    const TriangleMesh box = makeBox({-5, -4, 0}, {5, 4, 3});
    const LayerStack ls = sliceModel(box, h0);
    GooExportOptions o;
    o.file_time = "2026-01-01 00:00:00";
    const std::string thin_path = tmp("thin.goo"), path = tmp("adaptive.goo");
    const GooExportResult thin = exportGooLayers(ls, &box, s.header, s.grid, o, thin_path);
    AdaptiveSettings a;
    a.multiplier = 8;
    a.light_depth = 0.10;
    const GooExportResult res = exportGooAdaptive(ls, ls, &box, s.header, s.grid, o, a, path);

    // The plan the file must follow: nothing merged in the bottom and transition layers.
    AdaptiveOptions ao = opts(h0, 8, 4 + 2);
    const auto groups = planAdaptiveLayers(ls, ls.size(), ao);
    const GooFileInfo f = readGoo(path);
    REQUIRE(f.layers.size() == groups.size());
    CHECK(f.layers.size() < ls.size());
    CHECK(res.layers == groups.size());
    CHECK(res.thin_layers == ls.size());
    CHECK(res.merged_layers == mergedCount(groups));
    CHECK(f.header.print.advance_mode);
    CHECK(f.header.layer_height == Approx(0.03f));  // the header keeps the thin height
    CHECK(f.header.layer_count == groups.size());
    for (std::size_t k = 0; k < groups.size(); ++k) {
        const LayerGroup& g = groups[k];
        CHECK(f.layers[k].z == Approx(static_cast<double>(g.first + g.count) * h0).margin(1e-4));
        const GooLayerDef thin_def = layerDefFor(s.header, static_cast<uint32_t>(g.first));
        const double want = g.count > 1 ? adaptiveExposure(1.5, h0, static_cast<double>(g.count) * h0, 0.10)
                                        : static_cast<double>(thin_def.exposure);
        CHECK(f.layers[k].exposure == Approx(want).margin(1e-4));
    }
    // The first layers keep their bottom and transition exposures.
    CHECK(f.layers[0].exposure == Approx(27.5));
    CHECK(f.layers[4].exposure > 1.5);
    CHECK(f.layers[4].exposure < 27.5);
    const auto counts = gooThicknessCounts(f.header, f.layers);
    REQUIRE(counts.size() == 3);
    CHECK(counts[0].first == Approx(0.03).margin(1e-4));
    CHECK(counts[1].first == Approx(0.06).margin(1e-4));
    CHECK(counts[2].first == Approx(0.12).margin(1e-4));

    // The same part: volume within 0.5 % of the thin file's; time shorter.
    CHECK(res.volume_mm3 == Approx(thin.volume_mm3).epsilon(0.005));
    CHECK(f.header.volume == Approx(thin.volume_mm3).epsilon(0.005));
    CHECK(res.print_time_s < thin.print_time_s);
    const GooVerifyResult v = verifyGoo(path);
    CHECK(v.checksums_ok);
    CHECK(v.footer_ok);
    CHECK(v.layers == groups.size());
    CHECK(v.grey_values == 0);
    CHECK(v.volume_mm3 == Approx(res.volume_mm3).epsilon(0.005));  // thickness from each layer's own z
    CHECK(v.volume_mm3 == Approx(thin.volume_mm3).epsilon(0.005));
    const GooVerifyResult vt = verifyGoo(thin_path);
    CHECK(vt.volume_mm3 == Approx(thin.volume_mm3).epsilon(0.005));
    // Time: each layer's own exposure plus waits and motion.
    CHECK(res.print_time_s == static_cast<uint32_t>(estimatePrintSeconds(f.layers, o)));
    std::filesystem::remove(path);
    std::filesystem::remove(thin_path);
}

TEST_CASE("Adaptive: off, or nothing to merge, writes the equal-layers file byte for byte", "[adaptive][goo]") {
    const GooPrintSetup s = setup003();
    const TriangleMesh box = makeBox({-5, -4, 0}, {5, 4, 1});
    const LayerStack ls = sliceModel(box, 0.03);
    GooExportOptions o;
    o.file_time = "2026-01-01 00:00:00";
    const std::string a = tmp("equal.goo"), b = tmp("off.goo"), c = tmp("nothing.goo");
    exportGooLayers(ls, &box, s.header, s.grid, o, a);
    AdaptiveSettings off;
    off.multiplier = 1;
    exportGooAdaptive(ls, ls, &box, s.header, s.grid, o, off, b);
    CHECK(bytes(a) == bytes(b));
    CHECK_FALSE(readGoo(b).header.print.advance_mode);
    // A cone: asked to merge, but every layer's outline changes.
    const TriangleMesh cone = makeCone({0, 0, 0}, 10, {0, 0, 3}, 64);
    const LayerStack cs = sliceModel(cone, 0.03);
    AdaptiveSettings on;
    on.multiplier = 8;
    const GooExportResult r = exportGooAdaptive(cs, cs, &cone, s.header, s.grid, o, on, c);
    const std::string d = tmp("cone_equal.goo");
    exportGooLayers(cs, &cone, s.header, s.grid, o, d);
    CHECK(r.merged_layers == 0);
    CHECK(bytes(c) == bytes(d));
    CHECK_FALSE(readGoo(c).header.print.advance_mode);
    for (const auto& p : {a, b, c, d}) std::filesystem::remove(p);
}
