// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Model (already supported and placed in printer coordinates) → GOO file:
// slice every layer, rasterize with exact anti-aliasing, run-length encode
// in parallel, write in order.

#include <string>

#include "formats/GooFile.h"
#include "mesh/TriangleMesh.h"
#include "raster/Rasterizer.h"

namespace fruit {

struct GooExportOptions {
    int aa_levels = 8;
    unsigned threads = 0;           // 0 = all cores
    double resin_density = 1.1;     // g/ml, for the weight estimate
    // Seconds per layer spent tilting/lifting, added to the time estimate.
    // ~3 s from Elegoo's 5.5 s/layer fast-mode figure at 2.5 s exposure;
    // an estimate only.
    double motion_seconds_per_layer = 3.0;
};

struct GooExportResult {
    uint32_t layers = 0;
    double volume_mm3 = 0;          // from rasterized coverage
    uint32_t print_time_s = 0;
    uint64_t bytes = 0;
    double seconds = 0;
};

GooExportResult exportGoo(const TriangleMesh& mesh, GooHeader header, const RasterGrid& grid,
                          const GooExportOptions& o, const std::string& path);

}  // namespace fruit
