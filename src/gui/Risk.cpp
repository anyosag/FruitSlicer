// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Risk.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "Document.h"
#include "Hollows.h"
#include "Layers.h"
#include "Orient.h"
#include "Supports.h"
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
        case RiskPlace::Kind::Tip: {
            static const char* w[3] = {"May let go", "Likely to let go", "Very likely to let go"};
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
                // One that lets go while the model stays held is one of "the tips", below.
                if (!loses) continue;
                p.kind = RiskPlace::Kind::Tip;
                p.title = "Tip with too much on it" + up;
                p.holds = amount(s.load) + " mm² of layer on a " + fmt("%.1f", tipWidth(s.area)) +
                          " mm tip, trusted with " + amount(s.holds * usable);
                p.why = s.near >= 0.5 * s.load ? "the thin, fresh model next to it pulls on it alone"
                                               : "its share of the whole piece is more than a tip of its size is trusted with";
                p.if_goes = "about " + amount(s.lost_volume) + " mm³ of the model goes with it";
                break;
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

    // The tips. Every one that may let go is coloured in the view; those the
    // model stays held without are one place together.
    RiskPlace group;
    group.kind = RiskPlace::Kind::Tips;
    double lo = 1e300, hi = 0;  // what they carry, in multiples of what each is trusted with
    int first = -1, last = -1;
    const fruit::TipRisk* worst = nullptr;
    for (const fruit::TipRisk& t : res.tips) {
        const RiskBand band = riskBand(t.chance);
        if (band == RiskBand::None) continue;
        // A tip that only grazes the model, with nothing going with it: it may
        // snap and nothing comes of it (the replay does not list it either).
        if (t.area < fruit::kSliverContact && t.lost_volume < kRiskLosesModel) continue;
        out.tips.push_back({t.at, tipWidth(t.area), band});
        if (t.lost_volume >= kRiskLosesModel) continue;
        ++group.tips;
        if (!worst || t.chance > worst->chance) worst = &t;
        const double trusted = t.holds * usable;
        if (trusted > 0) {
            lo = std::min(lo, t.load / trusted);
            hi = std::max(hi, t.load / trusted);
        }
        first = first < 0 ? t.at_layer : std::min(first, t.at_layer);
        last = std::max(last, t.at_layer);
    }
    if (worst) {
        group.band = riskBand(worst->chance);
        group.chance = worst->chance;
        group.at = worst->at;
        group.layer = worst->at_layer;
        const bool one = group.tips == 1;
        group.title = one ? "1 tip may let go" : fmt("%.0f tips may let go", group.tips);
        group.brief = std::string(phrase(group.kind, group.band)) + " · the model stays held";
        const std::string times = hi - lo < 0.05 ? fmt("about %.1f times", hi) : fmt("%.1f to %.1f times", lo, hi);
        group.holds = (one ? "it carries " : "each carries ") + times + " what a tip of its size is trusted with";
        group.when = first == last ? whenText(std::max(0, first), h)
                                   : fmt("while layers %.0f to %.0f print", first + 1.0, last + 1.0);
        group.why = "the layers over them pull harder than these tips are trusted to hold";
        group.if_goes = one ? "the tips around it take its load, and the model stays held"
                            : "the tips around them take their load, and the model stays held";
        // (For the headline: the same numbers, as whole sentences.)
        group.note = (one ? "It carries " : "They carry ") + times +
                     (one ? " what it is trusted with. The model stays held without it."
                          : " what they are trusted with. The model stays held without them.");
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
    if (r.places_found == 1 && r.places.front().kind == RiskPlace::Kind::Tips) {
        hl.tips_only = true;
        hl.title = r.places.front().title;
        hl.detail = r.places.front().note;
        return hl;
    }
    hl.title = r.places_found == 1 ? "1 place may not print" : fmt("%.0f places may not print", static_cast<double>(r.places_found));
    hl.detail = replayed + " Pick a place to see it on the model.";
    if (r.places_found > r.places.size())
        hl.detail += fmt(" The first %.0f are listed.", static_cast<double>(r.places.size()));
    return hl;
}

std::string riskStrengthsNote(const RiskReport& r) {
    return r.borrowed ? "Strengths are borrowed, not measured for this resin."
                      : "Tip strengths are this resin's, from its tips plate. How strong the model is between its own "
                        "layers is taken to be the same: not measured.";
}

// ---------------------------------------------------------------- the paint

namespace {

struct Poly {
    fruit::Vec3 v[6];
    int n = 0;
};

// What of `in` lies at z >= z0 (keep_above) or z <= z0.
Poly clipZ(const Poly& in, double z0, bool keep_above) {
    Poly out;
    for (int i = 0; i < in.n; ++i) {
        const fruit::Vec3& a = in.v[i];
        const fruit::Vec3& b = in.v[(i + 1) % in.n];
        const bool ia = keep_above ? a.z >= z0 : a.z <= z0, ib = keep_above ? b.z >= z0 : b.z <= z0;
        if (ia && out.n < 6) out.v[out.n++] = a;
        if (ia != ib && out.n < 6) {
            const double t = (z0 - a.z) / (b.z - a.z);
            out.v[out.n++] = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, z0};
        }
    }
    return out;
}

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

// ---------------------------------------------------------------- the check

RiskReport checkRisk(const Document& doc, const SupportResult& sup, const fruit::JobControl& ctl) {
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
    const fruit::StabilityConfig cfg = fruit::stabilityConfig(supportConfig(doc.placement(), choices));
    r.borrowed = cfg.capacity.table.empty();
    ctl.step("stress", 0.2);
    auto res = std::make_shared<fruit::StabilityResult>(fruit::analyzeStability(in, cfg, ctl.sub(0.2, 0.9)));
    findRiskPlaces(*res, cfg.usable_fraction, r);
    ctl.step("paint", 0.9);
    paintRisk(doc.placed(), *res, r.paint, ctl);
    r.result = std::move(res);
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ctl.report("paint", 1.0);
    return r;
}

}  // namespace gui
