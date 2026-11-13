// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// What may not print. The print is replayed layer by layer for a job's
// supports (stability/Stability.h), off the UI thread, and what that finds is
// put in the words and colours the screens show. No UI here.
//
// Nothing here stops a print from being saved: the strengths behind it are
// borrowed until a resin's tips plate is fitted, and the bending is an upper
// limit. It says where to look.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/JobControl.h"
#include "geometry/Vec.h"
#include "mesh/TriangleMesh.h"
#include "stability/Stability.h"

namespace gui {

class Document;
struct SupportResult;
struct LayerImage;

// How likely, as the screens colour it: from 1 in 20, 1 in 5 and 1 in 2.
// Under 1 in 20 nothing is shown.
enum class RiskBand { None, May, Likely, VeryLikely };
inline constexpr double kRiskShownFrom = 0.05, kRiskLikelyFrom = 0.2, kRiskVeryLikelyFrom = 0.5;
RiskBand riskBand(double chance);
unsigned riskColour(RiskBand b);  // 0xRRGGBB (None: 0)

// Less of the model than this going with a place counts as the model staying held (mm³).
inline constexpr double kRiskLosesModel = fruit::kCountsAsLost;
// A tip shows when it carries clearly more than it is trusted with: this many
// times that load. (A fitted table trusts a tip with a load at which about 1
// in 18 already lets go, so a tip at just its rated load is not news.)
inline constexpr double kRiskTipOverTrust = 1.1;

// One place worth a look.
struct RiskPlace {
    enum class Kind {
        ThinPlace,  // the model's own cross-section, pulled apart or bent
        Tips,       // every tip that may let go: one place for all of them, with what goes with them
        Skin,       // a fresh layer reaching past what holds it
        MidAir,     // starts with nothing under it
        Sliver,     // held by less than a tip's width of contact
        Support,    // a support's own rod
    };
    Kind kind = Kind::ThinPlace;
    RiskBand band = RiskBand::None;
    double chance = 0;
    std::string title;  // "Thin place, 22.8 mm up"
    std::string brief;  // "Very likely to break · 160 mm³ would be lost"
    // What the Checks row says when this is the only place and "1 place may
    // not print" would say too little ("": nothing of its own).
    std::string headline;
    // The card's rows: what it holds, when it is at its worst, why, and what
    // goes with it. `note`: what is not known about it ("" for most).
    std::string holds, when, why, if_goes, note;
    fruit::Vec3 at;       // printer coordinates
    int layer = -1;       // the layer printing when it is at its worst (0-based, from the model's first; -1: none)
    double lost_mm3 = 0;  // of the model, when it goes (Tips: in the replays in which model went with them)
    int tips = 0;         // Tips: how many
};

// A tip to colour in the view.
struct RiskTip {
    fruit::Vec3 at;
    double diameter = 0.4;
    RiskBand band = RiskBand::May;
};

struct RiskReport {
    std::shared_ptr<const fruit::StabilityResult> result;
    std::vector<RiskPlace> places;  // what would take part of the model with it first, then the likeliest
    std::size_t places_found = 0;   // places.size(), and those left off a very long list
    std::vector<RiskTip> tips;      // every tip with a chance of 1 in 20 or more of letting go
    bool borrowed = true;           // the tip strengths are FruitSlicer's own, not measured for this resin
    int replays = 0;
    // By layer: how close the weakest place is to letting go while that layer
    // prints (1: half of such places would), and the layer where it is closest.
    std::vector<float> closeness;
    int riskiest_layer = -1;
    double lost_mm3[3] = {0, 0, 0};  // the model's volume that may, is likely and is very likely not to print
    // The model's surface over that volume, in printer coordinates: for the 3D view to paint.
    fruit::TriangleMesh paint[3];
    double seconds = 0;
};

// The Checks row: green with nothing to show, else what to look at.
struct RiskHeadline {
    bool ok = true;
    bool tips_only = false;  // only tips may let go, and the model stays held without them
    std::string title, detail;
};
RiskHeadline riskHeadline(const RiskReport& r);
// What the strengths behind it are worth, in a sentence (the line under the row).
std::string riskStrengthsNote(const RiskReport& r);

// Replays the print for `sup`, the supports of `doc` as it is now. Progress:
// cutting the layers up to 0.2, the replay up to 0.9, then the paint.
RiskReport checkRisk(const Document& doc, const SupportResult& sup, const fruit::JobControl& ctl);

// Its places from the engine's result (checkRisk does this; apart, for tests).
// `usable` is the share of a tip's half-fail load it is trusted with.
void findRiskPlaces(const fruit::StabilityResult& res, double usable, RiskReport& out);
// The model's surface (`mesh`, printer coordinates) over the parts that may
// not print, by band (checkRisk does this; apart, for tests).
void paintRisk(const fruit::TriangleMesh& mesh, const fruit::StabilityResult& res, fruit::TriangleMesh paint[3],
               const fruit::JobControl& ctl);

// For the layer picture: how much of each image pixel is print that may not
// print, by band (May, Likely, VeryLikely), on the picture's own grid.
// `layer` counts from the model's first layer. Empty vectors: nothing there.
struct RiskLayerPaint {
    std::vector<std::uint8_t> cover[3];
    bool any = false;
};
RiskLayerPaint paintRiskLayer(const fruit::StabilityResult& res, int layer, const LayerImage& grid);

}  // namespace gui
