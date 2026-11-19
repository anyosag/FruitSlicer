// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The Fit page: a resin's scored plates turned into its values, each with
// how it was found, and what the plates couldn't pin down.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <algorithm>
#include <map>
#include <cmath>
#include <string>

#include "Fit.h"
#include "Plates.h"
#include "calibration/CalibrationFit.h"
#include "support/SupportTree.h"
#include "support/SupportGeometry.h"
#include "support/SupportSettings.h"

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using Catch::Matchers::EndsWith;
using namespace gui;

namespace {

// A plate scored as by someone with a resin that behaves as `ok` says;
// only the first `n` coupons are scored (all if n < 0).
template <class Ok>
fruit::PlateResults scored(const std::string& kind, Ok ok, int n = -1) {
    const fruit::CalibrationPlate p = makePlate(kind, fruit::PrinterProfile::saturn4Ultra16k());
    fruit::PlateResults r;
    r.kind = kind;
    r.settings = p.settings;
    for (const auto& c : p.coupons) {
        fruit::CouponResult x;
        x.id = c.id;
        x.params = c.params;
        if (n < 0 || static_cast<int>(r.coupons.size()) < n) {
            x.recorded = true;
            x.ok = ok(c.params);
            x.scar = x.ok ? std::clamp(static_cast<int>(std::lround(1 + 6 * c.params.value("tip_diameter", 0.4))), 1, 5)
                          : 0;
        }
        r.coupons.push_back(x);
    }
    return r;
}

// Tips hold 12 mm² at 0.4 mm, scaling with tip area.
bool tipsHold(const nlohmann::json& p) {
    const double d = p["tip_diameter"].get<double>();
    return p["load_per_tip_mm2"].get<double>() < 12.0 * (d / 0.4) * (d / 0.4);
}

const FitValue* valueOf(const FitSummary& s, const std::string& label) {
    for (const FitValue& v : s.values)
        if (v.label == label) return &v;
    return nullptr;
}

fruit::FitOptions options() {
    fruit::FitOptions o;
    o.resin = "Test resin";
    o.layer_height = 0.05;
    return o;
}

}  // namespace

// Tips that hold `strength` mm² of layer per mm² they touch, times `times(d)` for a size that doesn't follow it.
template <class Times>
fruit::PlateResults scoredContact(double strength, Times times) {
    return scored("tips", [&](const nlohmann::json& p) {
        fruit::PinheadSpec sp;
        sp.tip_diameter = p["tip_diameter"].get<double>();
        sp.penetration = p["penetration"].get<double>();
        sp.shaft_diameter = fruit::shaftDiameterFor(sp.tip_diameter);
        sp.neck_back = p.value("neck_back", 0.0);
        sp.neck_ratio = p.value("neck_ratio", 0.6);
        const double cd = fruit::contactDiameter(sp);
        return p["load_per_tip_mm2"].get<double>() < strength * times(sp.tip_diameter) * 3.14159265358979 * cd * cd / 4;
    });
}

