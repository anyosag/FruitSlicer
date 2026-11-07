// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Orient.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "Document.h"
#include "mesh/ModelCheck.h"
#include "support/CosmeticField.h"
#include "support/OrientationSearch.h"
#include "support/PointGenerator.h"
#include "support/SupportSettings.h"

namespace gui {

fruit::Vec3 sideDirection(Side s) {
    switch (s) {
        case Side::Front: return {0, -1, 0};
        case Side::Back: return {0, 1, 0};
        case Side::Left: return {-1, 0, 0};
        case Side::Right: return {1, 0, 0};
        case Side::Top: return {0, 0, 1};
        case Side::Bottom: return {0, 0, -1};
    }
    return {0, 0, 1};
}

const char* sideName(Side s) {
    static const char* names[] = {"Front", "Back", "Left", "Right", "Top", "Bottom"};
    return names[static_cast<int>(s)];
}

// The planner's settings for scoring: built-in values, then the job's
// resin's measured ones (the same the Supports step uses).
fruit::SupportConfig supportConfig(const fruit::Placement& p, const nlohmann::json& profile) {
    fruit::SupportConfig cfg;
    fruit::SupportChoices ch;
    ch.layer_height = kLayerHeight;
    ch.on_plate = p.on_plate;
    fruit::resolveSettings(profile.is_object() ? &profile : nullptr, ch, cfg, nullptr);
    return cfg;
}

std::vector<fruit::Vec3> visibleInPrinter(const std::vector<fruit::Vec3>& model_dirs, const fruit::Placement& p) {
    const fruit::Transform r = fruit::Transform::rotationXYZ(p.rotation_deg.x, p.rotation_deg.y, p.rotation_deg.z);
    std::vector<fruit::Vec3> out;
    for (const fruit::Vec3& v : model_dirs) out.push_back(fruit::normalized(r.applyLinear(v)));
    return out;
}

namespace {

std::size_t layersFor(double top) { return static_cast<std::size_t>(std::ceil(top / kLayerHeight - 1e-9)); }

// The rotation, turned about the vertical in 15° steps until it fits the
// plate (supports do not depend on that turn). Unchanged if nothing fits.
bool fitYaw(const Document& doc, fruit::Vec3& rotation_deg) {
    fruit::Placement q = doc.placement();
    for (int k = 0; k < 12; ++k) {
        q.rotation_deg = rotation_deg;
        q.rotation_deg.z = std::fmod(rotation_deg.z + 15.0 * k, 360.0);
        if (fruit::checkFit(doc.model.mesh, q, doc.printer).fits) {
            rotation_deg = q.rotation_deg;
            return true;
        }
    }
    return false;
}

std::string fmt(const char* f, double a, double b = 0) {
    char s[160];
    std::snprintf(s, sizeof s, f, a, b);
    return s;
}

}  // namespace

OrientMetrics measure(const Document& doc, const std::vector<fruit::Vec3>& visible_model,
                      const fruit::JobControl& ctl) {
    const fruit::Placement& p = doc.placement();
    const fruit::SupportConfig cfg = supportConfig(p, doc.supports.choices.profile);
    // The same planner run as the search's finalists, so the numbers compare.
    fruit::CosmeticSpec cos;
    cos.visible_from = visibleInPrinter(visible_model, p);
    fruit::PointGenOptions po;
    po.cosmetic = &cos;
    const fruit::PointGenResult res = fruit::generateSupportPoints(doc.placed(), cfg, po, ctl);
    fruit::CostField cf;
    cf.cosmetic = &cos;
    OrientMetrics m;
    m.tips = res.points.size();
    for (const auto& pt : res.points) {
        m.island_tips += pt.category == fruit::PointCategory::Island;
        m.cosmetic_tips += cf.isCosmetic(pt.pos, pt.normal);
    }
    m.short_of_hold = res.stats.capacity_deficits;
    m.biggest_layer = res.stats.peak_layer_area;
    m.top = doc.placed().bbox().max.z;
    m.layers = layersFor(m.top);
    return m;
}

OrientSearch search(const Document& doc, const std::vector<fruit::Vec3>& visible_model, const fruit::JobControl& ctl) {
    const auto t0 = std::chrono::steady_clock::now();
    OrientSearch s;
    const fruit::Placement& p = doc.placement();
    s.now.rotation_deg = p.rotation_deg;
    s.now.m = measure(doc, visible_model, ctl.sub(0.0, 0.12));

    // Search the model at its chosen scale, unrotated: the results are then
    // the placement's rotation angles.
    fruit::TriangleMesh m = doc.model.mesh;
    fruit::Transform scale;
    for (int i = 0; i < 3; ++i) scale.m[i][i] = p.scale;
    m.transform(scale);
    fruit::OrientOptions o;
    o.cfg = supportConfig(p, doc.supports.choices.profile);
    o.elevation = p.lift();
    o.visible_from = visible_model;
    o.keep_points = false;
    const auto results = fruit::searchOrientations(m, o, ctl.sub(0.12, 0.95));
    s.directions = o.directions + 1;
    const fruit::OrientationResult* input = nullptr;
    for (const auto& r : results) {
        s.finalists += r.evaluated && !r.is_input;  // how it sits now is planned too, but is not a finalist
        if (r.is_input) input = &r;
    }
    for (const auto& r : results) {
        if (!r.evaluated || r.is_input) continue;
        if (input && input->evaluated && r.score >= input->score) continue;  // not better than now
        if (s.options.size() == 3) break;
        OrientOption op;
        op.rotation_deg = r.euler_deg;
        op.fits = fitYaw(doc, op.rotation_deg);
        op.m.tips = r.points;
        op.m.island_tips = r.islands;
        op.m.cosmetic_tips = r.cosmetic_points;
        op.m.short_of_hold = r.capacity_deficits;
        op.m.biggest_layer = r.peak_layer_area;
        op.m.top = r.height + p.lift();
        op.m.layers = layersFor(op.m.top);
        op.summary = summarize(op.m, s.now.m);
        s.options.push_back(std::move(op));
    }
    ctl.check();

    // How it sits now, against the options.
    {
        std::vector<std::string> parts;
        bool shortest = !s.options.empty();
        for (const auto& op : s.options) shortest = shortest && s.now.m.top < op.m.top - 0.05;
        if (shortest) parts.push_back("Quickest to print, as it stands lowest");
        if (s.now.m.short_of_hold > 0)
            parts.push_back(fmt("%.0f layers are bigger than their tips can hold", static_cast<double>(s.now.m.short_of_hold)));
        if (s.options.empty()) s.now.summary = "No other way to stand it does better.";
        else if (parts.empty()) s.now.summary = "Keeps the rotation it has now.";
        else if (parts.size() == 1) s.now.summary = parts[0] + ".";
        else s.now.summary = parts[0] + ", but " + parts[1] + ".";
        if (!s.now.summary.empty()) s.now.summary[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s.now.summary[0])));
    }

    // Pictures from below, all at one scale so sizes compare.
    std::vector<fruit::TriangleMesh> placed;
    double extent = 1;
    auto place = [&](const fruit::Vec3& rot) {
        fruit::Placement q = p;
        q.rotation_deg = rot;
        placed.push_back(fruit::placeModel(doc.model.mesh, q));
        const fruit::Vec3 sz = placed.back().bbox().size();
        extent = std::max({extent, sz.x, sz.y});
    };
    for (const auto& op : s.options) place(op.rotation_deg);
    place(s.now.rotation_deg);
    const int w = 568, h = 352;  // 284 × 176 points at 2x
    const double mmpp = extent / (std::min(w, h) * 0.82);
    for (std::size_t i = 0; i < s.options.size(); ++i) s.options[i].below = silhouetteBelow(placed[i], mmpp, w, h);
    s.now.below = silhouetteBelow(placed.back(), mmpp, w, h);
    s.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ctl.report("orient.evaluate", 1);
    return s;
}

