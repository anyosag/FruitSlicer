// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Orient.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <future>
#include <cmath>
#include <cstdio>

#include "Document.h"
#include "Placing.h"
#include "Print.h"
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
// resin's measured ones and its own (the same the Supports step uses).
fruit::SupportConfig supportConfig(const fruit::Placement& p, const nlohmann::json& profile) {
    fruit::SupportConfig cfg;
    fruit::SupportChoices ch;
    ch.layer_height = kLayerHeight;
    ch.on_plate = p.on_plate;
    fruit::resolveSettings(profile.is_object() ? &profile : nullptr, ch, cfg, nullptr);
    fruit::TreeConfig tc;
    applyResinValues(profile, cfg, tc);
    return cfg;
}

std::vector<fruit::Vec3> visibleInPrinter(const std::vector<fruit::Vec3>& model_dirs, const fruit::Placement& p) {
    const fruit::Transform r = fruit::placementRotation(p);
    std::vector<fruit::Vec3> out;
    for (const fruit::Vec3& v : model_dirs) out.push_back(fruit::normalized(r.applyLinear(v)));
    return out;
}

namespace {

// Finalists given supports and replayed at the same time.
constexpr std::size_t kReplayLanes = 4;

std::size_t layersFor(double top) { return static_cast<std::size_t>(std::ceil(top / kLayerHeight - 1e-9)); }

// The rotation, turned about the vertical in 15° steps until it fits the
// plate (supports do not depend on that turn). Unchanged if nothing fits.
bool fitYaw(const Document& doc, fruit::Vec3& rotation_deg) {
    fruit::Placement q = doc.placement();
    for (int k = 0; k < 12; ++k) {
        q.rotation_deg = rotation_deg;
        q.rotation_deg.z = std::fmod(rotation_deg.z + 15.0 * k, 360.0);
        if (fruit::checkFit(doc.part().mesh(), q, doc.printer).fits_centred) {  // useOrientation slides it on
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

bool saferThan(const OrientRisk& a, const OrientRisk& b) {
    if (!a.checked || !b.checked) return false;
    if (b.worst_chance - a.worst_chance >= kRiskClearly) return true;
    if (a.worst_chance - b.worst_chance >= kRiskClearly) return false;
    return b.lost_mm3 >= kRiskLosesModel && a.lost_mm3 < 0.8 * b.lost_mm3;
}

std::string riskWords(const OrientRisk& r) {
    if (!r.checked) return "";
    if (r.places == 0) return "nothing likely to be lost";
    return r.places == 1 ? std::string("1 place may not print") : fmt("%.0f places may not print", static_cast<double>(r.places));
}

namespace {

// The selected part of `doc` on its own, standing as `p` has it.
Document alone(const Document& doc, const fruit::Placement& p) {
    Document one(doc);
    const int keep = doc.selectedId();
    for (const Part& q : doc.parts())
        if (q.id() != keep) one.removePart(q.id());
    one.select(keep);
    one.place(p);
    return one;
}

// The supports the Supports step would make for `one` with `choices`, and
// what the replay of its print finds with them.
OrientRisk replayOf(const Document& one, const SupportChoices& choices, const std::vector<fruit::Vec3>& visible_model,
                    OrientMetrics& m, const fruit::JobControl& ctl) {
    const fruit::Placement& p = one.placement();
    const std::vector<fruit::Vec3> visible = visibleInPrinter(visible_model, p);
    const SupportResult sup = generate(one, choices, visible, ctl.sub(0.0, 0.6));
    const RiskReport r = checkRisk(one, sup, ctl.sub(0.6, 1.0), false);
    m.tips = sup.points.size();
    m.island_tips = sup.island_tips;
    fruit::CosmeticSpec cos;
    cos.visible_from = visible;
    fruit::CostField cf;
    cf.cosmetic = &cos;
    m.cosmetic_tips = 0;
    for (const auto& pt : sup.points) m.cosmetic_tips += cf.isCosmetic(pt.pos, pt.normal);
    OrientRisk out;
    out.checked = true;
    std::size_t tips_only = 0;
    for (const RiskPlace& place : r.places) {
        if (place.kind == RiskPlace::Kind::Tips && place.lost_mm3 < kRiskLosesModel) {
            ++tips_only;  // the model stays held without them
            continue;
        }
        out.worst = std::max(out.worst, place.band);
        out.worst_chance = std::max(out.worst_chance, place.chance);
    }
    out.places = r.places_found - std::min(tips_only, r.places_found);
    out.tips = r.tips.size();
    out.lost_mm3 = r.result ? r.result->stats.expected_loss : 0;
    return out;
}

}  // namespace

OrientRisk riskFor(const Document& doc, const fruit::Placement& p, const std::vector<fruit::Vec3>& visible_model,
                   OrientMetrics& m, const fruit::JobControl& ctl) {
    return replayOf(alone(doc, p), doc.supports.choices, visible_model, m, ctl);
}

OrientMetrics measure(const Document& doc, const std::vector<fruit::Vec3>& visible_model,
                      const fruit::JobControl& ctl) {
    const fruit::Placement& p = doc.placement();
    fruit::SupportConfig cfg = supportConfig(p, doc.supports.choices.profile);
    cfg.plate_hold_height = modelInBottomLayers(doc);
    // The same planner run as the search's finalists, so the numbers compare.
    fruit::CosmeticSpec cos;
    cos.visible_from = visibleInPrinter(visible_model, p);
    fruit::PointGenOptions po;
    po.cosmetic = &cos;
    const fruit::PointGenResult res = fruit::generateSupportPoints(doc.part().placed(), cfg, po, ctl);
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
    m.top = doc.part().placed().bbox().max.z;
    m.layers = layersFor(m.top);
    return m;
}

OrientSearch search(const Document& doc, const std::vector<fruit::Vec3>& visible_model, const fruit::JobControl& ctl) {
    const auto t0 = std::chrono::steady_clock::now();
    OrientSearch s;
    const fruit::Placement& p = doc.placement();
    s.now.rotation_deg = p.rotation_deg;
    s.now.m = measure(doc, visible_model, ctl.sub(0.0, 0.04));

    // Search the model at its chosen scale, unrotated: the results are then
    // the placement's rotation angles.
    fruit::TriangleMesh m = doc.part().mesh();
    fruit::Transform scale;
    for (int i = 0; i < 3; ++i) scale.m[i][i] = p.scale;
    m.transform(scale);
    // A mirrored model is searched as the mirror image it is: the sides people
    // will see are mirrored with it.
    const fruit::Transform mirror = fruit::mirrorTransform(p);
    std::vector<fruit::Vec3> visible = visible_model;
    if (p.mirrored()) {
        m.transform(mirror);
        for (fruit::Vec3& v : visible) v = mirror.applyLinear(v);
    }
    fruit::OrientOptions o;
    o.cfg = supportConfig(p, doc.supports.choices.profile);
    o.cfg.plate_hold_height = modelInBottomLayers(doc);
    o.elevation = p.lift();
    o.visible_from = visible;
    o.keep_points = false;
    const auto results = fruit::searchOrientations(m, o, ctl.sub(0.04, 0.3));
    s.directions = o.directions + 1;
    for (const auto& r : results) s.finalists += r.evaluated && !r.is_input;  // how it sits now is planned too, but is not a finalist
    // Every finalist, and how it sits now, gets the supports the Supports
    // step would make and a replay of its print.
    struct Finalist {
        OrientOption op;
        double score = 0;  // the planner's ranking (lower is better)
    };
    std::vector<Finalist> fin;
    for (const auto& r : results) {
        if (!r.evaluated || r.is_input) continue;
        Finalist f;
        f.score = r.score;
        OrientOption& op = f.op;
        op.rotation_deg = r.euler_deg;
        op.fits = fitYaw(doc, op.rotation_deg);
        op.m.tips = r.points;
        op.m.island_tips = r.islands;
        op.m.cosmetic_tips = r.cosmetic_points;
        op.m.short_of_hold = r.capacity_deficits;
        op.m.biggest_layer = r.peak_layer_area;
        op.m.top = r.height + p.lift();
        op.m.layers = layersFor(op.m.top);
        fin.push_back(std::move(f));
    }
    double now_score = 0;
    // One run each, side by side: each part copy is made here first (a copy
    // prepares what it is copied from), then planned and replayed on its own
    // thread. Progress counts the runs done.
    struct Run {
        Document one;
        OrientMetrics* m;
    };
    std::vector<Run> runs;
    runs.push_back({alone(doc, p), &s.now.m});
    for (Finalist& f : fin) {
        if (!f.op.fits) continue;
        fruit::Placement q = p;
        q.rotation_deg = f.op.rotation_deg;
        runs.push_back({alone(doc, slidOnto(doc.part().mesh(), q, doc.printer)), &f.op.m});
    }
    {
        // A few at a time: each holds its model's and supports' layers (about
        // half a gigabyte on a 60 mm part), and each already uses several cores.
        std::atomic<std::size_t> next{0}, done{0};
        const fruit::JobControl quiet = ctl.silent();
        const std::size_t lanes = std::min<std::size_t>(runs.size(), kReplayLanes);
        std::vector<std::future<void>> work;
        for (std::size_t lane = 0; lane < lanes; ++lane)
            work.push_back(std::async(std::launch::async, [&] {
                for (std::size_t i; (i = next++) < runs.size();) {
                    Run& run = runs[i];
                    run.m->risk = replayOf(run.one, doc.supports.choices, visible_model, *run.m, quiet);
                    ctl.report("orient.replay",
                               0.3 + 0.67 * static_cast<double>(++done) / static_cast<double>(runs.size()));
                }
            }));
        std::exception_ptr err;
        for (auto& w : work) {
            try {
                w.get();
            } catch (...) {
                if (!err) err = std::current_exception();
            }
        }
        if (err) std::rethrow_exception(err);
    }
    s.replayed = static_cast<int>(runs.size());
    // The planner's ranking again, with the supports' own counts where they
    // were made (the planner alone left out the tips it adds where one tip
    // would carry too much): tips, those on the sides you'll see, the biggest
    // layer and the height against the best of them, as the search weighs them.
    {
        const fruit::OrientWeights w;
        double best_tips = 1e300, best_peak = 1e300, best_h = 1e300;
        auto best = [&](const OrientMetrics& m) {
            best_tips = std::min(best_tips, static_cast<double>(std::max<std::size_t>(m.tips, 1)));
            best_peak = std::min(best_peak, std::max(m.biggest_layer, 1e-9));
            best_h = std::min(best_h, std::max(m.top, 1e-9));
        };
        best(s.now.m);
        for (const Finalist& f : fin) best(f.op.m);
        auto score = [&](const OrientMetrics& m) {
            return static_cast<double>(m.tips) / best_tips + w.cosmetic * static_cast<double>(m.cosmetic_tips) / best_tips +
                   w.peak_area * m.biggest_layer / best_peak + w.height * m.top / best_h +
                   10.0 * static_cast<double>(m.short_of_hold) / best_tips;
        };
        now_score = score(s.now.m);
        for (Finalist& f : fin) f.score = score(f.op.m);
    }
    // Safest first: the likeliest place that takes some of the model with it,
    // then how much of it goes, then that ranking.
    std::stable_sort(fin.begin(), fin.end(), [](const Finalist& a, const Finalist& b) {
        const OrientRisk &x = a.op.m.risk, &y = b.op.m.risk;
        if (x.checked != y.checked) return x.checked;
        // In steps of kRiskClearly, so a few hundredths either way don't reorder them.
        const int sx = static_cast<int>(x.worst_chance / kRiskClearly), sy = static_cast<int>(y.worst_chance / kRiskClearly);
        if (sx != sy) return sx < sy;
        if (x.lost_mm3 != y.lost_mm3 && std::max(x.lost_mm3, y.lost_mm3) >= kRiskLosesModel) return x.lost_mm3 < y.lost_mm3;
        return a.score < b.score;
    });
    for (Finalist& f : fin) {
        const OrientRisk& r = f.op.m.risk;
        if (saferThan(s.now.m.risk, r)) {  // may lose more of the model: never offered
            ++s.riskier;
            continue;
        }
        if (s.options.size() == 3) continue;
        const bool safer = saferThan(r, s.now.m.risk);
        if (!safer && f.score >= now_score) continue;  // not better than now
        f.op.summary = summarize(f.op.m, s.now.m);
        s.options.push_back(std::move(f.op));
    }
    ctl.check();

    // How it sits now, against the options.
    {
        std::vector<std::string> parts;
        bool shortest = !s.options.empty();
        for (const auto& op : s.options) shortest = shortest && s.now.m.top < op.m.top - 0.05;
        if (shortest) parts.push_back("Quickest to print, as it stands lowest");
        if (s.now.m.risk.places > 0) parts.push_back(riskWords(s.now.m.risk));
        if (s.now.m.short_of_hold > 0)
            parts.push_back(fmt("%.0f layers are bigger than their tips can hold", static_cast<double>(s.now.m.short_of_hold)));
        if (s.options.empty() && s.riskier > 0 && s.riskier == static_cast<int>(fin.size()))
            s.now.summary = fin.size() == 1 ? std::string("The replay of its print finds places that may not print on the "
                                                          "other way the planner liked.")
                                            : fmt("The replay of their print finds places that may not print on all %.0f "
                                                  "other ways the planner liked best.",
                                                  static_cast<double>(fin.size()));
        else if (s.options.empty() && s.riskier > 0)
            s.now.summary = fmt("Of the %.0f other ways the planner liked best, the replay finds places that may not print "
                                "on %.0f, and the rest do no better.",
                                static_cast<double>(fin.size()), static_cast<double>(s.riskier));
        else if (s.options.empty()) s.now.summary = "No other way to stand it does better.";
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
        placed.push_back(fruit::placeModel(doc.part().mesh(), q));
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
    // What the replay finds comes first.
    if (saferThan(o.risk, now.risk)) {
        if (o.risk.places == 0) good.push_back("nothing is likely to be lost in printing");
        else if (now.risk.worst_chance - o.risk.worst_chance >= kRiskClearly)
            good.push_back("what may not print is less likely to go");
        else good.push_back("less of the model is likely to be lost");
    } else if (saferThan(now.risk, o.risk) || o.risk.places > now.risk.places) {
        bad.push_back(riskWords(o.risk));
    }
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