TEST_CASE("each tip size's hold says how it was found, and bounds and guesses say so", "[fit]") {
    Resin r;
    r.plates["tips"] = PlateFiles{};
    // Tips that hold 45 mm² of layer per mm² they touch, but 0.6 mm ones twice that.
    const FitSummary s = summarizeFit({scoredContact(45, [](double d) { return d > 0.5 ? 2.0 : 1.0; })}, options(), r);
    REQUIRE(s.kinds == std::vector<std::string>{"tips"});
    CHECK(s.from == "From the tips plate (128 of 128 scored).");
    REQUIRE(s.holds.size() == 4);
    // One strength from the sizes that follow it (0.6 mm 0.2 mm deep left out, its discs held past it), within a
    // step of the true 45.
    const nlohmann::json& model = s.profile["fits"]["tips_model"];
    CHECK(model["coupons"].get<int>() == 94);
    CHECK(model["strength_50pct"].get<double>() > 45 / fruit::kOneLoadStep);
    CHECK(model["strength_50pct"].get<double>() < 45 * fruit::kOneLoadStep);
    // 0.4 mm: that strength times what it touches 0.2 mm deep, 70 % of it.
    const FitValue* four = valueOf(s, "Hold per 0.4 mm tip");
    REQUIRE(four);
    CHECK(four->basis == Basis::Measured);
    CHECK_THAT(four->how, StartsWith("From 94 coupons: a tip holds about 48 mm² of layer for each mm² of the part it "
                                     "touches before half let go. This one, 0.2 mm deep, touches 0.13 mm²"));
    CHECK_THAT(four->how, EndsWith("Its own coupons there held 6 of 10."));  // 2.5, 3.5 and 5 on each side
    // 0.2 mm never held at 0.2 mm deep... yet its hold is measured, through the others.
    const FitValue* two = valueOf(s, "Hold per 0.2 mm tip");
    REQUIRE(two);
    CHECK(two->basis == Basis::Measured);
    CHECK(two->value == "1.5 mm²");
    // 0.6 mm 0.2 mm deep held everything, well past the strength: its own floor, said so.
    const FitValue* six = valueOf(s, "Hold per 0.6 mm tip");
    REQUIRE(six);
    CHECK(six->basis == Basis::Bound);
    CHECK(six->value == "at least 14 mm²");
    CHECK_THAT(six->how, StartsWith("Its 10 coupons 0.2 mm deep all held, even at 20 mm²"));
    for (const TipHold& h : s.holds) CHECK(h.borrowed == Approx(fruit::CapacityModel{}.capacity(h.tip)));

    // The usual tip: the least mark among sizes holding a grid cell's layer (1 mm²).
    const FitValue* usual = valueOf(s, "Usual tip size");
    REQUIRE(usual);
    CHECK(usual->value == "0.2 mm");
    // Depth: marks alike, so the deeper one, which touches more.
    const FitValue* depth = valueOf(s, "Contact depth for 0.4 mm tips");
    REQUIRE(depth);
    CHECK(depth->value == "0.2 mm");
    CHECK(depth->how == "The marks were within half a point (3.0 and 3.0), so the deeper one is kept: it touches more "
                        "of the part (0.13 mm² against 0.09), so it holds more.");
    REQUIRE(s.unsure.size() == 1);
    CHECK(s.unsure[0] == "0.6 mm tips never let go, so their hold is at least 14 mm².");

    // Every disc held: floors, said so once.
    const FitSummary all = summarizeFit({scoredContact(1000, [](double) { return 1.0; })}, options(), r);
    CHECK(all.profile["fits"]["tips_model"]["basis"] == "lower bound");
    CHECK(all.profile["capacity_measured"] == false);
    for (const TipHold& h : all.holds) CHECK(h.basis == Basis::Bound);
    CHECK(std::count(all.unsure.begin(), all.unsure.end(),
                     "No disc on the tips plate let go, so each hold is only a floor: 70 % of the heaviest load.") == 1);
    // ...and a floor isn't trusted nearer to where tips let go: no tighter limit on each tip's own load.
    fruit::SupportConfig cfg;
    fruit::applyProfile(all.profile, cfg, nullptr, nullptr);
    CHECK(cfg.tip_load_limit == Approx(fruit::SupportConfig{}.tip_load_limit));
    // Every disc let go: guesses.
    const FitSummary none = summarizeFit({scoredContact(1, [](double) { return 1.0; })}, options(), r);
    CHECK(none.profile["fits"]["tips_model"]["basis"] == "upper bound");
    for (const TipHold& h : none.holds) CHECK(h.basis == Basis::Guess);
    CHECK_THAT(holdText(none.holds[0]), EndsWith(", a guess"));

    // The other plates are worth printing next.
    REQUIRE(s.next.size() == 3);
    CHECK_THAT(s.next[0], StartsWith("The spacing plate:"));
    CHECK_THAT(s.next[1], StartsWith("The islands plate:"));
    CHECK_THAT(s.next[2], StartsWith("The gap plate:"));
}

