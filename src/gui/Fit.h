// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The Fit page (canvas board "Fit") without the UI: a resin's scored test
// plates turned into its support values, each with how it was found, and
// what the plates couldn't pin down.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Resins.h"
#include "calibration/CalibrationFit.h"

namespace gui {

// How firmly the plates pin a value down.
enum class Basis {
    Measured,  // between a load that held and one that didn't
    Bound,     // only a floor: nothing tried was too much
    Guess,     // everything tried was too much: below that, but how far is a guess
};

// One tip size's hold, beside FruitSlicer's starting value for it.
struct TipHold {
    double tip = 0;       // mm
    double yours = 0;     // mm² of layer per tip, as kept
    double borrowed = 0;  // the starting value
    Basis basis = Basis::Measured;
    std::string how;      // one or two sentences
};

// A value the fit sets, and how it was found.
struct FitValue {
    std::string label, value, how;
    Basis basis = Basis::Measured;
};

struct FitSummary {
    nlohmann::json profile;          // as `fruitslicer fit` writes it
    std::vector<std::string> kinds;  // the plates it fitted, in order
    std::string from;                // "From the tips plate (48 of 48 scored)."
    std::vector<TipHold> holds;      // for the chart and its table
    std::vector<FitValue> values;    // each value, and how it was found
    std::vector<std::string> next;   // plates worth printing or scoring next
    std::vector<std::string> unsure; // what the plates couldn't pin down
};

// `plates`: the resin's plates with scores, as read back. A plate with no
// score is left out of the fit.
FitSummary summarizeFit(const std::vector<fruit::PlateResults>& plates, const fruit::FitOptions& o, const Resin& r);

// `old` with the values of the plates in `kinds` replaced by `fit`'s; the
// values of other plates (an imported profile's, say) stay.
nlohmann::json mergedProfile(const nlohmann::json& old, const nlohmann::json& fit, const std::vector<std::string>& kinds);

std::string holdText(const TipHold& h);  // "10.5 mm²", "at least 14 mm²", "2.5 mm², a guess"
std::string oneDecimal(double v);        // "10.5", "14": rounded half away from zero

}  // namespace gui
