// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Risk.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "Document.h"
#include "Format.h"
#include "Hollows.h"
#include "Layers.h"
#include "Orient.h"
#include "Supports.h"
#include "core/Parallel.h"
#include "mesh/AabbTree.h"
#include "geometry/Polygon.h"
#include "layers/LayerStack.h"
#include "raster/Rasterizer.h"

namespace gui {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::size_t kMaxPlaces = 12;        // a longer list is no list to read
constexpr std::size_t kMaxPaintFaces = 3000000;

std::string fmt(const char* f, double a = 0, double b = 0, double c = 0) {
    char s[320];
    std::snprintf(s, sizeof s, f, a, b, c);
    return s;
}

// "4.5" under ten, "37" from there on.
std::string amount(double v) { return fmt(v < 9.95 ? "%.1f" : "%.0f", v); }

// A layer as the Layers place counts it (from 1), and how high its top is.
std::string whenText(int layer, double h) {
    return fmt("while layer %.0f prints, %.2f mm up", layer + 1.0, (layer + 1) * h);
}

const char* phrase(RiskPlace::Kind k, RiskBand b) {
    const int i = b == RiskBand::VeryLikely ? 2 : b == RiskBand::Likely ? 1 : 0;
    switch (k) {
        case RiskPlace::Kind::ThinPlace: {
            static const char* w[3] = {"May break", "Likely to break", "Very likely to break"};
            return w[i];
        }
        case RiskPlace::Kind::Tips: {
            static const char* w[3] = {"May let go", "Some are likely to let go", "Some are very likely to let go"};
            return w[i];
        }
        case RiskPlace::Kind::Skin: {
            static const char* w[3] = {"May not form", "Likely not to form", "Very likely not to form"};
            return w[i];
        }
        case RiskPlace::Kind::MidAir: return "Nothing holds it";
        case RiskPlace::Kind::Sliver: {
            static const char* w[3] = {"May come off", "Likely to come off", "Very likely to come off"};
            return w[i];
        }
        case RiskPlace::Kind::Support: {
            static const char* w[3] = {"May snap", "Likely to snap", "Very likely to snap"};
            return w[i];
        }
    }
    return "";
}

// "· 160 mm³ would be lost", or that the model stays held.
std::string lostText(double mm3) {
    if (mm3 >= kRiskLosesModel) return amount(mm3) + " mm³ would be lost";
    return "the model stays held";
}

// A tip's width from the area it touches the model with, to a tenth.
double tipWidth(double area) { return std::round(2 * std::sqrt(std::max(area, 0.0) / kPi) * 10) / 10; }

int bandIndex(RiskBand b) { return static_cast<int>(b) - 1; }  // -1: none

}  // namespace

RiskBand riskBand(double chance) {
    if (chance >= kRiskVeryLikelyFrom) return RiskBand::VeryLikely;
    if (chance >= kRiskLikelyFrom) return RiskBand::Likely;
    if (chance >= kRiskShownFrom) return RiskBand::May;
    return RiskBand::None;
}

unsigned riskColour(RiskBand b) {
    switch (b) {
        case RiskBand::None: return 0;
        case RiskBand::May: return 0xF2D34B;
        case RiskBand::Likely: return 0xFF6B6B;
        case RiskBand::VeryLikely: return 0xB3214A;
    }
    return 0;
}

