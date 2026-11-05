// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "printer/PrinterProfile.h"

namespace fruit {

PrinterProfile PrinterProfile::saturn4Ultra16k() {
    PrinterProfile p;
    p.name = "Elegoo Saturn 4 Ultra 16K";
    p.res_x = 15120;
    p.res_y = 6230;
    p.build_x = 211.68;
    p.build_y = 118.37;
    p.build_z = 220.0;
    p.layer_height = 0.05;
    // Bottom/transition layer counts are resin-profile settings, not printer
    // constants; these are conservative placeholders (UNVERIFIED for any resin).
    p.bottom_layers = 4;
    p.transition_layers = 4;
    return p;
}

}  // namespace fruit
