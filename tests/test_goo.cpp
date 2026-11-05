// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>

#include "formats/GooExport.h"
#include "formats/GooFile.h"
#include "mesh/Primitives.h"
#include "raster/Rasterizer.h"

using namespace fruit;
using Catch::Approx;

namespace {

std::string tmp(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / "fruit_test_goo";
    std::filesystem::create_directories(dir);
    return (dir / name).string();
}

// Encode a pixel vector run by run, then decode it back.
std::vector<uint8_t> roundTrip(const std::vector<uint8_t>& px, std::vector<uint8_t>* rle_out = nullptr) {
    GooRleEncoder enc;
    size_t i = 0;
    while (i < px.size()) {
        size_t j = i + 1;
        while (j < px.size() && px[j] == px[i]) ++j;
        enc.add(px[i], static_cast<uint32_t>(j - i));
        i = j;
    }
    const auto rle = enc.finish();
    if (rle_out) *rle_out = rle;
    std::vector<uint8_t> back(px.size());
    decodeGooRle(rle, back);
    return back;
}

}  // namespace

TEST_CASE("GOO RLE round trips every chunk type and run-length size", "[goo]") {
    for (uint32_t run : {1u, 2u, 15u, 16u, 255u, 256u, 0xFFFu, 0x1000u, 0xFFFFFu, 0x100000u, 0x2345678u}) {
        for (uint8_t v : {uint8_t(0), uint8_t(1), uint8_t(128), uint8_t(254), uint8_t(255)}) {
            std::vector<uint8_t> px(run, v);
            px.push_back(uint8_t(v ^ 0x5A));  // a different neighbour so runs end
            CHECK(roundTrip(px) == px);
        }
    }
    // Smooth gradients exercise the difference chunks (both directions, runs 1 and >1).
    std::vector<uint8_t> g;
    for (int k = 0; k < 3; ++k)
        for (int v = 1; v < 255; ++v) g.insert(g.end(), (v % 3) + 1, uint8_t(k % 2 ? 255 - v : v));
    CHECK(roundTrip(g) == g);
    std::mt19937 rng(7);
    std::vector<uint8_t> noisy(50000);
    for (auto& v : noisy) v = (rng() % 4 == 0) ? uint8_t(rng()) : uint8_t(0);
    CHECK(roundTrip(noisy) == noisy);
}

TEST_CASE("GOO RLE framing: magic first, inverted byte-sum checksum last", "[goo]") {
    std::vector<uint8_t> rle;
    roundTrip(std::vector<uint8_t>(1000, 255), &rle);
    REQUIRE(rle.size() >= 3);
    CHECK(rle.front() == 0x55);
    uint8_t sum = 0;
    for (size_t i = 1; i + 1 < rle.size(); ++i) sum = uint8_t(sum + rle[i]);
    CHECK(rle.back() == uint8_t(~sum));
    // 1000 white pixels: type 11, two extra length bytes? 1000 = 0x3E8 -> one extra byte.
    CHECK(rle[1] == uint8_t(0b11 << 6 | 1 << 4 | 0x8));
    CHECK(rle[2] == 0x3E);
    rle[1] ^= 1;
    std::vector<uint8_t> px(1000);
    CHECK_THROWS(decodeGooRle(rle, px));
}

