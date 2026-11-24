// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Fit.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "Format.h"
#include "support/SupportConfig.h"

namespace gui {

using nlohmann::json;

namespace {

bool isNum(const json& j, const char* k) { return j.is_object() && j.contains(k) && j[k].is_number(); }
double num(const json& j, const char* k, double dflt = 0) { return isNum(j, k) ? j[k].get<double>() : dflt; }

// "10.5 mm²", "14 mm²"
std::string area(double v) { return oneDecimal(v) + " mm²"; }

std::string mm(double v) { return strf("%g mm", v); }

const char* plateWhat(const std::string& kind) {
    return kind == "tips"      ? "how much each tip size holds, and which leaves the smallest mark"
           : kind == "spacing" ? "how far apart tips can be, and how steep a face prints without them"
           : kind == "gaps"    ? "how close to the model a support can stand without printing stuck to it"
                               : "how big a spot that starts in mid-air can be on one tip, or two";
}

std::string join(const std::vector<std::string>& v, const char* last) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) out += (i == 0 ? "" : i + 1 == v.size() ? last : ", ") + v[i];
    return out;
}

int recordedIn(const fruit::PlateResults& p) {
    int n = 0;
    for (const auto& c : p.coupons) n += c.recorded ? 1 : 0;
    return n;
}

// Breakaway necks, per tip size: the one kept, or why none.
void neckValues(FitSummary& s, const json& necks) {
    for (const json& t : necks) {
        const double d = num(t, "tip_diameter");
        FitValue v;
        v.label = strf("Breakaway neck on %g mm tips", d);
        const bool plain_mark = t.contains("plain_mark") && t["plain_mark"].is_number();
        std::string tried;
        for (const json& n : t["necks"]) {
            tried += (tried.empty() ? "" : "; ") + strf("%g mm out: held %d of %d", num(n, "neck_back"),
                                                        n.value("held", 0), n.value("coupons", 0));
            if (n.contains("mark") && n["mark"].is_number()) tried += strf(", mark %.1f", n["mark"].get<double>());
        }
        const std::string plain = strf("Plain tips at the same loads held %d of %d", t.value("plain_held", 0),
                                       t.value("plain_coupons", 0)) +
                                  (plain_mark ? strf(", mark %.1f", t["plain_mark"].get<double>()) : std::string()) + ". ";
        if (t.contains("kept")) {
            v.value = strf("%g mm out", t["kept"].get<double>());
            v.how = plain + "Necks: " + tried + ". The one with the least mark that held as well is kept.";
        } else {
            v.value = "none";
            v.how = plain + "Necks: " + tried + ". None held as well and left a mark half a point less, so these "
                                                "tips stay plain.";
        }
        s.values.push_back(v);
    }
}

// The tips plate in two sets: how much harder the peel pulls far from the hinge.
void peelValues(FitSummary& s, const json& p) {
    if (p.value("unpinned", false)) {
        s.unsure.push_back("By the hinge and far from it, too few coupons held to tell the two sides apart: any extra "
                           "pull far from the hinge fits the scores as well, so it is still the 25 % guess.");
        return;
    }
    if (!p.contains("bias")) {
        s.unsure.push_back("By the hinge and far from it, no tip size let go at a load both sides measure, so how much "
                           "harder the peel pulls far from the hinge is still the 25 % guess.");
        return;
    }
    FitValue v;
    v.label = "Extra hold away from the hinge";
    v.value = strf("up to +%.0f %%", 100 * num(p, "bias"));
    const json& r = p.contains("bias_range") ? p["bias_range"] : json::array({num(p, "bias"), num(p, "bias")});
    const int bad = static_cast<int>(num(p, "misfits"));
    v.how = strf("With the pull that much harder at the far edge, the coupons that held and the ones that let go, by "
                 "the hinge and far from it, fit each tip size's hold %s; anything from +%.0f %% to +%.0f %% fits as "
                 "well, the middle is kept.",
                 bad == 0 ? "with none out of place" : strf("with %d out of place", bad).c_str(),
                 100 * r[0].get<double>(), 100 * r[1].get<double>());
    v.basis = Basis::Measured;
    s.values.push_back(v);
}

