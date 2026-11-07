// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The resin test plates (canvas boards "PlatePreview" and "Results")
// without the UI: the three plates, each coupon in words, and the scores
// kept in the plate's blind score sheet.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "calibration/CalibrationFit.h"
#include "calibration/CalibrationPlates.h"
#include "printer/PrinterProfile.h"

namespace gui {

// The plates, in the order the Calibrate page shows them.
inline const char* const kPlateKinds[] = {"tips", "spacing", "islands"};
bool isPlateKind(const std::string& kind);

// A plate with FruitSlicer's own options (as `fruitslicer calibrate` makes it).
fruit::CalibrationPlate makePlate(const std::string& kind, const fruit::PrinterProfile& printer);
std::string plateTitle(const std::string& kind);                // "Tips plate"
std::string plateSubtitle(const fruit::CalibrationPlate& p);    // "48 coupons · 6 mm tall"
std::vector<std::string> plateTests(const fruit::CalibrationPlate& p);  // what it tests, from its coupons

// Which coupon is where. Row 0 holds the L marker in column 0; seen from
// above, rows count up the plate (+Y) and columns to the right (+X).
struct PlateLayout {
    struct Cell {
        int id = 0, row = 0, col = 0;
    };
    int rows = 0, cols = 0;
    double cell_mm = 0;       // the grid's pitch; each raft is 2 mm smaller
    std::vector<Cell> cells;  // the coupons, in id order
};
PlateLayout layoutOf(const fruit::CalibrationPlate& p);
PlateLayout layoutOf(const nlohmann::json& manifest);  // throws on a malformed one

// One coupon in words: what it holds or leans, and what its result means.
struct CouponFact {
    std::string label, value;
};
std::vector<CouponFact> couponFacts(const std::string& kind, const nlohmann::json& params);
std::string couponMeaning(const std::string& kind, const nlohmann::json& params);

// A saved plate: where its file went (a .goo, or an STL for SatelLite), and
// the manifest and score sheet FruitSlicer keeps for it in its own folder,
// so they stay put when the plate went onto a USB drive.
struct PlateFiles {
    std::string file, manifest, results;
    int scored = 0, coupons = 0;  // as of the last save, for the Calibrate page
    bool operator==(const PlateFiles&) const = default;
};

// Two manifests with the same coupons, settings and places.
bool samePlate(const nlohmann::json& a, const nlohmann::json& b);

// Keeps `p`'s manifest at `f.manifest` and a blank score sheet at
// `f.results`, unless they are for this same plate already (its scores
// stay). Scores for any other plate are first set aside, with their
// manifest, as "<name>.earlier.json" and "<name>.earlier.results.csv".
// Fills in the counts. Throws std::runtime_error with a readable reason.
void keepPlate(const fruit::CalibrationPlate& p, PlateFiles& f);

// The scores so far: every coupon of the manifest, scored or not.
struct PlateScores {
    std::string kind;
    nlohmann::json manifest;
    PlateLayout layout;
    std::vector<fruit::CouponResult> coupons;  // in the manifest's order
    int scored() const;
    const fruit::CouponResult* find(int id) const;
};
// Reads the manifest and the sheet (a missing sheet: nothing scored yet).
// Throws std::runtime_error with a readable reason.
PlateScores loadScores(const PlateFiles& f);
// Writes the sheet (blind: no settings) and updates the counts in `f`.
void saveScores(PlateFiles& f, const PlateScores& s);

// A sheet filled in elsewhere ("x.results.csv", with its "x.json" beside
// it, as `fruitslicer calibrate` writes them). Throws with a readable reason.
PlateScores readScoreSheet(const std::string& csv);
// Keeps `s` (manifest and scores) as `f`; scores kept there before are set
// aside first, as keepPlate does.
void keepScores(const PlateScores& s, PlateFiles& f);

// The coupon to score after `id`: the next one not scored yet, in id order
// (wrapping round), else simply the next one.
int nextToScore(const PlateScores& s, int id);

// Shared with the A/B test's kept files.
void writeWhole(const std::string& path, const std::string& text);  // whole or not at all; throws
// Moves a kept sheet and its manifest ("<name>.json") to the first free
// "<name>.earlier[-n].results.csv" / ".json" beside them.
void setAsideEarlier(const std::string& manifest, const std::string& results);

}  // namespace gui