void findRiskPlaces(const fruit::StabilityResult& res, double usable, RiskReport& out) {
    const double h = res.layer_height;
    std::vector<RiskPlace> places;
    for (const fruit::WeakSpot& s : res.spots) {
        const RiskBand band = riskBand(s.chance);
        if (band == RiskBand::None) continue;
        RiskPlace p;
        p.band = band;
        p.chance = s.chance;
        p.at = s.at;
        p.layer = s.at_layer;
        p.lost_mm3 = s.lost_volume;
        const std::string up = fmt(", %.1f mm up", s.at.z);
        const bool loses = s.lost_volume >= kRiskLosesModel;
        switch (s.cause) {
            case fruit::RiskCause::Tip:
                continue;  // one of "the tips", below: they are one place together
            case fruit::RiskCause::Support:
                p.kind = RiskPlace::Kind::Support;
                p.title = "Thin support" + up;
                p.holds = amount(s.load) + " mm² of layer on " + amount(s.area) + " mm² of support";
                p.why = "all that its tips carry goes through this one thin place";
                p.if_goes = loses ? "about " + amount(s.lost_volume) + " mm³ of the model goes with it"
                                  : "the supports around it take its load, and the model stays held";
                break;
            case fruit::RiskCause::Neck:
            case fruit::RiskCause::Bend:
                p.kind = RiskPlace::Kind::ThinPlace;
                p.title = "Thin place" + up;
                p.holds = amount(s.load) + " mm² of layer on " + amount(s.area) + " mm² of resin";
                if (s.cause == fruit::RiskCause::Bend) {
                    p.why = "what is above it lies to one side, so the pull bends it";
                    if (s.holds > 0)
                        p.note = fmt("Bent in full it is at %.0f %% of its limit. If the film holds the layer sideways, at "
                                     "%.0f %%. Which one is true has not been measured.",
                                     s.closeness * 100, s.load / s.holds * 100);
                } else {
                    p.why = "all that is above it hangs on this one thin place";
                }
                p.if_goes = loses ? "about " + amount(s.lost_volume) + " mm³ above it is lost"
                                  : "what is above it is still held another way";
                break;
            case fruit::RiskCause::Skin:
                p.kind = RiskPlace::Kind::Skin;
                p.title = "Skin out of reach" + up;
                p.holds = amount(s.area) + " mm² of fresh skin, one layer thick";
                p.why = "it reaches further past what holds it than one fresh layer spans";
                p.if_goes = "that patch comes out torn or sagging";
                p.lost_mm3 = 0;
                break;
            case fruit::RiskCause::Unheld:
                if (s.holds > 0) {
                    p.kind = RiskPlace::Kind::Sliver;
                    p.title = "Hardly held" + up;
                    p.holds = amount(s.load) + " mm² of layer on " + fmt("%.2f", s.area) + " mm² of contact";
                    p.why = "it touches what holds it over less than the smallest tip would";
                    p.if_goes = loses ? "about " + amount(s.lost_volume) + " mm³ of the model goes with it"
                                      : "the model stays held without it";
                } else {
                    p.kind = RiskPlace::Kind::MidAir;
                    p.title = "Starts in mid-air" + up;
                    p.holds = "nothing: " + amount(s.area) + " mm² starts with nothing under it";
                    p.why = "no tip and no model is under where it starts";
                    p.if_goes = "about " + amount(s.lost_volume) + " mm³ prints as loose flakes";
                }
                break;
            case fruit::RiskCause::None: continue;
        }
        p.when = whenText(std::max(0, s.at_layer), h);
        p.brief = std::string(phrase(p.kind, band)) + " · " +
                  (p.kind == RiskPlace::Kind::Skin ? amount(s.area) + " mm² with nothing near it" : lostText(p.lost_mm3));
        places.push_back(std::move(p));
    }

    // The tips: every one that may let go is coloured in the view, and they
    // are one place together. When they let go one after another the model
    // falls with whichever went last, so what goes with them is said for all
    // of them, as the replays had it: how often, and how much then.
    const bool cost = res.stats.tips_cost_chance >= kRiskShownFrom && res.stats.tips_cost_volume >= kRiskLosesModel;
    auto shows = [&](const fruit::TipRisk& t, bool over_trust) {
        if (riskBand(t.chance) == RiskBand::None) return false;
        // A tip that only grazes the model, with nothing going with it: it may
        // snap and nothing comes of it (the replay does not list it either).
        if (t.area < fruit::kSliverContact && t.lost_volume < kRiskLosesModel) return false;
        return !over_trust || t.load > kRiskTipOverTrust * t.holds * usable;
    };
    bool over_trust = true;
    {
        bool any = false;
        for (const fruit::TipRisk& t : res.tips) any = any || shows(t, true);
        // None carries more than it is trusted with, yet model goes with them: they go at their rated loads.
        if (!any && cost) over_trust = false;
    }
    RiskPlace group;
    group.kind = RiskPlace::Kind::Tips;
    double lo = 1e300, hi = 0;  // what they carry, in multiples of what each is trusted with
    int first = -1, last = -1;
    const fruit::TipRisk* worst = nullptr;   // the likeliest to let go
    const fruit::TipRisk* costly = nullptr;  // the one the model most often goes with
    for (const fruit::TipRisk& t : res.tips) {
        if (!shows(t, over_trust)) continue;
        out.tips.push_back({t.at, tipWidth(t.area), riskBand(t.chance)});
        ++group.tips;
        if (!worst || t.chance > worst->chance) worst = &t;
        if (t.lost_per_print > 0 && (!costly || t.lost_per_print > costly->lost_per_print)) costly = &t;
        const double trusted = t.holds * usable;
        if (trusted > 0) {
            lo = std::min(lo, t.load / trusted);
            hi = std::max(hi, t.load / trusted);
        }
        first = first < 0 ? t.at_layer : std::min(first, t.at_layer);
        last = std::max(last, t.at_layer);
    }
    if (worst) {
        const fruit::TipRisk* lead = cost && costly ? costly : worst;
        group.band = riskBand(worst->chance);
        group.chance = worst->chance;
        group.at = lead->at;
        group.layer = lead->at_layer;
        group.lost_mm3 = cost ? res.stats.tips_cost_volume : 0.0;
        const bool one = group.tips == 1;
        const int b = bandIndex(group.band);
        if (one) {
            static const char* t1[3] = {"1 tip may let go", "1 tip is likely to let go", "1 tip is very likely to let go"};
            group.title = t1[b];
        } else {
            static const char* more[3] = {"", ", some likely", ", some very likely"};
            group.title = fmt("%.0f tips may let go", group.tips) + more[b];
        }
        static const char* one_phrase[3] = {"May let go", "Likely to let go", "Very likely to let go"};
        const std::string how = one ? one_phrase[b] : phrase(group.kind, group.band);
        group.brief = how + " · " +
                      (cost ? amount(group.lost_mm3) + (one ? " mm³ may go with it" : " mm³ may go with them")
                            : std::string("the model stays held"));
        const std::string times = hi - lo < 0.05 ? fmt("about %.1f times", hi) : fmt("%.1f to %.1f times", lo, hi);
        group.holds = (one ? "it carries " : "each carries ") + times + " what a tip of its size is trusted with";
        group.when = first == last ? whenText(std::max(0, first), h)
                                   : fmt("while layers %.0f to %.0f print", first + 1.0, last + 1.0);
        group.why = one ? "the layers over it pull harder than this tip is trusted to hold"
                        : "the layers over them pull harder than these tips are trusted to hold";
        if (cost) {
            const double n = std::max(1.0, std::round(res.stats.tips_cost_chance * res.stats.replays));
            group.if_goes = fmt("in %.0f of %.0f replays part of the model went with ", n, res.stats.replays) +
                            (one ? "it" : "them") + ": about " + amount(group.lost_mm3) + " mm³";
            // Said outright where it is the only finding: how likely, and whether it is all of the model.
            static const char* comes_off[3] = {" may come off its tips", " is likely to come off its tips",
                                               " is very likely to come off its tips"};
            const bool whole = group.lost_mm3 >= 0.9 * res.stats.model_volume && res.stats.model_volume > 0;
            group.headline = std::string(whole ? "The whole model" : "Part of the model") +
                             comes_off[std::max(0, bandIndex(riskBand(res.stats.tips_cost_chance)))];
        } else {
            group.if_goes = one ? "the tips around it take its load, and the model stays held"
                                : "the tips around them take their load, and the model stays held";
            // (For the headline: the same numbers, as whole sentences.)
            group.note = (one ? "It carries " : "They carry ") + times +
                         (one ? " what it is trusted with. The model stays held without it."
                              : " what they are trusted with. The model stays held without them.");
        }
        places.push_back(std::move(group));
    }

    // What takes part of the model with it first, the most first; then the likeliest.
    std::stable_sort(places.begin(), places.end(), [](const RiskPlace& a, const RiskPlace& b) {
        const bool la = a.lost_mm3 >= kRiskLosesModel, lb = b.lost_mm3 >= kRiskLosesModel;
        if (la != lb) return la;
        if (a.band != b.band) return a.band > b.band;
        if (la && a.lost_mm3 != b.lost_mm3) return a.lost_mm3 > b.lost_mm3;
        return a.chance > b.chance;
    });
    out.places_found = places.size();
    if (places.size() > kMaxPlaces) places.resize(kMaxPlaces);
    out.places = std::move(places);

    out.closeness = res.closeness_by_layer;
    out.riskiest_layer = -1;
    float top = 0;
    for (std::size_t i = 0; i < out.closeness.size(); ++i)
        if (out.closeness[i] > top) {
            top = out.closeness[i];
            out.riskiest_layer = static_cast<int>(i);
        }
    for (double& v : out.lost_mm3) v = 0;
    for (std::size_t id = 0; id < res.parts.size(); ++id) {
        if (!res.parts[id].model) continue;
        const int b = bandIndex(riskBand(res.partChance(static_cast<int>(id))));
        if (b >= 0) out.lost_mm3[b] += res.where[id].area * h;
    }
    out.replays = res.stats.replays;
}

RiskHeadline riskHeadline(const RiskReport& r) {
    RiskHeadline hl;
    const std::string replayed =
        r.replays > 0 ? fmt("The print was replayed layer by layer, %.0f times.", r.replays)
                      : std::string("Every layer's pull was followed through the print.");
    if (r.places.empty()) {
        hl.title = "Nothing likely to be lost in printing";
        hl.detail = replayed;
        return hl;
    }
    hl.ok = false;
    if (r.places_found == 1 && r.places.front().kind == RiskPlace::Kind::Tips &&
        r.places.front().lost_mm3 < kRiskLosesModel) {
        hl.tips_only = true;
        hl.title = r.places.front().title;
        hl.detail = r.places.front().note;
        return hl;
    }
    if (r.places_found == 1 && !r.places.front().headline.empty()) {
        hl.title = r.places.front().headline;
        hl.detail = replayed + " Pick it to see it on the model.";
        return hl;
    }
    hl.title = r.places_found == 1 ? "1 place may not print" : fmt("%.0f places may not print", static_cast<double>(r.places_found));
    hl.detail = replayed + " Pick a place to see it on the model.";
    if (r.places_found > r.places.size())
        hl.detail += fmt(" The first %.0f are listed.", static_cast<double>(r.places.size()));
    return hl;
}

std::string riskStrengthsNote(const RiskReport& r) {
    // The guesses the answer moves most with, whatever the resin: how a pull
    // spreads through thin model (from 2 to 4 mm, the lid's likeliest tip
    // went from under 1 in 100 to more than 1 in 4), and a cup's suction.
    std::string s = r.borrowed ? "Strengths are borrowed, not measured for this resin."
                               : "Tip strengths are this resin's, from its tips plate. How strong the model is between "
                                 "its own layers is taken to be the same: not measured.";
    s += " How far a layer's pull spreads through thin model is a guess too: with another, a tip can come out many "
         "times likelier to let go.";
    if (r.cups) s += " So is how hard the suction in a cup pulls.";
    return s;
}

