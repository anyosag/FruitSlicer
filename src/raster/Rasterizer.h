// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Layer rasterizer for the printer's LCD mask.
//
// Every pixel gets its exact covered fraction (analytic anti-aliasing, the
// signed-area accumulation method used by font rasterizers), computed in
// millimetres so the Saturn 4 Ultra 16K's non-square 14 × 19 µm pixels need
// no special handling. Output is a stream of runs (value, count) in
// row-major order: empty rows and the margins around the part become single
// runs, so a 94-megapixel layer costs only its bounding box.

#include <cstdint>
#include <functional>
#include <vector>

#include "geometry/Polygon.h"
#include "printer/PrinterProfile.h"

namespace fruit {

struct RasterGrid {
    int width = 0, height = 0;   // pixels
    double px = 0, py = 0;       // mm per pixel in X and Y
    double x_left = 0;           // mm at the left edge of column 0
    double y_top = 0;            // mm at the top edge of row 0 (row 0 is at +Y)
    // Orientation of the image relative to the plate. PROVISIONAL until
    // compared with a SatelLite file of the same model (coupon orientation).
    bool mirror_x = false, mirror_y = false;

    static RasterGrid fromPrinter(const PrinterProfile& p);
    // Continuous pixel coordinates (u right, v down) of a plate point in mm.
    double u(double x_mm) const;
    double v(double y_mm) const;
};

// Receives runs of identical pixel values in row-major order.
using RunSink = std::function<void(uint8_t value, uint32_t count)>;

// Coverage → grey: `aa_levels` grey levels (≥ 2) from 0 to 255; 1 = on/off
// at 50 % coverage.
uint8_t quantize(double coverage, int aa_levels);

// Rasterize one layer (unioned polygons, plate coordinates in mm) and emit
// its pixels as runs. Polygons are clipped to the plate first.
void rasterizeLayer(const Paths& layer, const RasterGrid& g, int aa_levels, const RunSink& sink);

// Dense image (width × height bytes), for tests and previews of small grids.
std::vector<uint8_t> rasterizeDense(const Paths& layer, const RasterGrid& g, int aa_levels);

}  // namespace fruit
