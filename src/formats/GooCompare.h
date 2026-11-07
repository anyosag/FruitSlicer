// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Compares two GOO files: header fields, and whether one layer's image
// matches the other's as is or only after a flip. Used to confirm
// FruitSlicer's image orientation against SatelLite with the orientation
// coupon (an asymmetric "F"): slicers may re-centre a model on import, so
// the images are aligned by their lit-area centroids before comparing.

#include <string>
#include <vector>

#include "formats/GooFile.h"

namespace fruit {

struct GooFieldDiff {
    std::string field, a, b;
};

struct GooOrientationScore {
    std::string transform;  // identity, flip_x, flip_y, flip_xy (applied to file A)
    double iou = 0;
};

struct GooCompareResult {
    std::vector<GooFieldDiff> header;
    uint32_t layer_a = 0, layer_b = 0;
    double z_a = 0, z_b = 0;
    size_t cells_a = 0, cells_b = 0;  // lit cells on the comparison grid
    std::vector<GooOrientationScore> scores;
    std::string best;
    double best_iou = 0;
};

// `z` < 0: compare the layers nearest half the shorter file's height.
GooCompareResult compareGoo(const GooFileInfo& a, const GooFileInfo& b, double z = -1, double cell_mm = 0.12);

std::vector<GooFieldDiff> diffHeaders(const GooHeader& a, const GooHeader& b);

// What a comparison of the same model sliced by FruitSlicer (A) and by
// SatelLite (B) says about the image orientation.
enum class OrientationVerdict { Matches, FlipX, FlipY, Rotated180, Undetermined };

// Undetermined when even the best overlap is below `min_iou` (different
// models, scale or layer).
OrientationVerdict orientationVerdict(const GooCompareResult& c, double min_iou = 0.5);
const char* verdictText(OrientationVerdict v);  // one line, as the CLI prints it

}  // namespace fruit
