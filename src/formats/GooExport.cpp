// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "formats/GooExport.h"

#include <atomic>
#include <cmath>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <thread>

#include "io/Render.h"
#include "layers/LayerStack.h"

namespace fruit {

GooExportResult exportGoo(const TriangleMesh& mesh, GooHeader h, const RasterGrid& grid, const GooExportOptions& o,
                          const std::string& path) {
    const auto t0 = std::chrono::steady_clock::now();
    GooExportResult res;
    // Header heights are float32; slice at the intended value (to the µm).
    const double layer_h = std::round(static_cast<double>(h.layer_height) * 1e6) / 1e6;
    const LayerStack layers = sliceModel(mesh, layer_h);
    h.layer_count = static_cast<uint32_t>(layers.size());
    h.res_x = static_cast<uint16_t>(grid.width);
    h.res_y = static_cast<uint16_t>(grid.height);
    h.mirror_x = grid.mirror_x;
    h.mirror_y = grid.mirror_y;
    if (o.aa_levels <= 1) {  // off: pure black/white, flags as SatelLite writes them
        h.aa_level = 0;
        h.grey_level = 0;
        h.grayscale_8bit = false;
    } else {
        h.aa_level = static_cast<uint16_t>(o.aa_levels);
        h.grey_level = 1;
        h.grayscale_8bit = true;
    }
    {
        const std::time_t now = std::time(nullptr);
        char buf[32];
        std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", std::gmtime(&now));
        h.file_time = buf;
    }
    // Thumbnails seen from the front, slightly above.
    RenderView view;
    view.azimuth_deg = 20;
    view.elevation_deg = 30;
    view.size = kGooSmallPreview;
    h.small_preview = toRgb565BE(renderPreviewRgb(mesh, {}, view));
    view.size = kGooBigPreview;
    h.big_preview = toRgb565BE(renderPreviewRgb(mesh, {}, view));

    GooWriter writer(path, h);
    unsigned threads = o.threads ? o.threads : std::max(1u, std::thread::hardware_concurrency());
    const size_t chunk = std::max<size_t>(threads * 4, 8);
    double coverage = 0;  // Σ pixel coverage
    for (size_t base = 0; base < layers.size(); base += chunk) {
        const size_t end = std::min(layers.size(), base + chunk);
        std::vector<std::vector<uint8_t>> rle(end - base);
        std::vector<double> cov(end - base, 0);
        std::atomic<size_t> next{base};
        auto work = [&]() {
            for (size_t i = next++; i < end; i = next++) {
                GooRleEncoder enc;
                double c = 0;
                rasterizeLayer(layers.slices[i], grid, o.aa_levels, [&](uint8_t v, uint32_t n) {
                    enc.add(v, n);
                    c += v / 255.0 * n;
                });
                rle[i - base] = enc.finish();
                cov[i - base] = c;
            }
        };
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work);
        work();
        for (auto& th : pool) th.join();
        for (size_t i = base; i < end; ++i) {
            writer.addLayer(layerDefFor(h, static_cast<uint32_t>(i)), rle[i - base]);
            coverage += cov[i - base];
        }
    }
    res.layers = h.layer_count;
    res.volume_mm3 = coverage * grid.px * grid.py * layer_h;
    double t = 0;
    for (uint32_t i = 0; i < h.layer_count; ++i) {
        const GooLayerDef d = layerDefFor(h, i);
        t += d.exposure + d.wait_before_lift + d.wait_after_lift + d.wait_after_retract + o.motion_seconds_per_layer;
    }
    res.print_time_s = static_cast<uint32_t>(t);
    h.print_time = res.print_time_s;
    h.volume = static_cast<float>(res.volume_mm3);
    h.weight = static_cast<float>(res.volume_mm3 / 1000.0 * o.resin_density);
    writer.finish(h);
    res.bytes = std::filesystem::file_size(path);
    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return res;
}

}  // namespace fruit
