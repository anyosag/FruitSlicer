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

TEST_CASE("each tip size's hold says how it was found, and bounds and guesses say so", "[fit]") {
    Resin r;
    r.plates["tips"] = PlateFiles{};
    const FitSummary s = summarizeFit({scored("tips", tipsHold)}, options(), r);
    REQUIRE(s.kinds == std::vector<std::string>{"tips"});
    CHECK(s.from == "From the tips plate (96 of 96 scored).");
    REQUIRE(s.holds.size() == 4);

    // 0.4 mm: held at 5 and 10, let go at 20: 70 % of halfway, 15.
    const FitValue* four = valueOf(s, "Hold per 0.4 mm tip");
    REQUIRE(four);
    CHECK(four->value == "10.5 mm²");
    CHECK(four->basis == Basis::Measured);
    CHECK_THAT(four->how, ContainsSubstring("held at up to 10 mm² and let go from 20 mm²"));
    // 0.6 mm never let go: only a floor, 70 % of the heaviest load.
    const FitValue* six = valueOf(s, "Hold per 0.6 mm tip");
    REQUIRE(six);
    CHECK(six->value == "at least 14 mm²");
    CHECK(six->basis == Basis::Bound);
    CHECK_THAT(six->how, StartsWith("All 12 held, even at 20 mm²"));
    // 0.2 mm let go at every load: below 70 % of the lightest, 3.5, but how
    // far below is a guess (half the load).
    const FitValue* two = valueOf(s, "Hold per 0.2 mm tip");
    REQUIRE(two);
    CHECK(two->value == "2.5 mm²");
    CHECK(two->basis == Basis::Guess);
    CHECK_THAT(two->how, ContainsSubstring("below 70 % of it, 3.5 mm²"));

    for (const TipHold& h : s.holds) CHECK(h.borrowed == Approx(fruit::CapacityModel{}.capacity(h.tip)));
    CHECK(s.holds[3].basis == Basis::Bound);
    CHECK(s.holds[0].basis == Basis::Guess);
    CHECK(holdText(s.holds[3]) == "at least 14 mm²");
    CHECK(holdText(s.holds[0]) == "2.5 mm², a guess");
    CHECK(holdText(s.holds[2]) == "10.5 mm²");

    // The usual tip: the smallest mark among those holding 4 mm² (0.3 and
    // 0.4 both mark 3; the smaller wins).
    const FitValue* usual = valueOf(s, "Usual tip size");
    REQUIRE(usual);
    CHECK(usual->value == "0.3 mm");
    CHECK_THAT(usual->how, ContainsSubstring("3.0 on average"));

    // Contact depth: both depths held alike and marked alike here, so the
    // shallower is kept; a size that never held has no depth to choose.
    const FitValue* depth = valueOf(s, "Contact depth for 0.4 mm tips");
    REQUIRE(depth);
    CHECK(depth->value == "0.1 mm");
    CHECK(depth->how == "Each depth held 4 of 6 and left the same mark, so the shallower one is kept.");
    CHECK_FALSE(valueOf(s, "Contact depth for 0.2 mm tips"));

    REQUIRE(s.unsure.size() == 2);
    CHECK(s.unsure[0] == "0.2 mm tips let go even at the lightest load, 5 mm², so their 2.5 mm² is only a guess.");
    CHECK_THAT(s.unsure[1], ContainsSubstring("0.6 mm tips never let go"));
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
    CHECK(s.from == "From the tips plate (20 of 96 scored).");
    REQUIRE(s.next.size() == 4);
    CHECK(s.next[0] == "The rest of the tips plate: 76 coupons are not scored yet.");
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

TEST_CASE("the tips plate in two sets: by the hinge and far from it, and how much harder the far side pulls", "[fit][peel]") {
    fruit::TipsPlateOptions o;
    o.peel_axis = {0, -1};  // hinged at the back (+y): the far side is the front
    const auto printer = fruit::PrinterProfile::saturn4Ultra16k();
    const fruit::CalibrationPlate plate = fruit::tipsPlate(o, printer);
    REQUIRE(plate.coupons.size() == 120);  // 48 by the hinge, 48 far from it, 24 necked in the middle
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
    CHECK(hinge == 48);
    CHECK(far == 48);
    CHECK(middle == 24);
    // Scores from a resin whose tips hold, at the hinge's edge, 6 / 9 / 12 / 18 mm² (0.2 / 0.3 / 0.4 / 0.6 mm),
    // where the peel pulls 40 % harder at the far edge than at the hinge's: a disc stays on while its load,
    // times the pull where it is, is under its tips' hold.
    const double true_bias = 0.4;
    const std::map<double, double> hold{{0.2, 6}, {0.3, 9}, {0.4, 12}, {0.6, 18}};
    fruit::PlateResults res;
    res.kind = "tips";
    res.settings = plate.settings;
    for (const auto& c : plate.coupons) {
        fruit::CouponResult r;
        r.id = c.id;
        r.params = c.params;
        r.recorded = true;
        const double pull = 1 + true_bias * 0.5 * (c.params["peel_t"].get<double>() + 1);
        r.ok = c.params["load_per_tip_mm2"].get<double>() * pull < hold.at(c.params["tip_diameter"].get<double>());
        r.scar = 2;
        res.coupons.push_back(r);
    }
    Resin resin;
    const FitSummary s = summarizeFit({res}, options(), resin);
    REQUIRE(s.profile.contains("peel_bias"));
    const double bias = s.profile["peel_bias"].get<double>();
    CAPTURE(bias, s.profile["fits"]["peel"].dump());
    // The loads step about 1.32 times, so one plate brackets the bias rather than pins it: the true one lies in
    // the range every disc agrees with, and the middle of that range is kept.
    const auto range = s.profile["fits"]["peel"]["bias_range"];
    CHECK(range[0].get<double>() <= true_bias);
    CHECK(range[1].get<double>() >= true_bias);
    CHECK(bias == Approx((range[0].get<double>() + range[1].get<double>()) / 2).margin(0.011));
    CHECK(s.profile["fits"]["peel"]["misfits"] == 0);
    const FitValue* v = valueOf(s, "Extra hold away from the hinge");
    REQUIRE(v);
    // Used: the planner's extra demand far from the hinge, and the replay's, are the measured one.
    fruit::SupportConfig cfg;
    fruit::TreeConfig tc;
    fruit::SupportChoices ch;
    ch.hinge = fruit::HingeSide::PlusY;
    fruit::resolveSettings(&s.profile, ch, cfg, &tc);
    CHECK(cfg.peel_bias == Approx(bias));
}

TEST_CASE("breakaway necks: kept where they leave less of a mark and hold as well, and drawn on the heads", "[fit][neck]") {
    const fruit::CalibrationPlate plate = fruit::tipsPlate(fruit::TipsPlateOptions{}, fruit::PrinterProfile::saturn4Ultra16k());
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
