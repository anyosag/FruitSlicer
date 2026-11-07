// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "formats/GooSetup.h"

#include <cmath>
#include <stdexcept>

namespace fruit {

GooPrintSetup buildGooSetup(const PrinterProfile& printer, const GooPrintRequest& r) {
    GooPrintSetup s;
    GooHeader& h = s.header;
    h.layer_height = static_cast<float>(r.layer_height);
    if (r.settings_from) {
        copyPrintSettings(*r.settings_from, h);
        if (std::abs(h.layer_height - r.layer_height) > 1e-6) s.notes.push_back("layer height taken from the sample");
    } else {
        // No sample: start from the printer's own bottom/transition counts so
        // the first layers get the long exposures that make the print stick.
        h.print.bottom_layers = static_cast<uint32_t>(std::max(0, printer.bottom_layers));
        h.print.transition_layers = static_cast<uint16_t>(std::max(0, printer.transition_layers));
        h.mirror_x = printer.image_mirror_x;
        h.mirror_y = printer.image_mirror_y;
    }
    if (r.exposure > 0) h.print.exposure = static_cast<float>(r.exposure);
    if (r.bottom_exposure > 0) h.print.bottom_exposure = static_cast<float>(r.bottom_exposure);
    if (r.bottom_layers >= 0) h.print.bottom_layers = static_cast<uint32_t>(r.bottom_layers);
    if (r.transition_layers >= 0) h.print.transition_layers = static_cast<uint16_t>(r.transition_layers);
    if (h.print.exposure <= 0 || h.print.bottom_exposure <= 0)
        throw std::runtime_error("exposure times are required: pass --exposure and --bottom-exposure (and "
                                 "--bottom-layers), or --settings-from a .goo exported by SatelLite");
    RasterGrid& g = s.grid;
    g = RasterGrid::fromPrinter(printer);
    g.width = h.res_x;
    g.height = h.res_y;
    g.px = h.size_x / h.res_x;
    g.py = h.size_y / h.res_y;
    g.x_left = -h.size_x / 2;
    g.y_top = h.size_y / 2;
    g.mirror_x = h.mirror_x != r.flip_x;
    g.mirror_y = h.mirror_y != r.flip_y;
    return s;
}

}  // namespace fruit
