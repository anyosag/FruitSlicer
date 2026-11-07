// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The Fit page: a resin's scored plates turned into its values, each with
// how it was found, and what the plates couldn't pin down.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <algorithm>
#include <cmath>
#include <string>

#include "Fit.h"
#include "Plates.h"

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
    CHECK(s.from == "From the tips plate (48 of 48 scored).");
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
    // The other two plates are worth printing next.
    REQUIRE(s.next.size() == 2);
    CHECK_THAT(s.next[0], StartsWith("The spacing plate:"));
    CHECK_THAT(s.next[1], StartsWith("The islands plate:"));
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
    REQUIRE(s.next.size() == 1);
    CHECK_THAT(s.next[0], StartsWith("The tips plate:"));
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
    CHECK(s.from == "From the tips plate (20 of 48 scored).");
    REQUIRE(s.next.size() == 3);
    CHECK(s.next[0] == "The rest of the tips plate: 28 coupons are not scored yet.");
    CHECK(s.next[1] == "Score the spacing plate: it's saved, but nothing is scored yet.");
    CHECK_THAT(s.next[2], StartsWith("The islands plate:"));

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
