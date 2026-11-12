// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "formats/GooCompare.h"

#include <cmath>
#include <sstream>
#include <stdexcept>

namespace fruit {

namespace {

std::string str(double v) {
    std::ostringstream s;
    s.precision(6);
    s << v;
    return s.str();
}

uint32_t layerNearest(const GooFileInfo& f, double z) {
    uint32_t best = 0;
    double bd = 1e300;
    for (uint32_t i = 0; i < f.layers.size(); ++i) {
        const double d = std::abs(f.layers[i].z - z);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

// Lit cells of a layer image on a grid of `cell` mm, as (col, row) pairs.
struct Cells {
    int w = 0, h = 0;
    std::vector<char> on;
    size_t count = 0;
    double cx = 0, cy = 0;  // centroid in cell units
};

Cells cellsOf(const GooFileInfo& f, uint32_t layer, double cell) {
    const GooHeader& hd = f.header;
    std::vector<uint8_t> px(static_cast<size_t>(hd.res_x) * hd.res_y);
    decodeGooRle(f.rle.at(layer), px);
    const double pxw = hd.size_x / hd.res_x, pxh = hd.size_y / hd.res_y;
    Cells c;
    c.w = static_cast<int>(std::ceil(hd.size_x / cell));
    c.h = static_cast<int>(std::ceil(hd.size_y / cell));
    c.on.assign(static_cast<size_t>(c.w) * c.h, 0);
    for (int r = 0; r < hd.res_y; ++r) {
        const int cr = std::min(c.h - 1, static_cast<int>((r + 0.5) * pxh / cell));
        const uint8_t* row = px.data() + static_cast<size_t>(r) * hd.res_x;
        for (int col = 0; col < hd.res_x; ++col)
            if (row[col] >= 128) {
                const int cc = std::min(c.w - 1, static_cast<int>((col + 0.5) * pxw / cell));
                char& o = c.on[static_cast<size_t>(cr) * c.w + cc];
                if (!o) {
                    o = 1;
                    ++c.count;
                    c.cx += cc;
                    c.cy += cr;
                }
            }
    }
    if (c.count) {
        c.cx /= static_cast<double>(c.count);
        c.cy /= static_cast<double>(c.count);
    }
    return c;
}

double iou(const Cells& a, const Cells& b, bool fx, bool fy) {
    if (!a.count || !b.count) return 0;
    // Centroid of A after the flip, then the shift that lands it on B's.
    const double acx = fx ? (a.w - 1 - a.cx) : a.cx;
    const double acy = fy ? (a.h - 1 - a.cy) : a.cy;
    const int dx = static_cast<int>(std::lround(b.cx - acx));
    const int dy = static_cast<int>(std::lround(b.cy - acy));
    size_t inter = 0;
    for (int r = 0; r < a.h; ++r)
        for (int c = 0; c < a.w; ++c) {
            if (!a.on[static_cast<size_t>(r) * a.w + c]) continue;
            const int tc = (fx ? a.w - 1 - c : c) + dx;
            const int tr = (fy ? a.h - 1 - r : r) + dy;
            if (tc < 0 || tr < 0 || tc >= b.w || tr >= b.h) continue;
            inter += b.on[static_cast<size_t>(tr) * b.w + tc] != 0;
        }
    const size_t uni = a.count + b.count - inter;
    return uni ? static_cast<double>(inter) / static_cast<double>(uni) : 0;
}

}  // namespace

std::vector<GooFieldDiff> diffHeaders(const GooHeader& a, const GooHeader& b) {
    std::vector<GooFieldDiff> d;
    auto cmp = [&](const char* name, const std::string& x, const std::string& y) {
        if (x != y) d.push_back({name, x, y});
    };
    auto num = [&](const char* name, double x, double y) {
        if (std::abs(x - y) > 1e-6 * std::max(1.0, std::abs(x))) d.push_back({name, str(x), str(y)});
    };
    cmp("version", a.version, b.version);
    cmp("software", a.software_name + " " + a.software_version, b.software_name + " " + b.software_version);
    cmp("printer_name", a.printer_name, b.printer_name);
    cmp("printer_type", a.printer_type, b.printer_type);
    cmp("profile_name", a.profile_name, b.profile_name);
    num("aa_level", a.aa_level, b.aa_level);
    num("grey_level", a.grey_level, b.grey_level);
    num("blur_level", a.blur_level, b.blur_level);
    num("grayscale_8bit", a.grayscale_8bit, b.grayscale_8bit);
    num("res_x", a.res_x, b.res_x);
    num("res_y", a.res_y, b.res_y);
    num("mirror_x", a.mirror_x, b.mirror_x);
    num("mirror_y", a.mirror_y, b.mirror_y);
    num("size_x", a.size_x, b.size_x);
    num("size_y", a.size_y, b.size_y);
    num("size_z", a.size_z, b.size_z);
    num("layer_height", a.layer_height, b.layer_height);
    num("layer_count", a.layer_count, b.layer_count);
    const GooPrintSettings &p = a.print, &q = b.print;
    num("exposure", p.exposure, q.exposure);
    num("bottom_exposure", p.bottom_exposure, q.bottom_exposure);
    num("bottom_layers", p.bottom_layers, q.bottom_layers);
    num("transition_layers", p.transition_layers, q.transition_layers);
    num("delay_mode_static", p.delay_mode_static, q.delay_mode_static);
    num("turn_off_time", p.turn_off_time, q.turn_off_time);
    num("bottom_wait_before_lift", p.bottom_wait_before_lift, q.bottom_wait_before_lift);
    num("bottom_wait_after_lift", p.bottom_wait_after_lift, q.bottom_wait_after_lift);
    num("bottom_wait_after_retract", p.bottom_wait_after_retract, q.bottom_wait_after_retract);
    num("wait_before_lift", p.wait_before_lift, q.wait_before_lift);
    num("wait_after_lift", p.wait_after_lift, q.wait_after_lift);
    num("wait_after_retract", p.wait_after_retract, q.wait_after_retract);
    num("bottom_lift_distance", p.bottom_lift_distance, q.bottom_lift_distance);
    num("bottom_lift_speed", p.bottom_lift_speed, q.bottom_lift_speed);
    num("lift_distance", p.lift_distance, q.lift_distance);
    num("lift_speed", p.lift_speed, q.lift_speed);
    num("bottom_retract_distance", p.bottom_retract_distance, q.bottom_retract_distance);
    num("bottom_retract_speed", p.bottom_retract_speed, q.bottom_retract_speed);
    num("retract_distance", p.retract_distance, q.retract_distance);
    num("retract_speed", p.retract_speed, q.retract_speed);
    num("bottom_lift2", p.bottom_lift2_distance + p.bottom_lift2_speed, q.bottom_lift2_distance + q.bottom_lift2_speed);
    num("lift2", p.lift2_distance + p.lift2_speed, q.lift2_distance + q.lift2_speed);
    num("bottom_retract2", p.bottom_retract2_distance + p.bottom_retract2_speed,
        q.bottom_retract2_distance + q.bottom_retract2_speed);
    num("retract2", p.retract2_distance + p.retract2_speed, q.retract2_distance + q.retract2_speed);
    num("bottom_pwm", p.bottom_pwm, q.bottom_pwm);
    num("pwm", p.pwm, q.pwm);
    num("advance_mode", p.advance_mode, q.advance_mode);
    return d;
}

GooCompareResult compareGoo(const GooFileInfo& a, const GooFileInfo& b, double z, double cell) {
    if (a.layers.empty() || b.layers.empty()) throw std::runtime_error("goo-compare: a file has no layers");
    GooCompareResult r;
    r.header = diffHeaders(a.header, b.header);
    if (z < 0) z = 0.5 * std::min(a.layers.back().z, b.layers.back().z);
    r.layer_a = layerNearest(a, z);
    r.layer_b = layerNearest(b, z);
    r.z_a = a.layers[r.layer_a].z;
    r.z_b = b.layers[r.layer_b].z;
    const Cells ca = cellsOf(a, r.layer_a, cell);
    const Cells cb = cellsOf(b, r.layer_b, cell);
    r.cells_a = ca.count;
    r.cells_b = cb.count;
    const struct { const char* name; bool fx, fy; } t[] = {
        {"identity", false, false}, {"flip_x", true, false}, {"flip_y", false, true}, {"flip_xy", true, true}};
    for (const auto& k : t) {
        const double v = iou(ca, cb, k.fx, k.fy);
        r.scores.push_back({k.name, v});
        if (v > r.best_iou) {
            r.best_iou = v;
            r.best = k.name;
        }
    }
    return r;
}

OrientationVerdict orientationVerdict(const GooCompareResult& c, double min_iou) {
    if (c.best_iou < min_iou) return OrientationVerdict::Undetermined;
    if (c.best == "identity") return OrientationVerdict::Matches;
    if (c.best == "flip_x") return OrientationVerdict::FlipX;
    if (c.best == "flip_y") return OrientationVerdict::FlipY;
    return OrientationVerdict::Rotated180;
}

const char* verdictText(OrientationVerdict v) {
    switch (v) {
        case OrientationVerdict::Matches: return "orientation matches";
        case OrientationVerdict::FlipX: return "A is mirrored in X relative to B: use --flip-x";
        case OrientationVerdict::FlipY: return "A is mirrored in Y relative to B: use --flip-y";
        case OrientationVerdict::Rotated180: return "A is rotated 180 degrees relative to B: use --flip-x --flip-y";
        case OrientationVerdict::Undetermined: break;
    }
    return "images differ (different models, scale or layer?); orientation not determined";
}

}  // namespace fruit
