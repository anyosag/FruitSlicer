// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Builds the GOO header and pixel grid for a print: print settings from a
// SatelLite file or explicit values, the printer's resolution and image
// orientation, and any orientation correction.

#include <string>
#include <vector>

#include "formats/GooFile.h"
#include "printer/PrinterProfile.h"
#include "raster/Rasterizer.h"

namespace fruit {

struct GooPrintRequest {
    double layer_height = 0.05;
    // Header of a .goo exported by SatelLite for this printer: its print
    // settings, identity strings, resolution and mirror flags are copied.
    const GooHeader* settings_from = nullptr;
    // Explicit values override the sample (> 0 or >= 0 to apply).
    double exposure = 0, bottom_exposure = 0;
    int bottom_layers = -1, transition_layers = -1;
    // These too, each for bottom and normal layers alike. > 0: the print's
    // layer height, over the sample's; waits in s, >= 0; lift (and retract)
    // distance in mm, > 0; light power 1-255.
    double own_layer_height = 0;
    double wait_before_lift = -1, wait_after_lift = -1, wait_after_retract = -1;
    double lift_distance = -1;
    int light_power = -1;
    // Orientation correction found with the orientation check.
    bool flip_x = false, flip_y = false;
};

struct GooPrintSetup {
    GooHeader header;
    RasterGrid grid;
    std::vector<std::string> notes;  // e.g. layer height taken from the sample
};

// The header's settings: the sample's (else the printer's own bottom and
// transition counts and FruitSlicer's defaults), then the request's own
// values over them. Exposures may still be missing.
GooHeader printHeader(const PrinterProfile& printer, const GooPrintRequest& r, std::vector<std::string>* notes = nullptr);

// Without a sample, bottom and transition layer counts default to the
// printer's. Throws std::runtime_error when exposure times are missing.
GooPrintSetup buildGooSetup(const PrinterProfile& printer, const GooPrintRequest& r);

}  // namespace fruit
