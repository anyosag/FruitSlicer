// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once

#include <string>

#include "geometry/Vec.h"

namespace fruit {

// Printer constants. Coordinates are mm with (0, 0) at the centre of the
// build plate. Values for the Saturn 4 Ultra 16K come from Elegoo's product
// page (15120×6230 px LCD, 211.68×118.37×220 mm); pixel pitch is derived
// from them (14.0 × 19.0 µm — the pixels are not square).
struct PrinterProfile {
    std::string name;
    int res_x = 0, res_y = 0;
    double build_x = 0, build_y = 0, build_z = 0;
    double layer_height = 0.05;
    int bottom_layers = 4;      // long-exposure layers at the start of a print
    int transition_layers = 0;  // ramp from bottom to normal exposure

    // Tilt release: direction (in plate XY) from the hinge towards the far
    // side, where Wu et al. (2016) measured the highest peel stress. Unknown
    // until measured on the real printer, so the default is "no bias".
    bool peel_axis_known = false;
    Vec2 peel_axis{0, 0};

    double pixelX() const { return build_x / res_x; }
    double pixelY() const { return build_y / res_y; }
    // Minimum thickness the raft needs so long bottom exposures never reach tips.
    double minRaftThickness() const { return (bottom_layers + transition_layers) * layer_height; }
    bool fitsXY(double x, double y) const { return std::abs(x) <= build_x / 2 && std::abs(y) <= build_y / 2; }

    static PrinterProfile saturn4Ultra16k();
};

}  // namespace fruit
