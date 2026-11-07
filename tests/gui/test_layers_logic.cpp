// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The Layers step's logic: slicing, measuring and drawing layers.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>

#include "Document.h"
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