TEST_CASE("spacing and islands values say how they were found", "[fit]") {
    Resin r;
    // Plates held up to 3 mm apart at every angle; wedges printed up to 50°.
    const auto spacing = scored("spacing", [](const nlohmann::json& p) {
        if (p["kind"] == "wedge") return p["angle_deg"].get<double>() <= 50;
        return p["spacing_mm"].get<double>() <= 3.0;
    });
    // Spots need 1 tip up to 1 mm, 2 up to 3 mm, 3 beyond.
    const auto islands = scored("islands", [](const nlohmann::json& p) {
        const double D = p["island_diameter"].get<double>();
        return p["tips"].get<int>() >= (D <= 1.0 ? 1 : D <= 3.0 ? 2 : 3);
    });
    const FitSummary s = summarizeFit({spacing, islands}, options(), r);
    CHECK(s.kinds == std::vector<std::string>{"spacing", "islands"});
    CHECK(s.holds.empty());

    const FitValue* reach = valueOf(s, "Tip reach");
    REQUIRE(reach);
    CHECK_THAT(reach->value, StartsWith("2.1 → "));
    CHECK(reach->basis == Basis::Measured);
    CHECK_THAT(reach->how, ContainsSubstring("up to 3 mm apart, and not wider"));
    const FitValue* faces = valueOf(s, "Faces that need tips lean");
    REQUIRE(faces);
    CHECK(faces->value == "more than 50°");
    CHECK(faces->how == "Wedges printed cleanly up to 50°, and not at 55°.");

    const FitValue* one = valueOf(s, "Spots in mid-air on 1 tip");
    REQUIRE(one);
    CHECK(one->value == "up to 1 mm");
    CHECK(one->how == "Spots up to 1 mm across printed on 1 tip; 2 mm ones needed more.");
    const FitValue* two = valueOf(s, "Spots in mid-air on 2 tips");
    REQUIRE(two);
    CHECK(two->value == "up to 3 mm");
    CHECK(s.unsure.empty());
    REQUIRE(s.next.size() == 2);
    CHECK_THAT(s.next[0], StartsWith("The tips plate:"));
    CHECK_THAT(s.next[1], StartsWith("The gap plate:"));
}

TEST_CASE("bounds on the spacing plate are said to be bounds", "[fit]") {
    Resin r;
    const auto spacing = scored("spacing", [](const nlohmann::json&) { return true; });
    const FitSummary s = summarizeFit({spacing}, options(), r);
    const FitValue* reach = valueOf(s, "Tip reach");
    REQUIRE(reach);
    CHECK(reach->basis == Basis::Bound);
    CHECK_THAT(reach->how, ContainsSubstring("held at every spacing tried, up to 6 mm apart"));
    const FitValue* faces = valueOf(s, "Faces that need tips lean");
    REQUIRE(faces);
    CHECK(faces->basis == Basis::Bound);
    CHECK(faces->how == "Every wedge printed cleanly, up to 65°, the steepest tried.");
    CHECK(s.unsure.size() == 2);
}

TEST_CASE("a plate scored in part fits what is scored, and says the rest is left", "[fit]") {
    Resin r;
    r.plates["tips"] = PlateFiles{};
    r.plates["spacing"] = PlateFiles{};  // saved, nothing scored
    const FitSummary s = summarizeFit({scored("tips", tipsHold, 20), scored("spacing", tipsHold, 0)}, options(), r);
    CHECK(s.kinds == std::vector<std::string>{"tips"});
    CHECK(s.from == "From the tips plate (20 of 128 scored).");
    REQUIRE(s.next.size() == 4);
    CHECK(s.next[0] == "The rest of the tips plate: 108 coupons are not scored yet.");
    CHECK(s.next[1] == "Score the spacing plate: it's saved, but nothing is scored yet.");
    CHECK_THAT(s.next[2], StartsWith("The islands plate:"));
    CHECK_THAT(s.next[3], StartsWith("The gap plate:"));

    const FitSummary none = summarizeFit({scored("tips", tipsHold, 0)}, options(), r);
    CHECK(none.kinds.empty());
    CHECK(none.from == "Nothing is scored yet.");
    CHECK(none.profile.is_null());
    CHECK(none.values.empty());
}

