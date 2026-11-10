// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Supports.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>

#include "Document.h"
#include "Format.h"
#include "Orient.h"
#include "support/CosmeticField.h"
#include "support/PointGenerator.h"
#include "support/SupportSettings.h"

namespace gui {

namespace {

std::string fmt(const char* f, double a = 0, double b = 0) {
    char s[200];
    std::snprintf(s, sizeof s, f, a, b);
    return s;
}

void resolve(const fruit::Placement& p, const SupportChoices& c, fruit::SupportConfig& cfg, fruit::TreeConfig& tc) {
    fruit::SupportChoices ch;
    ch.layer_height = kLayerHeight;
    ch.on_plate = p.on_plate;
    ch.density = c.density;
    ch.hinge = c.hinge;
    // Built-in values, then the resin's measured ones, then the job's choices.
    fruit::resolveSettings(c.profile.is_object() ? &c.profile : nullptr, ch, cfg, &tc);
    cfg.default_tip = nearestTipClass(cfg, c.usual_tip);
    tc.merge_pillars = c.merge;
    applyAdvanced(c.advanced, cfg, tc);
}

using C = fruit::SupportConfig;
using T = fruit::TreeConfig;
using X = AdvancedContext;

std::vector<AdvancedSetting> makeAdvanced() {
    using K = AdvancedSetting::Kind;
    using S = AdvancedSetting::Source;
    std::vector<AdvancedSetting> v;
    // ---- Tips ----
    v.push_back({.group = "Tips", .label = "Usual tip size", .kind = K::TipClass, .source = S::Default,
                 .fields = {{kUsualTipKey, "", "", 1, 3, 0, [](const C& c, const T&) { return double(c.default_tip); },
                             [](C& c, T&, double x) { c.default_tip = int(std::lround(x)); }}},
                 .help = [](const X&) { return std::string("Most tips use this size."); }});
    v.push_back({.group = "Tips", .label = "Tips at low points", .kind = K::TipClass, .source = S::Default,
                 .fields = {{"minima_tip", "", "", 0, 3, 0, [](const C& c, const T&) { return double(c.minima_tip); },
                             [](C& c, T&, double x) { c.minima_tip = int(std::lround(x)); }}},
                 .help = [](const X&) {
                     return std::string("Bigger where a part starts in mid-air, unless the spot is too small for it.");
                 }});
    v.push_back({.group = "Tips", .label = "Tips near edges and visible faces", .kind = K::TipClass,
                 .source = S::Default,
                 .fields = {{"edge_tip", "", "", 0, 3, 0, [](const C& c, const T&) { return double(c.edge_tip); },
                             [](C& c, T&, double x) { c.edge_tip = int(std::lround(x)); }}},
                 .help = [](const X&) { return std::string("Smaller marks where they would show."); }});
    v.push_back({.group = "Tips", .label = "How much a tip holds", .kind = K::Number, .source = S::Borrowed,
                 // The value in use for a 0.4 mm tip. With a resin's measured table,
                 // setting it scales the whole table (its shape across sizes stays).
                 .fields = {{"capacity", "", "mm²", 2, 40, 1,
                             [](const C& c, const T&) {
                                 return c.capacity.capacity(c.capacity.reference_diameter) *
                                        std::max(c.capacity.safety, 1e-6);
                             },
                             [](C& c, T&, double x) {
                                 if (c.capacity.table.empty()) {
                                     c.capacity.reference_capacity = x;
                                     return;
                                 }
                                 const double now = c.capacity.capacity(c.capacity.reference_diameter) *
                                                    std::max(c.capacity.safety, 1e-6);
                                 if (now <= 0) return;
                                 for (auto& [d, cap] : c.capacity.table) cap *= x / now;
                             }}},
                 .help = [](const X& x) {
                     if (!x.cfg.capacity.table.empty())
                         return strf("Layer area one %.1f mm tip can hold as it peels, from your resin's tips plate: "
                                     "%.1f, %.1f and %.1f mm² for 0.2, 0.3 and 0.6 mm. A value here scales them all.",
                                     x.cfg.capacity.reference_diameter, x.cfg.capacity.capacity(0.2),
                                     x.cfg.capacity.capacity(0.3), x.cfg.capacity.capacity(0.6));
                     return strf("Layer area one %.1f mm tip can hold as it peels. Scales with tip area: %.1f, %.1f "
                                 "and %.1f mm² for 0.2, 0.3 and 0.6 mm.",
                                 x.cfg.capacity.reference_diameter, x.cfg.capacity.capacity(0.2),
                                 x.cfg.capacity.capacity(0.3), x.cfg.capacity.capacity(0.6));
                 }});
    v.push_back({.group = "Tips", .label = "Contact depth", .kind = K::Fixed, .source = S::Default,
                 .help = [](const X& x) {
                     if (!x.tc.penetration_table.empty())
                         return std::string("How far the tip reaches into the model, as your resin's tips plate "
                                            "held best for each size.");
                     return strf("How far the tip reaches into the model, kept between %.2f and %.2f mm.",
                                 fruit::tipPenetration(0.0, x.tc), fruit::tipPenetration(10.0, x.tc));
                 },
                 .text = [](const X& x) {
                     if (x.tc.penetration_table.empty()) return std::string("½ the tip size");
                     const double lo = fruit::tipPenetration(0.0, x.tc), hi = fruit::tipPenetration(10.0, x.tc);
                     return std::abs(hi - lo) < 1e-9 ? strf("%.2f mm", lo) : strf("%.2f–%.2f mm", lo, hi);
                 }});
    // ---- Where tips go ----
    v.push_back({.group = "Where tips go", .label = "Faces that need tips", .kind = K::Number, .source = S::Borrowed,
                 .fields = {{"critical_angle", "lean >", "°", 20, 70, 0,
                             [](const C& c, const T&) { return c.critical_angle_deg; },
                             [](C& c, T&, double x) { c.critical_angle_deg = x; }}},
                 .help = [](const X&) { return std::string("Faces closer to flat than this are held from below."); }});
    v.push_back({.group = "Where tips go", .label = "Short ledges", .kind = K::Number, .source = S::Default,
                 .fields = {{"ledge", "up to", "mm", 0, 3, 1,
                             [](const C& c, const T&) { return c.self_supported_reach; },
                             [](C& c, T&, double x) { c.self_supported_reach = x; }}},
                 .help = [](const X&) { return std::string("A ledge this short holds itself without a tip."); }});
    v.push_back({.group = "Where tips go", .label = "Tip reach", .kind = K::Fixed, .source = S::Borrowed,
                 .help = [](const X& x) {
                     return strf("Distance one tip covers: %.1f mm right above it, growing to %.1f mm 40 mm higher. "
                                 "Tip spacing on the Supports panel brings tips closer.",
                                 x.cfg.coverage.radius(0), x.cfg.coverage.radius(40));
                 },
                 .text = [](const X& x) {
                     return strf("%.1f → %.1f mm", x.cfg.coverage.radius(0), x.cfg.coverage.radius(40));
                 }});
    v.push_back({.group = "Where tips go", .label = "Spots in mid-air", .kind = K::Number, .source = S::Borrowed,
                 .fields = {{"island_one", "1 tip ≤", "", 0.5, 5, 1,
                             [](const C& c, const T&) { return c.island_one_tip_length; },
                             [](C& c, T&, double x) { c.island_one_tip_length = x; }},
                            {"island_two", "· 2 ≤", "mm", 2, 20, 1,
                             [](const C& c, const T&) { return c.island_two_tip_length; },
                             [](C& c, T&, double x) { c.island_two_tip_length = x; }}},
                 .help = [](const X& x) {
                     return strf("One tip up to %.1f mm across, two up to %.1f mm, at least %d above that.",
                                 x.cfg.island_one_tip_length, std::max(x.cfg.island_one_tip_length,
                                                                       x.cfg.island_two_tip_length),
                                 x.cfg.large_island_min_tips);
                 }});
    v.push_back({.group = "Where tips go", .label = "Tiny specks", .kind = K::Toggle, .source = S::Assumption,
                 .fields = {{"tiny_left_alone", "", "", 0, 1, 0,
                             [](const C& c, const T&) { return c.tiny_island_area > 0 ? 1.0 : 0.0; },
                             [](C& c, T&, double x) {
                                 if (x < 0.5) c.tiny_island_area = c.absorb_island_area = 0;
                             }}},
                 .help = [](const X& x) {
                     if (x.cfg.tiny_island_area <= 0) return std::string("Every speck gets a tip, however small.");
                     return strf("Under %.2f mm² and joining the model within %d layers, or under %.2f mm² and "
                                 "joining it on the next layer: a tip would leave a bigger mark.",
                                 x.cfg.tiny_island_area, x.cfg.tiny_island_merge_layers, x.cfg.absorb_island_area);
                 },
                 .text = [](const X&) { return std::string("left alone"); },
                 .off_text = "given tips"});
    v.push_back({.group = "Where tips go", .label = "Side tips", .kind = K::Number, .source = S::Default,
                 .fields = {{"side_tips", "over", "mm tall", 1, 50, 0,
                             [](const C& c, const T&) { return c.stabilize_min_height; },
                             [](C& c, T&, double x) { c.stabilize_min_height = x; }}},
                 .help = [](const X& x) {
                     return strf("Added to tall, thin parts (%.0f × taller than their tips are wide) so they don't "
                                 "wobble.",
                                 x.cfg.slenderness);
                 }});
    // ---- Keeping marks off ----
    v.push_back({.group = "Keeping marks off", .label = "Visible faces and keep-out boxes", .kind = K::Number,
                 .source = S::Default,
                 .fields = {{"visible_cost", "", "× the cost", 1, 20, 0,
                             [](const C& c, const T&) { return 1 + c.cosmetic_weight; },
                             [](C& c, T&, double x) { c.cosmetic_weight = x - 1; }}},
                 .help = [](const X&) { return std::string("Tips go there only when nothing else works."); }});
    v.push_back({.group = "Keeping marks off", .label = "Sharp edges", .kind = K::Number, .source = S::Default,
                 .fields = {{"sharp_edges", "sharper than", "°", 10, 90, 0,
                             [](const C& c, const T&) { return c.sharp_edge_angle_deg; },
                             [](C& c, T&, double x) { c.sharp_edge_angle_deg = x; }}},
                 .help = [](const X& x) {
                     const double k = 1 + x.cfg.edge_weight;
                     return k == 2 ? std::string("Tips near them cost twice as much, because a mark on an edge shows.")
                                   : strf("Tips near them cost %g times as much, because a mark on an edge shows.", k);
                 }});
    // ---- Heads ----
    v.push_back({.group = "Heads", .label = "Head tilt", .kind = K::Number, .source = S::Default,
                 .fields = {{"head_tilt", "up to", "°", 0, 60, 0,
                             [](const C&, const T& t) { return t.max_head_tilt_deg; },
                             [](C&, T& t, double x) { t.max_head_tilt_deg = x; }}},
                 .help = [](const X&) {
                     return std::string("Heads lean to meet sloped faces, never more than this from vertical.");
                 }});
    v.push_back({.group = "Heads", .label = "Head length", .kind = K::Number, .source = S::Default,
                 .fields = {{"head_length", "", "mm", 0.4, 3, 1, [](const C&, const T& t) { return t.head_length; },
                             [](C&, T& t, double x) { t.head_length = x; }}},
                 .help = [](const X&) { return std::string("From the tip to where it joins the support."); }});
    v.push_back({.group = "Heads", .label = "Gap to the model", .kind = K::Number, .source = S::Default,
                 .fields = {{"gap", "", "mm", 0.05, 1, 2, [](const C&, const T& t) { return t.clearance; },
                             [](C&, T& t, double x) { t.clearance = x; }}},
                 .help = [](const X&) {
                     return std::string("Supports keep this clear of the model everywhere except at the tip.");
                 }});
    // ---- Pillars and trunks ----
    v.push_back({.group = "Pillars and trunks", .label = "Heads per pillar", .kind = K::Number, .source = S::Default,
                 .fields = {{"heads_per_pillar", "up to", "", 1, 6, 0,
                             [](const C&, const T& t) { return double(t.max_heads_per_pillar); },
                             [](C&, T& t, double x) { t.max_heads_per_pillar = int(std::lround(x)); }}},
                 .help = [](const X& x) {
                     return strf("Nearby heads share a pillar through 45° bridges up to %.1f mm long.",
                                 x.tc.max_bridge_length);
                 }});
    v.push_back({.group = "Pillars and trunks", .label = "Finding a way down", .kind = K::Number,
                 .source = S::Default,
                 .fields = {{"route", "up to", "mm", 0, 40, 0, [](const C&, const T& t) { return t.max_route_length; },
                             [](C&, T& t, double x) { t.max_route_length = x; }}},
                 .help = [](const X&) {
                     return std::string("A blocked head walks diagonally outward until it can drop to the raft.");
                 }});
    v.push_back({.group = "Pillars and trunks", .label = "Standing on the model", .kind = K::Number,
                 .source = S::Default,
                 .fields = {{"anchor_drop", "drops up to", "mm", 0, 30, 0,
                             [](const C&, const T& t) { return t.max_model_anchor_drop; },
                             [](C&, T& t, double x) { t.max_model_anchor_drop = x; }}},
                 .help = [](const X& x) {
                     if (x.tc.max_model_anchor_drop <= 0)
                         return std::string("Never: a head with no way down is left out and reported.");
                     return std::string("A last resort, always reported, because it marks an upper surface. 0 turns "
                                        "it off.");
                 }});
    v.push_back({.group = "Pillars and trunks", .label = "Merge into trunks", .kind = K::Number,
                 .source = S::Default,
                 .fields = {{"merge_distance", "within", "mm", 1, 20, 0,
                             [](const C&, const T& t) { return t.max_merge_distance; },
                             [](C&, T& t, double x) { t.max_merge_distance = x; }}},
                 .help = [](const X& x) {
                     if (!x.choices.merge) return std::string("Off: merging is turned off on the Supports panel.");
                     return strf("Up to %d tips share one trunk, never thicker than %.1f mm.", x.tc.max_tips_per_trunk,
                                 x.tc.max_trunk_diameter);
                 }});
    // ---- Braces ----
    v.push_back({.group = "Braces", .label = "Which pillars", .kind = K::Number, .source = S::Default,
                 .fields = {{"brace_height", "taller than", "mm", 5, 100, 0,
                             [](const C&, const T& t) { return t.brace_min_height; },
                             [](C&, T& t, double x) { t.brace_min_height = x; }}},
                 .help = [](const X& x) {
                     return strf("Braced to neighbours within %.0f mm, every %.0f mm of height, %.1f mm thick.",
                                 x.tc.brace_max_distance, x.tc.brace_step, x.tc.brace_diameter);
                 }});
    v.push_back({.group = "Braces", .label = "Snap-off zone", .kind = K::Number, .source = S::Default,
                 .fields = {{"snap_zone", "", "mm", 0, 10, 1, [](const C&, const T& t) { return t.flex_zone; },
                             [](C&, T& t, double x) { t.flex_zone = x; }}},
                 .help = [](const X&) {
                     return std::string("No braces just below a head, so tips flex and snap off at the model.");
                 }});
    v.push_back({.group = "Braces", .label = "Never around the model", .kind = K::Fixed, .source = S::Default,
                 .help = [](const X&) {
                     return std::string("Two pillars are never braced if the model passes between them; the "
                                        "supports could not come off.");
                 },
                 .text = [](const X&) { return std::string("always on"); }});
    // ---- Raft ----
    v.push_back({.group = "Raft", .label = "Raft thickness", .kind = K::Number, .source = S::Default,
                 .fields = {{"raft_thickness", "", "mm", 0.2, 5, 1,
                             [](const C&, const T& t) { return t.raft_thickness; },
                             [](C&, T& t, double x) { t.raft_thickness = x; }}},
                 .help = [](const X& x) {
                     return strf("At least as thick as your bottom and transition layers: %d × %.2f = %.2f mm.",
                                 x.raft_layers, x.raft_layer_height, x.raft_layers * x.raft_layer_height);
                 }});
    v.push_back({.group = "Raft", .label = "Pads", .kind = K::Number, .source = S::Default,
                 .fields = {{"pad_diameter", "", "mm base", 1, 8, 1,
                             [](const C&, const T& t) { return t.base_diameter; },
                             [](C&, T& t, double x) { t.base_diameter = x; }}},
                 .help = [](const X& x) {
                     return strf("Each trunk stands on a %.1f mm tall pad with a %.1f mm brim.", x.tc.base_height,
                                 x.tc.pad_brim);
                 }});
    v.push_back({.group = "Raft", .label = "Links", .kind = K::Number, .source = S::Default,
                 .fields = {{"link_width", "", "mm wide", 0.5, 4, 1, [](const C&, const T& t) { return t.link_width; },
                             [](C&, T& t, double x) { t.link_width = x; }}},
                 .help = [](const X&) {
                     return std::string("Flat bars join the pads, so the raft and supports come off in one piece.");
                 }});
    v.push_back({.group = "Raft", .label = "Lift", .kind = K::Fixed, .source = S::Default,
                 .help = [](const X&) {
                     return std::string("Gap between the plate and the lowest point of the model. Set in the Model "
                                        "step.");
                 },
                 .text = [](const X& x) {
                     return x.placement.on_plate ? std::string("on the plate") : strf("%.0f mm", x.placement.lift());
                 }});
    // ---- Peel side ----
    v.push_back({.group = "Peel side", .label = "Vat hinge", .kind = K::Fixed, .source = S::Default,
                 .help = [](const X&) {
                     return std::string("Which side the tilting vat hinges on, seen from the front. Set it on the "
                                        "Printer page, under Calibrate.");
                 },
                 .text = [](const X& x) { return x.hinge; }});
    v.push_back({.group = "Peel side", .label = "Extra hold away from the hinge", .kind = K::Fixed,
                 .source = S::Assumption,
                 .help = [](const X&) {
                     return std::string("Peel pulls hardest on the far side: up to +25 % more hold there once the "
                                        "hinge side is set and the image orientation confirmed. The right amount "
                                        "hasn't been measured.");
                 },
                 .text = [](const X& x) {
                     return x.cfg.peel_bias > 0 ? strf("up to +%.0f %%", x.cfg.peel_bias * 100) : std::string("off");
                 }});
    return v;
}

struct Known {
    const char* code;
    SupportProblem::Level level;
    const char* title;
    const char* body;
};

// The engine's warnings in plain words. Unknown codes keep the engine's text.
const Known kKnown[] = {
    {"uncovered", SupportProblem::Level::Blocks, "Spots with no tip in reach",
     "Some faces that need holding have no tip close enough. They could sag or come loose."},
    {"no_route", SupportProblem::Level::Review, "Tips with no way down",
     "These tips found no clear path to the raft and were left out, so the spot above them is held less."},
    {"head_blocked", SupportProblem::Level::Review, "Tips the model blocks",
     "There was no room for a support head here, so the tip was left out."},
    {"anchor_on_model", SupportProblem::Level::Review, "Supports standing on the model",
     "Where nothing reached the raft, a support stands on the model instead. It leaves a mark there."},
    {"capacity_deficit", SupportProblem::Level::Review, "Layers short of hold",
     "Some layers are bigger than their tips can hold while they print. Turning the part a little, or one more "
     "tip, usually fixes it."},
    {"tiny_islands_skipped", SupportProblem::Level::Review, "Tiny specks with no tips",
     "Each speck is so small and joins the model so soon that the layer above usually carries it. A tip there "
     "would leave a mark bigger than the speck."},
    {"suction_cup", SupportProblem::Level::Review, "Suction cups",
     "A hollow faces the film, so every layer pulls on a vacuum. Turn the model, or give the hollow a hole on "
     "the Hollow screen."},
    {"resin_trap", SupportProblem::Level::Review, "Resin traps",
     "A sealed hollow keeps liquid resin inside, and pressure builds as it closes. Drain it or fill it on the "
     "Hollow screen."},
    {"cavity_ceiling", SupportProblem::Level::Review, "Ceilings inside hollows",
     "No supports go inside a hollow, as they could never be removed, so its ceiling has to hold itself up and a "
     "flat one may sag. Hollowing the model shapes ceilings that hold; filling it prints it solid."},
    {"peak_area", SupportProblem::Level::Note, "A big layer",
     "One layer is large, so it pulls hard on the film as it peels. Tilting the model makes layers smaller."},
    {"flat_face", SupportProblem::Level::Note, "Large flat faces facing down",
     "Flat faces parallel to the film peel all at once. Tilting the model a few degrees helps."},
    {"manual_point_unattached", SupportProblem::Level::Review, "Hand-placed tips off the model",
     "A tip you placed is no longer on the surface, so it was left out."},
};

void addWarnings(const std::vector<fruit::Warning>& ws, std::map<std::string, SupportProblem>& by_code) {
    for (const fruit::Warning& w : ws) {
        SupportProblem& p = by_code[w.code];
        if (p.count++ == 0) {
            p.code = w.code;
            p.engine = w.message;
            p.where = w.where;
            p.has_where = w.where.x != 0 || w.where.y != 0 || w.where.z != 0;
            p.title = w.code;
            p.body = w.message;
            for (const Known& k : kKnown)
                if (w.code == k.code) {
                    p.level = k.level;
                    p.title = k.title;
                    p.body = k.body;
                }
        }
    }
}

}  // namespace

std::size_t SupportResult::count(SupportProblem::Level l) const {
    std::size_t n = 0;
    for (const auto& p : problems) n += p.level == l;
    return n;
}

const std::vector<AdvancedSetting>& advancedSettings() {
    static const std::vector<AdvancedSetting> rows = makeAdvanced();
    return rows;
}

void applyAdvanced(const std::map<std::string, double>& values, fruit::SupportConfig& cfg, fruit::TreeConfig& tc) {
    for (const AdvancedSetting& s : advancedSettings())
        for (const AdvancedField& f : s.fields) {
            const auto it = values.find(f.key);
            if (it == values.end() || !f.set || std::string(f.key) == kUsualTipKey) continue;
            double v = std::clamp(it->second, f.lo, f.hi);
            // A tip size by FruitSlicer's own classes: the nearest the resin has.
            if (s.kind == AdvancedSetting::Kind::TipClass)
                v = nearestTipClass(cfg, static_cast<int>(std::lround(v)));
            f.set(cfg, tc, v);
        }
}


namespace {
const std::vector<fruit::TipClass>& standardTips() {
    static const std::vector<fruit::TipClass> t = fruit::SupportConfig{}.tip_classes;
    return t;
}
}  // namespace

int nearestTipClass(const fruit::SupportConfig& cfg, int standard_class) {
    const auto& std_tips = standardTips();
    const double d = std_tips[static_cast<std::size_t>(std::clamp(standard_class, 0, int(std_tips.size()) - 1))].diameter;
    int best = 0;
    double best_err = 1e9;
    for (std::size_t i = 0; i < cfg.tip_classes.size(); ++i) {
        const double e = std::abs(cfg.tip_classes[i].diameter - d);
        if (e < best_err - 1e-12) {
            best_err = e;
            best = static_cast<int>(i);
        }
    }
    return best;
}

int standardTipClass(const fruit::SupportConfig& cfg, int cls) {
    if (cfg.tip_classes.empty()) return 0;
    const double d = cfg.tip_classes[static_cast<std::size_t>(std::clamp(cls, 0, int(cfg.tip_classes.size()) - 1))]
                         .diameter;
    const auto& std_tips = standardTips();
    int best = 0;
    double best_err = 1e9;
    for (std::size_t i = 0; i < std_tips.size(); ++i) {
        const double e = std::abs(std_tips[i].diameter - d);
        if (e < best_err - 1e-12) {
            best_err = e;
            best = static_cast<int>(i);
        }
    }
    return best;
}

int usualTipFor(const nlohmann::json& profile) {
    if (!profile.is_object() || !profile.contains("default_tip_diameter") || !profile["default_tip_diameter"].is_number())
        return SupportChoices{}.usual_tip;
    const double want = profile["default_tip_diameter"].get<double>();
    const auto& own = standardTips();
    int best = 1;  // the sizes the Supports step offers: 0.3, 0.4 and 0.6 mm
    for (int i = 2; i <= 3; ++i)
        if (std::abs(own[static_cast<std::size_t>(i)].diameter - want) <
            std::abs(own[static_cast<std::size_t>(best)].diameter - want) - 1e-12)
            best = i;
    return best;
}

const char* measuredKey(const AdvancedSetting& s) {
    static const std::pair<const char*, const char*> keys[] = {
        {"Usual tip size", "default_tip_diameter"}, {"Tips at low points", "minima_tip_diameter"},
        // With a measured table, edges get the smallest size the tips plate measured.
        {"Tips near edges and visible faces", "capacity_table"},
        {"How much a tip holds", "capacity_table"},  {"Contact depth", "penetration_table"},
        {"Faces that need tips", "critical_angle_deg"}, {"Tip reach", "coverage_curve"},
        {"Spots in mid-air", "island_one_tip_length"}};
    for (const auto& [label, key] : keys)
        if (std::string(s.label) == label) return key;
    return "";
}

bool measuredByResin(const AdvancedSetting& s, const SupportChoices& c) {
    const std::string key = measuredKey(s);
    if (key.empty() || !c.profile.is_object() || !c.profile.contains(key) || c.profile[key].is_null()) return false;
    if (key == "default_tip_diameter") {
        // The job's usual tip is set from the resin; it is the measured one while it matches.
        const double d = c.profile[key].get<double>();
        const auto& own = standardTips();
        int nearest = 0;
        for (std::size_t i = 1; i < own.size(); ++i)
            if (std::abs(own[i].diameter - d) < std::abs(own[static_cast<std::size_t>(nearest)].diameter - d) - 1e-12)
                nearest = static_cast<int>(i);
        return c.usual_tip == nearest;
    }
    return !advancedChanged(s, c);
}

bool advancedChanged(const AdvancedSetting& s, const SupportChoices& c) {
    for (const AdvancedField& f : s.fields) {
        if (std::string(f.key) == kUsualTipKey) {
            if (c.usual_tip != usualTipFor(c.profile)) return true;
        } else if (c.advanced.count(f.key)) {
            return true;
        }
    }
    return false;
}

fruit::SupportConfig supportConfig(const fruit::Placement& p, const SupportChoices& c) {
    fruit::SupportConfig cfg;
    fruit::TreeConfig tc;
    resolve(p, c, cfg, tc);
    return cfg;
}

fruit::TreeConfig treeConfig(const SupportChoices& c) {
    fruit::SupportConfig cfg;
    fruit::TreeConfig tc;
    resolve(fruit::Placement{}, c, cfg, tc);
    return tc;
}

const std::vector<std::string>& supportStages() {
    static const std::vector<std::string> s = {"Slice the model",           "Find spots that start in mid-air",
                                               "Plan tips: reach and hold", "Route supports to the raft",
                                               "Build the support shapes",  "Check for parts in mid-air"};
    return s;
}

int supportStage(const std::string& stage, double f) {
    if (f >= 0.85) return 5;
    if (f >= 0.80) return 4;
    if (f >= 0.55) return 3;
    if (stage == "islands") return 1;
    if (stage == "points") return 2;
    return 0;
}

namespace {

fruit::Vec3 unit(const fruit::Vec3& v) {
    const double n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return n > 0 ? fruit::Vec3{v.x / n, v.y / n, v.z / n} : fruit::Vec3{0, 0, -1};
}

double dist(const fruit::Vec3& a, const fruit::Vec3& b) {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

fruit::SupportPoint manualTip(const fruit::Vec3& pos, const fruit::Vec3& normal, double d) {
    fruit::SupportPoint p;
    p.pos = pos;
    p.normal = unit(normal);
    p.tip_diameter = d;
    p.category = fruit::PointCategory::Manual;
    p.pinned = true;
    p.layer = static_cast<int>(std::floor(pos.z / kLayerHeight));
    // Stable for the same place: quantised position, as the planner's ids are.
    const auto q = [](double v) { return static_cast<std::uint64_t>(std::llround(v * 1000)) & 0xFFFFF; };
    p.id = (q(pos.x) << 40) ^ (q(pos.y) << 20) ^ q(pos.z) ^ 0x4D414E00000000ULL;
    return p;
}

constexpr double kMatch = 0.05;  // mm: an edit's tip is the one this close to where it was

bool keepsOut(TipEdit::Kind k) {
    return k == TipEdit::Kind::Delete || k == TipEdit::Kind::Move || k == TipEdit::Kind::Zone;
}

// Where an edit's own tip is, if it makes one (model coordinates).
std::optional<fruit::Vec3> yourTip(const TipEdit& e) {
    switch (e.kind) {
        case TipEdit::Kind::Add:
        case TipEdit::Kind::Resize:
        case TipEdit::Kind::Pin: return e.at;
        case TipEdit::Kind::Move: return e.to;
        case TipEdit::Kind::Delete:
        case TipEdit::Kind::Zone: return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

SupportPlan planTips(const Document& doc, const SupportChoices& c, const std::vector<fruit::Vec3>& visible_model,
                     const fruit::JobControl& ctl, const std::vector<TipEdit>& edits) {
    fruit::SupportConfig cfg;
    fruit::TreeConfig tc;
    const fruit::Placement& p = doc.placement();
    resolve(p, c, cfg, tc);
    fruit::CosmeticSpec cos;
    cos.visible_from = visibleInPrinter(visible_model, p);
    fruit::PointGenOptions po;
    po.cosmetic = &cos;
    // Hand edits: your tips are pinned, and the planner's own stay out of the zones.
    const fruit::Transform& t = doc.transform();
    for (const TipEdit& e : edits) {
        if (keepsOut(e.kind)) cos.exclusions.push_back({t.apply(e.at), e.radius});
        if (const auto at = yourTip(e))
            po.manual.push_back(manualTip(t.apply(*at), unit(t.applyLinear(e.normal)), e.diameter));
    }
    // Hollowed, drilled or filled on the Hollow screen: the tips are planned
    // on those layers, none goes in a hole's mouth, and none inside a hollow.
    std::shared_ptr<const HollowPlan> hollow;
    fruit::JobControl rest = ctl;
    if (doc.hollow.applied) {
        if (doc.hollow.planFresh(doc.revision()) && doc.hollow.plan->layer_height == kLayerHeight) {
            hollow = doc.hollow.plan;
        } else {
            hollow = std::make_shared<const HollowPlan>(
                planHollow(doc, doc.hollow.choices, doc.hollow.manual, kLayerHeight, ctl.sub(0.0, 0.3)));
            rest = ctl.sub(0.3, 1.0);
        }
        if (!hollow->changes()) hollow.reset();
    }
    if (hollow) {
        po.layers = hollow->layers;
        if (!hollow->filling()) po.hollows = &hollow->cavities().interior;  // their ceilings get no tips
        for (std::size_t i = 0; i < hollow->holes.size(); ++i)
            if (hollow->placed[i].cavity >= 0)
                cos.exclusions.push_back({t.apply(hollow->holes[i].at), hollow->placed[i].radius + kHoleMouthMargin});
    }
    fruit::PointGenResult pg = fruit::generateSupportPoints(doc.placed(), cfg, po, rest);
    // Your tips come back as the planner placed them; applyEdits puts back the edits' own.
    std::erase_if(pg.points, [](const fruit::SupportPoint& q) { return q.category == fruit::PointCategory::Manual; });
    return SupportPlan{std::move(pg.points), std::move(pg.warnings), edits, std::move(hollow)};
}

SupportResult generate(const Document& doc, const SupportChoices& c, const std::vector<fruit::Vec3>& visible_model,
                       const fruit::JobControl& ctl, const std::vector<TipEdit>& edits) {
    const auto t0 = std::chrono::steady_clock::now();
    SupportResult r =
        finishWithEdits(doc, c, planTips(doc, c, visible_model, ctl.sub(0.0, 0.55), edits), edits, ctl.sub(0.55, 1.0));
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

SupportResult finishWithEdits(const Document& doc, const SupportChoices& c, SupportPlan plan,
                              const std::vector<TipEdit>& edits, const fruit::JobControl& ctl) {
    std::vector<fruit::SupportPoint> planned = plan.points;
    std::vector<TipEdit> plan_edits = plan.edits;
    plan.points = applyEdits(std::move(plan.points), edits, doc.transform());
    SupportResult r = finishSupports(doc, c, std::move(plan), ctl);
    r.planned = std::move(planned);
    r.plan_edits = std::move(plan_edits);
    r.edits = edits;
    return r;
}

std::vector<fruit::SupportPoint> applyEdits(std::vector<fruit::SupportPoint> pts, const std::vector<TipEdit>& edits,
                                            const fruit::Transform& t) {
    using K = TipEdit::Kind;
    const auto planners = [](const fruit::SupportPoint& p) { return p.category != fruit::PointCategory::Manual; };
    // The planner's tip the edit was made on: the nearest one where it was.
    auto takeTip = [&](const fruit::Vec3& at) {
        auto best = pts.end();
        double best_d = kMatch;
        for (auto it = pts.begin(); it != pts.end(); ++it)
            if (planners(*it) && dist(it->pos, at) <= best_d) {
                best_d = dist(it->pos, at);
                best = it;
            }
        if (best != pts.end()) pts.erase(best);
    };
    // The planner's tips inside a zone (the planner's own test: within the radius).
    auto keepOut = [&](const fruit::Vec3& at, double r) {
        std::erase_if(pts, [&](const fruit::SupportPoint& p) { return planners(p) && dist(p.pos, at) <= r; });
    };
    std::vector<fruit::SupportPoint> yours;
    for (const TipEdit& e : edits) {
        const fruit::Vec3 at = t.apply(e.at);
        const fruit::Vec3 n = unit(t.applyLinear(e.normal));
        if (e.kind == K::Move || e.kind == K::Delete || e.kind == K::Resize || e.kind == K::Pin) takeTip(at);
        if (keepsOut(e.kind)) keepOut(at, e.radius);
        if (const auto mine = yourTip(e)) yours.push_back(manualTip(t.apply(*mine), n, e.diameter));
    }
    pts.insert(pts.end(), yours.begin(), yours.end());
    return pts;
}

bool planStillFits(const std::vector<TipEdit>& plan_edits, const std::vector<TipEdit>& edits) {
    std::vector<bool> used(edits.size(), false);
    for (const TipEdit& p : plan_edits) {
        bool found = false;
        for (std::size_t i = 0; i < edits.size() && !found; ++i)
            if (!used[i] && edits[i] == p) used[i] = found = true;
        if (!found) return false;
    }
    return true;
}

namespace {

// The edit that made a tip of yours (printer coordinates), or -1.
int editOf(const std::vector<TipEdit>& edits, const fruit::SupportPoint& tip, const fruit::Transform& t) {
    if (tip.category != fruit::PointCategory::Manual) return -1;
    for (int i = static_cast<int>(edits.size()) - 1; i >= 0; --i)
        if (const auto at = yourTip(edits[static_cast<std::size_t>(i)]); at && dist(t.apply(*at), tip.pos) <= kMatch)
            return i;
    return -1;
}

// Takes the edit out of the list, for changing and putting back at the end.
TipEdit takeEdit(std::vector<TipEdit>& edits, int i) {
    const TipEdit e = edits[static_cast<std::size_t>(i)];
    edits.erase(edits.begin() + i);
    return e;
}

}  // namespace

bool recordMove(std::vector<TipEdit>& edits, const fruit::SupportPoint& tip, const fruit::Vec3& to,
                const fruit::Vec3& normal, const fruit::Transform& t) {
    using K = TipEdit::Kind;
    if (dist(tip.pos, to) <= kMatch) return false;
    const fruit::Transform m = t.inverse();
    TipEdit e;
    if (const int i = editOf(edits, tip, t); i >= 0) {
        e = takeEdit(edits, i);
        if (e.kind == K::Add) {
            e.at = m.apply(to);
        } else {  // moved again, or a resized or pinned tip moved: from where the planner had it
            e.kind = K::Move;
            e.to = m.apply(to);
            e.radius = kDeletedZone;
        }
    } else {
        e.kind = K::Move;
        e.at = m.apply(tip.pos);
        e.to = m.apply(to);
        e.diameter = tip.tip_diameter;
        e.radius = kDeletedZone;
    }
    e.normal = m.applyLinear(normal);
    edits.push_back(e);
    return true;
}

bool recordDelete(std::vector<TipEdit>& edits, const fruit::SupportPoint& tip, const fruit::Transform& t) {
    using K = TipEdit::Kind;
    const fruit::Transform m = t.inverse();
    TipEdit e;
    if (const int i = editOf(edits, tip, t); i >= 0) {
        e = takeEdit(edits, i);
        if (e.kind == K::Add) return true;  // your tip: gone with the edit that added it
        // A moved, resized or pinned tip of the planner's: deleted where the planner had it.
        e.kind = K::Delete;
        e.to = {};
        e.normal = {};
    } else {
        e.kind = K::Delete;
        e.at = m.apply(tip.pos);
        e.diameter = tip.tip_diameter;
    }
    e.radius = kDeletedZone;
    edits.push_back(e);
    return true;
}

bool recordResize(std::vector<TipEdit>& edits, const fruit::SupportPoint& tip, double diameter,
                  const fruit::Transform& t) {
    using K = TipEdit::Kind;
    if (std::abs(tip.tip_diameter - diameter) < 1e-9) return false;
    const fruit::Transform m = t.inverse();
    TipEdit e;
    if (const int i = editOf(edits, tip, t); i >= 0) {
        e = takeEdit(edits, i);
        if (e.kind == K::Pin) e.kind = K::Resize;
    } else {
        e.kind = K::Resize;
        e.at = m.apply(tip.pos);
        e.normal = m.applyLinear(tip.normal);
    }
    e.diameter = diameter;
    edits.push_back(e);
    return true;
}

bool recordPin(std::vector<TipEdit>& edits, const fruit::SupportPoint& tip, const fruit::Transform& t) {
    if (tip.category == fruit::PointCategory::Manual) return false;  // yours are pinned already
    const fruit::Transform m = t.inverse();
    TipEdit e;
    e.kind = TipEdit::Kind::Pin;
    e.at = m.apply(tip.pos);
    e.normal = m.applyLinear(tip.normal);
    e.diameter = tip.tip_diameter;
    edits.push_back(e);
    return true;
}

std::pair<std::string, std::string> describeEdit(const TipEdit& e, const fruit::Transform& t) {
    using K = TipEdit::Kind;
    const double z = t.apply(e.at).z;
    switch (e.kind) {
        case K::Add: return {"Added a tip", fmt("%.1f mm, %.1f mm up", e.diameter, z)};
        case K::Move: return {"Moved a tip", fmt("%.1f mm, now %.1f mm up", dist(t.apply(e.at), t.apply(e.to)),
                                                 t.apply(e.to).z)};
        case K::Delete: return {"Deleted a tip", fmt("Leaves a %.1f mm no-tip zone, %.1f mm up", 2 * e.radius, z)};
        case K::Resize: return {"Resized a tip", fmt("Now %.1f mm, %.1f mm up", e.diameter, z)};
        case K::Pin: return {"Pinned a tip", fmt("Kept when supports are regenerated, %.1f mm up", z)};
        case K::Zone: return {"No-tip zone", fmt("%.1f mm across, %.1f mm up", 2 * e.radius, z)};
    }
    return {"", ""};
}

EditCounts countEdits(const std::vector<TipEdit>& edits) {
    EditCounts c;
    for (const TipEdit& e : edits) switch (e.kind) {
            case TipEdit::Kind::Add: ++c.added; break;
            case TipEdit::Kind::Move: ++c.moved; break;
            case TipEdit::Kind::Delete: ++c.deleted; break;
            case TipEdit::Kind::Resize: ++c.resized; break;
            case TipEdit::Kind::Pin: ++c.pinned; break;
            case TipEdit::Kind::Zone: ++c.zones; break;
        }
    return c;
}

std::string describeCounts(const EditCounts& n) {
    std::string out;
    auto add = [&](std::size_t k, const char* what) {
        if (k == 0) return;
        if (!out.empty()) out += ", ";
        out += std::to_string(k) + " " + what;
    };
    add(n.added, "added");
    add(n.moved, "moved");
    add(n.deleted, "deleted");
    add(n.resized, "resized");
    add(n.pinned, "pinned");
    add(n.zones, n.zones == 1 ? "no-tip zone" : "no-tip zones");
    return out;
}

SupportResult finishSupports(const Document& doc, const SupportChoices& c, SupportPlan plan,
                             const fruit::JobControl& ctl) {
    const auto t0 = std::chrono::steady_clock::now();
    SupportResult r;
    r.choices = c;
    const fruit::Placement& p = doc.placement();
    resolve(p, c, r.cfg, r.tree_cfg);
    r.points = std::move(plan.points);
    r.plan_warnings = std::move(plan.warnings);
    r.hollow = std::move(plan.hollow);

    // Progress as generate() reports it: the tree from 0.55, the check from 0.85.
    const double min_raft =
        fruit::minRaftThickness(doc.printer.bottom_layers, doc.printer.transition_layers, kLayerHeight);
    r.tree = fruit::buildSupportTree(doc.placed(), r.points, r.tree_cfg, min_raft, ctl.sub(0.0, 0.55));
    ctl.step("mesh", 0.55);
    r.mesh = r.tree.mesh(r.tree_cfg);
    ctl.step("check", 0.67);
    // Tips the tree could not build: what only they held has nothing now.
    fruit::DropCheck drops;
    if (r.tree.dropped_points > 0) {
        std::vector<fruit::SupportPoint> built, dropped;
        fruit::splitByTree(r.tree, r.points, built, dropped);
        fruit::PointGenOptions po;  // the layers the tips were planned on
        if (r.hollow) {
            po.layers = r.hollow->layers;
            if (!r.hollow->filling()) po.hollows = &r.hollow->cavities().interior;
        }
        drops = fruit::checkDroppedTips(doc.placed(), r.cfg, built, dropped, po, ctl.silent());
    }
    const double pixel = doc.printer.pixelX() * doc.printer.pixelY();
    if (r.hollow) {
        // The hollowed model's own layers with the supports': nothing floats.
        const fruit::LayerStack print =
            fruit::unionLayers(*r.hollow->layers, fruit::sliceModel(r.mesh, kLayerHeight, nullptr, 0, ctl.sub(0.67, 0.9)));
        r.validation = fruit::validateLayers(print, r.cfg.link_grow, r.cfg.absorb_island_area, pixel, ctl.sub(0.9, 1.0));
    } else {
        r.validation = fruit::validateSupported(doc.placed(), r.mesh, kLayerHeight, r.cfg.link_grow,
                                                r.cfg.absorb_island_area, pixel, ctl.sub(0.67, 1.0));
    }

    // Counts.
    r.tips = r.points.size();
    std::map<double, std::size_t> sizes;
    for (const auto& pt : r.points) {
        r.island_tips += pt.category == fruit::PointCategory::Island;
        r.extra_hold += pt.category == fruit::PointCategory::Capacity;
        ++sizes[std::round(pt.tip_diameter * 100) / 100];
    }
    for (const auto& [d, n] : sizes) r.tip_sizes.emplace_back(d, n);
    std::stable_sort(r.tip_sizes.begin(), r.tip_sizes.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    r.pillars = r.tree.pillars.size();
    r.trunks = r.tree.trunks();
    r.braces = r.tree.braces.size();
    for (const auto& pl : r.tree.pillars) r.on_model += pl.on_model;
    r.support_ml = std::max(0.0, r.mesh.volume()) / 1000.0;
    fruit::BBox3 b = doc.placed().bbox();
    b.extend(r.mesh.bbox());
    r.size_with_supports = b.size();

    // Checks, as the canvas lists them.
    const auto& v = r.validation;
    r.checks.push_back({v.floating.empty(), "No part starts in mid-air",
                        v.floating.empty() ? "" : fmt("%.0f islands above the first layer have nothing under them",
                                                      static_cast<double>(v.floating.size()))});
    if (p.on_plate)
        r.checks.push_back({true, "Stands on the plate", "No raft: the model's flat side is its first layer"});
    else
        r.checks.push_back({v.first_layer_islands <= 1, "Raft comes off in one piece",
                            v.first_layer_islands <= 1 ? ""
                                                       : fmt("The first layer has %.0f separate pieces",
                                                             static_cast<double>(v.first_layer_islands))});
    r.checks.push_back({r.on_model == 0, "No support stands on the model",
                        r.on_model == 0 ? "" : fmt("%.0f stand on it and leave marks there", static_cast<double>(r.on_model))});
    r.checks.push_back({true, "No brace loops around the model",
                        r.tree.braces_skipped_trap ? fmt("%.0f braces were left out so none closes around the model",
                                                         static_cast<double>(r.tree.braces_skipped_trap))
                                                   : ""});

    // Problems, grouped by kind.
    std::map<std::string, SupportProblem> by_code;
    addWarnings(r.plan_warnings, by_code);
    addWarnings(r.tree.warnings, by_code);
    if (!v.floating.empty()) {
        SupportProblem fp;
        fp.level = SupportProblem::Level::Blocks;
        fp.code = "floating";
        fp.title = "Parts start in mid-air";
        fp.body = "After supports, these islands still have nothing under them and would print as loose flakes.";
        fp.count = v.floating.size();
        fp.has_where = true;
        fp.where = {v.floating[0].at.x, v.floating[0].at.y, v.floating[0].z};
        fp.engine = fmt("first at layer %.0f (z %.2f mm)", v.floating[0].layer, v.floating[0].z);
        by_code["floating"] = fp;
    }
    if (drops.tips > 0) {
        SupportProblem u;
        u.level = SupportProblem::Level::Blocks;
        u.code = "unsupported";
        u.title = "Spots no support reaches";
        u.body = "Tips were planned here, but no support could get to them, so these faces would print with nothing "
                 "holding them. Turn the model, or allow longer routes and drops onto the model under Advanced "
                 "settings.";
        u.count = drops.tips;
        u.has_where = true;
        u.where = drops.at.front();
        u.engine = fmt("%.0f tips with no support were the only cover of %.0f surface samples",
                       static_cast<double>(drops.tips), static_cast<double>(drops.samples));
        by_code["unsupported"] = u;
    }
    if (drops.short_layers > 0)
        addWarnings({{"capacity_deficit",
                      fmt("%.0f layers are short of hold without the tips no support could reach",
                          static_cast<double>(drops.short_layers)),
                      -1, drops.at.empty() ? fruit::Vec3{} : drops.at.front()}},
                    by_code);
    if (v.subpixel) {
        SupportProblem n;
        n.level = SupportProblem::Level::Note;
        n.code = "subpixel";
        n.title = v.subpixel == 1 ? "One sliver thinner than a pixel" : "Slivers thinner than a pixel";
        n.body = "Smaller than one 14 × 19 µm pixel, so it won't print. Nothing to do.";
        n.count = v.subpixel;
        by_code["subpixel"] = n;
    }
    for (auto& [code, pr] : by_code) r.problems.push_back(std::move(pr));
    std::stable_sort(r.problems.begin(), r.problems.end(),
                     [](const SupportProblem& a, const SupportProblem& b) { return a.level < b.level; });
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ctl.report("check", 1.0);
    return r;
}

std::string tipKind(const fruit::SupportPoint& p) {
    const char* kind = "Tip";
    switch (p.category) {
        case fruit::PointCategory::Island: kind = "Island tip"; break;
        case fruit::PointCategory::Overhang: kind = "Overhang tip"; break;
        case fruit::PointCategory::Capacity: kind = "Extra-hold tip"; break;
        case fruit::PointCategory::Stabilization: kind = "Steadying tip"; break;
        case fruit::PointCategory::Manual: kind = "Hand-placed tip"; break;
    }
    return std::string(kind) + fmt(" · %.1f mm", p.tip_diameter);
}

std::string tipWhy(const fruit::SupportPoint& p) {
    switch (p.category) {
        case fruit::PointCategory::Island:
            return "A part starts printing in mid-air here. This tip sits at its lowest point so the first layer of "
                   "it has something to hold on to.";
        case fruit::PointCategory::Overhang:
            return "This face leans too far to print on its own. Tips are spaced by how far each one's hold "
                   "reaches.";
        case fruit::PointCategory::Capacity:
            return "The layers here are bigger than the other tips can hold while they peel, so this one shares "
                   "the pull.";
        case fruit::PointCategory::Stabilization:
            return "Steadies a tall, slender part so it does not sway as it prints.";
        case fruit::PointCategory::Manual: return "You placed this tip by hand.";
    }
    return "";
}

std::string tipNumbers(const fruit::SupportPoint& p, const fruit::SupportConfig& cfg, const fruit::TreeConfig& tc) {
    return fmt("Contact depth %.2f mm · holds %.1f mm² of layer", fruit::tipPenetration(p.tip_diameter, tc),
               cfg.capacity.capacity(p.tip_diameter));
}

}  // namespace gui
