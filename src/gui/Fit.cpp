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

const fruit::PlateResults* plateOf(const std::vector<fruit::PlateResults>& plates, const std::string& kind) {
    for (const auto& p : plates)
        if (p.kind == kind) return &p;
    return nullptr;
}

void tipValues(FitSummary& s, const json& tips, const fruit::FitOptions& o) {
    const fruit::CapacityModel borrowed;
    for (const json& t : tips) {
        TipHold h;
        h.tip = num(t, "tip_diameter");
        h.yours = num(t, "capacity_mm2");
        h.borrowed = borrowed.capacity(h.tip);
        const std::string basis = t.value("basis", std::string());
        const int n = t.value("coupons", 0), held = t.value("held", 0);
        const double heaviest_held = num(t, "max_held_mm2", -1), lightest_failed = num(t, "min_failed_mm2", -1);
        if (basis == "lower bound") {
            h.basis = Basis::Bound;
            h.how = strf("All %d held, even at %s, the heaviest load tested. ", n, area(heaviest_held).c_str()) +
                    "Half would let go somewhere above it, so this is only a floor: 70 % of that load.";
            s.unsure.push_back(strf("%g mm tips never let go, so their hold is at least %s.", h.tip,
                                    area(h.yours).c_str()));
        } else if (basis == "upper bound") {
            h.basis = Basis::Guess;
            h.how = strf("None of %d held, even at %s, the lightest load tested, ", n, area(lightest_failed).c_str()) +
                    "so the hold is below 70 % of it, " + area(0.7 * lightest_failed) +
                    ". How far below is unknown: this is a guess, half that load.";
            s.unsure.push_back(strf("%g mm tips let go even at the lightest load, %s, so their %s is only a guess.",
                                    h.tip, area(lightest_failed).c_str(), area(h.yours).c_str()));
        } else if (basis == "gap midpoint") {
            h.how = strf("They held at up to %s and let go from %s. ", area(heaviest_held).c_str(),
                         area(lightest_failed).c_str()) +
                    "FruitSlicer keeps 70 % of halfway between.";
        } else if (basis == "logistic") {
            h.how = strf("%d of %d held. Half would let go at about %s, ", held, n,
                         area(num(t, "load_50pct")).c_str()) +
                    "and FruitSlicer keeps 70 % of that.";
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
        int held_any = 0;
        for (const json& p : depths) held_any += p.value("held", 0);
        if (held_any == 0) continue;  // none held at any depth: nothing to choose by
        std::string how;
        if (!chosen || others.empty()) {
            how = "The only depth tested.";
        } else {
            const int ch = chosen->value("held", 0), cn = chosen->value("coupons", 0);
            bool same = true;
            for (const json* p : others) same &= p->value("held", 0) * cn == ch * p->value("coupons", 0);
            const json& other = *others.front();
            if (!same) {
                how = strf("It held %d of %d", ch, cn);
                for (const json* p : others)
                    how += strf("; %g mm held %d of %d", num(*p, "penetration"), p->value("held", 0),
                                p->value("coupons", 0));
                how += ".";
            } else if (isNum(*chosen, "mean_scar") && isNum(other, "mean_scar") &&
                       num(*chosen, "mean_scar") < num(other, "mean_scar") - 1e-9) {
                how = strf("Each depth held %d of %d; this one left the smaller mark (%.1f against %.1f).", ch, cn,
                           num(*chosen, "mean_scar"), num(other, "mean_scar"));
            } else if (isNum(*chosen, "mean_scar") && isNum(other, "mean_scar")) {
                how = strf("Each depth held %d of %d and left the same mark, so the shallower one is kept.", ch, cn);
            } else {
                how = strf("Each depth held %d of %d, so the shallower one is kept.", ch, cn);
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
        from.push_back(strf("the %s plate (%d of %zu scored)", p.kind.c_str(), n, p.coupons.size()));
    }
    for (const char* kind : kPlateKinds) {
        const fruit::PlateResults* p = plateOf(scored, kind);
        if (p) {
            const int left = static_cast<int>(p->coupons.size()) - recordedIn(*p);
            if (left > 0)
                s.next.push_back(strf("The rest of the %s plate: %d coupon%s not scored yet.", kind, left,
                                      left == 1 ? " is" : "s are"));
        } else if (r.plates.count(kind)) {
            s.next.push_back("Score the " + std::string(kind) + " plate: it's saved, but nothing is scored yet.");
        } else if (!plateMeasured(r.profile, kind)) {
            s.next.push_back("The " + std::string(kind) + " plate: " + plateWhat(kind) + ".");
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
    return s;
}

json mergedProfile(const json& old, const json& fit, const std::vector<std::string>& kinds) {
    static const std::map<std::string, std::vector<const char*>> keys = {
        {"tips", {"capacity_table", "penetration_table", "default_tip_diameter", "minima_tip_diameter"}},
        {"spacing", {"coverage_curve", "critical_angle_deg"}},
        {"islands", {"island_one_tip_length", "island_two_tip_length"}}};
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
