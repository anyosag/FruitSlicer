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
    int aa_levels = 1;              // 1 = off (SatelLite's default for the 16K)
    unsigned threads = 0;           // 0 = all cores
    double resin_density = 1.15;    // g/ml (weight/volume in SatelLite's 16K file)
    // Seconds per layer spent tilting, added to the time estimate. ~2.4 s
    // reproduces the print time SatelLite 1.0.2.43 wrote for its 16K file.
    double motion_seconds_per_layer = 2.4;
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
