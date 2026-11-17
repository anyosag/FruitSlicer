// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Hollows.h"

#include <algorithm>
#include <chrono>
#include <cmath>

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

std::size_t HollowPlan::toFix() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < closed.cavities.size(); ++i)
        n += !isAccepted(i) && grade(i) == fruit::CupGrade::Problem;
    return n;
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

namespace {

bool sameSize(double was, double now) { return std::abs(now - was) <= was / 3 + 1e-9; }

}  // namespace

bool acceptedIs(const AcceptedHollow& a, const fruit::HollowResult& closed, std::size_t i, const PartFrames& frames,
                double layer_height) {
    if (i >= closed.cavities.size() || layer_height <= 0) return false;
    const fruit::Transform* t = frames.find(a.part);
    if (!t) return false;
    const fruit::Cavity& c = closed.cavities[i];
    if (c.opens_up != a.cup || !sameSize(a.volume, c.volume)) return false;
    const fruit::Vec3 p = t->apply(a.at);
    if (p.z < 0) return false;
    const auto layer = static_cast<std::size_t>(p.z / layer_height);
    return closed.cavityAt(layer, {p.x, p.y}) == static_cast<int>(i);
}

bool acceptedIs(const AcceptedHollow& a, const fruit::Warning& w, const PartFrames& frames) {
    const bool cup = w.code.rfind("suction_cup", 0) == 0;
    if (!cup && w.code != "resin_trap") return false;
    if (cup != a.cup) return false;
    auto v = [&](const char* k) {
        const auto it = w.values.find(k);
        return it == w.values.end() ? std::nan("") : it->second;
    };
    const double x0 = v("x0"), y0 = v("y0"), x1 = v("x1"), y1 = v("y1"), z0 = v("z0"), z1 = v("z1");
    if (std::isnan(x0 + y0 + x1 + y1 + z0 + z1 + v("ix") + v("iy") + v("wz") + v("volume"))) return false;  // an older job
    if (!sameSize(a.volume, v("volume"))) return false;
    const fruit::Transform* t = frames.find(a.part);
    if (!t) return false;
    // The part may have moved since: the warning's point moved with it.
    const fruit::Vec3 shift = w.where - fruit::Vec3{v("ix"), v("iy"), v("wz")};
    const fruit::Vec3 p = t->apply(a.at) - shift;
    const double m = 0.01;
    return p.x >= x0 - m && p.x <= x1 + m && p.y >= y0 - m && p.y <= y1 + m && p.z >= z0 - m && p.z <= z1 + m;
}

bool acceptedIs(const AcceptedHollow& a, const fruit::HollowComponent& c, double layer_height, const PartFrames& frames) {
    if ((c.kind == fruit::HollowKind::SuctionCup) != a.cup || !sameSize(a.volume, c.volume)) return false;
    const fruit::Transform* t = frames.find(a.part);
    if (!t) return false;
    const fruit::Vec3 p = t->apply(a.at);
    const double m = 0.01;
    return p.x >= c.box.min.x - m && p.x <= c.box.max.x + m && p.y >= c.box.min.y - m && p.y <= c.box.max.y + m &&
           p.z >= c.first_layer * layer_height - m && p.z <= (c.last_layer + 1) * layer_height + m;
}

AcceptedHollow acceptedFor(const Document& doc, const HollowPlan& p, std::size_t i) {
    const fruit::Cavity& c = p.closed.cavities.at(i);
    fruit::Vec3 at = c.inside_layer >= 0
                         ? fruit::Vec3{c.inside.x, c.inside.y, (c.inside_layer + 0.5) * p.layer_height}
                         : cavityLabelAt(p.closed, i, p.layer_height);
    AcceptedHollow a;
    a.part = doc.partNear(at);
    const PartFrames frames = doc.frames();
    if (const fruit::Transform* t = frames.find(a.part)) at = t->inverse().apply(at);
    a.at = at;
    a.volume = c.volume;
    a.cup = c.opens_up;
    return a;
}

