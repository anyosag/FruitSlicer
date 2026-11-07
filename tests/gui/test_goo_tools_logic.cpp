// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The .goo tools: a print file's header in plain words, what reading it
// back finds, and two files' differences.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <string>

#include "GooTools.h"
#include "formats/GooSetup.h"

using Catch::Matchers::ContainsSubstring;
using namespace gui;

namespace {

fruit::GooHeader sampleHeader() {
    fruit::GooPrintRequest r;
    r.layer_height = 0.03;
    r.exposure = 1.5;
    r.bottom_exposure = 27.5;
    r.bottom_layers = 6;
    r.transition_layers = 6;
    fruit::GooHeader h = fruit::buildGooSetup(fruit::PrinterProfile::saturn4Ultra16k(), r).header;
    h.software_name = "ELEGOO SatelLite";
    h.software_version = "1.0.2.43";
    h.file_time = "2026-11-05 15:43:12";
    h.layer_count = 100;
    h.print_time = 1120;
    h.volume = 13100;
    h.weight = 15.1f;
    h.mirror_y = true;
    return h;
}

const GooRow* rowOf(const GooSummary& s, const std::string& group, const std::string& label) {
    for (const GooGroup& g : s.groups)
        if (g.title == group)
            for (const GooRow& r : g.rows)
                if (r.label == label) return &r;
    return nullptr;
}

}  // namespace

TEST_CASE("a print file's header in plain words", "[goo]") {
    const GooSummary s = summarizeGoo(sampleHeader(), 15'300'000 + fruit::kGooHeaderSize + 11);
    CHECK(s.made_by == "Made by ELEGOO SatelLite 1.0.2.43 on 2026-11-05 15:43 · for ELEGOO Saturn 4 Ultra 16K");
    CHECK(s.layers == 100);
    CHECK(s.height_mm == Catch::Approx(3.0).margin(1e-4));
    CHECK(s.volume_ml == Catch::Approx(13.1));
    REQUIRE(rowOf(s, "File", "Format"));
    CHECK(rowOf(s, "File", "Format")->value == "GOO version 3.0");
    CHECK(rowOf(s, "Image", "Screen")->value == "15120 × 6230");
    CHECK(rowOf(s, "Image", "Mirror")->value == "top to bottom");
    CHECK(rowOf(s, "Image", "Anti-aliasing")->value == "off");
    CHECK(rowOf(s, "Exposure", "Layer height")->value == "0.03 mm");
    CHECK(rowOf(s, "Exposure", "Bottom layers")->value == "6 × 27.5 s");
    CHECK(rowOf(s, "Exposure", "Transition")->value == "6 layers");
    CHECK(rowOf(s, "Exposure", "Normal")->value == "1.5 s");
    CHECK(rowOf(s, "Waits", "After retract")->value == "5 s");
    CHECK(rowOf(s, "Waits", "Bottom layers")->value == "the same");
    CHECK(rowOf(s, "Movement", "Second stage")->value == "none");
    CHECK(rowOf(s, "Movement", "Light power")->value == "255 of 255");
    CHECK(rowOf(s, "Layers", "First")->value == "z 0.03 mm · 27.5 s");
    CHECK(rowOf(s, "Layers", "Last")->value == "z 3 mm · 1.5 s");
    CHECK(rowOf(s, "Layers", "Layer data")->value == "15.3 MB");
}