// The gap plate: the narrowest gap at which every shaft as far or further came off cleanly, and a pixel.
void gapValues(FitSummary& s, const json& g) {
    const double clean = num(g, "clean"), stuck = num(g, "stuck"), pixel = num(g, "pixel");
    if (clean <= 0) {
        s.unsure.push_back(strf("On the gap plate a shaft stuck even %s from the model: supports need more room than "
                                "it tests. The gap stays as it was.",
                                mm(stuck).c_str()));
        return;
    }
    FitValue v;
    v.label = "Gap to the model";
    v.value = strf("%.2f mm", clean + pixel);
    v.how = (stuck > 0 ? strf("Shafts %s from the model came off cleanly, and so did every one further; at %s one "
                              "stuck. ",
                              mm(clean).c_str(), mm(stuck).c_str())
                       : strf("Every shaft came off cleanly, down to %s from the model. ", mm(clean).c_str())) +
            strf("One pixel (%.3f mm) more, as an edge can land half a pixel nearer on each side.", pixel);
    v.basis = stuck > 0 ? Basis::Measured : Basis::Bound;
    s.values.push_back(v);
}

const fruit::PlateResults* plateOf(const std::vector<fruit::PlateResults>& plates, const std::string& kind) {
    for (const auto& p : plates)
        if (p.kind == kind) return &p;
    return nullptr;
}

