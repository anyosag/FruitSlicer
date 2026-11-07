// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Model (already supported and placed in printer coordinates) → GOO file:
// slice every layer, rasterize with exact anti-aliasing, run-length encode
// in parallel, write in order.

#include <string>

#include "core/JobControl.h"
#include "formats/GooFile.h"
#include "layers/LayerStack.h"
#include "mesh/TriangleMesh.h"
#include "raster/Rasterizer.h"

namespace fruit {

struct GooExportOptions {
    int aa_levels = 1;              // 1 = off (SatelLite's default for the 16K)
    unsigned threads = 0;           // 0 = all cores (slicing and rasterizing)
    double resin_density = 1.15;    // g/ml (weight/volume in SatelLite's 16K file)
    // Seconds per layer spent tilting, added to the time estimate. ~2.4 s
    // reproduces the print time SatelLite 1.0.2.43 wrote for its 16K file.
    double motion_seconds_per_layer = 2.4;
    bool sync_to_device = false;    // force the data onto the drive (USB sticks)
    std::string file_time;          // "yyyy-MM-dd HH:mm:ss" to stamp; empty: now (fixed for reproducible files)
};

struct GooExportResult {
    uint32_t layers = 0;
    double volume_mm3 = 0;          // from rasterized coverage
    uint32_t print_time_s = 0;
    uint64_t bytes = 0;
    double seconds = 0;
};

// Slices `mesh` at the header's layer height, then writes the file. Reports
// stages "slice" (first 30 %) and "rasterize". On error or cancel no file is
// left at `path` (a previous file there is kept).
GooExportResult exportGoo(const TriangleMesh& mesh, GooHeader header, const RasterGrid& grid,
                          const GooExportOptions& o, const std::string& path,
                          const JobControl& ctl = JobControl::none());

// Writes already-sliced layers (their layer height must match the header's).
// `preview` (may be null) is rendered for the printer's file thumbnails.
// Reports stage "rasterize".
GooExportResult exportGooLayers(const LayerStack& layers, const TriangleMesh* preview, GooHeader header,
                                const RasterGrid& grid, const GooExportOptions& o, const std::string& path,
                                const JobControl& ctl = JobControl::none());

// Print time the printer will take for `layers` layers with `h`'s settings:
// each layer's exposure and waits, plus the tilt motion per layer.
double estimatePrintSeconds(const GooHeader& h, uint32_t layers, const GooExportOptions& o);

// The layer height a header's float32 value stands for (to the µm).
double gooLayerHeight(const GooHeader& h);

}  // namespace fruit