TEST_CASE("a fit replaces only its own plates' values in a resin", "[fit]") {
    // A resin whose spacing values came from an imported profile.
    const nlohmann::json old = {{"resin", "Old"},
                                {"coverage_curve", {{0, 2.0}, {40, 4.0}}},
                                {"critical_angle_deg", 40},
                                {"capacity_table", {{0.4, 3.0}}},
                                {"fits", {{"spacing", {{"radius0", 2.0}}}}}};
    Resin r;
    const FitSummary s = summarizeFit({scored("tips", tipsHold)}, options(), r);
    const nlohmann::json merged = mergedProfile(old, s.profile, s.kinds);
    CHECK(merged["capacity_table"] == s.profile["capacity_table"]);
    CHECK(merged["default_tip_diameter"] == s.profile["default_tip_diameter"]);
    CHECK(merged["coverage_curve"] == old["coverage_curve"]);  // not refitted: kept
    CHECK(merged["critical_angle_deg"] == 40);
    CHECK(merged["fits"]["spacing"] == old["fits"]["spacing"]);
    CHECK(merged["fits"]["tips"] == s.profile["fits"]["tips"]);
    CHECK(merged["resin"] == "Test resin");
    CHECK(plateMeasured(merged, "tips"));
    CHECK(plateMeasured(merged, "spacing"));
    CHECK_FALSE(plateMeasured(merged, "islands"));
    // Merging again changes nothing: the resin's values are the fit's.
    CHECK(mergedProfile(merged, s.profile, s.kinds) == merged);
    // A tips fit with no usual tip drops the old one rather than keep a stale value.
    nlohmann::json no_default = s.profile;
    no_default.erase("default_tip_diameter");
    const nlohmann::json stale = {{"default_tip_diameter", 0.6}};
    CHECK_FALSE(mergedProfile(stale, no_default, {"tips"}).contains("default_tip_diameter"));
}

TEST_CASE("numbers are rounded the same way everywhere", "[fit]") {
    CHECK(oneDecimal(5.25) == "5.3");  // half away from zero, in the chart and the text alike
    CHECK(oneDecimal(10.5) == "10.5");
    CHECK(oneDecimal(14.0) == "14");
    CHECK(oneDecimal(2.775) == "2.8");
}

TEST_CASE("the gap plate gives the narrowest gap every shaft came off cleanly from, and a pixel", "[fit][gaps]") {
    const fruit::CalibrationPlate plate = fruit::gapsPlate(fruit::GapsPlateOptions{}, fruit::PrinterProfile::saturn4Ultra16k());
    CHECK(plate.coupons.size() == 48);  // 6 gaps × 2 lengths × wall and cup × 2
    fruit::PlateResults res;
    res.kind = "gaps";
    res.settings = plate.settings;
    for (const auto& c : plate.coupons) {
        fruit::CouponResult r;
        r.id = c.id;
        r.params = c.params;
        r.recorded = true;
        const double g = c.params["gap"].get<double>();
        // Stuck up to 0.20 mm; at 0.25 the long shaft in the cup stuck too; from 0.30 all came off.
        r.ok = g > 0.26 || (g > 0.21 && !(c.params["place"] == "cup" && c.params["height"].get<double>() > 10));
        res.coupons.push_back(r);
    }
    Resin resin;
    const FitSummary s = summarizeFit({res}, options(), resin);
    const double pixel = fruit::PrinterProfile::saturn4Ultra16k().pixelY();
    REQUIRE(s.profile.contains("shaft_gap"));
    CHECK(s.profile["shaft_gap"].get<double>() == Approx(0.30 + std::max(pixel, fruit::PrinterProfile::saturn4Ultra16k().pixelX())));
    const FitValue* v = valueOf(s, "Gap to the model");
    REQUIRE(v);
    CHECK(v->value == "0.32 mm");
    CHECK_THAT(v->how, StartsWith("Shafts 0.3 mm from the model came off cleanly, and so did every one further; at 0.25 mm one stuck."));
    // The resin's gap is what supports keep then.
    fruit::SupportConfig cfg;
    fruit::TreeConfig tc;
    fruit::applyProfile(s.profile, cfg, &tc, nullptr);
    CHECK(tc.clearance == Approx(s.profile["shaft_gap"].get<double>()));
}