TEST_CASE("GOO header is 195477 bytes; empty file round trips", "[goo]") {
    GooHeader h;
    h.print.exposure = 2.5f;
    h.print.bottom_exposure = 30.0f;
    h.print.bottom_layers = 4;
    h.print.transition_layers = 3;
    h.print.lift_speed = 1.25f;
    h.printer_name = "Test Printer";
    const std::string path = tmp("empty.goo");
    {
        GooWriter w(path, h);
        w.finish(h);
    }
    CHECK(std::filesystem::file_size(path) == kGooHeaderSize + 11);
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
    CHECK(std::string(b.begin(), b.begin() + 4) == "V3.0");
    CHECK(b[4] == 0x07);
    CHECK(b[8] == 0x44);
    // Total layers at offset 195310, X resolution (15120 = 0x3B10) right after.
    CHECK(b[195314] == 0x3B);
    CHECK(b[195315] == 0x10);
    // Layer-content offset (field 59) holds the header size.
    const size_t off = kGooHeaderSize - 2 - 1 - 4;
    CHECK((uint32_t(b[off]) << 24 | uint32_t(b[off + 1]) << 16 | uint32_t(b[off + 2]) << 8 | b[off + 3]) == kGooHeaderSize);
    const GooFileInfo r = readGoo(path);
    CHECK(r.footer_ok);
    CHECK(r.header.printer_name == "Test Printer");
    CHECK(r.header.print.exposure == 2.5f);
    CHECK(r.header.print.lift_speed == 1.25f);
    CHECK(r.header.print.transition_layers == 3);
}

TEST_CASE("GOO file round trip on a small grid: pixels and layer settings", "[goo]") {
    RasterGrid g;
    g.width = 64;
    g.height = 48;
    g.px = 0.25;
    g.py = 0.25;
    g.x_left = -8;
    g.y_top = 6;
    GooHeader h;
    h.res_x = 64;
    h.res_y = 48;
    h.size_x = 16;
    h.size_y = 12;
    h.layer_height = 0.1f;
    h.layer_count = 3;
    h.print.exposure = 2.0f;
    h.print.bottom_exposure = 20.0f;
    h.print.bottom_layers = 1;
    h.print.transition_layers = 1;
    const std::vector<Paths> layers = {
        {{toPoint({-3, -3}), toPoint({3, -3}), toPoint({3, 3}), toPoint({-3, 3})}},
        {{toPoint({-5.1, -2.2}), toPoint({4.3, -1}), toPoint({0, 4.9})}},
        {},
    };
    const std::string path = tmp("small.goo");
    std::vector<std::vector<uint8_t>> dense;
    {
        GooWriter w(path, h);
        for (uint32_t i = 0; i < 3; ++i) {
            GooRleEncoder enc;
            rasterizeLayer(layers[i], g, 8, [&](uint8_t v, uint32_t n) { enc.add(v, n); });
            w.addLayer(layerDefFor(h, i), enc.finish());
            dense.push_back(rasterizeDense(layers[i], g, 8));
        }
        w.finish(h);
    }
    const GooFileInfo r = readGoo(path);
    REQUIRE(r.layers.size() == 3);
    CHECK(r.footer_ok);
    for (int i = 0; i < 3; ++i) {
        std::vector<uint8_t> px(64 * 48);
        decodeGooRle(r.rle[i], px);
        CHECK(px == dense[i]);
    }
    CHECK(r.layers[0].exposure == 20.0f);           // bottom
    CHECK(r.layers[1].exposure == Approx(2.0f));    // a 1-layer ramp reaches normal (SatelLite's rule)
    CHECK(r.layers[2].exposure == 2.0f);            // normal
    CHECK(r.layers[2].z == Approx(0.3f));
}

TEST_CASE("settings copied from a sample file", "[goo]") {
    GooHeader sample;
    sample.version = "V3.0";
    sample.printer_name = "ELEGOO Saturn 4 Ultra 16K (sample)";
    sample.layer_height = 0.03f;
    sample.mirror_x = true;
    sample.print.exposure = 1.8f;
    sample.print.bottom_exposure = 25.0f;
    sample.print.bottom_layers = 5;
    sample.print.wait_after_lift = 0.7f;
    GooHeader mine;
    copyPrintSettings(sample, mine);
    CHECK(mine.printer_name == sample.printer_name);
    CHECK(mine.layer_height == 0.03f);
    CHECK(mine.mirror_x);
    CHECK(mine.print.exposure == 1.8f);
    CHECK(mine.print.wait_after_lift == 0.7f);
    CHECK(mine.software_name == "FruitSlicer");
}

