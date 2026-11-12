// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The Layers step's logic: slicing, measuring and drawing layers.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>

#include "Document.h"
#include "LayerScroll.h"
#include "Layers.h"
#include "mesh/Primitives.h"
#include "mesh/StlIO.h"

using Catch::Approx;
using namespace gui;

namespace {

std::shared_ptr<Document> load(const fruit::TriangleMesh& m, const char* name) {
    const auto path = std::filesystem::temp_directory_path() / name;
    fruit::writeStl(path.string(), m);
    auto doc = loadDocument(path.string(), fruit::PrinterProfile::saturn4Ultra16k(), fruit::JobControl::none());
    std::filesystem::remove(path);
    return doc;
}

}  // namespace

TEST_CASE("Layers: a raised box slices into its layers and resin", "[gui][layers]") {
    const auto doc = load(fruit::makeBox({0, 0, 0}, {20, 10, 4}), "layers_box.stl");  // placed 5 mm up
    const LayerResult r = sliceLayers(*doc, {}, 0.0, fruit::JobControl::none());
    REQUIRE(r.layers);
    // 9 mm tall with the lift, 0.05 mm layers.
    CHECK(r.layers->size() == 180);
    CHECK(r.top == Approx(9.0));
    CHECK(r.area.front() == Approx(0.0));        // under the lift: empty
    CHECK(r.area.back() == Approx(200.0).margin(1.0));
    CHECK(r.resin_ml == Approx(0.8).margin(0.01));  // 20 × 10 × 4 mm
    CHECK(r.raft_layers == 0);
    CHECK(r.area[r.biggest] == Approx(200.0).margin(1.0));
    CHECK(r.cups() == 0);
    CHECK(r.traps() == 0);
    // A raft top of 1 mm counts the first 20 layers as raft.
    CHECK(sliceLayers(*doc, {}, 1.0, fruit::JobControl::none()).raft_layers == 20);
}

TEST_CASE("Layers: drawing keeps whole printer pixels and is not mirrored", "[gui][layers]") {
    const auto doc = load(fruit::makeBox({0, 0, 0}, {10, 10, 2}), "layers_draw.stl");
    const LayerResult r = sliceLayers(*doc, {}, 0.0, fruit::JobControl::none());
    const auto& pr = doc->printer;
    const std::size_t top = r.layers->size() - 1;
    // The crop is the box's right half and a margin: lit pixels on the left.
    fruit::BBox3 crop = r.bounds;
    crop.min.x = (r.bounds.min.x + r.bounds.max.x) / 2;
    crop.max.x += 5;
    const LayerImage big = drawLayer(*r.layers, top, crop, 4000, 4000, pr, false);
    CHECK(big.printer_pixels == 1);
    CHECK(big.px == Approx(pr.pixelX()));
    CHECK(big.py == Approx(pr.pixelY()));
    std::size_t left = 0, right = 0;
    for (int y = 0; y < big.h; ++y)
        for (int x = 0; x < big.w; ++x)
            if (big.lit[static_cast<std::size_t>(y) * big.w + x] > 127) (x < big.w / 2 ? left : right)++;
    CHECK(left > 0);
    CHECK(right == 0);  // the right part of the crop is beyond the box
    const LayerImage small = drawLayer(*r.layers, top, crop, 100, 100, pr, true);
    CHECK(small.printer_pixels > 1);
    CHECK(small.w <= 100);
    CHECK(small.h <= 100);
    CHECK(small.px / small.py == Approx(pr.pixelX() / pr.pixelY()));
    CHECK(small.below.size() == small.lit.size());
}

TEST_CASE("Layers: scrolling over the chart moves about 1 % of the layers per unit", "[gui][layers][scroll]") {
    // 674 layers: one unit is 6.74 layers; the fraction is kept for the next time.
    LayerScrollStep s = scrollLayer(100, 674, 1.0, 0);
    CHECK(s.layer == 106);
    CHECK(s.carry == Approx(0.74));
    s = scrollLayer(s.layer, 674, 1.0, s.carry);  // 0.74 + 6.74 = 7.48
    CHECK(s.layer == 113);
    CHECK(s.carry == Approx(0.48));
    // Up and right (positive) go higher, down and left lower.
    CHECK(scrollLayer(100, 674, -1.0, 0).layer == 94);  // trunc(-6.74) = -6
    CHECK(scrollLayer(100, 674, 3.0, 0).layer == 120);
}

TEST_CASE("Layers: a slow swipe steps one layer at a time, a short job still moves one", "[gui][layers][scroll]") {
    // 50 layers: 1 % is under a layer, so a unit is one layer.
    CHECK(scrollLayer(10, 50, 1.0, 0).layer == 11);
    // A trackpad sends small fractions: they add up to whole layers.
    int layer = 20;
    double carry = 0;
    for (int i = 0; i < 10; ++i) {
        const LayerScrollStep s = scrollLayer(layer, 50, 0.1, carry);  // ten tenths of a unit
        layer = s.layer;
        carry = s.carry;
    }
    CHECK(layer == 21);
    CHECK(carry == Approx(0).margin(1e-9));
    // The same, backwards.
    layer = 20;
    carry = 0;
    for (int i = 0; i < 25; ++i) {
        const LayerScrollStep s = scrollLayer(layer, 50, -0.1, carry);
        layer = s.layer;
        carry = s.carry;
    }
    CHECK(layer == 18);
    CHECK(carry == Approx(-0.5));
}

TEST_CASE("Layers: scrolling stops at the first and last layer and owes nothing there", "[gui][layers][scroll]") {
    LayerScrollStep s = scrollLayer(670, 674, 5.0, 0);
    CHECK(s.layer == 673);
    CHECK(s.carry == 0);
    s = scrollLayer(2, 674, -5.0, 0.3);
    CHECK(s.layer == 0);
    CHECK(s.carry == 0);
    // Pushing on at the end does not build up a debt that delays turning back.
    s = scrollLayer(673, 674, 0.1, 0.5);
    s = scrollLayer(s.layer, 674, -1.0, s.carry);
    CHECK(s.layer < 673);
    // Nothing to move through, or no scroll.
    CHECK(scrollLayer(0, 1, 3.0, 0).layer == 0);
    CHECK(scrollLayer(0, 0, 3.0, 0).layer == 0);
    CHECK(scrollLayer(7, 100, 0.0, 0.25).layer == 7);
    CHECK(scrollLayer(7, 100, 0.0, 0.25).carry == Approx(0.25));
    // A shown layer past the end (the job changed) is brought back in.
    CHECK(scrollLayer(900, 674, 0.0, 0).layer == 673);
}
