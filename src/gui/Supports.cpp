// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Supports.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>

#include "Document.h"
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
    fruit::resolveSettings(nullptr, ch, cfg, &tc);
    cfg.default_tip = std::clamp(c.usual_tip, 0, static_cast<int>(cfg.tip_classes.size()) - 1);
    tc.merge_pillars = c.merge;
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
     "A hollow faces the film, so every layer pulls on a vacuum. Turn the model, or add a drain hole later."},
    {"resin_trap", SupportProblem::Level::Review, "Resin traps",
     "A cup facing up holds liquid resin, which can spill or cure where it shouldn't."},
    {"cavity_ceiling", SupportProblem::Level::Review, "Closed hollows",
     "A closed hollow has no supports inside, so its ceiling may sag. Drain holes come in a later update."},
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

SupportResult generate(const Document& doc, const SupportChoices& c, const std::vector<fruit::Vec3>& visible_model,
                       const fruit::JobControl& ctl) {
    const auto t0 = std::chrono::steady_clock::now();
    SupportResult r;
    r.choices = c;
    const fruit::Placement& p = doc.placement();
    resolve(p, c, r.cfg, r.tree_cfg);

    fruit::CosmeticSpec cos;
    cos.visible_from = visibleInPrinter(visible_model, p);
    fruit::PointGenOptions po;
    po.cosmetic = &cos;
    fruit::PointGenResult pg = fruit::generateSupportPoints(doc.placed(), r.cfg, po, ctl.sub(0.0, 0.55));
    r.points = std::move(pg.points);

    const double min_raft =
        fruit::minRaftThickness(doc.printer.bottom_layers, doc.printer.transition_layers, kLayerHeight);
    r.tree = fruit::buildSupportTree(doc.placed(), r.points, r.tree_cfg, min_raft, ctl.sub(0.55, 0.80));
    ctl.step("mesh", 0.80);
    r.mesh = r.tree.mesh(r.tree_cfg);
    ctl.step("check", 0.85);
    r.validation = fruit::validateSupported(doc.placed(), r.mesh, kLayerHeight, r.cfg.link_grow,
                                            r.cfg.absorb_island_area, doc.printer.pixelX() * doc.printer.pixelY(),
                                            ctl.sub(0.85, 1.0));

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
    addWarnings(pg.warnings, by_code);
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
