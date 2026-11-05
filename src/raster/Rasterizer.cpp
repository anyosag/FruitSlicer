// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "raster/Rasterizer.h"

#include <algorithm>
#include <cmath>

namespace fruit {

RasterGrid RasterGrid::fromPrinter(const PrinterProfile& p) {
    RasterGrid g;
    g.width = p.res_x;
    g.height = p.res_y;
    g.px = p.pixelX();
    g.py = p.pixelY();
    g.x_left = -p.build_x / 2;
    g.y_top = p.build_y / 2;
    return g;
}

double RasterGrid::u(double x) const {
    const double c = (x - x_left) / px;
    return mirror_x ? width - c : c;
}

double RasterGrid::v(double y) const {
    const double r = (y_top - y) / py;
    return mirror_y ? height - r : r;
}

uint8_t quantize(double c, int levels) {
    c = std::clamp(c, 0.0, 1.0);
    if (levels <= 1) return c >= 0.5 ? 255 : 0;
    if (levels >= 256) return static_cast<uint8_t>(std::lround(c * 255.0));
    const double step = std::round(c * (levels - 1));
    return static_cast<uint8_t>(std::lround(step * 255.0 / (levels - 1)));
}

namespace {

struct Edge {
    double u0, v0, u1, v1;  // pixel space, v0 < v1
    int dir;                // +1 when the original edge went down (v increasing)
};

// Adds the signed area an edge sweeps into rows [r0, r1) of `acc`
// (row-major, stride `w`, columns relative to `c0`). Coverage of pixel
// (r, c) is |prefix sum of acc over the row up to c|.
void drawEdge(const Edge& e, int r0, int r1, int c0, int w, std::vector<float>& acc) {
    const double top = std::max(e.v0, static_cast<double>(r0));
    const double bot = std::min(e.v1, static_cast<double>(r1));
    if (bot <= top) return;
    const double dudv = (e.u1 - e.u0) / (e.v1 - e.v0);
    double u = e.u0 + (top - e.v0) * dudv;
    for (int r = static_cast<int>(std::floor(top)); r < r1 && r < static_cast<int>(std::ceil(bot)); ++r) {
        const double ya = std::max(top, static_cast<double>(r));
        const double yb = std::min(bot, static_cast<double>(r + 1));
        const double dy = yb - ya;
        if (dy <= 0) continue;
        const double unext = u + dudv * dy;
        const double d = dy * e.dir;
        float* row = acc.data() + static_cast<size_t>(r - r0) * w;
        double xa = std::min(u, unext) - c0, xb = std::max(u, unext) - c0;
        xa = std::clamp(xa, 0.0, w - 2.0);
        xb = std::clamp(xb, 0.0, w - 2.0);
        const int ia = static_cast<int>(std::floor(xa));
        const int ib = static_cast<int>(std::ceil(xb));
        if (ib <= ia + 1) {
            // Both ends in one pixel column: split at the mean position.
            const double xm = 0.5 * (xa + xb) - ia;
            row[ia] += static_cast<float>(d * (1.0 - xm));
            row[ia + 1] += static_cast<float>(d * xm);
        } else {
            // Edge crosses several columns: trapezoid areas per column.
            const double s = 1.0 / (xb - xa);
            const double fa = xa - ia;
            const double a0 = 0.5 * s * (1.0 - fa) * (1.0 - fa);
            const double fb = xb - ib + 1.0;
            const double am = 0.5 * s * fb * fb;
            row[ia] += static_cast<float>(d * a0);
            if (ib == ia + 2) {
                row[ia + 1] += static_cast<float>(d * (1.0 - a0 - am));
            } else {
                const double a1 = s * (1.5 - fa);
                row[ia + 1] += static_cast<float>(d * (a1 - a0));
                for (int i = ia + 2; i < ib - 1; ++i) row[i] += static_cast<float>(d * s);
                const double a2 = a1 + (ib - ia - 3) * s;
                row[ib - 1] += static_cast<float>(d * (1.0 - a2 - am));
            }
            row[ib] += static_cast<float>(d * am);
        }
        u = unext;
    }
}

}  // namespace

void rasterizeLayer(const Paths& layer, const RasterGrid& g, int aa_levels, const RunSink& sink) {
    const uint64_t total = static_cast<uint64_t>(g.width) * g.height;
    auto emitZeros = [&](uint64_t n) {
        while (n > 0) {
            const auto k = static_cast<uint32_t>(std::min<uint64_t>(n, 0xFFFFFFF));
            sink(0, k);
            n -= k;
        }
    };
    if (layer.empty()) {
        emitZeros(total);
        return;
    }
    // Clip to the plate so every coordinate is inside the grid.
    const double xl = g.x_left, xr = g.x_left + g.width * g.px;
    const double yt = g.y_top, yb = g.y_top - g.height * g.py;
    const Path plate{toPoint({xl, yb}), toPoint({xr, yb}), toPoint({xr, yt}), toPoint({xl, yt})};
    const Paths clipped = intersection(layer, {plate});
    std::vector<Edge> edges;
    double umin = 1e300, umax = -1e300, vmin = 1e300, vmax = -1e300;
    for (const Path& p : clipped)
        for (size_t i = 0; i < p.size(); ++i) {
            const Vec2 a = toVec(p[i]), b = toVec(p[(i + 1) % p.size()]);
            const double ua = g.u(a.x), va = g.v(a.y), ub = g.u(b.x), vb = g.v(b.y);
            umin = std::min({umin, ua, ub});
            umax = std::max({umax, ua, ub});
            vmin = std::min({vmin, va, vb});
            vmax = std::max({vmax, va, vb});
            if (va == vb) continue;
            if (va < vb) edges.push_back({ua, va, ub, vb, 1});
            else edges.push_back({ub, vb, ua, va, -1});
        }
    if (edges.empty()) {
        emitZeros(total);
        return;
    }
    const int r_first = std::clamp(static_cast<int>(std::floor(vmin)), 0, g.height);
    const int r_last = std::clamp(static_cast<int>(std::ceil(vmax)), 0, g.height);  // exclusive
    const int c0 = std::clamp(static_cast<int>(std::floor(umin)), 0, g.width);
    const int c1 = std::clamp(static_cast<int>(std::ceil(umax)), 0, g.width);    // exclusive
    const int w = c1 - c0 + 2;  // accumulation row with slack for the right-hand spill
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.v0 < b.v0; });

    emitZeros(static_cast<uint64_t>(r_first) * g.width);
    constexpr int kBand = 64;
    std::vector<float> acc;
    std::vector<uint8_t> row(static_cast<size_t>(c1 - c0));
    size_t first_edge = 0;
    for (int r0 = r_first; r0 < r_last; r0 += kBand) {
        const int r1 = std::min(r_last, r0 + kBand);
        acc.assign(static_cast<size_t>(r1 - r0) * w, 0.0f);
        while (first_edge < edges.size() && edges[first_edge].v1 <= r0) ++first_edge;
        for (size_t i = first_edge; i < edges.size() && edges[i].v0 < r1; ++i)
            if (edges[i].v1 > r0) drawEdge(edges[i], r0, r1, c0, w, acc);
        for (int r = r0; r < r1; ++r) {
            const float* a = acc.data() + static_cast<size_t>(r - r0) * w;
            double sum = 0;
            for (int c = 0; c < c1 - c0; ++c) {
                sum += a[c];
                row[c] = quantize(std::abs(sum), aa_levels);
            }
            // Row = zeros left, the bounding-box span, zeros right.
            emitZeros(static_cast<uint64_t>(c0));
            int i = 0;
            while (i < c1 - c0) {
                int j = i + 1;
                while (j < c1 - c0 && row[j] == row[i]) ++j;
                sink(row[i], static_cast<uint32_t>(j - i));
                i = j;
            }
            emitZeros(static_cast<uint64_t>(g.width - c1));
        }
    }
    emitZeros(static_cast<uint64_t>(g.height - r_last) * g.width);
}

std::vector<uint8_t> rasterizeDense(const Paths& layer, const RasterGrid& g, int aa_levels) {
    std::vector<uint8_t> img;
    img.reserve(static_cast<size_t>(g.width) * g.height);
    rasterizeLayer(layer, g, aa_levels, [&](uint8_t v, uint32_t n) { img.insert(img.end(), n, v); });
    return img;
}

}  // namespace fruit