void tipValues(FitSummary& s, const json& tips, const fruit::FitOptions& o) {
    const fruit::CapacityModel borrowed;
    const json model = s.profile.contains("fits") ? s.profile["fits"].value("tips_model", json::object()) : json::object();
    const int n_all = model.value("coupons", 0);
    const std::string model_basis = model.value("basis", std::string());
    if (model_basis == "lower bound")
        s.unsure.push_back("No coupon on the tips plate let go, so each hold is only a floor: 70 % of the heaviest load.");
    else if (model_basis == "upper bound")
        s.unsure.push_back("Every coupon on the tips plate let go, so each hold is a guess: half the lightest load.");
    for (const json& t : tips) {
        TipHold h;
        h.tip = num(t, "tip_diameter");
        h.yours = num(t, "capacity_mm2");
        h.borrowed = borrowed.capacity(h.tip);
        const std::string basis = t.value("basis", std::string());
        const int n = t.value("coupons", 0), held = t.value("held", 0);
        const double heaviest_held = num(t, "max_held_mm2", -1), lightest_failed = num(t, "min_failed_mm2", -1);
        const double pen = num(t, "preferred_penetration", 0), contact = num(t, "contact_mm2", 0);
        // Its own coupons at the depth kept, as at the hinge's edge.
        const json at = t.value("at_depth", json::object());
        const int dn = at.value("coupons", n), dheld = at.value("held", held);
        const double dmax = num(at, "max_held_mm2", heaviest_held), dmin = num(at, "min_failed_mm2", lightest_failed);
        if (basis == "contact model") {
            h.how = strf("From %d coupons: a tip holds about %.0f mm² of layer for each mm² of the part it touches "
                         "before half let go. This one, %g mm deep, touches %.2f mm², so half would let go at about "
                         "%s, and FruitSlicer keeps 70 %% of that. Its own coupons there held %d of %d.",
                         n_all, num(model, "strength_50pct"), pen, contact, area(num(t, "load_50pct")).c_str(), dheld, dn);
        } else if (basis == "own floor") {
            h.basis = Basis::Bound;
            h.how = strf("Its %d coupons %g mm deep all held, even at %s, well past what the other sizes hold for what "
                         "they touch, so its own are used: only a floor, 70 %% of that load.",
                         dn, pen, area(dmax).c_str());
            s.unsure.push_back(strf("%g mm tips never let go, so their hold is at least %s.", h.tip, area(h.yours).c_str()));
        } else if (basis == "own guess") {
            h.basis = Basis::Guess;
            h.how = strf("None of its %d coupons %g mm deep held, even at %s, well short of what the other sizes hold "
                         "for what they touch, so its own are used: a guess, half that load.",
                         dn, pen, area(dmin).c_str());
            s.unsure.push_back(strf("%g mm tips let go even at the lightest load, %s, so their %s is only a guess.", h.tip,
                                    area(dmin).c_str(), area(h.yours).c_str()));
        } else if (basis == "own") {
            h.how = strf("Its own coupons %g mm deep held at up to %s and let go from %s, which doesn't fit what the "
                         "other sizes hold for what they touch, so FruitSlicer keeps 70 %% of halfway between its own.",
                         pen, area(dmax).c_str(), area(dmin).c_str());
        } else if (basis == "lower bound") {
            h.basis = Basis::Bound;
            h.how = strf("Every coupon held, so this is only a floor: 70 %% of the heaviest load, for what this tip "
                         "touches (%.2f mm²). Its own coupons held %d of %d.", contact, held, n);
        } else if (basis == "upper bound") {
            h.basis = Basis::Guess;
            h.how = strf("Every coupon let go, so this is a guess: half the lightest load, for what this tip touches "
                         "(%.2f mm²).", contact);
        } else {
            h.how = "Light loads let go as often as heavy ones, so this is set low: 70 % of the lightest load tested.";
        }
        s.holds.push_back(h);
        s.values.push_back({strf("Hold per %g mm tip", h.tip),
                            h.basis == Basis::Bound ? holdText(h) : area(h.yours), h.how, h.basis});
    }

    if (isNum(s.profile, "default_tip_diameter")) {
        const double d = s.profile["default_tip_diameter"].get<double>();
        double scar = -1;
        for (const json& t : tips)
            if (num(t, "tip_diameter") == d) scar = num(t, "mean_scar", -1);
        s.values.push_back({"Usual tip size", mm(d),
                            "Of the sizes that hold at least " + area(o.min_default_capacity) +
                                ", it left the smallest mark" +
                                (scar > 0 ? strf(": %.1f on average, on the scale of 1 to 5.", scar) : ".")});
    } else {
        s.unsure.push_back("No tip size held " + area(o.min_default_capacity) +
                           ", so the fit doesn't choose a usual tip size.");
    }

    for (const json& t : tips) {
        if (!isNum(t, "preferred_penetration")) continue;
        const double d = num(t, "tip_diameter"), pen = num(t, "preferred_penetration");
        const json depths = t.value("penetrations", json::array());
        const json* chosen = nullptr;
        std::vector<const json*> others;
        for (const json& p : depths) {
            if (num(p, "penetration") == pen) chosen = &p;
            else others.push_back(&p);
        }
        // One depth tried (the tips plate since 2026-11-24 puts each size as deep as a job does): nothing chosen.
        if (depths.size() < 2) continue;
        int held_any = 0;
        for (const json& p : depths) held_any += p.value("held", 0);
        if (held_any == 0) continue;  // none held at any depth: nothing to choose by
        std::string how;
        if (!chosen || others.empty()) {
            how = "The only depth tested.";
        } else {
            // The other depth it was weighed against: the deepest of the others when this is shallower.
            const json* other = others.front();
            for (const json* p : others)
                if (num(*p, "penetration") > num(*other, "penetration")) other = p;
            const bool shallower = num(*other, "penetration") > pen;
            const std::string mine = strf("%.2f", num(*chosen, "contact_mm2")), theirs = strf("%.2f", num(*other, "contact_mm2"));
            if (shallower) {
                how = strf("It left the smaller mark, %.1f against %.1f at %g mm, so it's kept, though it touches less "
                           "of the part (%s mm² against %s) and so holds less.",
                           num(*chosen, "mean_scar"), num(*other, "mean_scar"), num(*other, "penetration"), mine.c_str(),
                           theirs.c_str());
            } else if (isNum(*chosen, "mean_scar") && isNum(*other, "mean_scar")) {
                how = strf("The marks were within half a point (%.1f and %.1f), so the deeper one is kept: it touches "
                           "more of the part (%s mm² against %s), so it holds more.",
                           num(*chosen, "mean_scar"), num(*other, "mean_scar"), mine.c_str(), theirs.c_str());
            } else {
                how = strf("The marks weren't scored at both depths, so the deeper one is kept: it touches more of the "
                           "part (%s mm² against %s), so it holds more.", mine.c_str(), theirs.c_str());
            }
        }
        s.values.push_back({strf("Contact depth for %g mm tips", d), mm(pen), how});
    }
}

