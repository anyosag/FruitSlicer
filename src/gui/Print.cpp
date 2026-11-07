// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Print.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <system_error>

#include "Document.h"
#include "Layers.h"
#include "raster/Rasterizer.h"
#include "support/SupportSettings.h"

namespace gui {

std::shared_ptr<const fruit::GooHeader> loadSample(const std::string& path) {
    auto h = std::make_shared<const fruit::GooHeader>(fruit::readGooHeader(path));
    // Exposures are for one printer's light: a file made for another is no guide.
    const fruit::PrinterProfile pr = fruit::PrinterProfile::saturn4Ultra16k();
    if (h->res_x != pr.res_x || h->res_y != pr.res_y) {
        const std::string who = h->printer_name.empty() ? std::string("another printer") : h->printer_name;
        throw std::runtime_error("This file was made for " + who + " (" + std::to_string(h->res_x) + " × " +
                                 std::to_string(h->res_y) + " pixels). FruitSlicer needs one made for the " +
                                 fruit::GooHeader{}.printer_name + " (" + std::to_string(pr.res_x) + " × " +
                                 std::to_string(pr.res_y) + ").");
    }
    return h;
}

bool fitsPlate(const fruit::PrinterProfile& pr, const fruit::BBox3& b) {
    const double e = 1e-6;
    return std::max(std::abs(b.min.x), std::abs(b.max.x)) <= pr.build_x / 2 + e &&
           std::max(std::abs(b.min.y), std::abs(b.max.y)) <= pr.build_y / 2 + e && b.min.z >= -e &&
           b.max.z <= pr.build_z + e;
}

PrintPlan planPrint(const Document& doc, const LayerResult& layers, double raft_top, const PrintSettings& s) {
    PrintPlan p;
    p.fits = fitsPlate(doc.printer, layers.bounds);
    fruit::GooPrintRequest r;
    r.layer_height = layers.layers ? layers.layers->layer_height : kLayerHeight;
    r.settings_from = s.sample.get();
    r.exposure = s.exposure;
    r.bottom_exposure = s.bottom_exposure;
    r.bottom_layers = s.bottom_layers;
    r.transition_layers = s.transition_layers;
    r.flip_x = s.flip_x;
    r.flip_y = s.flip_y;
    try {
        p.setup = fruit::buildGooSetup(doc.printer, r);
    } catch (const std::exception&) {
        p.why_not = s.sample ? "The SatelLite file has no exposure times. Type them in."
                             : "Choose a SatelLite .goo saved for this printer, or type the exposure times.";
        return p;
    }
    p.ready = true;
    p.layer_height = fruit::gooLayerHeight(p.setup.header);
    const double job_h = layers.layers ? layers.layers->layer_height : kLayerHeight;
    p.reslice = std::abs(p.layer_height - job_h) > 1e-6;
    p.height = layers.top;
    p.layers = static_cast<std::size_t>(std::ceil(layers.top / p.layer_height - 1e-9));
    p.resin_ml = layers.resin_ml;
    const fruit::GooExportOptions o;
    p.resin_g = p.resin_ml * o.resin_density;
    p.print_time_s = fruit::estimatePrintSeconds(p.setup.header, static_cast<uint32_t>(p.layers), o);
    const fruit::GooHeader& h = p.setup.header;
    p.raft_needed = fruit::minRaftThickness(h.print.bottom_layers, h.print.transition_layers, p.layer_height);
    p.raft_thick_enough = doc.placement().on_plate || raft_top + 1e-9 >= p.raft_needed;
    return p;
}

SaveResult savePrint(const Document& doc, const LayerResult& layers, const fruit::TriangleMesh& supports,
                     const PrintPlan& plan, const std::string& path, bool removable, const fruit::JobControl& ctl) {
    if (!plan.ready) throw std::runtime_error(plan.why_not);
    const auto t0 = std::chrono::steady_clock::now();
    SaveResult r;
    r.path = path;
    fruit::TriangleMesh all = doc.placed();
    all.append(supports);
    fruit::GooExportOptions o;
    o.sync_to_device = removable;
    if (plan.reslice) {
        // The sample's exposures are for its layer height: cut the layers again.
        r.written = fruit::exportGoo(all, plan.setup.header, plan.setup.grid, o, path, ctl.sub(0.0, 0.85));
    } else {
        r.written = fruit::exportGooLayers(*layers.layers, &all, plan.setup.header, plan.setup.grid, o, path,
                                           ctl.sub(0.0, 0.85));
    }
    try {
        r.check = fruit::verifyGoo(path, ctl.sub(0.85, 1.0));
    } catch (...) {
        // Cancelled or unreadable: a file that was not checked is not left
        // looking saved.
        std::error_code ec;
        std::filesystem::remove(path, ec);
        throw;
    }
    r.volume_matches = r.written.volume_mm3 > 0 &&
                       std::abs(r.check.volume_mm3 - r.written.volume_mm3) <= 0.005 * r.written.volume_mm3;
    r.good = r.check.checksums_ok && r.check.footer_ok && r.check.layers == r.written.layers && r.volume_matches &&
             r.check.grey_values == 0;
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

}  // namespace gui
