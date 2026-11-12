// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "formats/GooSetup.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace fruit {

GooHeader printHeader(const PrinterProfile& printer, const GooPrintRequest& r, std::vector<std::string>* notes) {
    GooHeader h;
    h.layer_height = static_cast<float>(r.layer_height);
    if (r.settings_from) {
        copyPrintSettings(*r.settings_from, h);
        if (notes && r.own_layer_height <= 0 && std::abs(h.layer_height - r.layer_height) > 1e-6)
            notes->push_back("layer height taken from the sample");
    } else {
        // No sample: start from the printer's own bottom/transition counts so
        // the first layers get the long exposures that make the print stick.
        h.print.bottom_layers = static_cast<uint32_t>(std::max(0, printer.bottom_layers));
        h.print.transition_layers = static_cast<uint16_t>(std::max(0, printer.transition_layers));
        h.mirror_x = printer.image_mirror_x;
        h.mirror_y = printer.image_mirror_y;
    }
    GooPrintSettings& p = h.print;
    if (r.own_layer_height > 0) h.layer_height = static_cast<float>(r.own_layer_height);
    if (r.exposure > 0) p.exposure = static_cast<float>(r.exposure);
    if (r.bottom_exposure > 0) p.bottom_exposure = static_cast<float>(r.bottom_exposure);
    if (r.bottom_layers >= 0) p.bottom_layers = static_cast<uint32_t>(r.bottom_layers);
    if (r.transition_layers >= 0) p.transition_layers = static_cast<uint16_t>(r.transition_layers);
    if (r.wait_before_lift >= 0) p.wait_before_lift = p.bottom_wait_before_lift = static_cast<float>(r.wait_before_lift);
    if (r.wait_after_lift >= 0) p.wait_after_lift = p.bottom_wait_after_lift = static_cast<float>(r.wait_after_lift);
    if (r.wait_after_retract >= 0)
        p.wait_after_retract = p.bottom_wait_after_retract = static_cast<float>(r.wait_after_retract);
    if (r.bottom_wait_before_lift >= 0) p.bottom_wait_before_lift = static_cast<float>(r.bottom_wait_before_lift);
    if (r.bottom_wait_after_lift >= 0) p.bottom_wait_after_lift = static_cast<float>(r.bottom_wait_after_lift);
    if (r.bottom_wait_after_retract >= 0) p.bottom_wait_after_retract = static_cast<float>(r.bottom_wait_after_retract);
    if (r.lift_distance > 0)
        p.lift_distance = p.bottom_lift_distance = p.retract_distance = p.bottom_retract_distance =
            static_cast<float>(r.lift_distance);
    if (r.light_power > 0) p.pwm = p.bottom_pwm = static_cast<uint16_t>(std::min(r.light_power, 255));
    return h;
}

GooPrintSetup buildGooSetup(const PrinterProfile& printer, const GooPrintRequest& r) {
    GooPrintSetup s;
    s.header = printHeader(printer, r, &s.notes);
    const GooHeader& h = s.header;
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
