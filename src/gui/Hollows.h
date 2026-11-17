// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The Hollow screen's work, off the UI thread (canvas board "Hollow"): the
// model's own hollows that hold resin, hollowing a solid model, the drain and
// air holes, and the layers that then print. No UI.

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "Parts.h"
#include "core/JobControl.h"
#include "layers/Drain.h"
#include "layers/Hollow.h"
#include "layers/LayerStack.h"
#include "support/SupportPoint.h"

namespace gui {

class Document;

// What the Hollow screen sets.
struct HollowChoices {
    // The model's own hollows that hold resin: drill holes into them, print
    // them solid, or leave them (they are then a problem before saving).
    enum class Fix { Holes, Fill, Leave };
    Fix fix = Fix::Holes;
    // Take the inside of the model out, keeping a wall. Its own hollows are
    // filled first and hollowed out with the rest, and the holes go into
    // what hollowing leaves.
    bool hollow = false;
    double wall = 2.0;           // mm
    double ceiling_lean = 45;    // degrees from vertical a cavity's ceiling may lean
    double hole_diameter = 3.0;  // mm
    bool auto_holes = true;      // a drain and an air hole for each cavity; else the ones placed by hand
    bool operator==(const HollowChoices&) const = default;
};

// A hollow chosen to print as it is ("Accept for this model"), kept with
// the job: a point inside it in its part's coordinates, and how big it was.
// A hollow found later is the same one when that point is inside it, it is
// still a suction cup (or still a resin trap) and its volume is within a
// third of this; one that changed shows again.
struct AcceptedHollow {
    int part = 0;           // the app's part id
    fruit::Vec3 at;         // part coordinates
    double volume = 0;      // mm³
    bool cup = false;       // a suction cup (else a resin trap)
    bool operator==(const AcceptedHollow&) const = default;
};

// The model as it will print with those choices.
struct HollowPlan {
    HollowChoices choices;
    std::vector<fruit::DrainHole> manual;             // the hand-placed holes it was made with
    std::vector<AcceptedHollow> accepted_with;        // and the hollows accepted then
    // For each of closed.cavities: accepted (it prints as it is, whatever
    // the fix), and how a suction cup is graded (a resin trap: Problem).
    std::vector<char> accepted;
    std::vector<fruit::CupGrade> grades;
    fruit::HollowResult fixing;                       // `closed` less the accepted: what holes and filling work on
    double layer_height = 0;
    std::shared_ptr<const fruit::LayerStack> solid;   // the placed model, sliced
    fruit::HollowResult closed;                       // its own hollows that hold resin
    fruit::HollowResult hollowed;                     // what hollowing takes out
    std::vector<fruit::DrainHole> holes;              // the holes, each in its part's coordinates
    std::vector<fruit::PlacedHole> placed;            // the same as drilled, printer coordinates
    std::vector<char> drains;                         // for each of cavities(): it no longer holds resin
    std::shared_ptr<const fruit::LayerStack> layers;  // what prints
    double resin_saved = 0;  // mm³, against the model printed solid (a hollow that holds resin saves none)
    double seconds = 0;

