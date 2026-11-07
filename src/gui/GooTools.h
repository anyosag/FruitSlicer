// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The .goo tools (canvas boards "GooInspect" and "GooCompare") without the
// UI: a print file's header in plain words, what reading it back finds, and
// how two files differ.

#include <cstdint>
#include <string>
#include <vector>

#include "formats/GooCompare.h"
#include "formats/GooFile.h"

namespace gui {

struct GooRow {
    std::string label, value;
};
struct GooGroup {
    std::string title;
    std::vector<GooRow> rows;
};

// One file at a glance.
struct GooSummary {
    std::string made_by;  // "Made by ELEGOO SatelLite 1.0.2.43 on 2026-11-05 15:43 · for ELEGOO Saturn 4 Ultra 16K"
    uint32_t layers = 0;
    double height_mm = 0;
    uint32_t print_time_s = 0;
    double volume_ml = 0, weight_g = 0;
    std::string resin;             // "13.1 ml · 15.1 g" ("—" if the header gives none)
    std::vector<GooGroup> groups;  // File, Image, Exposure, Waits, Movement, Layers
};
// `file_bytes`: the file's size, for the layer data it holds.
GooSummary summarizeGoo(const fruit::GooHeader& h, uint64_t file_bytes);

std::string durationText(uint32_t seconds);  // "18 min 40 s", "2 h 05 min"
std::string volumeText(double mm3, int decimals = 1);  // "13.1 ml"; under 0.1 ml, "4.8 mm³"
std::string bytesText(uint64_t bytes);       // "15.3 MB", "12 KB"
std::string mirrorText(bool x, bool y);      // "none", "left to right", "top to bottom", "both ways"

// What reading every layer back found, a line each.
struct GooCheckLine {
    bool ok = true;
    std::string text;
};
std::vector<GooCheckLine> checkLines(const fruit::GooVerifyResult& v, const fruit::GooHeader& h);

// A stored preview (RGB565, big-endian) as RGBA; empty if it isn't w × h.
std::vector<unsigned char> previewRgba(const std::vector<uint8_t>& rgb565, int w, int h);

// A header difference in words, and whether it changes how the print cures
// (the printer, software and names don't; exposures, waits, motion, light
// and the image do).
struct GooDiffRow {
    std::string label, a, b;
    bool cures = false;
};
std::vector<GooDiffRow> diffRows(const std::vector<fruit::GooFieldDiff>& d);

// "As is", "Flip left to right", "Flip top to bottom", "Turn 180°".
std::string transformText(const std::string& transform);

// What the two layer images say, in words. A shape that matches about as
// well flipped as not (a symmetric part) can't show the image orientation,
// and is said to be so rather than called a match "as is".
struct ImageVerdict {
    std::string headline, detail;
    bool clear = false;  // one way round matches clearly better than the rest
};
ImageVerdict imageVerdict(const fruit::GooCompareResult& c);

}  // namespace gui
