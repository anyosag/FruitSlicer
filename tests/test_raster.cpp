// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numeric>

#include "raster/Rasterizer.h"

using namespace fruit;
using Catch::Approx;

namespace {

// 100 × 80 pixels of 0.1 × 0.2 mm (anisotropic like the 16K), covering
// x ∈ [0, 10], y ∈ [0, 16] mm with row 0 at the top (y = 16).
RasterGrid smallGrid() {
    RasterGrid g;
    g.width = 100;
    g.height = 80;
    g.px = 0.1;
    g.py = 0.2;
    g.x_left = 0;
    g.y_top = 16;
    return g;
}

Path rect(double x0, double y0, double x1, double y1) {
    return {toPoint({x0, y0}), toPoint({x1, y0}), toPoint({x1, y1}), toPoint({x0, y1})};
}

Path circle(Vec2 c, double r, int n, bool ccw = true) {
    Path p;
    for (int i = 0; i < n; ++i) {
        const double a = 2 * kPi * i / n * (ccw ? 1 : -1);
        p.push_back(toPoint({c.x + r * std::cos(a), c.y + r * std::sin(a)}));
    }
    return p;
}

// Covered area in mm² from 256-level coverage.
double coveredArea(const std::vector<uint8_t>& img, const RasterGrid& g) {
    double s = 0;
    for (uint8_t v : img) s += v / 255.0;
    return s * g.px * g.py;
}

}  // namespace

TEST_CASE("pixel-aligned rectangle is exactly 255 inside and 0 outside", "[raster]") {
    const RasterGrid g = smallGrid();
    // x 2..5 mm = columns 20..49; y 4..10 mm = rows (16-10)/0.2=30 .. (16-4)/0.2=60 (exclusive)
    const auto img = rasterizeDense({rect(2, 4, 5, 10)}, g, 256);
    REQUIRE(img.size() == 100u * 80u);
    for (int r = 0; r < 80; ++r)
        for (int c = 0; c < 100; ++c) {
            const bool in = c >= 20 && c < 50 && r >= 30 && r < 60;
            CHECK(img[r * 100 + c] == (in ? 255 : 0));
        }
}

TEST_CASE("half-pixel edge gives half coverage", "[raster]") {
    const RasterGrid g = smallGrid();
    // Right edge at x = 5.05 mm: column 50 is half covered.
    const auto img = rasterizeDense({rect(2, 4, 5.05, 10)}, g, 256);
    CHECK(img[40 * 100 + 50] == Approx(128).margin(1));
    CHECK(img[40 * 100 + 49] == 255);
    CHECK(img[40 * 100 + 51] == 0);
}

TEST_CASE("covered area matches polygon area for curved, holed and rotated shapes", "[raster]") {
    const RasterGrid g = smallGrid();
    const Paths disc{circle({5, 8}, 3.3, 200)};
    CHECK(coveredArea(rasterizeDense(disc, g, 256), g) == Approx(area(disc)).epsilon(1e-3));
    const Paths ring{circle({5, 8}, 4, 200), circle({5, 8}, 2, 200, false)};
    CHECK(coveredArea(rasterizeDense(ring, g, 256), g) == Approx(area(ring)).epsilon(1e-3));
    Path rot;
    for (const Vec2 q : {Vec2{-3, -1.5}, Vec2{3, -1.5}, Vec2{3, 1.5}, Vec2{-3, 1.5}}) {
        const double a = deg2rad(30);
        rot.push_back(toPoint({5 + q.x * std::cos(a) - q.y * std::sin(a), 8 + q.x * std::sin(a) + q.y * std::cos(a)}));
    }
    CHECK(coveredArea(rasterizeDense({rot}, g, 256), g) == Approx(18.0).epsilon(1e-3));
}

TEST_CASE("orientation: +X right, +Y up (row 0 at the top); mirrors flip", "[raster]") {
    RasterGrid g = smallGrid();
    const Paths corner{rect(8, 13, 9, 15)};  // near +X, +Y
    auto img = rasterizeDense(corner, g, 256);
    CHECK(img[10 * 100 + 85] == 255);  // row (16-14)/0.2 = 10, column 85
    CHECK(img[70 * 100 + 15] == 0);
    g.mirror_x = true;
    img = rasterizeDense(corner, g, 256);
    CHECK(img[10 * 100 + 14] == 255);  // mirrored column 100-86
}

TEST_CASE("empty layers, quantisation and the full 16K grid", "[raster]") {
    const RasterGrid g = smallGrid();
    const auto empty = rasterizeDense({}, g, 8);
    CHECK(std::all_of(empty.begin(), empty.end(), [](uint8_t v) { return v == 0; }));
    CHECK(quantize(0.0, 8) == 0);
    CHECK(quantize(1.0, 8) == 255);
    CHECK(quantize(0.5, 8) == 146);  // round(3.5)=4 of 7 steps -> 145.7
    CHECK(quantize(0.49, 1) == 0);
    CHECK(quantize(0.5, 1) == 255);

    // Full Saturn 16K grid: runs add up to every pixel; area is preserved.
    const RasterGrid big = RasterGrid::fromPrinter(PrinterProfile::saturn4Ultra16k());
    const Paths disc{circle({10, -5}, 6, 400)};
    uint64_t pixels = 0;
    double covered = 0;
    rasterizeLayer(disc, big, 256, [&](uint8_t v, uint32_t n) {
        pixels += n;
        covered += v / 255.0 * n;
    });
    CHECK(pixels == 15120ull * 6230ull);
    CHECK(covered * big.px * big.py == Approx(area(disc)).epsilon(1e-3));
}