    // The cavities: the hollowed ones when hollowing, else the model's own.
    const fruit::HollowResult& cavities() const { return choices.hollow ? hollowed : closed; }
    bool hollowing() const { return choices.hollow; }
    bool drilling() const { return choices.hollow || choices.fix == HollowChoices::Fix::Holes; }
    bool filling() const { return !choices.hollow && choices.fix == HollowChoices::Fix::Fill; }
    std::size_t undrained() const;  // cavities that still hold resin
    // The model's own hollows that count as problems: not drained, not
    // accepted, and a resin trap or a suction cup graded a problem.
    std::size_t toFix() const;
    bool isAccepted(std::size_t i) const { return i < accepted.size() && accepted[i]; }
    fruit::CupGrade grade(std::size_t i) const { return i < grades.size() ? grades[i] : fruit::CupGrade::Problem; }
    std::size_t missed() const;     // holes that reach no cavity (not drilled)
    // Whether anything prints but the model as it is.
    bool changes() const { return layers && layers != solid; }
};

// Plans every part of `doc` as placed, together, at `layer_height`. Holes
// placed by hand on a part that is not on the plate are left out. Progress:
// "slice", then "hollow" when hollowing, then "holes".
HollowPlan planHollow(const Document& doc, const HollowChoices& c, const std::vector<fruit::DrainHole>& manual,
                      double layer_height, const fruit::JobControl& ctl);

// fruit::placeHoles for holes on several parts: each with its part's frame
// (one whose part has none is not drilled: cavity -1).
std::vector<fruit::PlacedHole> placeHoles(const std::vector<fruit::DrainHole>& holes, const PartFrames& frames,
                                          const fruit::LayerStack& solid, const fruit::HollowResult& hollow);

// A tip may not sit in a hole's mouth: where the hole enters the surface,
// with this much room around its edge.
inline constexpr double kHoleMouthMargin = 0.5;  // mm

// Whether `a` is cavity `i` of `closed`, sliced at `layer_height` with the
// parts placed as `frames` has them.
bool acceptedIs(const AcceptedHollow& a, const fruit::HollowResult& closed, std::size_t i, const PartFrames& frames,
                double layer_height);
// Whether `a` is the suction cup a support warning describes (its "values":
// the cup's box, heights and volume), the parts placed as `frames` has them.
bool acceptedIs(const AcceptedHollow& a, const fruit::Warning& w, const PartFrames& frames);
// Whether `a` is hollow `c` found on slices `layer_height` apart.
bool acceptedIs(const AcceptedHollow& a, const fruit::HollowComponent& c, double layer_height, const PartFrames& frames);
// The fingerprint to keep for cavity `i` of p.closed.
AcceptedHollow acceptedFor(const Document& doc, const HollowPlan& p, std::size_t i);
// How the job grades suction cups: the Advanced settings' limits.
fruit::CupLimits cupLimits(const Document& doc);

// Words for the screen.
// "Resin trap", "Suction cup", "Hollowed".
std::string cavityKind(const fruit::Cavity& c, bool hollowed);
// "Suction cup · note"; "Resin trap"; "Suction cup · accepted".
std::string cavityGradeText(const HollowPlan& p, std::size_t i);
// What its numbers say: how much harder it pulls, and the limit that grades it.
std::string cavityPull(const fruit::Cavity& c, fruit::CupGrade g, const fruit::CupLimits& l);
// A fix that fits its shape (empty for a resin trap).
std::string cavityFix(const fruit::Cavity& c);
// "9.1 ml · from 7.0 to 33.0 mm".
std::string cavityFacts(const fruit::Cavity& c, double layer_height);
// What it does to the print, as it is.
std::string cavityMeaning(const fruit::Cavity& c, bool hollowed);
// The header's button: "Add 2 drain holes", "Print it solid", "Hollow it";
// "Leave it" when it changes nothing.
std::string applyLabel(const HollowPlan& p);
// After the change: one line per cavity on whether it still holds resin.
struct HollowOutcome {
    bool ok = true;
    std::string title, detail;
};
std::vector<HollowOutcome> hollowOutcomes(const HollowPlan& p);
// The 3D view's label on cavity `i` of p.cavities(), as things stand: "9.1
// ml of trapped resin" until the holes are applied, then "9.1 ml · drains".
HollowOutcome cavityLabel(const HollowPlan& p, std::size_t i, bool applied);
// Where that label goes: inside the cavity, half-way up (printer coordinates).
fruit::Vec3 cavityLabelAt(const fruit::HollowResult& h, std::size_t i, double layer_height);

// The 3D view's cut through the plan: a vertical plane through `at` along
// `along` (unit length; printer coordinates), and where it crosses each
// layer. `outline` is the whole model with its own hollows filled;
// `hollow` the cavities (of cavities(); none when they are filled);
// `prints` what prints. What lies in the outline but in neither of the
// others is a hole.
struct SectionBand {
    std::size_t layer = 0;
    double t0 = 0, t1 = 0;  // along the plane, from `at`
};
struct HollowSection {
    fruit::Vec2 at, along;
    double layer_height = 0;
    std::vector<SectionBand> outline, hollow, prints;
};
HollowSection hollowSection(const HollowPlan& p, fruit::Vec2 at, fruit::Vec2 along);

// The model's own hollows that still hold resin as things stand (the step
// bar's "1 to fix"): the plan's, once it is up to date; else the health
// check's, unless a fix was applied.
std::size_t hollowsToFix(const Document& doc);
// The Orient step's card on them: "1 hollow holds resin", "Drained",
// "Hollowed · 2.0 mm wall", "No hollows found"; `ok` false while one holds resin.
HollowOutcome hollowSummary(const Document& doc);

}  // namespace gui