// Scores from a resin whose tips hold `strength` mm² of layer per mm² they touch at the hinge's edge, where the
// peel pulls `bias` harder at the far edge: a disc stays on while its load, times the pull where it is, is under
// its tips' hold.
fruit::PlateResults scoredSides(const fruit::CalibrationPlate& plate, double strength, double bias) {
    fruit::PlateResults res;
    res.kind = "tips";
    res.settings = plate.settings;
    for (const auto& c : plate.coupons) {
        fruit::CouponResult r;
        r.id = c.id;
        r.params = c.params;
        r.recorded = true;
        fruit::PinheadSpec sp;
        sp.tip_diameter = c.params["tip_diameter"].get<double>();
        sp.penetration = c.params["penetration"].get<double>();
        sp.shaft_diameter = fruit::shaftDiameterFor(sp.tip_diameter);
        sp.neck_back = c.params.value("neck_back", 0.0);
        sp.neck_ratio = c.params.value("neck_ratio", 0.6);
        const double cd = fruit::contactDiameter(sp), touch = 3.14159265358979 * cd * cd / 4;
        const double pull = 1 + bias * 0.5 * (c.params["peel_t"].get<double>() + 1);
        r.ok = c.params["load_per_tip_mm2"].get<double>() * pull < strength * touch;
        r.scar = r.ok ? 2 : 0;
        res.coupons.push_back(r);
    }
    return res;
}

TEST_CASE("the tips plate in two sets: by the hinge and far from it, and how much harder the far side pulls", "[fit][peel]") {
    fruit::TipsPlateOptions o;
    o.peel_axis = {0, -1};  // hinged at the back (+y): the far side is the front
    const auto printer = fruit::PrinterProfile::saturn4Ultra16k();
    const fruit::CalibrationPlate plate = fruit::tipsPlate(o, printer);
    REQUIRE(plate.coupons.size() == 128);  // 52 by the hinge, 52 far from it, 24 necked in the middle
    int hinge = 0, far = 0, middle = 0;
    for (const auto& c : plate.coupons) {
        if (c.params["side"] == "middle") {
            ++middle;
            CHECK(c.params.value("neck_back", 0.0) > 0);
            continue;
        }
        const bool h = c.params["side"] == "hinge";
        (h ? hinge : far) += 1;
        // Each set along its own edge: by the hinge, at the back; far from it, at the front.
        if (h) CHECK(c.center.y > 0);
        else CHECK(c.center.y < 0);
        CHECK((c.params["peel_t"].get<double>() < 0) == h);
    }
    CHECK(hinge == 52);
    CHECK(far == 52);
    CHECK(middle == 24);
    const double true_bias = 0.4;
    Resin resin;
    const FitSummary s = summarizeFit({scoredSides(plate, 45, true_bias)}, options(), resin);
    const nlohmann::json& peel = s.profile["fits"]["peel"];
    CAPTURE(peel.dump());
    // Its loads step 1.41 times, one coupon a step and side: the biases that fit as well spread wider than
    // kPeelBiasPinned, so none is kept and the planner's 25 % stays, though the range holds the true one.
    CHECK(peel["bias_range"][0].get<double>() <= true_bias);
    CHECK(peel["bias_range"][1].get<double>() >= true_bias);
    CHECK(peel["bias_range"][1].get<double>() - peel["bias_range"][0].get<double>() >= fruit::kPeelBiasPinned);
    CHECK(peel["unpinned"] == true);
    CHECK_FALSE(s.profile.contains("peel_bias"));
    CHECK_FALSE(valueOf(s, "Extra hold away from the hinge"));
    fruit::SupportConfig cfg;
    fruit::TreeConfig tc;
    fruit::SupportChoices ch;
    ch.hinge = fruit::HingeSide::PlusY;
    fruit::resolveSettings(&s.profile, ch, cfg, &tc);
    CHECK(cfg.peel_bias == Approx(fruit::kAssumedPeelBias));

    // A plate made for it, with steps of 1.1 times around the hold on one size, pins it: kept and used.
    fruit::TipsPlateOptions fine = o;
    fine.ladders = {{0.4, 0.2, {3.4, 3.7, 4.1, 4.5, 5.0, 5.5, 6.0, 6.6, 7.3, 8.0, 8.8, 9.7}}};
    fine.tip_diameters = {0.4};
    fine.neck_sets.clear();
    const FitSummary f = summarizeFit({scoredSides(fruit::tipsPlate(fine, printer), 45, true_bias)}, options(), resin);
    CAPTURE(f.profile["fits"]["peel"].dump());
    REQUIRE(f.profile.contains("peel_bias"));
    const double bias = f.profile["peel_bias"].get<double>();
    const auto range = f.profile["fits"]["peel"]["bias_range"];
    CHECK(range[0].get<double>() <= true_bias);
    CHECK(range[1].get<double>() >= true_bias);
    CHECK(range[1].get<double>() - range[0].get<double>() < fruit::kPeelBiasPinned);
    CHECK(bias == Approx((range[0].get<double>() + range[1].get<double>()) / 2).margin(0.011));
    CHECK(f.profile["fits"]["peel"]["misfits"] == 0);
    REQUIRE(valueOf(f, "Extra hold away from the hinge"));
    fruit::resolveSettings(&f.profile, ch, cfg, &tc);
    CHECK(cfg.peel_bias == Approx(bias));
}