fruit::CupLimits cupLimits(const Document& doc) { return cupLimitsOf(doc.supports.choices); }

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
    // How each grades, and those accepted: holes and filling leave them.
    const PartFrames frames = doc.frames();
    const fruit::CupLimits limits = cupLimits(doc);
    p.accepted_with = doc.hollow.accepted;
    const std::size_t own = p.closed.cavities.size();
    p.accepted.assign(own, 0);
    p.grades.assign(own, fruit::CupGrade::Problem);
    std::vector<char> keep(own, 1);
    for (std::size_t i = 0; i < own; ++i) {
        const fruit::Cavity& k = p.closed.cavities[i];
        if (k.opens_up) p.grades[i] = fruit::gradeCup(k.cup, k.last_layer - k.first_layer + 1, limits);
        for (const AcceptedHollow& a : p.accepted_with)
            if (acceptedIs(a, p.closed, i, frames, layer_height)) p.accepted[i] = 1;
        keep[i] = !p.accepted[i];
    }
    p.fixing = std::count(keep.begin(), keep.end(), 0) ? fruit::keepCavities(p.closed, keep) : p.closed;
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
    const fruit::HollowResult& cav = c.hollow ? p.hollowed : p.fixing;  // what is drilled or filled
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
    p.drains = fruit::cavitiesDrain(*p.layers, p.cavities());
    // Resin used: what prints, and what stays inside a hollow that doesn't
    // drain. Against the model printed solid, as its own hollows' resin
    // is used either way, cured or trapped.
    double held = 0;
    const fruit::HollowResult& all = p.cavities();
    for (std::size_t i = 0; i < all.cavities.size(); ++i)
        if (i >= p.drains.size() || !p.drains[i]) held += all.cavities[i].volume;
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
        return "It opens towards the film, so while its layers print, each peel pulls on the resin sealed in it as "
               "well as on the layer that just cured.";
    return "Uncured resin stays inside, and pressure builds as the top closes. The inside ceiling can't have tips: "
           "they could never be removed.";
}

namespace {

std::string mm2(double a) { return thousands(static_cast<std::size_t>(std::lround(a))) + " mm²"; }

std::string pct(double share) {
    const double p = 100 * share;
    return p < 9.95 && p >= 0.05 ? strf("%.1f %%", p) : strf("%.0f %%", p);
}

}  // namespace

std::string cavityGradeText(const HollowPlan& p, std::size_t i) {
    const fruit::Cavity& c = p.closed.cavities.at(i);
    const std::string t = cavityKind(c, false);
    if (p.isAccepted(i)) return t + " · accepted";
    return c.opens_up ? t + " · " + fruit::gradeName(p.grade(i)) : t;
}

std::string cavityPull(const fruit::Cavity& c, fruit::CupGrade g, const fruit::CupLimits& l) {
    if (!c.opens_up || c.cup.share_layer < 0) return "";
    const fruit::CupMeasures& m = c.cup;
    const int layers = c.last_layer - c.first_layer + 1;
    std::string s = strf("Its opening is %s, %s of the %s cured round it, so each of its %d layers pulls about %s "
                         "harder as it peels. ",
                         mm2(m.open_area).c_str(), pct(m.share).c_str(), mm2(m.cured_around).c_str(), layers,
                         pct(m.share).c_str());
    const std::string lo = pct(l.note_below), hi = pct(l.problem_from);
    if (g == fruit::CupGrade::Note) s += "Under " + lo + ", so it is a note.";
    else if (g == fruit::CupGrade::Warning) s += "Between " + lo + " and " + hi + ", so it is a warning.";
    else if (m.share >= l.problem_from) s += hi + " or more, so it is a problem.";
    else s += strf("Between %s and %s, on %d layers or more, so it is a problem.", lo.c_str(), hi.c_str(), l.deep_layers);
    s += strf(" The %s and %s limits and the %d layers are assumed, not measured on prints: change them under "
              "Advanced.",
              lo.c_str(), hi.c_str(), l.deep_layers);
    return s;
}