// ---------------------------------------------------------------- the paint

namespace {

// A convex polygon: a triangle, or what clipping it by up to six planes leaves.
struct Poly {
    static constexpr int kMax = 12;
    fruit::Vec3 v[kMax];
    int n = 0;
};

double along(const fruit::Vec3& p, int axis) { return axis == 0 ? p.x : axis == 1 ? p.y : p.z; }

// What of `in` lies at or above `at` along `axis` (keep_above), or at or below it.
Poly clipAxis(const Poly& in, int axis, double at, bool keep_above) {
    Poly out;
    for (int i = 0; i < in.n; ++i) {
        const fruit::Vec3& a = in.v[i];
        const fruit::Vec3& b = in.v[(i + 1) % in.n];
        const double va = along(a, axis), vb = along(b, axis);
        const bool ia = keep_above ? va >= at : va <= at, ib = keep_above ? vb >= at : vb <= at;
        if (ia && out.n < Poly::kMax) out.v[out.n++] = a;
        if (ia != ib && out.n < Poly::kMax) {
            const double t = (at - va) / (vb - va);
            fruit::Vec3 q{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
            if (axis == 0) q.x = at;
            else if (axis == 1) q.y = at;
            else q.z = at;
            out.v[out.n++] = q;
        }
    }
    return out;
}

// What of `in` lies at z >= z0 (keep_above) or z <= z0.
Poly clipZ(const Poly& in, double z0, bool keep_above) { return clipAxis(in, 2, z0, keep_above); }

}  // namespace

void paintRisk(const fruit::TriangleMesh& mesh, const fruit::StabilityResult& res, fruit::TriangleMesh paint[3],
               const fruit::JobControl& ctl) {
    const double h = res.layer_height;
    const int layers = static_cast<int>(res.in_layer.size());
    if (layers == 0 || !(h > 0)) return;
    // The model's parts that may not print, layer by layer.
    struct Hit {
        int id;
        int band;
    };
    std::vector<std::vector<Hit>> risky(static_cast<std::size_t>(layers));
    std::vector<int> upto(static_cast<std::size_t>(layers) + 1, 0);  // how many risky layers there are below each
    for (int L = 0; L < layers; ++L) {
        for (int id : res.in_layer[static_cast<std::size_t>(L)]) {
            if (!res.parts[static_cast<std::size_t>(id)].model) continue;
            const int b = bandIndex(riskBand(res.partChance(id)));
            if (b >= 0) risky[static_cast<std::size_t>(L)].push_back({id, b});
        }
        upto[static_cast<std::size_t>(L) + 1] = upto[static_cast<std::size_t>(L)] + !risky[static_cast<std::size_t>(L)].empty();
    }
    if (upto.back() == 0) return;
    auto layerOf = [&](double z) { return std::clamp(static_cast<int>(std::floor(z / h)), 0, layers - 1); };
    // The band of the model at a point just under the surface, or -1.
    auto bandAt = [&](const fruit::Vec3& p) {
        const fruit::Vec2 xy{p.x, p.y};
        for (const Hit& hit : risky[static_cast<std::size_t>(layerOf(p.z))]) {
            const fruit::PrintPart& w = res.where[static_cast<std::size_t>(hit.id)];
            if (xy.x < w.box.min.x || xy.x > w.box.max.x || xy.y < w.box.min.y || xy.y > w.box.max.y) continue;
            if (w.shape->contains(xy)) return hit.band;
        }
        return -1;
    };
    std::size_t made = 0;
    for (std::size_t f = 0; f < mesh.faces.size() && made < kMaxPaintFaces; ++f) {
        if ((f & 8191) == 0) ctl.check();
        const fruit::Face& face = mesh.faces[f];
        Poly tri;
        tri.n = 3;
        for (int i = 0; i < 3; ++i) tri.v[i] = mesh.vertices[static_cast<std::size_t>(face[static_cast<std::size_t>(i)])];
        const double z0 = std::min({tri.v[0].z, tri.v[1].z, tri.v[2].z}), z1 = std::max({tri.v[0].z, tri.v[1].z, tri.v[2].z});
        const int l0 = layerOf(z0), l1 = layerOf(z1);
        // Nothing at risk in its layers, or the ones next to them (a flat face lies between two).
        if (upto[static_cast<std::size_t>(std::min(layers, l1 + 2))] == upto[static_cast<std::size_t>(std::max(0, l0 - 1))]) continue;
        const fruit::Vec3 n = mesh.faceNormal(f);
        if (n.x == 0 && n.y == 0 && n.z == 0) continue;
        for (int L = l0; L <= l1; ++L) {
            // The face within this layer.
            Poly part = tri;
            if (l1 > l0) {
                part = clipZ(clipZ(tri, L * h, true), (L + 1) * h, false);
                if (part.n < 3) continue;
            }
            fruit::Vec3 c{};
            for (int i = 0; i < part.n; ++i) c = c + part.v[i];
            c = c / static_cast<double>(part.n);
            // The model is on the inside of its surface (on either side, for a
            // mesh turned inside out): a step in of less than half a layer,
            // so a flat face asks the layer it bounds and not the next one.
            int band = -1;
            for (double e : {-0.02, 0.02}) {
                band = bandAt(c + n * e);
                if (band >= 0) break;
            }
            if (band < 0) continue;
            fruit::TriangleMesh& m = paint[band];
            const auto base = static_cast<std::int32_t>(m.vertices.size());
            for (int i = 0; i < part.n; ++i) m.vertices.push_back(part.v[i]);
            for (int i = 1; i + 1 < part.n; ++i) m.faces.push_back({base, base + i, base + i + 1});
            made += static_cast<std::size_t>(part.n - 2);
        }
    }
}

RiskLayerPaint paintRiskLayer(const fruit::StabilityResult& res, int layer, const LayerImage& grid) {
    RiskLayerPaint out;
    if (layer < 0 || static_cast<std::size_t>(layer) >= res.in_layer.size() || grid.w <= 0 || grid.h <= 0) return out;
    fruit::Paths by_band[3];
    for (int id : res.in_layer[static_cast<std::size_t>(layer)]) {
        const double pc = res.partChance(id);
        const int b = bandIndex(riskBand(pc));
        if (b >= 0) {
            const fruit::Paths ps = res.where[static_cast<std::size_t>(id)].shape->toPaths();
            by_band[b].insert(by_band[b].end(), ps.begin(), ps.end());
        }
        // Its fresh skin, where that is likelier not to form than the part is to be lost.
        if (static_cast<std::size_t>(layer) < res.skins.size())
            for (const fruit::SkinBand& s : res.skins[static_cast<std::size_t>(layer)]) {
                if (s.part != id) continue;
                const int sb = bandIndex(riskBand(1 - (1 - pc) * (1 - s.chance)));
                if (sb > b) by_band[sb].insert(by_band[sb].end(), s.region.begin(), s.region.end());
            }
    }
    fruit::RasterGrid g;
    g.width = grid.w;
    g.height = grid.h;
    g.px = grid.px;
    g.py = grid.py;
    g.x_left = grid.x_left;
    g.y_top = grid.y_top;
    for (int b = 0; b < 3; ++b) {
        if (by_band[b].empty()) continue;
        out.cover[b] = fruit::rasterizeDense(fruit::unionNonZero(by_band[b]), g, 16);
        out.any = true;
    }
    return out;
}

// ---------------------------------------------------------------- the heatmap

namespace {

// The scale: chances, and where each lies along it (between them it goes by the chance's logarithm).
constexpr double kHeatAnchors[][2] = {{0.001, 0.0}, {0.01, 0.22}, {0.05, 0.45}, {0.2, 0.68}, {0.5, 0.88}, {1.0, 1.0}};
// Its colours, at those places along it: blue for fine, then teal, yellow, orange, red and dark red.
struct HeatStop {
    double at;
    unsigned rgb;
};
constexpr HeatStop kHeatStops[] = {{0.0, 0x3E5C8A}, {0.22, 0x3F9C9A}, {0.45, 0xE2C255},
                                   {0.68, 0xEE8A3C}, {0.88, 0xD5344F}, {1.0, 0x9E1838}};

// Whether `p` is inside paths as Clipper returns them (outer rings one way round, holes the other).
bool insidePaths(const fruit::Paths& ps, const fruit::Vec2& p) {
    const Clipper2Lib::Point64 q = fruit::toPoint(p);
    int count = 0;
    for (const fruit::Path& path : ps) {
        if (path.size() < 3) continue;
        if (Clipper2Lib::PointInPolygon(q, path) == Clipper2Lib::PointInPolygonResult::IsOutside) continue;
        count += Clipper2Lib::Area(path) >= 0 ? 1 : -1;
    }
    return count > 0;
}

// An island's own chance, by the heat's measure (the skins aside).
double islandChance(const fruit::StabilityResult& res, int id, HeatOf of) {
    return of == HeatOf::Breaks ? res.parts[static_cast<std::size_t>(id)].break_chance : res.partChance(id);
}

// With a skin of chance `skin` over it as well.
double withSkin(double island, double skin) { return 1 - (1 - island) * (1 - skin); }

// The heat at `xy` of the model's island `id` in `layer` (`xy` inside it).
double islandHeat(const fruit::StabilityResult& res, int id, std::size_t layer, const fruit::Vec2& xy, HeatOf of) {
    const double pc = islandChance(res, id, of);
    if (of == HeatOf::Breaks || layer >= res.skins.size()) return pc;
    double skin = 0;
    for (const fruit::SkinBand& b : res.skins[layer])
        if (b.part == id && b.chance > skin && insidePaths(b.region, xy)) skin = b.chance;
    return withSkin(pc, skin);
}

// For each layer: the step every spot of its model islands shares (none:
// 0), or -1 when it differs from island to island or along a skin; and how
// many layers below each have a spot warmer than fine, to skip what is all
// fine quickly. The skins' outlines' boxes, by layer, for where a skin may
// colour part of a face.
struct HeatLayers {
    std::vector<int> uniform;
    std::vector<int> warm_upto;
    std::vector<std::vector<fruit::BBox2>> skin_boxes;
};

HeatLayers heatLayers(const fruit::StabilityResult& res, HeatOf of) {
    const std::size_t layers = res.in_layer.size();
    HeatLayers h;
    h.uniform.assign(layers, 0);
    h.warm_upto.assign(layers + 1, 0);
    h.skin_boxes.resize(layers);
    for (std::size_t L = 0; L < layers; ++L) {
        int lo = kHeatSteps, hi = -1;
        bool varies = false;
        for (int id : res.in_layer[L]) {
            if (!res.parts[static_cast<std::size_t>(id)].model) continue;
            const double pc = islandChance(res, id, of);
            const int st = heatStep(pc);
            lo = std::min(lo, st);
            hi = std::max(hi, st);
            if (of != HeatOf::Lost || L >= res.skins.size()) continue;
            for (const fruit::SkinBand& b : res.skins[L]) {
                if (b.part != id) continue;
                const int sb = heatStep(withSkin(pc, b.chance));
                if (sb == st) continue;
                varies = true;
                hi = std::max(hi, sb);
                fruit::BBox2 box;
                for (const fruit::Path& path : b.region)
                    for (const auto& q : path) box.extend(fruit::toVec(q));
                h.skin_boxes[L].push_back(box);
            }
        }
        h.uniform[L] = hi < 0 ? 0 : lo == hi && !varies ? lo : -1;
        h.warm_upto[L + 1] = h.warm_upto[L] + (hi > 0 ? 1 : 0);
    }
    return h;
}

bool overlaps(const fruit::BBox2& a, const fruit::BBox2& b) {
    return a.min.x <= b.max.x && b.min.x <= a.max.x && a.min.y <= b.max.y && b.min.y <= a.max.y;
}

}  // namespace

double heatPosition(double chance) {
    if (!(chance > kHeatAnchors[0][0])) return 0;
    if (chance >= 1) return 1;
    for (std::size_t i = 1; i < std::size(kHeatAnchors); ++i) {
        if (chance > kHeatAnchors[i][0]) continue;
        const double a = std::log10(kHeatAnchors[i - 1][0]), b = std::log10(kHeatAnchors[i][0]);
        const double t = (std::log10(chance) - a) / (b - a);
        return kHeatAnchors[i - 1][1] + t * (kHeatAnchors[i][1] - kHeatAnchors[i - 1][1]);
    }
    return 1;
}

int heatStep(double chance) {
    return std::clamp(static_cast<int>(heatPosition(chance) * kHeatSteps), 0, kHeatSteps - 1);
}

unsigned heatColour(double position) {
    const double p = std::clamp(position, 0.0, 1.0);
    for (std::size_t i = 1; i < std::size(kHeatStops); ++i) {
        if (p > kHeatStops[i].at) continue;
        const HeatStop& a = kHeatStops[i - 1];
        const HeatStop& b = kHeatStops[i];
        const double t = (p - a.at) / (b.at - a.at);
        unsigned rgb = 0;
        for (int shift : {16, 8, 0}) {
            const double ca = (a.rgb >> shift) & 0xFF, cb = (b.rgb >> shift) & 0xFF;
            rgb |= static_cast<unsigned>(std::lround(ca + (cb - ca) * t)) << shift;
        }
        return rgb;
    }
    return kHeatStops[std::size(kHeatStops) - 1].rgb;
}

unsigned heatStepColour(int step) {
    return heatColour((std::clamp(step, 0, kHeatSteps - 1) + 0.5) / kHeatSteps);
}

const std::vector<HeatMark>& heatMarks() {
    static const std::vector<HeatMark> marks = {{0.01, "1 in 100"}, {0.05, "1 in 20"}, {0.2, "1 in 5"}, {0.5, "1 in 2"}};
    return marks;
}

bool HeatPaint::any() const {
    for (const fruit::TriangleMesh& m : steps)
        if (!m.faces.empty()) return true;
    return false;
}

void paintHeat(const fruit::TriangleMesh& mesh, const fruit::StabilityResult& res, HeatOf of, HeatPaint& out,
               const fruit::JobControl& ctl) {
    const double h = res.layer_height;
    const int layers = static_cast<int>(res.in_layer.size());
    if (layers == 0 || !(h > 0)) return;
    const HeatLayers hl = heatLayers(res, of);
    if (hl.warm_upto.back() == 0) return;  // all of it fine: the model's own colour says so
    auto layerOf = [&](double z) { return std::clamp(static_cast<int>(std::floor(z / h)), 0, layers - 1); };
    // The step of the model just under the surface at `c` (the face's normal `n`) in layer L: inside the
    // surface first, then (for a mesh turned inside out) the other side; fine where no island of it is.
    auto stepAt = [&](int L, const fruit::Vec3& c, const fruit::Vec3& n) {
        for (double e : {-0.02, 0.02}) {
            const fruit::Vec2 xy{c.x + n.x * e, c.y + n.y * e};
            for (int id : res.in_layer[static_cast<std::size_t>(L)]) {
                if (!res.parts[static_cast<std::size_t>(id)].model) continue;
                const fruit::PrintPart& w = res.where[static_cast<std::size_t>(id)];
                if (xy.x < w.box.min.x || xy.x > w.box.max.x || xy.y < w.box.min.y || xy.y > w.box.max.y) continue;
                if (w.shape->contains(xy)) return heatStep(islandHeat(res, id, static_cast<std::size_t>(L), xy, of));
            }
        }
        return 0;
    };
    // Faces in blocks, each block on its own core into its own paint; joined in order after, so it comes out
    // the same however many cores there are.
    constexpr std::size_t kBlock = 4096;
    const std::size_t blocks = (mesh.faces.size() + kBlock - 1) / kBlock;
    std::vector<HeatPaint> parts(blocks);
    std::atomic<std::size_t> made{0};
    fruit::parallelFor(blocks, 0, [&](std::size_t blk) {
        ctl.check();
        HeatPaint& mine = parts[blk];
        auto emit = [&](int step, const Poly& part) {
            if (step <= 0 || part.n < 3 || made.load(std::memory_order_relaxed) >= kMaxPaintFaces) return;
            fruit::TriangleMesh& m = mine.steps[step];
            const auto base = static_cast<std::int32_t>(m.vertices.size());
            for (int i = 0; i < part.n; ++i) m.vertices.push_back(part.v[i]);
            for (int i = 1; i + 1 < part.n; ++i) m.faces.push_back({base, base + i, base + i + 1});
            made.fetch_add(static_cast<std::size_t>(part.n - 2), std::memory_order_relaxed);
        };
        // A piece of a face in a layer where a skin may colour part of it: in small squares, each its own step.
        auto emitSquares = [&](int L, const Poly& piece, const fruit::Vec3& n) {
            fruit::BBox2 box;
            for (int i = 0; i < piece.n; ++i) box.extend({piece.v[i].x, piece.v[i].y});
            const double w = box.max.x - box.min.x, d = box.max.y - box.min.y;
            const double cell = std::max(0.25, std::sqrt(std::max(w * d, 1e-6) / 900.0));
            const int nx = std::max(1, static_cast<int>(std::ceil(w / cell))), ny = std::max(1, static_cast<int>(std::ceil(d / cell)));
            for (int i = 0; i < nx; ++i) {
                const Poly col = clipAxis(clipAxis(piece, 0, box.min.x + i * w / nx, true), 0, box.min.x + (i + 1) * w / nx, false);
                if (col.n < 3) continue;
                for (int j = 0; j < ny; ++j) {
                    const Poly sq = clipAxis(clipAxis(col, 1, box.min.y + j * d / ny, true), 1, box.min.y + (j + 1) * d / ny, false);
                    if (sq.n < 3) continue;
                    fruit::Vec3 c{};
                    for (int k = 0; k < sq.n; ++k) c = c + sq.v[k];
                    emit(stepAt(L, c / static_cast<double>(sq.n), n), sq);
                }
            }
        };
        const std::size_t end = std::min(mesh.faces.size(), (blk + 1) * kBlock);
        for (std::size_t f = blk * kBlock; f < end; ++f) {
            const fruit::Face& face = mesh.faces[f];
            Poly tri;
            tri.n = 3;
            for (int i = 0; i < 3; ++i) tri.v[i] = mesh.vertices[static_cast<std::size_t>(face[static_cast<std::size_t>(i)])];
            const double z0 = std::min({tri.v[0].z, tri.v[1].z, tri.v[2].z}), z1 = std::max({tri.v[0].z, tri.v[1].z, tri.v[2].z});
            const int l0 = layerOf(z0), l1 = layerOf(z1);
            // All fine in its layers and the ones next to them (a flat face lies between two).
            if (hl.warm_upto[static_cast<std::size_t>(std::min(layers, l1 + 2))] ==
                hl.warm_upto[static_cast<std::size_t>(std::max(0, l0 - 1))])
                continue;
            const fruit::Vec3 n = mesh.faceNormal(f);
            if (n.x == 0 && n.y == 0 && n.z == 0) continue;
            // The face within layers a..b.
            auto within = [&](int a, int b) {
                return l1 > l0 ? clipZ(clipZ(tri, a * h, true), (b + 1) * h, false) : tri;
            };
            auto centre = [](const Poly& p) {
                fruit::Vec3 c{};
                for (int i = 0; i < p.n; ++i) c = c + p.v[i];
                return c / static_cast<double>(p.n);
            };
            // Up the face, layer by layer, in runs of one step.
            int run_from = l0, run_step = -1;
            for (int L = l0; L <= l1; ++L) {
                // The layer the model under this piece is in: its own, or for a flat face the one inside it.
                int at = L;
                if (l1 == l0) at = layerOf(centre(tri).z - n.z * 0.02);
                int step = hl.uniform[static_cast<std::size_t>(at)];
                if (step < 0) {
                    const Poly piece = within(L, L);
                    if (piece.n < 3) continue;
                    fruit::BBox2 box;
                    for (int i = 0; i < piece.n; ++i) box.extend({piece.v[i].x, piece.v[i].y});
                    bool skin = false;
                    for (const fruit::BBox2& sb : hl.skin_boxes[static_cast<std::size_t>(at)]) skin = skin || overlaps(box, sb);
                    if (skin) {
                        if (run_step > 0) emit(run_step, within(run_from, L - 1));
                        emitSquares(at, piece, n);
                        run_from = L + 1;
                        run_step = -1;
                        continue;
                    }
                    step = stepAt(at, centre(piece), n);
                }
                if (step != run_step) {
                    if (run_step > 0) emit(run_step, within(run_from, L - 1));
                    run_from = L;
                    run_step = step;
                }
            }
            if (run_step > 0 && run_from <= l1) emit(run_step, within(run_from, l1));
        }
    });
    for (HeatPaint& part : parts)
        for (int s = 1; s < kHeatSteps; ++s) {
            fruit::TriangleMesh& to = out.steps[s];
            const fruit::TriangleMesh& from = part.steps[s];
            if (from.faces.empty()) continue;
            const auto base = static_cast<std::int32_t>(to.vertices.size());
            to.vertices.insert(to.vertices.end(), from.vertices.begin(), from.vertices.end());
            for (const fruit::Face& f : from.faces) to.faces.push_back({f[0] + base, f[1] + base, f[2] + base});
        }
}

RiskLayerHeat heatRiskLayer(const fruit::StabilityResult& res, int layer, const LayerImage& grid, HeatOf of) {
    RiskLayerHeat out;
    if (layer < 0 || static_cast<std::size_t>(layer) >= res.in_layer.size() || grid.w <= 0 || grid.h <= 0) return out;
    const auto L = static_cast<std::size_t>(layer);
    fruit::Paths by_step[kHeatSteps];
    for (int id : res.in_layer[L]) {
        if (!res.parts[static_cast<std::size_t>(id)].model) continue;
        const double pc = islandChance(res, id, of);
        const int st = heatStep(pc);
        const fruit::Paths ps = res.where[static_cast<std::size_t>(id)].shape->toPaths();
        by_step[st].insert(by_step[st].end(), ps.begin(), ps.end());
        // Its skins, where they are warmer than it (drawn over it).
        if (of != HeatOf::Lost || L >= res.skins.size()) continue;
        for (const fruit::SkinBand& s : res.skins[L]) {
            if (s.part != id) continue;
            const int sb = heatStep(withSkin(pc, s.chance));
            if (sb > st) by_step[sb].insert(by_step[sb].end(), s.region.begin(), s.region.end());
        }
    }
    fruit::RasterGrid g;
    g.width = grid.w;
    g.height = grid.h;
    g.px = grid.px;
    g.py = grid.py;
    g.x_left = grid.x_left;
    g.y_top = grid.y_top;
    for (int s = 0; s < kHeatSteps; ++s) {
        if (by_step[s].empty()) continue;
        out.cover[s] = fruit::rasterizeDense(fruit::unionNonZero(by_step[s]), g, 16);
        out.any = true;
    }
    return out;
}

std::vector<float> heatByLayer(const fruit::StabilityResult& res, HeatOf of) {
    std::vector<float> out(res.in_layer.size(), 0.0f);
    for (std::size_t L = 0; L < res.in_layer.size(); ++L) {
        double worst = 0;
        for (int id : res.in_layer[L]) {
            if (!res.parts[static_cast<std::size_t>(id)].model) continue;
            const double pc = islandChance(res, id, of);
            double skin = 0;
            if (of == HeatOf::Lost && L < res.skins.size())
                for (const fruit::SkinBand& s : res.skins[L])
                    if (s.part == id) skin = std::max(skin, static_cast<double>(s.chance));
            worst = std::max(worst, withSkin(pc, skin));
        }
        out[L] = static_cast<float>(worst);
    }
    return out;
}

// ---------------------------------------------------------------- the check

RiskReport checkRisk(const Document& doc, const SupportResult& sup, const fruit::JobControl& ctl, bool paint) {
    const auto t0 = std::chrono::steady_clock::now();
    RiskReport r;
    // The model's layers as they print (hollowed or drilled, where the Hollow
    // tool changed them), and the supports' own.
    std::shared_ptr<const fruit::LayerStack> model;
    if (sup.hollow && sup.hollow->layers && sup.hollow->layer_height == kLayerHeight) model = sup.hollow->layers;
    else
        model = std::make_shared<const fruit::LayerStack>(
            fruit::sliceModel(doc.placed(), kLayerHeight, nullptr, 0, ctl.sub(0.0, 0.1)));
    const bool with_supports = !sup.mesh.empty();
    fruit::LayerStack supports;
    if (with_supports) supports = fruit::sliceModel(sup.mesh, kLayerHeight, nullptr, 0, ctl.sub(0.1, 0.18));
    ctl.step("slice", 0.18);
    fruit::LayerStack both;
    if (with_supports) both = fruit::unionLayers(*model, supports);

    fruit::StabilityInput in;
    in.print = with_supports ? &both : model.get();
    in.model = model.get();
    if (with_supports) in.supports = &supports;
    for (const fruit::Head& hd : sup.tree.heads) in.tips.push_back({hd.contact, hd.spec.tip_diameter});
    // A support that stands on the model does so on a tip turned upside down;
    // a stub (where no head fits) is as wide as its tip at both ends.
    for (const fruit::Pillar& p : sup.tree.pillars)
        if (p.on_model) in.tips.push_back({p.anchor_contact, p.foot_tip, true});
    for (const fruit::Rod& s : sup.tree.stubs) in.tips.push_back({s.b, s.diameter, true});

    // What a tip holds is the resin's: the Hold slider changes how much of it
    // the planner asks of each tip, not how strong a tip is.
    SupportChoices choices = sup.choices;
    choices.hold = kHoldDefault;
    fruit::SupportConfig planner = supportConfig(doc.placement(), choices);
    // With no raft: what the bottom layers print of the model stands on the plate, as these supports were made for.
    planner.plate_hold_height = sup.cfg.plate_hold_height;
    const fruit::StabilityConfig cfg = fruit::stabilityConfig(planner);
    r.borrowed = cfg.capacity.table.empty();
    ctl.step("stress", 0.2);
    auto res = std::make_shared<fruit::StabilityResult>(fruit::analyzeStability(in, cfg, ctl.sub(0.2, 0.9)));
    findRiskPlaces(*res, cfg.usable_fraction, r);
    r.cups = res->stats.cup_layers > 0;
    if (paint) {
        ctl.step("paint", 0.9);
        paintRisk(doc.placed(), *res, r.paint, ctl);
        paintHeat(doc.placed(), *res, HeatOf::Lost, r.heat[0], ctl);
        paintHeat(doc.placed(), *res, HeatOf::Breaks, r.heat[1], ctl);
    }
    r.layer_heat[0] = heatByLayer(*res, HeatOf::Lost);
    r.layer_heat[1] = heatByLayer(*res, HeatOf::Breaks);
    r.result = std::move(res);
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ctl.report("paint", 1.0);
    return r;
}

// ---------------------------------------------------------------- the loop

namespace {

// The closeness at which a joint has chance `p` of letting go (breakChance turned round).
double closenessFor(double p, double steepness) { return std::pow(-std::log2(1 - p), 1.0 / std::max(steepness, 1.0)); }

// Down-facing face found by looking along -n from just off `at` (printer coordinates), within `dz` of its height.
std::optional<fruit::RayHit> faceAt(const fruit::AabbTree& tree, const fruit::TriangleMesh& mesh, const fruit::Vec3& at,
                                    const fruit::Vec3& n, double dz) {
    const auto hit = tree.raycast(at + n * 0.6, n * -1.0, 1.2);
    if (!hit) return std::nullopt;
    const fruit::Vec3 hn = mesh.faceNormal(static_cast<std::size_t>(hit->face));
    if (fruit::dot(hn, n) < 0.8 || hn.z > -0.2 || std::abs(hit->point.z - at.z) > dz) return std::nullopt;
    return hit;
}

// The likeliest place to fail, as a chance: a tip letting go, or any weak place the replay lists (a fresh skin
// tearing, a piece in mid-air, a thin place, a rod). A tip's closeness alone (the "how close" line) leaves out
// skins: with too few tips, the pull of a skin far from them is discounted from their load and counted as the
// skin tearing instead, so fewer tips can show a lower line and a likelier failure.
double worstChance(const fruit::StabilityResult& res) {
    double w = 0;
    for (const fruit::TipRisk& t : res.tips) w = std::max(w, t.chance);
    for (const fruit::WeakSpot& s : res.spots) w = std::max(w, s.chance);
    return w;
}

// How many failures to expect: every tip's chance, and every weak place's but the tips' own (counted already).
double expectedFailures(const fruit::StabilityResult& res) {
    double e = 0;
    for (const fruit::TipRisk& t : res.tips) e += t.chance;
    for (const fruit::WeakSpot& s : res.spots)
        if (s.cause != fruit::RiskCause::Tip) e += s.chance;
    return e;
}

}  // namespace

SupportResult lowerPeak(const Document& doc, const SupportSetup& s, JobPlan plans, const std::vector<TipEdit>& edits,
                        SupportResult r, const fruit::JobControl& ctl) {
    const auto t0 = std::chrono::steady_clock::now();
    PeakFix fix;
    fix.ran = true;
    if (r.tree.heads.empty()) {
        r.peak = fix;
        return r;
    }
    ctl.step("strengthen", 0);
    RiskReport risk = checkRisk(doc, r, ctl.silent(), false);
    fruit::SupportConfig planner = supportConfig(doc.placement(), r.choices);
    const double steep = fruit::stabilityConfig(planner).steepness;
    fix.target = closenessFor(kPeakChance, steep);
    fix.before = fix.after = risk.result->stats.worst_closeness;
    fix.chance_before = fix.chance_after = worstChance(*risk.result);
    fix.expected_before = fix.expected_after = expectedFailures(*risk.result);
    double best = fix.chance_before, best_expected = fix.expected_before;

    const fruit::TriangleMesh& mesh = doc.placed();
    const fruit::AabbTree tree(mesh);
    const double share_reach = fruit::stabilityConfig(planner).share_reach;
    std::vector<double> classes;
    for (const fruit::TipClass& c : r.cfg.tip_classes) classes.push_back(c.diameter);
    std::sort(classes.begin(), classes.end());
    std::set<std::uint64_t> made_bigger;  // a tip made bigger once gets a neighbour next time
    std::vector<fruit::Vec3> rejected;    // where an added tip only made things worse: not tried again

    auto leftOver = [&](const fruit::StabilityResult& res) {
        std::size_t n = 0;
        for (const fruit::WeakSpot& w : res.spots)
            if (w.chance > kPeakChance && (w.cause == fruit::RiskCause::Neck || w.cause == fruit::RiskCause::Bend ||
                                           w.cause == fruit::RiskCause::Support))
                ++n;
        return n;
    };

    for (int round = 0; round < kPeakRounds && best > kPeakChance; ++round) {
        ctl.check();
        ctl.report("strengthen", static_cast<double>(round) / kPeakRounds);
        const fruit::StabilityResult& res = *risk.result;
        JobPlan next = plans;
        std::size_t bigger = 0, added = 0;
        std::vector<std::pair<int, std::uint64_t>> new_ids;  // (part, id) of the tips this round adds
        // Every planned tip, to find the one a replayed tip is, and to keep new ones apart from.
        struct Ref { int part; std::size_t i; };
        auto nearestPlanned = [&](const fruit::Vec3& at, double reach) -> std::optional<Ref> {
            std::optional<Ref> out;
            double d_best = reach;
            for (auto& [part, plan] : next)
                for (std::size_t i = 0; i < plan.points.size(); ++i) {
                    const fruit::Vec3& q = plan.points[i].pos;
                    if (std::abs(q.z - at.z) > 0.3) continue;
                    const double d = std::hypot(q.x - at.x, q.y - at.y);
                    if (d < d_best) d_best = d, out = Ref{part, i};
                }
            return out;
        };
        auto roomFrom = [&](const fruit::Vec3& at) {
            double d_min = 1e9;
            for (const auto& [part, plan] : next)
                for (const fruit::SupportPoint& q : plan.points)
                    if (std::abs(q.pos.z - at.z) < 2.0) d_min = std::min(d_min, fruit::distance(q.pos, at));
            return d_min;
        };
        auto addAt = [&](int part, fruit::SupportPoint q) {
            q.pinned = true;
            q.id = fruit::stablePointId(q.pos, q.category);
            next[part].points.push_back(q);
            new_ids.push_back({part, q.id});
            ++added;
        };

        // The tips likelier than 1 in 100 to let go, worst first: each followed from the replay to the head the
        // tree built (the replay's tips start with the heads, in order) and from there to its planned tip.
        std::map<std::uint64_t, Ref> by_id;
        for (auto& [part, plan] : next)
            for (std::size_t i = 0; i < plan.points.size(); ++i) by_id[plan.points[i].id] = Ref{part, i};
        std::vector<const fruit::TipRisk*> over;
        for (const fruit::TipRisk& t : res.tips)
            if (!t.foot && t.closeness > fix.target && t.contact >= 0 && static_cast<std::size_t>(t.contact) < r.tree.heads.size())
                over.push_back(&t);
        std::sort(over.begin(), over.end(), [](const auto* a, const auto* b) { return a->closeness > b->closeness; });
        for (const fruit::TipRisk* t : over) {
            const fruit::Head& h = r.tree.heads[static_cast<std::size_t>(t->contact)];
            if (h.point < 0 || static_cast<std::size_t>(h.point) >= r.points.size()) continue;
            const auto it = by_id.find(r.points[static_cast<std::size_t>(h.point)].id);
            if (it == by_id.end()) continue;  // a tip of yours, not the planner's
            const Ref ref = it->second;
            fruit::SupportPoint& p = next[ref.part].points[ref.i];
            if (p.category == fruit::PointCategory::Manual) continue;
            // A size bigger where the head stands (moved there by the tree, the planned tip goes there too), where
            // the bigger contact still sits on that face.
            if (!made_bigger.count(p.id)) {
                const fruit::ClosestPoint at = tree.closest(h.contact);
                const fruit::Vec3 n = at.face >= 0 ? mesh.faceNormal(static_cast<std::size_t>(at.face)) : fruit::normalized(p.normal);
                const auto up = std::upper_bound(classes.begin(), classes.end(), h.spec.tip_diameter + 1e-9);
                if (up != classes.end() && n.z < -0.2) {
                    const fruit::Vec3 a = fruit::normalized(std::abs(n.z) < 0.9 ? fruit::cross(n, {0, 0, 1}) : fruit::cross(n, {1, 0, 0}));
                    const fruit::Vec3 b = fruit::cross(n, a);
                    bool fits = true;
                    for (int k = 0; k < 8 && fits; ++k) {
                        const double ang = k * 3.14159265358979323846 / 4;
                        fits = faceAt(tree, mesh, h.contact + (a * std::cos(ang) + b * std::sin(ang)) * (*up / 2 + 0.05), n, 0.05).has_value();
                    }
                    if (fits) {
                        made_bigger.insert(p.id);
                        if (h.moved > 0) p.pos = h.contact, p.normal = n;
                        p.tip_diameter = *up;
                        p.enlarged = true;
                        ++bigger;
                        continue;
                    }
                }
            }
            // A neighbour where the pull comes from. Most of its load from thin, fresh model near it
            // (TipRisk::near): under that, within the reach over which the replay shares a patch's pull, nearest
            // the layer it pulls at, as far from the tips there as it can. Most of it its share of the piece it
            // holds: under that piece (as the replay outlines it), as near this tip as there is room for.
            const bool patch = t->near > 0.5 * t->load && t->at_layer >= 0;
            const double z_pull = t->at_layer >= 0 ? (t->at_layer + 0.5) * res.layer_height : h.contact.z;
            const double reach = patch ? share_reach : 3.0;
            const double z_hi = patch ? z_pull + 0.1 : h.contact.z + 1.0;
            // Its share of a piece: only spots under that piece, as the replay outlines it at the tip's layer.
            const fruit::PrintPart* piece = nullptr;
            if (!patch) {
                const auto L = static_cast<std::size_t>(std::max(0.0, std::floor(h.contact.z / res.layer_height + 0.5)));
                for (std::size_t l = L; l < L + 3 && !piece; ++l) {
                    const int id = res.partAt(l, h.contact.xy());
                    if (id >= 0 && res.parts[static_cast<std::size_t>(id)].model) piece = &res.where[static_cast<std::size_t>(id)];
                }
            }
            std::optional<fruit::RayHit> best_hit;
            double best_score = -1e9;
            for (double rad = 0.6; rad <= reach + 1e-9; rad += 0.3)
                for (int k = 0; k < 12; ++k) {
                    const double ang = k * 3.14159265358979323846 / 6;
                    const fruit::Vec3 from{h.contact.x + rad * std::cos(ang), h.contact.y + rad * std::sin(ang), h.contact.z - 0.3};
                    if (piece && !piece->shape->contains(from.xy())) continue;
                    const auto hit = tree.raycast(from, {0, 0, 1}, z_hi - from.z);
                    if (!hit) continue;
                    const fruit::Vec3 hn = mesh.faceNormal(static_cast<std::size_t>(hit->face));
                    if (hn.z > -0.2 || hit->point.z < h.contact.z - 0.15) continue;
                    const double room = std::min(roomFrom(hit->point), fruit::distance(hit->point, h.contact));
                    if (room < 0.5) continue;
                    bool tried_here = false;
                    for (const fruit::Vec3& x : rejected) tried_here = tried_here || fruit::distance(x, hit->point) < 0.5;
                    if (tried_here) continue;
                    // A patch: as far from the tips there as it can, near the layer it pulls at. A share of the
                    // piece: right beside this tip, which is what takes some of its load.
                    const double score = patch ? std::min(room, 1.5) - 0.5 * std::abs(hit->point.z - z_pull)
                                               : -fruit::distance(hit->point, h.contact);
                    if (score > best_score) best_score = score, best_hit = hit;
                }
            if (!best_hit) continue;
            fruit::SupportPoint q = p;
            q.pos = best_hit->point;
            q.normal = mesh.faceNormal(static_cast<std::size_t>(best_hit->face));
            q.category = fruit::PointCategory::Capacity;
            q.tip_diameter = h.spec.tip_diameter;
            q.enlarged = false;
            addAt(ref.part, q);
        }
        // A fresh skin likelier than 1 in 100 to tear: tips over it, each at the point of it furthest from any
        // tip, until none of it is further than the planner's skin reach from one (or 20 have gone in).
        const double skin_reach = r.cfg.skin_reach > 0 ? r.cfg.skin_reach : 1.6;
        for (std::size_t L = 0; L < res.skins.size(); ++L)
            for (const fruit::SkinBand& band : res.skins[L]) {
                if (band.chance <= kPeakChance || band.region.empty()) continue;
                const double z = (static_cast<double>(L) + 0.5) * res.layer_height;
                const fruit::BBox2 box = fruit::bbox(band.region);
                std::vector<fruit::Vec2> near_tips;
                for (const auto& [part, plan] : next)
                    for (const fruit::SupportPoint& q : plan.points)
                        if (std::abs(q.pos.z - z) < 1.0 && q.pos.x > box.min.x - skin_reach && q.pos.x < box.max.x + skin_reach &&
                            q.pos.y > box.min.y - skin_reach && q.pos.y < box.max.y + skin_reach)
                            near_tips.push_back(q.pos.xy());
                for (int k = 0; k < 20; ++k) {
                    double far = 0;
                    fruit::Vec2 at;
                    for (double y = box.min.y; y <= box.max.y; y += 0.4)
                        for (double x = box.min.x; x <= box.max.x; x += 0.4) {
                            bool inside = false;
                            for (const fruit::Path& path : band.region)
                                inside = inside || Clipper2Lib::PointInPolygon(fruit::toPoint({x, y}), path) != Clipper2Lib::PointInPolygonResult::IsOutside;
                            if (!inside) continue;
                            double d = 1e9;
                            for (const fruit::Vec2& t : near_tips) d = std::min(d, fruit::norm(t - fruit::Vec2{x, y}));
                            if (d > far) far = d, at = {x, y};
                        }
                    if (far <= skin_reach) break;
                    near_tips.push_back(at);  // counted as covered, whether a tip goes in or not
                    const auto hit = tree.raycast({at.x, at.y, z - 0.6}, {0, 0, 1}, 0.8);
                    if (!hit || mesh.faceNormal(static_cast<std::size_t>(hit->face)).z > -0.2) continue;
                    const auto ref = nearestPlanned(hit->point, 1e9);
                    if (!ref) continue;
                    fruit::SupportPoint q = next[ref->part].points[ref->i];
                    q.pos = hit->point;
                    q.normal = mesh.faceNormal(static_cast<std::size_t>(hit->face));
                    q.category = fruit::PointCategory::Overhang;
                    q.layer = static_cast<int>(L);
                    q.enlarged = false;
                    addAt(ref->part, q);
                }
            }
        // A piece in mid-air short of hold: a tip right there.
        for (const fruit::WeakSpot& w : res.spots) {
            if (w.chance <= kPeakChance || w.cause != fruit::RiskCause::Unheld) continue;
            const auto hit = faceAt(tree, mesh, w.at, {0, 0, -1}, 0.5);
            if (!hit || roomFrom(hit->point) < 0.5) continue;
            const auto ref = nearestPlanned(hit->point, 1e9);
            if (!ref) continue;
            fruit::SupportPoint q = next[ref->part].points[ref->i];
            q.pos = hit->point;
            q.normal = mesh.faceNormal(static_cast<std::size_t>(hit->face));
            q.category = fruit::PointCategory::Island;
            q.layer = w.layer;
            q.enlarged = false;
            addAt(ref->part, q);
        }
        if (bigger + added == 0) break;
        // Built and replayed. Worse? The tips it added that are now weaker than the weakest place was are taken
        // out (and their spots not tried again), and it is built once more without them.
        SupportResult tried;
        RiskReport again;
        double worst = 0, expected = 0;
        bool better = false;
        for (int attempt = 0; attempt < 2; ++attempt) {
            tried = finishWithEdits(doc, s, next, edits, ctl.silent());
            again = checkRisk(doc, tried, ctl.silent(), false);
            worst = worstChance(*again.result);
            expected = expectedFailures(*again.result);
            // Kept when the likeliest failure comes down, or, no likelier, fewer failures are to be expected.
            better = worst < 0.95 * best || (worst <= 1.05 * best && expected < 0.9 * best_expected);
            if (better || new_ids.empty()) break;
            std::map<std::uint64_t, double> by_point;
            for (const fruit::TipRisk& t : again.result->tips)
                if (t.contact >= 0 && static_cast<std::size_t>(t.contact) < tried.tree.heads.size()) {
                    const fruit::Head& h = tried.tree.heads[static_cast<std::size_t>(t.contact)];
                    if (h.point >= 0 && static_cast<std::size_t>(h.point) < tried.points.size())
                        by_point[tried.points[static_cast<std::size_t>(h.point)].id] = t.closeness;
                }
            std::size_t dropped = 0;
            for (const auto& [part, id] : new_ids) {
                const auto c = by_point.find(id);
                if (c == by_point.end() || fruit::breakChance(c->second, steep) < best) continue;
                auto& pts = next[part].points;
                for (auto it = pts.begin(); it != pts.end(); ++it)
                    if (it->id == id) {
                        rejected.push_back(it->pos);
                        pts.erase(it);
                        ++dropped;
                        break;
                    }
            }
            if (dropped == 0) break;
            added -= std::min(added, dropped);
        }
        if (!better) break;
        r = std::move(tried);
        plans = std::move(next);
        risk = std::move(again);
        best = worst;
        best_expected = expected;
        fix.after = risk.result->stats.worst_closeness;
        fix.chance_after = worst;
        fix.expected_after = expected;
        ++fix.rounds;
        fix.enlarged += bigger;
        fix.added += added;
    }
    fix.left = leftOver(*risk.result);
    fix.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    r.peak = fix;
    ctl.report("strengthen", 1.0);
    return r;
}

std::string peakLine(const PeakFix& f) {
    if (!f.ran) return "";
    const auto oneIn = [](double c) { return c > 0 ? strf("1 in %.0f", std::max(1.0, std::round(1 / c))) : std::string("none"); };
    std::string out;
    if (f.enlarged + f.added > 0) {
        std::string what;
        if (f.added) what = strf("%zu %s added", f.added, f.added == 1 ? "tip" : "tips");
        if (f.enlarged) what += (what.empty() ? "" : ", ") + strf("%zu %s a size bigger", f.enlarged, f.enlarged == 1 ? "tip" : "tips");
        out = "After replaying the print: " + what + ". The likeliest place to fail went from " + oneIn(f.chance_before) + " to " +
              oneIn(f.chance_after);
        out += f.chance_after <= kPeakChance + 1e-12 ? ", under 1 in 100."
                                                     : "; under 1 in 100 is the aim, and more tips here didn't get it lower.";
    } else if (f.chance_before <= kPeakChance + 1e-12) {
        out = "After replaying the print: no place is likelier than 1 in 100 to fail.";
    } else {
        out = "After replaying the print: the likeliest place to fail is " + oneIn(f.chance_before) +
              ", and more or bigger tips here didn't make it less likely.";
    }
    if (f.left > 0)
        out += strf(" %zu thin %s in the model %s more tips don't fix: see the places that may not print.", f.left,
                    f.left == 1 ? "place" : "places", f.left == 1 ? "is one" : "are ones");
    return out;
}

}  // namespace gui