TEST_CASE("tips plate in two sets: a peel bias every value fits is left out", "[fit][peel]") {
    fruit::TipsPlateOptions o;
    o.peel_axis = {0, -1};  // hinged at the back, and the loads, as that print was
    o.ladders.clear();
    for (double d : {0.2, 0.3, 0.4, 0.6})
        for (double pen : {0.1, 0.2}) o.ladders.push_back({d, pen, {5, 6.6, 8.7, 11.5, 15.2, 20}});
    o.neck_sets.clear();
    for (double d : {0.2, 0.3, 0.4, 0.6}) o.neck_sets.push_back({d, {6.6, 11.5, 20}});
    o.neck_copies = 1;
    const fruit::CalibrationPlate plate = fruit::tipsPlate(o, fruit::PrinterProfile::saturn4Ultra16k());
    REQUIRE(plate.coupons.size() == 120);
    // A real print (2026-11-18, 1.5 s at 0.03 mm): 14 of 120 discs stayed on, 11 of them on 0.6 mm tips.
    // By side, tip, contact depth, load per tip and neck.
    struct Held { const char* side; double tip, depth, load, neck; };
    const std::vector<Held> held{
        {"hinge", 0.6, 0.2, 11.5, 0}, {"hinge", 0.6, 0.2, 8.7, 0}, {"hinge", 0.6, 0.2, 6.6, 0},
        {"hinge", 0.3, 0.2, 5.0, 0},  {"hinge", 0.6, 0.2, 5.0, 0}, {"hinge", 0.6, 0.1, 5.0, 0},
        {"hinge", 0.4, 0.2, 5.0, 0},  {"far", 0.6, 0.2, 6.6, 0},   {"far", 0.6, 0.2, 8.7, 0},
        {"far", 0.6, 0.2, 5.0, 0},    {"far", 0.3, 0.2, 20.0, 0},  {"far", 0.6, 0.1, 5.0, 0},
        {"middle", 0.6, 0.1, 6.6, 0.25}, {"middle", 0.6, 0.1, 6.6, 0.1}};
    fruit::PlateResults res;
    res.kind = "tips";
    res.settings = plate.settings;
    int ok = 0;
    for (const auto& c : plate.coupons) {
        fruit::CouponResult r;
        r.id = c.id;
        r.params = c.params;
        r.recorded = true;
        for (const Held& h : held)
            r.ok = r.ok || (c.params["side"] == h.side && c.params["tip_diameter"].get<double>() == Approx(h.tip) &&
                            c.params["penetration"].get<double>() == Approx(h.depth) &&
                            c.params["load_per_tip_mm2"].get<double>() == Approx(h.load) &&
                            c.params.value("neck_back", 0.0) == Approx(h.neck));
        if (r.ok) r.scar = 4, ++ok;
        res.coupons.push_back(r);
    }
    REQUIRE(ok == 14);
    Resin resin;
    const FitSummary s = summarizeFit({res}, options(), resin);
    CAPTURE(s.profile["fits"]["peel"].dump());
    // Every bias from the first step to the last fits as well: nothing to keep, the planner's 25 % stays.
    CHECK(s.profile["fits"]["peel"]["unpinned"] == true);
    CHECK_FALSE(s.profile.contains("peel_bias"));
    CHECK_FALSE(valueOf(s, "Extra hold away from the hinge"));
    fruit::SupportConfig cfg;
    fruit::TreeConfig tc;
    fruit::SupportChoices ch;
    ch.hinge = fruit::HingeSide::PlusY;
    fruit::resolveSettings(&s.profile, ch, cfg, &tc);
    CHECK(cfg.peel_bias == Approx(0.25));
    // The tips themselves are still fitted: 0.6 mm tips hold, by the logistic, about 8 mm².
    CHECK(s.profile.contains("capacity_table"));
}