std::string cavityFix(const fruit::Cavity& c) {
    if (!c.opens_up || c.cup.share_layer < 0) return "";
    const fruit::CupMeasures& m = c.cup;
    const std::string sealed = c.first_layer == 0
                                   ? " It has no floor of its own: the plate is its floor and seals it."
                                   : " A hole down through its floor wouldn't help: the floor stands on the plate, which "
                                     "seals it.";
    if (m.channel() && m.wall >= 0)
        return strf("A groove %.1f mm wide. To take the pull away, a notch through the %.1f mm wall to the outside "
                    "lets resin in from the side.",
                    m.width, m.wall) +
               (m.floor_on_plate ? sealed : std::string());
    if (!m.floor_on_plate)
        return "To take the pull away, a vent hole up through its floor lets resin in as each layer peels, or turn "
               "the model so it opens towards the plate.";
    return (m.wall >= 0 ? strf("To take the pull away, a hole through the %.1f mm side wall at its floor lets resin "
                               "in, or turn the model so it opens towards the plate.",
                               m.wall)
                        : std::string("To take the pull away, a hole through a side wall at its floor lets resin in, "
                                      "or turn the model so it opens towards the plate.")) +
           sealed;
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
    const bool accepted = p.isAccepted(i);
    if (applied && p.filling() && !accepted) return {true, v + " · printed solid", ""};
    if (drains) return {true, v + " · drains", ""};
    if (accepted) return {true, v + (c.opens_up ? " suction cup" : " of trapped resin") + " · accepted", ""};
    if (c.opens_up)
        return {p.grade(i) != fruit::CupGrade::Problem, v + " suction cup · " + fruit::gradeName(p.grade(i)), ""};
    return {false, v + " of trapped resin", ""};
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
    if (hs.planFresh(doc.revision())) {
        const HollowPlan& p = *hs.plan;
        if (hs.applied && p.hollowing()) return p.undrained();
        // Notes, warnings and the accepted don't count.
        std::size_t n = 0;
        for (std::size_t i = 0; i < p.closed.cavities.size(); ++i)
            n += !p.isAccepted(i) && p.grade(i) == fruit::CupGrade::Problem &&
                 !(hs.applied && i < p.drains.size() && p.drains[i]);
        return n;
    }
    // Until it is planned again: what was chosen is taken to work.
    if (hs.applied && (hs.choices.hollow || hs.choices.fix != HollowChoices::Fix::Leave)) return 0;
    if (hs.plan && hs.plan_revision == doc.revision()) return hs.plan->toFix();
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
    if (left == 0) {
        const HollowPlan* p = hs.plan && hs.plan_revision == doc.revision() ? hs.plan.get() : nullptr;
        const std::size_t found = p ? p->closed.cavities.size() : 0;
        if (found == 0) return {true, "No hollows found", "Hollow and drain options"};
        return {true, found == 1 ? std::string("1 hollow, not a problem") : strf("%zu hollows, none a problem", found),
                "Notes, warnings or accepted: on the Hollow screen"};
    }
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
        const bool counts = p.hollowing() || p.grade(i) == fruit::CupGrade::Problem;
        const std::string graded = p.hollowing() ? std::string()
                                                 : std::string("A suction cup graded a ") + fruit::gradeName(p.grade(i)) +
                                                       ": not counted as a problem.";
        if (!p.hollowing() && p.isAccepted(i)) {
            out.push_back({true, which + " prints as it is", "Accepted for this job: not counted as a problem."});
        } else if (p.filling()) {
            out.push_back({true, which + " is printed solid", "More resin, and nothing trapped."});
        } else if (!p.drilling()) {
            out.push_back({!counts, which + " stays as it is", counts ? "Shown as a problem before saving." : graded});
        } else if (ok) {
            out.push_back({true, which + " drains",
                           "A hole lets resin in and out of it, so it no longer traps resin or pulls a vacuum."});
        } else {
            out.push_back({!counts, which + " still holds resin",
                           "No hole reaches its lowest point. Place one there, or let FruitSlicer place them." +
                               (counts ? std::string() : " " + graded)});
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
