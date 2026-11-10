// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "ops/Arrange.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace fruit::ops {

namespace {

struct Rect {
    double x0, y0, x1, y1;  // min/max corners
    double w() const { return x1 - x0; }
    double h() const { return y1 - y0; }
};

bool inside(const Rect& a, const Rect& b) {  // a inside b
    return a.x0 >= b.x0 && a.y0 >= b.y0 && a.x1 <= b.x1 && a.y1 <= b.y1;
}

}  // namespace

Footprint footprintOf(const TriangleMesh& mesh) {
    const BBox3 b = mesh.bbox();
    if (b.empty()) return {};
    return {b.max.x - b.min.x, b.max.y - b.min.y};
}

ArrangeResult arrange(const std::vector<Footprint>& fps, const ArrangeOptions& o) {
    ArrangeResult res;
    res.parts.assign(fps.size(), {});
    const double eps = 1e-9;
    // Each footprint grows by the gap on two sides, and the plate by one gap:
    // then touching rectangles are a gap apart and the edge margin is kept.
    const double gap = std::max(0.0, o.gap), margin = std::max(0.0, o.edge_margin);
    const double pw = o.plate_width - 2 * margin + gap, pd = o.plate_depth - 2 * margin + gap;
    std::vector<Rect> free_rects;
    if (pw > 0 && pd > 0) free_rects.push_back({0, 0, pw, pd});

    std::vector<size_t> order(fps.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const double aa = fps[a].width * fps[a].depth, bb = fps[b].width * fps[b].depth;
        if (aa != bb) return aa > bb;
        return std::max(fps[a].width, fps[a].depth) > std::max(fps[b].width, fps[b].depth);
    });

    for (size_t idx : order) {
        const Footprint& f = fps[idx];
        const bool usable = f.width > 0 && f.depth > 0 && std::isfinite(f.width) && std::isfinite(f.depth);
        bool done = false;
        if (usable) {
            // Best short-side fit; ties go to the lowest y, then x, then no rotation.
            double best_short = 1e300, best_long = 1e300, best_y = 0, best_x = 0;
            Rect best{};
            bool best_rot = false, found = false;
            for (const Rect& fr : free_rects) {
                for (int rot = 0; rot < (o.allow_rotate ? 2 : 1); ++rot) {
                    const double w = (rot ? f.depth : f.width) + gap, d = (rot ? f.width : f.depth) + gap;
                    if (w > fr.w() + eps || d > fr.h() + eps) continue;
                    const double dw = fr.w() - w, dd = fr.h() - d;
                    const double short_side = std::min(dw, dd), long_side = std::max(dw, dd);
                    const bool better = !found || short_side < best_short - eps ||
                        (std::abs(short_side - best_short) <= eps &&
                         (long_side < best_long - eps ||
                          (std::abs(long_side - best_long) <= eps &&
                           (fr.y0 < best_y - eps || (std::abs(fr.y0 - best_y) <= eps && fr.x0 < best_x - eps)))));
                    if (better) {
                        found = true;
                        best_short = short_side;
                        best_long = long_side;
                        best_y = fr.y0;
                        best_x = fr.x0;
                        best = {fr.x0, fr.y0, fr.x0 + w, fr.y0 + d};
                        best_rot = rot == 1;
                    }
                }
            }
            if (found) {
                done = true;
                ArrangedPart& p = res.parts[idx];
                p.placed = true;
                p.rotated = best_rot;
                const double w = best.w() - gap, d = best.h() - gap;
                p.centre = {margin + best.x0 + w / 2 - o.plate_width / 2, margin + best.y0 + d / 2 - o.plate_depth / 2};
                // Cut the used rectangle out of every free rectangle it touches.
                std::vector<Rect> next;
                for (const Rect& fr : free_rects) {
                    if (best.x0 >= fr.x1 - eps || best.x1 <= fr.x0 + eps || best.y0 >= fr.y1 - eps ||
                        best.y1 <= fr.y0 + eps) {
                        next.push_back(fr);
                        continue;
                    }
                    if (best.x0 > fr.x0 + eps) next.push_back({fr.x0, fr.y0, best.x0, fr.y1});
                    if (best.x1 < fr.x1 - eps) next.push_back({best.x1, fr.y0, fr.x1, fr.y1});
                    if (best.y0 > fr.y0 + eps) next.push_back({fr.x0, fr.y0, fr.x1, best.y0});
                    if (best.y1 < fr.y1 - eps) next.push_back({fr.x0, best.y1, fr.x1, fr.y1});
                }
                free_rects.clear();
                for (size_t i = 0; i < next.size(); ++i) {
                    bool contained = false;
                    for (size_t j = 0; j < next.size() && !contained; ++j)
                        if (i != j && inside(next[i], next[j]) && (!inside(next[j], next[i]) || j < i)) contained = true;
                    if (!contained) free_rects.push_back(next[i]);
                }
            }
        }
        if (!done) res.not_placed.push_back(static_cast<int>(idx));
    }
    std::sort(res.not_placed.begin(), res.not_placed.end());
    return res;
}

}  // namespace fruit::ops