TEST_CASE("breakaway necks: kept where they leave less of a mark and hold as well, and drawn on the heads", "[fit][neck]") {
    // Necks on every size, at loads its shallower ladder has too.
    fruit::TipsPlateOptions o;
    o.neck_sets = {{0.2, {1.25, 1.8, 2.5}}, {0.3, {1.8, 2.5, 3.5}}, {0.4, {1.8, 2.5, 3.5}}, {0.6, {2.5, 3.5, 5}}};
    o.neck_copies = 1;
    const fruit::CalibrationPlate plate = fruit::tipsPlate(o, fruit::PrinterProfile::saturn4Ultra16k());
    // A resin where tips hold 12 mm² (any size, necked or not, on either depth), plain tips leave a 4 (a dent),
    // a neck 0.1 mm out leaves a 2, and one 0.25 mm out a 3 (the stub shows); except 0.2 mm tips, whose necks
    // let go at every load.
    fruit::PlateResults res;
    res.kind = "tips";
    res.settings = plate.settings;
    for (const auto& c : plate.coupons) {
        fruit::CouponResult r;
        r.id = c.id;
        r.params = c.params;
        r.recorded = true;
        const double back = c.params.value("neck_back", 0.0), d = c.params["tip_diameter"].get<double>();
        r.ok = c.params["load_per_tip_mm2"].get<double>() < 12 && !(back > 0 && d < 0.25);
        r.scar = r.ok ? (back == 0 ? 4 : back < 0.2 ? 2 : 3) : 0;
        res.coupons.push_back(r);
    }
    Resin resin;
    const FitSummary s = summarizeFit({res}, options(), resin);
    REQUIRE(s.profile.contains("neck_table"));
    std::map<double, double> kept;
    for (const auto& e : s.profile["neck_table"]) kept[e[0].get<double>()] = e[1].get<double>();
    CHECK(kept.count(0.2) == 0);  // its necks let go: plain it stays
    for (const double d : {0.3, 0.4, 0.6}) {
        CAPTURE(d);
        REQUIRE(kept.count(d) == 1);
        CHECK(kept[d] == Approx(0.1));  // the shallower neck: the least mark
    }
    // The tips' holds come from plain coupons only (a necked 0.2 mm one letting go doesn't lower it).
    fruit::SupportConfig cfg;
    fruit::TreeConfig tc;
    fruit::applyProfile(s.profile, cfg, &tc, nullptr);
    REQUIRE(tc.neck_table.size() == 3);
    // Drawn: a head with a neck is thinner just out from the surface than one without.
    fruit::PinheadSpec plain;
    plain.tip_diameter = 0.4;
    plain.penetration = 0.1;
    fruit::PinheadSpec necked = plain;
    necked.neck_back = 0.1;
    fruit::TriangleMesh a, b;
    fruit::appendPinhead(a, {0, 0, 5}, {0, 0, -1}, plain, {}, false);
    fruit::appendPinhead(b, {0, 0, 5}, {0, 0, -1}, necked, {}, false);
    auto widthAt = [](const fruit::TriangleMesh& m, double z) {
        double w = 0;
        for (const auto& v : m.vertices) if (std::abs(v.z - z) < 0.02) w = std::max(w, std::hypot(v.x, v.y));
        return w;
    };
    CHECK(widthAt(b, 4.9) < widthAt(a, 4.9));
    CHECK(widthAt(b, 4.9) == Approx(0.6 * 0.2).margin(0.01));
}
