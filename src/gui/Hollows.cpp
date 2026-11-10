// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Hollows.h"

#include <algorithm>
#include <chrono>

#include "Document.h"
#include "Format.h"
#include "GooTools.h"
#include "core/Parallel.h"

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

std::vector<fruit::PlacedHole> placeHoles(const std::vector<fruit::DrainHole>& holes, const PartFrames& frames,
                                          const fruit::LayerStack& solid, const fruit::HollowResult& hollow) {
    // Each part's holes with its own transform, back in the holes' order.
    std::vector<fruit::PlacedHole> out(holes.size());
    std::vector<int> parts;
    for (const fruit::DrainHole& h : holes)
        if (std::find(parts.begin(), parts.end(), h.part) == parts.end()) parts.push_back(h.part);
    for (const int part : parts) {
        const fruit::Transform* t = frames.find(part);
        std::vector<fruit::DrainHole> mine;
        std::vector<std::size_t> at;
        for (std::size_t i = 0; i < holes.size(); ++i)
            if (holes[i].part == part) {
                mine.push_back(holes[i]);
                at.push_back(i);
            }
        if (!t) continue;  // not on the plate: not drilled
        const std::vector<fruit::PlacedHole> placed = fruit::placeHoles(mine, *t, solid, hollow);
        for (std::size_t k = 0; k < placed.size(); ++k) out[at[k]] = placed[k];
    }
    return out;
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
    const PartFrames frames = doc.frames();
    if (p.drilling() && !cav.cavities.empty()) {
        if (!c.auto_holes) {
            // Those placed by hand, on the parts there are.
            for (const fruit::DrainHole& h : manual)
                if (frames.find(h.part)) p.holes.push_back(h);
        } else if (doc.partCount() == 1) {
            p.holes = fruit::suggestDrainHoles(doc.placed(), doc.part().transform(), *base, cav, c.hole_diameter);
            for (fruit::DrainHole& h : p.holes) h.part = doc.part().id();
        } else {
            // Found on the parts together, then kept on the part each goes into.
            p.holes = fruit::suggestDrainHoles(doc.placed(), fruit::Transform{}, *base, cav, c.hole_diameter);
            for (fruit::DrainHole& h : p.holes) {
                h.part = doc.partNear(h.at);
                const fruit::Transform to_part = frames.find(h.part)->inverse();
                h.at = to_part.apply(h.at);
                h.axis = fruit::normalized(to_part.applyLinear(h.axis));
            }
        }
    }
    if (!p.holes.empty()) {
        p.placed = placeHoles(p.holes, frames, *base, cav);
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
    // Resin used: what prints, and what stays inside a hollow that doesn't
    // drain. Against the model printed solid, as its own hollows' resin
    // is used either way, cured or trapped.
    double held = 0;
    for (std::size_t i = 0; i < cav.cavities.size(); ++i)
        if (i >= p.drains.size() || !p.drains[i]) held += cav.cavities[i].volume;
    p.resin_saved = volumeOf(*solid) + p.closed.volume - volumeOf(*p.layers) - held;
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
    const std::size_t n = p.holes.size();
    if (!p.drilling() || n == 0) return "Leave it";
    return n == 1 ? "Add a drain hole" : strf("Add %zu drain holes", n);
}

HollowOutcome cavityLabel(const HollowPlan& p, std::size_t i, bool applied) {
    const fruit::Cavity& c = p.cavities().cavities.at(i);
    const std::string v = volumeText(c.volume);
    const bool drains = applied && i < p.drains.size() && p.drains[i];
    if (p.hollowing())
        return {!applied || drains,
                v + (applied ? " taken out" : " to take out") + (applied && !drains ? " · holds resin" : ""), ""};
    if (applied && p.filling()) return {true, v + " · printed solid", ""};
    if (drains) return {true, v + " · drains", ""};
    return {false, c.opens_up ? v + " suction cup" : v + " of trapped resin", ""};
}

fruit::Vec3 cavityLabelAt(const fruit::HollowResult& h, std::size_t i, double layer_height) {
    const fruit::Cavity& c = h.cavities.at(i);
    const std::size_t mid = static_cast<std::size_t>((c.first_layer + c.last_layer) / 2);
    fruit::Paths own;
    if (mid < h.pieces.size())
        for (const fruit::InteriorPiece& k : h.pieces[mid])
            if (k.cavity == static_cast<int>(i)) {
                const fruit::Paths piece = h.piece(mid, k);
                own.insert(own.end(), piece.begin(), piece.end());
            }
    const fruit::BBox2 b = fruit::bbox(own);
    const double z = (c.first_layer + c.last_layer + 1) * layer_height / 2;
    return own.empty() ? fruit::Vec3{0, 0, z} : fruit::Vec3{(b.min.x + b.max.x) / 2, (b.min.y + b.max.y) / 2, z};
}

HollowSection hollowSection(const HollowPlan& p, fruit::Vec2 at, fruit::Vec2 along) {
    HollowSection s;
    s.at = at;
    s.along = along;
    s.layer_height = p.layer_height;
    if (!p.solid || !p.layers) return s;
    // Layer by layer, in parallel: a big model has millions of outline points.
    const std::size_t n = p.solid->size();
    struct Layer {
        std::vector<SectionBand> outline, hollow, prints;
    };
    std::vector<Layer> per(n);
    const fruit::HollowResult& cav = p.cavities();
    fruit::parallelFor(n, 0, [&](std::size_t k) {
        auto add = [&](std::vector<SectionBand>& to, const fruit::Paths& paths) {
            for (const auto& [t0, t1] : fruit::lineSpans(paths, at, along)) to.push_back({k, t0, t1});
        };
        // The model as it is, with its own hollows: one piece.
        fruit::Paths whole = p.solid->slices[k];
        if (k < p.closed.interior.size()) whole.insert(whole.end(), p.closed.interior[k].begin(), p.closed.interior[k].end());
        add(per[k].outline, whole);
        if (!p.filling() && k < cav.interior.size()) add(per[k].hollow, cav.interior[k]);
        if (k < p.layers->size()) add(per[k].prints, p.layers->slices[k]);
    });
    for (Layer& l : per) {
        s.outline.insert(s.outline.end(), l.outline.begin(), l.outline.end());
        s.hollow.insert(s.hollow.end(), l.hollow.begin(), l.hollow.end());
        s.prints.insert(s.prints.end(), l.prints.begin(), l.prints.end());
    }
    return s;
}

std::size_t hollowsToFix(const Document& doc) {
    const HollowState& hs = doc.hollow;
    if (hs.planFresh(doc.revision())) return hs.applied ? hs.plan->undrained() : hs.plan->closed.cavities.size();
    // Until it is planned again: what was chosen is taken to work.
    if (hs.applied && (hs.choices.hollow || hs.choices.fix != HollowChoices::Fix::Leave)) return 0;
    if (hs.plan && hs.plan_revision == doc.revision()) return hs.plan->closed.cavities.size();
    std::size_t n = 0;
    for (const Part& part : doc.parts()) n += static_cast<std::size_t>(std::max(0, part.check().cavities));
    return n;
}

HollowOutcome hollowSummary(const Document& doc) {
    const HollowState& hs = doc.hollow;
    const HollowChoices& c = hs.choices;
    const std::size_t left = hollowsToFix(doc);
    const PartFrames frames = doc.frames();  // holes on a part off the job don't count
    const auto m = static_cast<std::size_t>(
        std::count_if(hs.manual.begin(), hs.manual.end(), [&](const fruit::DrainHole& h) { return frames.find(h.part); }));
    const std::string by_hand = m == 1 ? std::string("1 drain hole placed by hand")
                                       : strf("%zu drain holes placed by hand", m);
    const std::string holes = c.auto_holes ? std::string("Drain holes placed automatically") : by_hand;
    const std::string holding = left == 1 ? std::string("1 hollow still holds resin")
                                          : strf("%zu hollows still hold resin", left);
    if (hs.applied && c.hollow) return {left == 0, strf("Hollowed · %.1f mm wall", c.wall), left ? holding : holes};
    if (hs.applied && c.fix == HollowChoices::Fix::Holes)
        return {left == 0, left ? holding : "Drain holes added", left ? holes : (c.auto_holes ? "Placed automatically" : by_hand)};
    if (hs.applied && c.fix == HollowChoices::Fix::Fill) return {true, "Printed solid", "Nothing holds resin."};
    if (left == 0) return {true, "No hollows found", "Hollow and drain options"};
    const std::string n = left == 1 ? std::string("1 hollow holds resin") : strf("%zu hollows hold resin", left);
    if (hs.applied) return {false, n, "Left as it is: shown as a problem before saving."};
    return {false, n, left == 1 ? "Drain it, fill it or leave it." : "Drain them, fill them or leave them."};
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
                                     : std::string("What was inside drains out instead of staying in the part.")});
    return out;
}

}  // namespace gui
