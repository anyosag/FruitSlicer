// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Turns filled-in calibration sheets into a resin profile (closes the loop
// between `fruitslicer calibrate` and the support generator).
//
//   tips    → capacity per tip diameter: 1-D logistic fit of success against
//             load per tip; usable capacity = fraction (0.7) × the 50 % load.
//             Also the preferred penetration per diameter and a default tip
//             chosen by scar score among tips that hold enough.
//   spacing → coverage radius: largest tip pitch that held flat on every
//             tested angle, r0 = pitch / √2 (worst point of a square grid);
//             the seed curve's shape is kept and scaled to r0. Critical
//             angle = steepest wedge that printed clean (and all below it).
//   islands → island thresholds: largest island that one tip (two tips)
//             reliably holds, for all smaller sizes too.
//
// Every estimate that rests on censored data (no failures seen, or nothing
// held) is flagged in the profile's "notes" so it is never mistaken for a
// measurement.

#include <limits>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/SupportConfig.h"
#include "support/SupportTree.h"

namespace fruit {

struct CouponResult {
    int id = 0;
    nlohmann::json params;
    bool recorded = false;  // result column filled in
    bool ok = false;        // "ok"; "partial" and "failed" count as failures
    int scar = 0;           // 1..5, 0 = not scored
    std::string notes;
};

struct PlateResults {
    std::string kind;
    nlohmann::json settings;
    std::vector<CouponResult> coupons;
};

// Reads a plate manifest (<plate>.stl.json) and its filled results CSV.
// Parameters come from the manifest by coupon id; the CSV only needs the
// id, result and scar columns. Throws on malformed input.
PlateResults loadPlateResults(const std::string& manifest_path, const std::string& csv_path);

// Manifest path for a results CSV written by `calibrate`
// ("x.stl.results.csv" -> "x.stl.json").
std::string manifestForResults(const std::string& csv_path);

struct LogisticFit {
    double a = 0, b = 0;   // p(ok) = 1 / (1 + exp(-(a + b x)))
    bool separable = false;
    double l50 = std::numeric_limits<double>::quiet_NaN();  // x where p = 0.5
};
LogisticFit fitLogistic(const std::vector<double>& x, const std::vector<int>& ok);

struct FitOptions {
    double capacity_fraction = 0.7;   // operate below the 50 % failure load
    double min_default_capacity = 4.0;  // mm²: weaker tips are not chosen as default
    std::string resin;
    double layer_height = 0;
    double exposure_s = 0;
    double vat_temp_c = 0;
};

nlohmann::json fitProfile(const std::vector<PlateResults>& plates, const FitOptions& o);

// Applies a profile to the generator (and optionally tree) settings.
// Anything the profile does not contain is left at its default. Notes about
// mismatches (e.g. layer height) are appended to `notes`.
void applyProfile(const nlohmann::json& profile, SupportConfig& cfg, TreeConfig* tree,
                  std::vector<std::string>* notes = nullptr);

}  // namespace fruit