TEST_CASE("export a cube on the full 16K grid: layers, volume, decodable", "[goo]") {
    TriangleMesh cube = makeBox({-5, -5, 0}, {5, 5, 2});
    GooHeader h;
    h.layer_height = 0.05f;
    h.print.exposure = 2.5f;
    h.print.bottom_exposure = 25.0f;
    h.print.bottom_layers = 4;
    const RasterGrid grid = RasterGrid::fromPrinter(PrinterProfile::saturn4Ultra16k());
    const std::string path = tmp("cube.goo");
    GooExportOptions o;
    o.aa_levels = 8;
    const GooExportResult res = exportGoo(cube, h, grid, o, path);
    CHECK(res.layers == 40);
    CHECK(res.volume_mm3 == Approx(200.0).epsilon(0.005));
    const GooFileInfo r = readGoo(path);
    CHECK(r.header.layer_count == 40);
    CHECK(r.header.volume == Approx(200.0).epsilon(0.005));
    CHECK(r.header.small_preview.size() == 116u * 116u * 2u);
    std::vector<uint8_t> px(15120u * 6230u);
    decodeGooRle(r.rle[20], px);
    double lit = 0;
    for (uint8_t v : px) lit += v / 255.0;
    CHECK(lit * grid.px * grid.py == Approx(100.0).epsilon(0.005));
}

#include "formats/GooCompare.h"

namespace {

// Small GOO file (64×48 px of 0.25 mm) holding `layers`.
std::string writeSmall(const std::string& name, const std::vector<Paths>& layers) {
    RasterGrid g;
    g.width = 64;
    g.height = 48;
    g.px = 0.25;
    g.py = 0.25;
    g.x_left = -8;
    g.y_top = 6;
    GooHeader h;
    h.res_x = 64;
    h.res_y = 48;
    h.size_x = 16;
    h.size_y = 12;
    h.layer_height = 0.1f;
    h.layer_count = static_cast<uint32_t>(layers.size());
    h.print.exposure = 2;
    h.print.bottom_exposure = 20;
    const std::string path = tmp(name);
    GooWriter w(path, h);
    for (uint32_t i = 0; i < layers.size(); ++i) {
        GooRleEncoder enc;
        rasterizeLayer(layers[i], g, 1, [&](uint8_t v, uint32_t n) { enc.add(v, n); });
        w.addLayer(layerDefFor(h, i), enc.finish());
    }
    w.finish(h);
    return path;
}

// An "F" made of three rectangles, optionally mirrored in Y and shifted.
Paths letterF(double dx, double dy, bool mirror_y) {
    auto rect = [&](double x0, double y0, double x1, double y1) {
        if (mirror_y) {
            std::swap(y0, y1);
            y0 = -y0;
            y1 = -y1;
        }
        return Path{toPoint({x0 + dx, y0 + dy}), toPoint({x1 + dx, y0 + dy}), toPoint({x1 + dx, y1 + dy}),
                    toPoint({x0 + dx, y1 + dy})};
    };
    return unionNonZero({rect(-2, -4, -1, 4), rect(-1, 3, 3, 4), rect(-1, 0, 2, 1)});
}

}  // namespace

TEST_CASE("transition ramp matches SatelLite's 16K file", "[goo]") {
    GooHeader h;
    h.print.bottom_exposure = 27.5f;
    h.print.exposure = 1.5f;
    h.print.bottom_layers = 6;
    h.print.transition_layers = 6;
    const float expect[] = {27.5f, 27.5f, 27.5f, 27.5f, 27.5f, 27.5f, 23.17f, 18.83f, 14.5f, 10.17f, 5.83f, 1.5f, 1.5f};
    for (uint32_t i = 0; i < 13; ++i) CHECK(layerDefFor(h, i).exposure == Approx(expect[i]).margin(1e-4));
    CHECK(layerDefFor(h, 0).wait_after_retract == 5.0f);  // SatelLite waits 5 s after retract
    CHECK(h.printer_type == "ELEGOO Saturn 4 Ultra 16K");
}

