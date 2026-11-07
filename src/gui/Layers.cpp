// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Layers.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "Document.h"
#include "Orient.h"
#include "geometry/Polygon.h"
#include "raster/Rasterizer.h"

namespace gui {

std::size_t LayerResult::cups() const {
    return static_cast<std::size_t>(std::count_if(hollows.components.begin(), hollows.components.end(),
                                                  [](const auto& c) { return c.kind == fruit::HollowKind::SuctionCup; }));
}

std::size_t LayerResult::traps() const {
    return static_cast<std::size_t>(std::count_if(hollows.components.begin(), hollows.components.end(),
                                                  [](const auto& c) { return c.kind == fruit::HollowKind::ResinTrap; }));
}

LayerResult sliceLayers(const Document& doc, const fruit::TriangleMesh& supports, double raft_top,
                        const fruit::JobControl& ctl) {
    const auto t0 = std::chrono::steady_clock::now();
    LayerResult r;
    fruit::TriangleMesh all = doc.placed();
    all.append(supports);
    r.bounds = all.bbox();
    auto ls = std::make_shared<fruit::LayerStack>(fruit::sliceModel(all, kLayerHeight, nullptr, 0, ctl.sub(0.0, 0.8)));
    r.area.reserve(ls->size());
    for (const auto& s : ls->slices) r.area.push_back(std::max(0.0, fruit::area(s)));
    for (double a : r.area) r.resin_ml += a * ls->layer_height / 1000.0;
    r.top = ls->layerTop(ls->size() ? ls->size() - 1 : 0);
    while (r.raft_layers < ls->size() && ls->layerTop(r.raft_layers) <= raft_top + 1e-9) ++r.raft_layers;
    r.biggest = r.raft_layers;
    for (std::size_t i = r.raft_layers; i < r.area.size(); ++i)
        if (r.area[i] > r.area[r.biggest]) r.biggest = i;
    ctl.step("hollows", 0.8);
    r.hollows = fruit::analyzeHollows(*ls);
    for (const auto& c : r.hollows.components) r.marks.push_back(static_cast<std::size_t>(std::max(0, c.first_layer)));
    if (!r.area.empty()) r.marks.push_back(r.biggest);
    std::sort(r.marks.begin(), r.marks.end());
    r.marks.erase(std::unique(r.marks.begin(), r.marks.end()), r.marks.end());
    r.layers = std::move(ls);
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ctl.report("hollows", 1.0);
    return r;
}

LayerImage drawLayer(const fruit::LayerStack& ls, std::size_t i, const fruit::BBox3& crop, int max_w, int max_h,
                     const fruit::PrinterProfile& printer, bool with_below) {
    LayerImage im;
    if (i >= ls.size() || crop.empty() || max_w <= 0 || max_h <= 0) return im;
    const double cw = (crop.max.x - crop.min.x) / printer.pixelX(), ch = (crop.max.y - crop.min.y) / printer.pixelY();
    // Whole printer pixels per image pixel, the same along both axes.
    im.printer_pixels = std::max(1, static_cast<int>(std::ceil(std::max(cw / max_w, ch / max_h))));
    im.px = printer.pixelX() * im.printer_pixels;
    im.py = printer.pixelY() * im.printer_pixels;
    im.w = std::max(1, static_cast<int>(std::ceil(cw / im.printer_pixels)));
    im.h = std::max(1, static_cast<int>(std::ceil(ch / im.printer_pixels)));
    im.x_left = crop.min.x;
    im.y_top = crop.max.y;
    fruit::RasterGrid g;
    g.width = im.w;
    g.height = im.h;
    g.px = im.px;
    g.py = im.py;
    g.x_left = im.x_left;
    g.y_top = im.y_top;
    im.lit = fruit::rasterizeDense(ls.slices[i], g, 16);
    if (with_below && i > 0) im.below = fruit::rasterizeDense(ls.slices[i - 1], g, 16);
    return im;
}

}  // namespace gui
