// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Hollows.h"

#include <algorithm>
#include <chrono>

#include "Document.h"
#include "Format.h"
#include "GooTools.h"

namespace gui {

namespace {

double volumeOf(const fruit::LayerStack& ls) {
    double v = 0;
    for (const fruit::Paths& s : ls.slices) v += std::max(0.0, fruit::area(s)) * ls.layer_height;
    return v;
}

}  // namespace

std::size_t HollowPlan::undrained() const {
    return static_cast<std::size_t>(std::count(drains.begin(), drains.end(), 0));
}

std::size_t HollowPlan::missed() const {
    return static_cast<std::size_t>(
        std::count_if(placed.begin(), placed.end(), [](const fruit::PlacedHole& h) { return h.cavity < 0; }));
}

HollowPlan planHollow(const Document& doc, const HollowChoices& c, const std::vector<fruit::DrainHole>& manual,
                      double layer_height, const fruit::JobControl& ctl) {
    const auto t0 = std::chrono::steady_clock::now();
    HollowPlan p;
    p.choices = c;
    p.manual = manual;
    p.layer_height = layer_height;
    auto solid = std::make_shared<const fruit::LayerStack>(
        fruit::sliceModel(doc.placed(), layer_height, nullptr, 0, ctl.sub(0.0, 0.15)));
    p.solid = solid;
    p.closed = fruit::closedHollows(*solid);
    ctl.check();
    // What holes are drilled into: the model hollowed (its own hollows filled
    // first, to go with the rest), or the model as it is.
    std::shared_ptr<const fruit::LayerStack> base = solid;
    if (c.hollow) {
        base = std::make_shared<const fruit::LayerStack>(fruit::filledLayers(*solid, p.closed));
        fruit::HollowOptions o;
        o.wall = c.wall;
        o.roof_lean = c.ceiling_lean;
        p.hollowed = fruit::hollowInterior(*base, o, ctl.sub(0.15, 0.85));
    }
    ctl.step("holes", 0.85);
    const fruit::HollowResult& cav = p.cavities();
    if (p.drilling() && !cav.cavities.empty()) {
        p.holes = c.auto_holes ? fruit::suggestDrainHoles(doc.placed(), doc.transform(), *base, cav, c.hole_diameter)
                               : manual;
        p.placed = fruit::placeHoles(p.holes, doc.transform(), *base, cav);
        p.layers = std::make_shared<const fruit::LayerStack>(fruit::drilledLayers(*base, cav, p.placed));
    } else if (c.hollow) {
        p.layers = std::make_shared<const fruit::LayerStack>(fruit::hollowedLayers(*base, cav));
    } else if (p.filling() && !cav.cavities.empty()) {
        p.layers = std::make_shared<const fruit::LayerStack>(fruit::filledLayers(*solid, cav));
    } else {
        p.layers = solid;
    }
    ctl.check();
    p.drains = fruit::cavitiesDrain(*p.layers, cav);
    p.resin_saved = volumeOf(*solid) - volumeOf(*p.layers);
    p.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ctl.report("holes", 1.0);
    return p;
}

std::string cavityKind(const fruit::Cavity& c, bool hollowed) {
    if (hollowed) return "Hollowed";
    return c.opens_up ? "Suction cup" : "Resin trap";
}

std::string cavityFacts(const fruit::Cavity& c, double layer_height) {
    return volumeText(c.volume) + strf(" · from %.1f to %.1f mm", c.first_layer * layer_height,
                                       (c.last_layer + 1) * layer_height);
}

std::string cavityMeaning(const fruit::Cavity& c, bool hollowed) {
    if (hollowed)
        return "Taken out to save resin, with a ceiling steep enough to hold itself up. Resin stays inside until "
               "a hole lets it out.";
    if (c.opens_up)
        return "It opens towards the film, so every layer pulls on a vacuum as it peels, and it comes off the "
               "printer full of resin.";
    return "Uncured resin stays inside, and pressure builds as the top closes. The inside ceiling can't have tips: "
           "they could never be removed.";
}

std::string applyLabel(const HollowPlan& p) {
    if (p.hollowing()) return "Hollow it";
    if (p.filling()) return "Print it solid";
    if (!p.drilling()) return "Leave it";
    const std::size_t n = p.holes.size();
    return n == 1 ? "Add a drain hole" : strf("Add %zu drain holes", n);
}

std::vector<HollowOutcome> hollowOutcomes(const HollowPlan& p) {
    std::vector<HollowOutcome> out;
    const auto& cavities = p.cavities().cavities;
    const bool several = cavities.size() > 1;
    for (std::size_t i = 0; i < cavities.size(); ++i) {
        const std::string which = several ? strf("Hollow %zu", i + 1) : std::string("The hollow");
        const bool ok = i < p.drains.size() && p.drains[i];
        if (p.filling()) {
            out.push_back({true, which + " is printed solid", "More resin, and nothing trapped."});
        } else if (!p.drilling()) {
            out.push_back({false, which + " stays as it is", "Shown as a problem before saving."});
        } else if (ok) {
            out.push_back({true, which + " drains",
                           "It opens toward the plate, so it no longer traps resin or pulls a vacuum."});
        } else {
            out.push_back({false, which + " still holds resin",
                           "No hole reaches its lowest point. Place one there, or let FruitSlicer place them."});
        }
    }
    if (p.missed())
        out.push_back({false, p.missed() == 1 ? "A hole reaches no hollow" : strf("%zu holes reach no hollow", p.missed()),
                       "It runs into solid or out of the model again, so it isn't drilled."});
    if (p.drilling() && p.undrained() < cavities.size())
        out.push_back({true, "Wash it out after printing", "Flush through the lower hole, then cure."});
    if (p.resin_saved > 0.5)
        out.push_back({true, "Saves " + volumeText(p.resin_saved) + " of resin",
                       p.hollowing() ? strf("Walls stay at least %.1f mm thick everywhere.", p.choices.wall)
                                     : std::string("What the holes take out.")});
    return out;
}

}  // namespace gui
