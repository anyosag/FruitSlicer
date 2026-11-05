// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
    CHECK(r.layers[1].exposure == Approx(11.0f));   // halfway through the 1-layer ramp
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
