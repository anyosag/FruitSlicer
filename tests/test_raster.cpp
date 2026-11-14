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


// The rasterizer as it was before it skipped the columns no edge wrote:
// summing every column of the box, row by row. The rasterizer must give
// exactly the same runs.

struct RefEdge {
    double u0, v0, u1, v1;  // pixel space, v0 < v1
    int dir;                // +1 when the original edge went down (v increasing)
};

// Adds the signed area an edge sweeps into rows [r0, r1) of `acc`
// (row-major, stride `w`, columns relative to `c0`). Coverage of pixel
// (r, c) is |prefix sum of acc over the row up to c|.
void refDrawEdge(const RefEdge& e, int r0, int r1, int c0, int w, std::vector<float>& acc) {
    const double top = std::max(e.v0, static_cast<double>(r0));
    const double bot = std::min(e.v1, static_cast<double>(r1));
    if (bot <= top) return;
    const double dudv = (e.u1 - e.u0) / (e.v1 - e.v0);
    double u = e.u0 + (top - e.v0) * dudv;
    for (int r = static_cast<int>(std::floor(top)); r < r1 && r < static_cast<int>(std::ceil(bot)); ++r) {
        const double ya = std::max(top, static_cast<double>(r));
        const double yb = std::min(bot, static_cast<double>(r + 1));
        const double dy = yb - ya;
        if (dy <= 0) continue;
        const double unext = u + dudv * dy;
        const double d = dy * e.dir;
        float* row = acc.data() + static_cast<size_t>(r - r0) * w;
        double xa = std::min(u, unext) - c0, xb = std::max(u, unext) - c0;
        xa = std::clamp(xa, 0.0, w - 2.0);
        xb = std::clamp(xb, 0.0, w - 2.0);
        const int ia = static_cast<int>(std::floor(xa));
        const int ib = static_cast<int>(std::ceil(xb));
        if (ib <= ia + 1) {
            // Both ends in one pixel column: split at the mean position.
            const double xm = 0.5 * (xa + xb) - ia;
            row[ia] += static_cast<float>(d * (1.0 - xm));
            row[ia + 1] += static_cast<float>(d * xm);
        } else {
            // Edge crosses several columns: trapezoid areas per column.
            const double s = 1.0 / (xb - xa);
            const double fa = xa - ia;
            const double a0 = 0.5 * s * (1.0 - fa) * (1.0 - fa);
            const double fb = xb - ib + 1.0;
            const double am = 0.5 * s * fb * fb;
            row[ia] += static_cast<float>(d * a0);
            if (ib == ia + 2) {
                row[ia + 1] += static_cast<float>(d * (1.0 - a0 - am));
            } else {
                const double a1 = s * (1.5 - fa);
                row[ia + 1] += static_cast<float>(d * (a1 - a0));
                for (int i = ia + 2; i < ib - 1; ++i) row[i] += static_cast<float>(d * s);
                const double a2 = a1 + (ib - ia - 3) * s;
                row[ib - 1] += static_cast<float>(d * (1.0 - a2 - am));
            }
            row[ib] += static_cast<float>(d * am);
        }
        u = unext;
    }
}


