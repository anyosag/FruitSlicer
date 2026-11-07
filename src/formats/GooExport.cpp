// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "formats/GooExport.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <stdexcept>

#include "core/Parallel.h"
#include "io/Render.h"

namespace fruit {

namespace {

std::string utcNow() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

}  // namespace

double gooLayerHeight(const GooHeader& h) {
    // Header heights are float32; slice at the intended value (to the µm).
    return std::round(static_cast<double>(h.layer_height) * 1e6) / 1e6;
}

GooExportResult exportGoo(const TriangleMesh& mesh, GooHeader h, const RasterGrid& grid, const GooExportOptions& o,
                          const std::string& path, const JobControl& ctl) {
    const auto t0 = std::chrono::steady_clock::now();
    const LayerStack layers = sliceModel(mesh, gooLayerHeight(h), nullptr, o.threads, ctl.sub(0, 0.3));
    GooExportResult res = exportGooLayers(layers, &mesh, std::move(h), grid, o, path, ctl.sub(0.3, 1));
    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return res;
}

GooExportResult exportGooLayers(const LayerStack& layers, const TriangleMesh* preview, GooHeader h,
                                const RasterGrid& grid, const GooExportOptions& o, const std::string& path,
                                const JobControl& ctl) {
    const auto t0 = std::chrono::steady_clock::now();
    const double layer_h = gooLayerHeight(h);
    if (std::abs(layers.layer_height - layer_h) > 1e-6)
        throw std::invalid_argument("layer height of the slices does not match the GOO header");
    GooExportResult res;
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
    h.file_time = utcNow();
    // Thumbnails seen from the front, slightly above.
    RenderView view;
    view.azimuth_deg = 20;
    view.elevation_deg = 30;
    const TriangleMesh empty;
    const TriangleMesh& pm = preview ? *preview : empty;
    view.size = kGooSmallPreview;
    h.small_preview = toRgb565BE(renderPreviewRgb(pm, {}, view));
    view.size = kGooBigPreview;
    h.big_preview = toRgb565BE(renderPreviewRgb(pm, {}, view));

    GooWriter writer(path, h);
    const unsigned threads = resolveThreads(o.threads);
    const size_t chunk = std::max<size_t>(threads * 4, 8);
    const double total = static_cast<double>(std::max<size_t>(layers.size(), 1));
    double coverage = 0;  // Σ pixel coverage
    for (size_t base = 0; base < layers.size(); base += chunk) {
        ctl.check();
        const size_t end = std::min(layers.size(), base + chunk);
        std::vector<std::vector<uint8_t>> rle(end - base);
        std::vector<double> cov(end - base, 0);
        parallelFor(end - base, threads, [&](size_t k) {
            ctl.check();
            const size_t i = base + k;
            GooRleEncoder enc;
            double c = 0;
            rasterizeLayer(layers.slices[i], grid, o.aa_levels, [&](uint8_t v, uint32_t n) {
                enc.add(v, n);
                c += v / 255.0 * n;
            });
            rle[k] = enc.finish();
            cov[k] = c;
        });
        for (size_t i = base; i < end; ++i) {
            writer.addLayer(layerDefFor(h, static_cast<uint32_t>(i)), rle[i - base]);
            coverage += cov[i - base];
        }
        ctl.report("rasterize", static_cast<double>(end) / total);
    }
    ctl.check();
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
    writer.finish(h, o.sync_to_device);
    res.bytes = std::filesystem::file_size(path);
    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return res;
}

}  // namespace fruit