void spacingValues(FitSummary& s, const json& sp, const fruit::PlateResults* plate) {
    // The spacings tried at each angle, from the plate itself.
    std::map<double, std::set<double>> tried;
    if (plate)
        for (const auto& c : plate->coupons)
            if (c.params.value("kind", std::string("plate")) == "plate")
                tried[num(c.params, "angle_deg")].insert(num(c.params, "spacing_mm"));
    const json& curve = s.profile.value("coverage_curve", json());
    if (sp.contains("plates") && curve.is_array() && !curve.empty()) {
        const json* limit = nullptr;  // the angle that sets the reach
        for (const json& p : sp["plates"])
            if (!limit || num(p, "radius") < num(*limit, "radius")) limit = &p;
        const double ang = num(*limit, "angle_deg"), r = num(*limit, "radius");
        const std::set<double>& at = tried[ang];
        FitValue v{"Tip reach", strf("%.1f → %.1f mm", curve.front()[1].get<double>(), curve.back()[1].get<double>()),
                   ""};
        const std::string rest = " Steeper faces reach further, in the proportions of FruitSlicer's starting values.";
        if (!isNum(*limit, "max_ok_pitch")) {
            v.basis = Basis::Guess;
            v.how = strf("Even with tips %g mm apart, the closest tried, the %g° plates sagged, so the reach is set "
                         "below that.",
                         at.empty() ? 0.0 : *at.begin(), ang);
            s.unsure.push_back(strf("Tips sagged even %g mm apart on %g° plates, so the reach is only a guess below "
                                    "that.",
                                    at.empty() ? 0.0 : *at.begin(), ang));
        } else {
            const double pitch = num(*limit, "max_ok_pitch");
            const bool widest = !at.empty() && pitch >= *at.rbegin();
            v.basis = widest ? Basis::Bound : Basis::Measured;
            v.how = widest ? strf("The %g° plates held at every spacing tried, up to %g mm apart, so tips reach at "
                                  "least %g ÷ √2 = %.1f mm.",
                                  ang, pitch, pitch, r) +
                                 rest
                           : strf("The %g° plates held with tips up to %g mm apart, and not wider, so tips reach "
                                  "%g ÷ √2 = %.1f mm there.",
                                  ang, pitch, pitch, r) +
                                 rest;
            if (widest)
                s.unsure.push_back(strf("Every spacing held on %g° plates, so tips may reach further than %.1f mm.",
                                        ang, r));
        }
        s.values.push_back(v);
    }
    if (isNum(s.profile, "critical_angle_deg")) {
        const double crit = s.profile["critical_angle_deg"].get<double>();
        std::vector<std::pair<double, bool>> wedges;
        for (const json& w : sp.value("wedges", json::array())) wedges.push_back({num(w, "angle_deg"), w.value("ok", false)});
        std::sort(wedges.begin(), wedges.end());
        FitValue v{"Faces that need tips lean", strf("more than %g°", crit), ""};
        const bool all_ok = !wedges.empty() && std::all_of(wedges.begin(), wedges.end(), [](auto& w) { return w.second; });
        const bool none_ok = !wedges.empty() && !wedges.front().second;
        if (all_ok) {
            v.basis = Basis::Bound;
            v.how = strf("Every wedge printed cleanly, up to %g°, the steepest tried.", wedges.back().first);
            s.unsure.push_back(strf("Every wedge printed, so faces may lean further than %g° without tips.", crit));
        } else if (none_ok) {
            v.basis = Basis::Guess;
            v.how = strf("Even the %g° wedge didn't print cleanly, so this is set 5° below it.", wedges.front().first);
            s.unsure.push_back("Even the shallowest wedge failed, so the steepest face without tips is only a guess.");
        } else {
            double failed = crit;
            for (const auto& w : wedges)
                if (!w.second) {
                    failed = w.first;
                    break;
                }
            v.how = strf("Wedges printed cleanly up to %g°, and not at %g°.", crit, failed);
        }
        s.values.push_back(v);
    }
}

void islandValues(FitSummary& s, const json& is) {
    std::vector<std::pair<double, int>> spots;  // diameter, fewest tips that held (0: none)
    for (const json& d : is) {
        int need = 0;
        if (d.contains("min_tips") && d["min_tips"].is_number()) need = d["min_tips"].get<int>();
        spots.push_back({num(d, "island_diameter"), need});
    }
    std::sort(spots.begin(), spots.end());
    if (spots.empty()) return;
    const double biggest = spots.back().first;
    for (int k : {1, 2}) {
        const double upto = num(s.profile, k == 1 ? "island_one_tip_length" : "island_two_tip_length");
        double needed_more = -1;  // the first size, in order, that needed more than k
        for (const auto& [D, need] : spots)
            if (need == 0 || need > k) {
                needed_more = D;
                break;
            }
        FitValue v{k == 1 ? "Spots in mid-air on 1 tip" : "Spots in mid-air on 2 tips",
                   upto > 0 ? strf("up to %g mm", upto) : std::string("none"), ""};
        const char* tips = k == 1 ? "tip" : "tips";
        if (upto <= 0) {
            v.how = strf("Even the smallest spots, %g mm across, needed more than %d %s.", spots.front().first, k, tips);
        } else if (needed_more < 0 || upto >= biggest) {
            v.basis = Basis::Bound;
            v.how = strf("Every spot printed on %d %s, up to %g mm across, the largest tried.", k, tips, biggest);
            s.unsure.push_back(strf("Spots of every size printed on %d %s, so bigger ones may too.", k, tips));
        } else {
            v.how = strf("Spots up to %g mm across printed on %d %s; %g mm ones needed more.", upto, k, tips,
                         needed_more);
        }
        s.values.push_back(v);
    }
}

}  // namespace