Silhouette silhouetteBelow(const fruit::TriangleMesh& placed, double mm_per_px, int w, int h) {
    Silhouette s;
    s.w = w;
    s.h = h;
    s.alpha.assign(static_cast<std::size_t>(w) * h, 0);
    const fruit::BBox3 b = placed.bbox();
    if (b.empty() || mm_per_px <= 0) return s;
    const int W = w * 2, H = h * 2;
    std::vector<std::uint8_t> hi(static_cast<std::size_t>(W) * H, 0);
    const double k = 2.0 / mm_per_px, cx = (b.min.x + b.max.x) / 2, cy = (b.min.y + b.max.y) / 2;
    struct P {
        double x, y;
    };
    // From underneath, +X runs to the left; +Y (the back) is up.
    auto px = [&](const fruit::Vec3& v) { return P{W / 2.0 - (v.x - cx) * k, H / 2.0 - (v.y - cy) * k}; };
    auto edge = [](P a, P b, double x, double y) { return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x); };
    for (const fruit::Face& f : placed.faces) {
        P a = px(placed.vertices[f[0]]), q = px(placed.vertices[f[1]]), c = px(placed.vertices[f[2]]);
        const double area = edge(a, q, c.x, c.y);
        if (std::abs(area) < 1e-12) continue;
        if (area < 0) std::swap(q, c);
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, q.x, c.x}))));
        const int x1 = std::min(W - 1, static_cast<int>(std::ceil(std::max({a.x, q.x, c.x}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, q.y, c.y}))));
        const int y1 = std::min(H - 1, static_cast<int>(std::ceil(std::max({a.y, q.y, c.y}))));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const double sx = x + 0.5, sy = y + 0.5;
                if (edge(a, q, sx, sy) >= 0 && edge(q, c, sx, sy) >= 0 && edge(c, a, sx, sy) >= 0)
                    hi[static_cast<std::size_t>(y) * W + x] = 1;
            }
    }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(2 * y) * W + 2 * x;
            const int n = hi[i] + hi[i + 1] + hi[i + W] + hi[i + W + 1];
            s.alpha[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint8_t>(n * 255 / 4);
        }
    return s;
}

