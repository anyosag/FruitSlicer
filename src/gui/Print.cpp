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
#include "Hollows.h"
#include "Layers.h"
#include "layers/AdaptiveLayers.h"
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

fruit::GooPrintRequest printRequest(const PrintSettings& s, double layer_height) {
    fruit::GooPrintRequest r;
    r.layer_height = layer_height;
    r.settings_from = s.sample.get();
    r.exposure = s.exposure;
    r.bottom_exposure = s.bottom_exposure;
    r.bottom_layers = s.bottom_layers;
    r.transition_layers = s.transition_layers;
    r.own_layer_height = s.layer_height;
    r.wait_before_lift = s.wait_before_lift;
    r.wait_after_lift = s.wait_after_lift;
    r.wait_after_retract = s.wait_after_retract;
    r.bottom_wait_before_lift = s.bottom_wait_before_lift;
    r.bottom_wait_after_lift = s.bottom_wait_after_lift;
    r.bottom_wait_after_retract = s.bottom_wait_after_retract;
    r.lift_distance = s.lift;
    r.light_power = s.light_power;
    r.flip_x = s.flip_x;
    r.flip_y = s.flip_y;
    return r;
}

double resinDensity(const PrintEdits& e) { return e.density > 0 ? e.density : fruit::GooExportOptions{}.resin_density; }

double heightCorrection(const PrintEdits& e) {
    return e.height_correction >= 0 ? e.height_correction : fruit::kDefaultHeightCorrection;
}

double lightDepth(const PrintEdits& e) {
    return e.light_depth >= 0 ? e.light_depth : fruit::kDefaultLightDepth;
}

int adaptiveCount(const PrintEdits& e, double layer_height) {
    return fruit::maxAdaptiveCount(layer_height, e.adaptive);
}

int baseLayers(const Document& doc, double layer_height) {
    if (doc.parts().empty() || !doc.placement().on_plate) return 0;
    return fruit::heightCorrectionLayers(heightCorrection(doc.print.settings), layer_height);
}

PrintPlan planPrint(const Document& doc, const LayerResult& layers, double raft_top, const PrintSettings& s) {
    PrintPlan p;
    p.fits = fitsPlate(doc.printer, layers.bounds);
    const fruit::GooPrintRequest r = printRequest(s, layers.layers ? layers.layers->layer_height : kLayerHeight);
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
    // The height correction is whole layers of the print file's height.
    const int base = baseLayers(doc, p.layer_height);
    p.base_layers = static_cast<std::size_t>(base);
    const double extra = base * p.layer_height - layers.base_layers * job_h;
    p.height = layers.top + extra;
    p.layers = static_cast<std::size_t>(std::ceil(p.height / p.layer_height - 1e-9));
    fruit::BBox3 b = layers.bounds;
    b.max.z += extra;
    p.fits = fitsPlate(doc.printer, b);
    p.adaptive = adaptiveCount(s, p.layer_height);
    p.light_depth = lightDepth(s);
    p.resin_ml = layers.resin_ml;
    fruit::GooExportOptions o;
    o.resin_density = p.density = resinDensity(s);
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
    o.resin_density = plan.density;
    // On the plate: the height correction's layers, at the print file's height.
    const int base = baseLayers(doc, fruit::gooLayerHeight(plan.setup.header));
    const double h = fruit::gooLayerHeight(plan.setup.header);
    fruit::AdaptiveSettings ad;
    ad.multiplier = plan.adaptive;
    ad.light_depth = plan.light_depth;
    ad.protected_layers = static_cast<std::size_t>(base);
    // Writes `print` (model and supports); with adaptive layers on, `model`
    // (the model alone, thin layers) decides where they merge.
    auto write = [&](const fruit::LayerStack& print, const fruit::LayerStack& model, const fruit::JobControl& c) {
        if (plan.adaptive > 1)
            r.written = fruit::exportGooAdaptive(print, model, &all, plan.setup.header, plan.setup.grid, o, ad, path, c);
        else
            r.written = fruit::exportGooLayers(print, &all, plan.setup.header, plan.setup.grid, o, path, c);
    };
    if (plan.reslice && layers.hollow) {
        // The same again at the sample's layer height: the model hollowed
        // or filled as before, with the same holes, and the supports.
        HollowChoices c = layers.hollow->choices;
        c.auto_holes = false;
        const HollowPlan again = planHollow(doc, c, layers.hollow->holes, h, ctl.sub(0.0, 0.3));
        fruit::LayerStack print =
            fruit::unionLayers(*again.layers, fruit::sliceModel(supports, h, nullptr, 0, ctl.sub(0.3, 0.4)));
        fruit::addBaseLayers(print, base);
        fruit::LayerStack model;
        if (plan.adaptive > 1) {
            model = *again.layers;  // the hollowed model's own layers
            fruit::addBaseLayers(model, base);
        }
        write(print, model, ctl.sub(0.4, 0.85));
    } else if (plan.reslice) {
        // The sample's exposures are for its layer height: cut the layers again.
        const bool merge = plan.adaptive > 1;
        fruit::LayerStack print = fruit::sliceModel(all, h, nullptr, 0, ctl.sub(0.0, merge ? 0.2 : 0.4));
        fruit::addBaseLayers(print, base);
        fruit::LayerStack model;
        if (plan.adaptive > 1) {
            model = fruit::sliceModel(doc.placed(), h, nullptr, 0, ctl.sub(0.2, 0.4));
            fruit::addBaseLayers(model, base);
        }
        write(print, model, ctl.sub(0.4, 0.85));
    } else {
        fruit::LayerStack model;
        if (plan.adaptive > 1) {
            if (layers.hollow) model = *layers.hollow->layers;
            else model = fruit::sliceModel(doc.placed(), h, nullptr, 0, ctl.sub(0.0, 0.3));
            fruit::addBaseLayers(model, static_cast<int>(layers.base_layers));
        }
        write(*layers.layers, model, plan.adaptive > 1 ? ctl.sub(0.3, 0.85) : ctl.sub(0.0, 0.85));
    }
    readBack(r, ctl.sub(0.85, 1.0));
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

void readBack(SaveResult& r, const fruit::JobControl& ctl) {
    try {
        r.check = fruit::verifyGoo(r.path, ctl);
    } catch (...) {
        // Cancelled or unreadable: a file that was not checked is not left
        // looking saved.
        std::error_code ec;
        std::filesystem::remove(r.path, ec);
        throw;
    }
    r.volume_matches = r.written.volume_mm3 > 0 &&
                       std::abs(r.check.volume_mm3 - r.written.volume_mm3) <= 0.005 * r.written.volume_mm3;
    r.good = r.check.checksums_ok && r.check.footer_ok && r.check.layers == r.written.layers && r.volume_matches &&
             r.check.grey_values == 0;
}

SaveResult saveMeshPrint(const fruit::TriangleMesh& mesh, const fruit::GooPrintSetup& setup, const std::string& path,
                         bool removable, const fruit::JobControl& ctl) {
    const auto t0 = std::chrono::steady_clock::now();
    SaveResult r;
    r.path = path;
    fruit::GooExportOptions o;
    o.sync_to_device = removable;
    r.written = fruit::exportGoo(mesh, setup.header, setup.grid, o, path, ctl.sub(0.0, 0.85));
    readBack(r, ctl.sub(0.85, 1.0));
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

}  // namespace gui