void referenceRasterize(const Paths& layer, const RasterGrid& g, int aa_levels, const RunSink& sink) {
    const uint64_t total = static_cast<uint64_t>(g.width) * g.height;
    auto emitZeros = [&](uint64_t n) {
        while (n > 0) {
            const auto k = static_cast<uint32_t>(std::min<uint64_t>(n, 0xFFFFFFF));
            sink(0, k);
            n -= k;
        }
    };
    if (layer.empty()) {
        emitZeros(total);
        return;
    }
    // Clip to the plate so every coordinate is inside the grid.
    const double xl = g.x_left, xr = g.x_left + g.width * g.px;
    const double yt = g.y_top, yb = g.y_top - g.height * g.py;
    const Path plate{toPoint({xl, yb}), toPoint({xr, yb}), toPoint({xr, yt}), toPoint({xl, yt})};
    const Paths clipped = intersection(layer, {plate});
    std::vector<RefEdge> edges;
    double umin = 1e300, umax = -1e300, vmin = 1e300, vmax = -1e300;
    for (const Path& p : clipped)
        for (size_t i = 0; i < p.size(); ++i) {
            const Vec2 a = toVec(p[i]), b = toVec(p[(i + 1) % p.size()]);
            const double ua = g.u(a.x), va = g.v(a.y), ub = g.u(b.x), vb = g.v(b.y);
            umin = std::min({umin, ua, ub});
            umax = std::max({umax, ua, ub});
            vmin = std::min({vmin, va, vb});
            vmax = std::max({vmax, va, vb});
            if (va == vb) continue;
            if (va < vb) edges.push_back({ua, va, ub, vb, 1});
            else edges.push_back({ub, vb, ua, va, -1});
        }
    if (edges.empty()) {
        emitZeros(total);
        return;
    }
    const int r_first = std::clamp(static_cast<int>(std::floor(vmin)), 0, g.height);
    const int r_last = std::clamp(static_cast<int>(std::ceil(vmax)), 0, g.height);  // exclusive
    const int c0 = std::clamp(static_cast<int>(std::floor(umin)), 0, g.width);
    const int c1 = std::clamp(static_cast<int>(std::ceil(umax)), 0, g.width);    // exclusive
    const int w = c1 - c0 + 2;  // accumulation row with slack for the right-hand spill
    std::sort(edges.begin(), edges.end(), [](const RefEdge& a, const RefEdge& b) { return a.v0 < b.v0; });

    emitZeros(static_cast<uint64_t>(r_first) * g.width);
    constexpr int kBand = 64;
    std::vector<float> acc;
    std::vector<uint8_t> row(static_cast<size_t>(c1 - c0));
    size_t first_edge = 0;
    for (int r0 = r_first; r0 < r_last; r0 += kBand) {
        const int r1 = std::min(r_last, r0 + kBand);
        acc.assign(static_cast<size_t>(r1 - r0) * w, 0.0f);
        while (first_edge < edges.size() && edges[first_edge].v1 <= r0) ++first_edge;
        for (size_t i = first_edge; i < edges.size() && edges[i].v0 < r1; ++i)
            if (edges[i].v1 > r0) refDrawEdge(edges[i], r0, r1, c0, w, acc);
        for (int r = r0; r < r1; ++r) {
            const float* a = acc.data() + static_cast<size_t>(r - r0) * w;
            double sum = 0;
            for (int c = 0; c < c1 - c0; ++c) {
                sum += a[c];
                row[c] = quantize(std::abs(sum), aa_levels);
            }
            // Row = zeros left, the bounding-box span, zeros right.
            emitZeros(static_cast<uint64_t>(c0));
            int i = 0;
            while (i < c1 - c0) {
                int j = i + 1;
                while (j < c1 - c0 && row[j] == row[i]) ++j;
                sink(row[i], static_cast<uint32_t>(j - i));
                i = j;
            }
            emitZeros(static_cast<uint64_t>(g.width - c1));
        }
    }
    emitZeros(static_cast<uint64_t>(g.height - r_last) * g.width);
}


// A layer's runs, as the print file gets them.
std::vector<std::pair<uint8_t, uint32_t>> runsOf(const Paths& layer, const RasterGrid& g, int aa, bool reference) {
    std::vector<std::pair<uint8_t, uint32_t>> out;
    const RunSink sink = [&](uint8_t v, uint32_t n) { out.push_back({v, n}); };
    if (reference) referenceRasterize(layer, g, aa, sink);
    else rasterizeLayer(layer, g, aa, sink);
    return out;
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

TEST_CASE("the runs are the same as summing every column of the box", "[raster]") {
    RasterGrid g = smallGrid();
    g.width = 420;  // wide, so most of each row is far from any edge
    g.x_left = -1;
    std::vector<Paths> layers;
    layers.push_back({rect(1, 1, 20, 9)});
    layers.push_back({circle({10, 8}, 6.3, 300), circle({10, 8}, 2.2, 80, false)});            // a ring
    layers.push_back({circle({3, 3}, 1.1, 40), circle({30, 12}, 3.7, 90), rect(36, 2, 41.5, 3)});  // apart, one past the edge
    layers.push_back({{toPoint({2, 1}), toPoint({39, 1.3}), toPoint({38.7, 1.31}), toPoint({2, 1.02})}});  // long, nearly level slivers
    Path star;
    for (int i = 0; i < 46; ++i) {
        const double a = 2 * kPi * i / 46, r = i % 2 ? 2.0 : 7.5;
        star.push_back(toPoint({20 + r * std::cos(a), 8 + 0.9 * r * std::sin(a)}));
    }
    layers.push_back({star});
    layers.push_back({rect(-3, -2, 50, 20)});  // past the plate on every side
    layers.push_back({});
    for (int aa : {1, 2, 4, 8, 16, 256})
        for (bool mx : {false, true}) {
            g.mirror_x = g.mirror_y = mx;
            for (const Paths& l : layers) CHECK(runsOf(l, g, aa, false) == runsOf(l, g, aa, true));
        }
}