TEST_CASE("small amounts keep their digits", "[goo]") {
    CHECK(volumeText(13100) == "13.1 ml");
    CHECK(volumeText(13060, 2) == "13.06 ml");
    CHECK(volumeText(4.8) == "4.8 mm³");  // a tiny part isn't "0.0 ml"
    CHECK(bytesText(15'300'000) == "15.3 MB");
    CHECK(bytesText(12'400) == "12 KB");
    CHECK(bytesText(900) == "900 bytes");
    fruit::GooHeader h = sampleHeader();
    CHECK(summarizeGoo(h, 0).resin == "13.1 ml · 15.1 g");
    h.volume = 4.8f;
    h.weight = 0.0055f;
    CHECK(summarizeGoo(h, 0).resin == "4.8 mm³ · 0.01 g");
    h.volume = 0;
    CHECK(summarizeGoo(h, 0).resin == "—");
}

TEST_CASE("durations and mirrors in words", "[goo]") {
    CHECK(durationText(59) == "59 s");
    CHECK(durationText(1120) == "18 min 40 s");
    CHECK(durationText(3600) == "1 h 00 min");
    CHECK(durationText(7500) == "2 h 05 min");
    CHECK(mirrorText(false, false) == "none");
    CHECK(mirrorText(true, false) == "left to right");
    CHECK(mirrorText(true, true) == "both ways");
}

TEST_CASE("reading a file back, line by line", "[goo]") {
    const fruit::GooHeader h = sampleHeader();
    fruit::GooVerifyResult v;
    v.layers = 100;
    v.footer_ok = true;
    v.volume_mm3 = 13060;
    auto lines = checkLines(v, h);
    REQUIRE(lines.size() == 4);
    CHECK(lines[0].text == "All 100 layers read back; every checksum is valid");
    CHECK(lines[1].text == "Only black and white pixels: anti-aliasing is off");
    CHECK(lines[2].text == "Lit pixels add up to 13.06 ml; the header says 13.10 ml");
    CHECK(lines[2].ok);
    CHECK(lines[3].text == "The file ends correctly");

    v.checksums_ok = false;
    v.first_bad_layer = 41;
    v.error = "bad checksum";
    v.layers = 99;
    v.footer_ok = false;
    v.grey_values = 14;
    v.volume_mm3 = 9000;
    lines = checkLines(v, h);
    CHECK_FALSE(lines[0].ok);
    CHECK(lines[0].text == "Layer 42 didn't read back: bad checksum");
    CHECK_FALSE(lines[1].ok);
    CHECK(lines[1].text == "The header says 100 layers, but the file holds 99");
    CHECK_THAT(lines[2].text, ContainsSubstring("14 grey levels"));
    CHECK_FALSE(lines[3].ok);  // 9.00 ml against 13.10
    CHECK_FALSE(lines[4].ok);
}

TEST_CASE("stored pictures decode from RGB565", "[goo]") {
    const std::vector<uint8_t> px = {0xF8, 0x00, 0x07, 0xE0, 0x00, 0x1F, 0xFF, 0xFF};  // red, green, blue, white
    const auto rgba = previewRgba(px, 2, 2);
    REQUIRE(rgba.size() == 16);
    CHECK(rgba[0] == 255);
    CHECK(rgba[1] == 0);
    CHECK(rgba[5] == 255);
    CHECK(rgba[10] == 255);
    CHECK(rgba[12] == 255);
    CHECK(rgba[13] == 255);
    CHECK(rgba[15] == 255);
    CHECK(previewRgba(px, 3, 3).empty());  // the wrong size: none
    CHECK(previewRgba({}, 116, 116).empty());
}

TEST_CASE("header differences in words, those that cure first", "[goo]") {
    const fruit::GooHeader a = sampleHeader();
    fruit::GooHeader b = a;
    b.software_name = "FruitSlicer";
    b.software_version = "0.1.0";
    b.print.exposure = 2.0f;
    b.mirror_x = true;
    const auto rows = diffRows(fruit::diffHeaders(a, b));
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].cures);
    CHECK(rows[1].cures);
    CHECK_FALSE(rows[2].cures);
    CHECK(rows[2].label == "Software");
    CHECK(rows[2].a == "ELEGOO SatelLite 1.0.2.43");
    bool exposure = false, mirror = false;
    for (const auto& r : rows) {
        if (r.label == "Normal exposure") {
            exposure = true;
            CHECK(r.a == "1.5 s");
            CHECK(r.b == "2 s");
        }
        if (r.label == "Mirrored left to right") {
            mirror = true;
            CHECK(r.a == "no");
            CHECK(r.b == "yes");
        }
    }
    CHECK(exposure);
    CHECK(mirror);
    CHECK(diffRows(fruit::diffHeaders(a, a)).empty());
    CHECK(transformText("identity") == "As is");
    CHECK(transformText("flip_xy") == "Turn 180°");
}

TEST_CASE("layer images: a symmetric shape can't show the orientation", "[goo]") {
    fruit::GooCompareResult c;
    auto set = [&](double id, double fx, double fy, double fxy) {
        c.scores = {{"identity", id}, {"flip_x", fx}, {"flip_y", fy}, {"flip_xy", fxy}};
    };
    set(1, 1, 1, 1);  // a square box: every way round matches
    ImageVerdict v = imageVerdict(c);
    CHECK_FALSE(v.clear);
    CHECK(v.headline == "The images match whichever way they're turned");
    set(0.98, 0.31, 0.35, 0.29);  // the F: only one way round
    v = imageVerdict(c);
    CHECK(v.clear);
    CHECK(v.headline == "The images match as they are");
    CHECK(v.detail == "98.0 % of the lit area overlaps that way; the next best, 35.0 %.");
    set(0.30, 0.97, 0.33, 0.28);
    CHECK(imageVerdict(c).headline == "They match once A is flipped left to right");
    set(0.12, 0.15, 0.155, 0.11);  // different models
    v = imageVerdict(c);
    CHECK_FALSE(v.clear);
    CHECK(v.headline == "These show different models");
    CHECK_THAT(v.detail, ContainsSubstring("only 15.5 %"));
}