TEST_CASE("16K defaults: anti-aliasing off like SatelLite, Y-mirrored rows", "[goo]") {
    const RasterGrid grid = RasterGrid::fromPrinter(PrinterProfile::saturn4Ultra16k());
    CHECK(grid.mirror_y);
    CHECK_FALSE(grid.mirror_x);
    TriangleMesh cube = makeBox({-3, -2, 0}, {4, 5, 0.3});
    GooHeader h;
    h.layer_height = 0.1f;
    h.print.exposure = 1.5f;
    h.print.bottom_exposure = 27.5f;
    const std::string path = tmp("aaoff.goo");
    exportGoo(cube, h, grid, GooExportOptions{}, path);
    const GooFileInfo r = readGoo(path);
    CHECK(r.header.aa_level == 0);
    CHECK(r.header.grey_level == 0);
    CHECK_FALSE(r.header.grayscale_8bit);
    CHECK(r.header.mirror_y);
    std::vector<uint8_t> px(15120u * 6230u);
    decodeGooRle(r.rle[1], px);
    const auto grey = std::count_if(px.begin(), px.end(), [](uint8_t v) { return v != 0 && v != 255; });
    CHECK(grey == 0);
    // Mirroring Y reverses the row order exactly.
    RasterGrid a;
    a.width = 40;
    a.height = 30;
    a.px = 0.2;
    a.py = 0.2;
    a.x_left = -4;
    a.y_top = 3;
    RasterGrid b = a;
    b.mirror_y = true;
    const Paths f = letterF(0, 0, false);
    const auto ia = rasterizeDense(f, a, 1), ib = rasterizeDense(f, b, 1);
    for (int y = 0; y < 30; ++y)
        for (int x = 0; x < 40; ++x) CHECK(ia[y * 40 + x] == ib[(29 - y) * 40 + x]);
}

TEST_CASE("goo-compare finds identity, a Y flip, and a 180-degree turn", "[goo]") {
    const std::string a = writeSmall("cmp_a.goo", {letterF(0, 0, false), letterF(0, 0, false)});
    const std::string same = writeSmall("cmp_same.goo", {letterF(2.5, -1, false), letterF(2.5, -1, false)});
    const std::string mirrored = writeSmall("cmp_flip.goo", {letterF(-1.5, 0.5, true), letterF(-1.5, 0.5, true)});
    const GooFileInfo fa = readGoo(a);
    GooCompareResult r = compareGoo(fa, readGoo(same), -1, 0.25);
    CHECK(r.best == "identity");
    CHECK(r.best_iou > 0.95);
    CHECK(r.header.empty());
    r = compareGoo(fa, readGoo(mirrored), -1, 0.25);
    CHECK(r.best == "flip_y");
    CHECK(r.best_iou > 0.95);
    for (const auto& s : r.scores)
        if (s.transform == "identity") CHECK(s.iou < 0.8);
    // Mirrored in X is told apart from mirrored in Y.
    Paths fx;
    for (Path p : letterF(0, 0, false)) {
        for (auto& q : p) q.x = -q.x;
        std::reverse(p.begin(), p.end());
        fx.push_back(p);
    }
    r = compareGoo(fa, readGoo(writeSmall("cmp_fx.goo", {fx, fx})), -1, 0.25);
    CHECK(r.best == "flip_x");
}

TEST_CASE("float32 layer heights from a print file do not add a layer", "[goo]") {
    TriangleMesh cyl = makeCylinder({0, 0, 0}, 5, 3.0, 64);
    const float h32 = 0.03f;  // stored as 0.0299999993
    GooHeader h;
    h.layer_height = h32;
    h.print.exposure = 1.5f;
    h.print.bottom_exposure = 27.5f;
    const GooExportResult r = exportGoo(cyl, h, RasterGrid::fromPrinter(PrinterProfile::saturn4Ultra16k()),
                                        GooExportOptions{}, tmp("float_layers.goo"));
    CHECK(r.layers == 100);
}