std::string oneDecimal(double v) {
    std::string s = strf("%.1f", std::round(v * 10) / 10);
    if (s.size() > 2 && s.compare(s.size() - 2, 2, ".0") == 0) s.resize(s.size() - 2);
    return s;
}

std::string holdText(const TipHold& h) {
    return h.basis == Basis::Bound   ? "at least " + area(h.yours)
           : h.basis == Basis::Guess ? area(h.yours) + ", a guess"
                                     : area(h.yours);
}

FitSummary summarizeFit(const std::vector<fruit::PlateResults>& plates, const fruit::FitOptions& o, const Resin& r) {
    FitSummary s;
    std::vector<fruit::PlateResults> scored;
    std::vector<std::string> from;
    for (const auto& p : plates) {
        const int n = recordedIn(p);
        if (n == 0) continue;
        scored.push_back(p);
        s.kinds.push_back(p.kind);
        from.push_back(strf("the %s plate (%d of %zu scored)", plateWord(p.kind).c_str(), n, p.coupons.size()));
    }
    for (const char* kind : kPlateKinds) {
        const fruit::PlateResults* p = plateOf(scored, kind);
        if (p) {
            const int left = static_cast<int>(p->coupons.size()) - recordedIn(*p);
            if (left > 0)
                s.next.push_back(strf("The rest of the %s plate: %d coupon%s not scored yet.", plateWord(kind).c_str(), left,
                                      left == 1 ? " is" : "s are"));
        } else if (r.plates.count(kind)) {
            s.next.push_back("Score the " + plateWord(kind) + " plate: it's saved, but nothing is scored yet.");
        } else if (!plateMeasured(r.profile, kind)) {
            s.next.push_back("The " + plateWord(kind) + " plate: " + plateWhat(kind) + ".");
        }
    }
    if (scored.empty()) {
        s.from = "Nothing is scored yet.";
        return s;
    }
    s.from = "From " + join(from, " and ") + ".";
    s.profile = fruit::fitProfile(scored, o);
    const json& fits = s.profile["fits"];
    if (fits.contains("tips")) tipValues(s, fits["tips"], o);
    if (fits.contains("spacing")) spacingValues(s, fits["spacing"], plateOf(scored, "spacing"));
    if (fits.contains("islands")) islandValues(s, fits["islands"]);
    if (fits.contains("gaps")) gapValues(s, fits["gaps"]);
    if (fits.contains("peel")) peelValues(s, fits["peel"]);
    if (fits.contains("necks")) neckValues(s, fits["necks"]);
    return s;
}

json mergedProfile(const json& old, const json& fit, const std::vector<std::string>& kinds) {
    static const std::map<std::string, std::vector<const char*>> keys = {
        {"tips", {"capacity_table", "penetration_table", "default_tip_diameter", "minima_tip_diameter"}},
        {"spacing", {"coverage_curve", "critical_angle_deg"}},
        {"islands", {"island_one_tip_length", "island_two_tip_length"}},
        {"gaps", {"shaft_gap"}}};
    json out = old.is_object() ? old : json::object();
    for (const std::string& kind : kinds) {
        const auto it = keys.find(kind);
        if (it == keys.end()) continue;
        for (const char* k : it->second) {
            out.erase(k);
            if (fit.contains(k)) out[k] = fit[k];
        }
        if (fit.contains("fits") && fit["fits"].contains(kind)) {
            if (!out.contains("fits") || !out["fits"].is_object()) out["fits"] = json::object();
            out["fits"][kind] = fit["fits"][kind];
        }
    }
    for (const char* k : {"version", "created_by", "resin", "layer_height", "exposure_s", "vat_temp_c"})
        if (fit.contains(k)) out[k] = fit[k];
    out["notes"] = fit.value("notes", json::array());
    return out;
}

}  // namespace gui