std::string summarize(const OrientMetrics& o, const OrientMetrics& now) {
    std::vector<std::string> good, bad;
    auto diff = [](std::size_t a, std::size_t b) { return static_cast<double>(a) - static_cast<double>(b); };
    const double islands = diff(o.island_tips, now.island_tips);
    if (islands < 0) good.push_back(fmt("%.0f fewer spots start in mid-air", -islands));
    if (islands > 0) bad.push_back(fmt("%.0f more spots start in mid-air", islands));
    if (o.short_of_hold < now.short_of_hold)
        good.push_back(o.short_of_hold == 0 ? "every layer is held"
                                            : fmt("%.0f fewer layers are short of hold", -diff(o.short_of_hold, now.short_of_hold)));
    if (o.short_of_hold > now.short_of_hold)
        bad.push_back(fmt("%.0f more layers are short of hold", diff(o.short_of_hold, now.short_of_hold)));
    if (now.biggest_layer > 0) {
        const double r = 1 - o.biggest_layer / now.biggest_layer;
        if (r >= 0.15) good.push_back(fmt("the biggest layer is %.0f %% smaller", r * 100));
        if (r <= -0.15) bad.push_back(fmt("the biggest layer is %.0f %% bigger", -r * 100));
    }
    const double tips = diff(o.tips, now.tips);
    if (tips < 0) good.push_back(fmt("%.0f fewer tips to sand off", -tips));
    if (tips > 0) bad.push_back(fmt("%.0f more tips", tips));
    const double cos = diff(o.cosmetic_tips, now.cosmetic_tips);
    if (cos < 0) good.push_back(o.cosmetic_tips == 0 ? "no tips on the sides you'll see"
                                                     : fmt("%.0f fewer tips on the sides you'll see", -cos));
    if (cos > 0) bad.push_back(fmt("%.0f more tips on the sides you'll see", cos));
    const double dh = o.top - now.top;
    const double dl = diff(o.layers, now.layers);
    if (dh > 1) bad.push_back(fmt("it stands %.1f mm taller (%.0f more layers)", dh, dl));
    if (dh < -1) good.push_back(fmt("it stands %.1f mm shorter (%.0f fewer layers)", -dh, -dl));

    auto join = [](const std::vector<std::string>& v) {
        std::string s = v[0];
        if (v.size() > 1) s += " and " + v[1];
        return s;
    };
    std::string out;
    if (!good.empty()) {
        out = join(good);
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
        out += ".";
    } else {
        out = "Much like now.";
    }
    if (!bad.empty()) {
        std::string b = join(bad);
        out += " But " + b + ".";
    }
    return out;
}

}  // namespace gui
