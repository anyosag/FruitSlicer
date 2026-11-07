// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The Supports step's work, off the UI thread: plan tips, build the tree,
// check the result, and say what needs a look. No UI here.

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/JobControl.h"
#include "geometry/Transform.h"
#include "mesh/Placement.h"
#include "mesh/TriangleMesh.h"
#include "support/SupportConfig.h"
#include "support/SupportSettings.h"
#include "support/SupportTree.h"
#include "support/SupportValidator.h"

namespace gui {

class Document;

// What the Supports step lets you choose before generating.
struct SupportChoices {
    int usual_tip = 2;     // tip class: 1 = 0.3 mm, 2 = 0.4 mm (default), 3 = 0.6 mm
    double density = 1.0;  // 1 = normal spacing; up to 2 = closer together
    bool merge = true;     // neighbouring supports join into trunks
    // Values set on the advanced page, by field key (see advancedSettings());
    // every other value is FruitSlicer's own.
    std::map<std::string, double> advanced;
    // The vat's hinge side, in plate coordinates: extra hold on the far side.
    // Set by the app from the printer's settings once the orientation is confirmed.
    std::optional<fruit::HingeSide> hinge;
    bool operator==(const SupportChoices&) const = default;
};

// ---- The advanced page: every value the planner and tree builder use ----

// One value a row lets you set, in the row's own units.
struct AdvancedField {
    const char* key;
    const char* prefix;  // words before the value ("up to")
    const char* suffix;  // after it ("mm")
    double lo, hi;       // allowed range
    int decimals;        // shown precision (0 = whole numbers)
    double (*get)(const fruit::SupportConfig&, const fruit::TreeConfig&);
    void (*set)(fruit::SupportConfig&, fruit::TreeConfig&, double);
};

// What a row's help and fixed text are drawn from.
struct AdvancedContext {
    const fruit::SupportConfig& cfg;
    const fruit::TreeConfig& tc;
    const SupportChoices& choices;
    const fruit::Placement& placement;
    int raft_layers = 8;            // bottom + transition layers of the print file
    double raft_layer_height = 0.05;
    std::string hinge = "not set";  // the hinge side as the Printer page has it
};

struct AdvancedSetting {
    // Number: one or two number fields. TipClass: one of the tip sizes from
    // fields[0].lo to .hi. Toggle: fields[0] is 1 or 0. Fixed: nothing to set.
    enum class Kind { Number, TipClass, Toggle, Fixed };
    // Where the starting value comes from (shown as a badge).
    enum class Source { Default, Borrowed, Assumption };
    const char* group;
    const char* label;
    Kind kind;
    Source source;
    std::vector<AdvancedField> fields = {};
    std::string (*help)(const AdvancedContext&) = nullptr;
    std::string (*text)(const AdvancedContext&) = nullptr;  // Fixed: the value; Toggle: the "on" words
    const char* off_text = nullptr;                         // Toggle: the "off" words
};

// The rows, in page order, grouped by `group`.
const std::vector<AdvancedSetting>& advancedSettings();
// The key of the usual tip size, which lives in SupportChoices::usual_tip.
inline constexpr const char* kUsualTipKey = "usual_tip";
// Applies `values` (clamped to each field's range) over `cfg` and `tc`.
void applyAdvanced(const std::map<std::string, double>& values, fruit::SupportConfig& cfg, fruit::TreeConfig& tc);
// Whether the row holds a value of the user's own.
bool advancedChanged(const AdvancedSetting& s, const SupportChoices& c);

// A change made by hand to the tips, in model coordinates so it survives
// turning or resizing the model. Edits apply on top of the planner's tips,
// in order; regenerating plans afresh and applies them again.
struct TipEdit {
    enum class Kind { Add, Move, Delete, Resize, Pin, Zone };
    Kind kind = Kind::Add;
    fruit::Vec3 at;         // the tip (Move: where from); a zone's centre
    fruit::Vec3 to;         // Move: where to
    fruit::Vec3 normal;     // the surface's outward normal where the tip ends up
    double diameter = 0.4;  // the tip's size after the edit
    double radius = 0;      // Delete, Zone: tips within this are taken away
    bool operator==(const TipEdit&) const = default;
};
inline constexpr double kDeletedZone = 0.5;  // mm, radius: a deleted tip's no-tip zone
inline constexpr double kZoneRadius = 1.5;   // mm, radius: a no-tip zone drawn by hand

// One line of the result's checks.
struct SupportCheck {
    bool ok = true;
    std::string title, detail;
};

// Something to look at, grouped by kind. Blocking ones stop saving.
struct SupportProblem {
    enum class Level { Blocks, Review, Note };
    Level level = Level::Review;
    std::string code;     // the engine's warning code
    std::string title;    // plain words
    std::string body;     // what it means and what to do
    std::string engine;   // the engine's first message, for the curious
    std::size_t count = 0;
    bool has_where = false;
    fruit::Vec3 where;    // printer coordinates of the first one
};

struct SupportResult {
    SupportChoices choices;
    fruit::SupportConfig cfg;
    fruit::TreeConfig tree_cfg;
    std::vector<fruit::SupportPoint> points;
    fruit::SupportTree tree;
    fruit::TriangleMesh mesh;  // the supports, printer coordinates
    fruit::ValidationReport validation;
    std::size_t tips = 0, island_tips = 0, extra_hold = 0;
    std::size_t trunks = 0, pillars = 0, braces = 0, on_model = 0;
    double support_ml = 0;  // at most: where parts overlap they are counted twice
    std::vector<std::pair<double, std::size_t>> tip_sizes;  // diameter (mm), tips; biggest first
    fruit::Vec3 size_with_supports;
    std::vector<SupportCheck> checks;
    std::vector<SupportProblem> problems;
    std::vector<fruit::Warning> plan_warnings;  // the tip planner's, kept so a saved job can rebuild its problems
    std::vector<fruit::SupportPoint> planned;   // the planner's tips, before hand edits
    std::vector<TipEdit> edits;                 // the hand edits `points` has
    double seconds = 0;

    std::size_t count(SupportProblem::Level l) const;
};

// The planner's settings for this placement and these choices.
fruit::SupportConfig supportConfig(const fruit::Placement& p, const SupportChoices& c);
fruit::TreeConfig treeConfig(const SupportChoices& c);

// Plans tips, builds the tree and checks the result. Progress: tips up to
// 0.55, the tree up to 0.85, the check after (stage "check").
SupportResult generate(const Document& doc, const SupportChoices& c, const std::vector<fruit::Vec3>& visible_model,
                       const fruit::JobControl& ctl, const std::vector<TipEdit>& edits = {});

// The tip planner's part: the tips and what it warned about.
struct SupportPlan {
    std::vector<fruit::SupportPoint> points;
    std::vector<fruit::Warning> warnings;
};
SupportPlan planTips(const Document& doc, const SupportChoices& c, const std::vector<fruit::Vec3>& visible_model,
                     const fruit::JobControl& ctl);
// Everything after the tips: the tree, its checks and the problems. Used
// when a saved job comes back with its tips.
SupportResult finishSupports(const Document& doc, const SupportChoices& c, SupportPlan plan,
                             const fruit::JobControl& ctl);
// The planner's tips with hand edits applied, then the rest (keeps both).
SupportResult finishWithEdits(const Document& doc, const SupportChoices& c, SupportPlan plan,
                              const std::vector<TipEdit>& edits, const fruit::JobControl& ctl);

// `planned` (printer coordinates) with `edits` applied in order; the model
// is placed by `model_to_printer`.
std::vector<fruit::SupportPoint> applyEdits(std::vector<fruit::SupportPoint> planned, const std::vector<TipEdit>& edits,
                                            const fruit::Transform& model_to_printer);
// The edit as a line of "Your edits": a title and a detail.
std::pair<std::string, std::string> describeEdit(const TipEdit& e, const fruit::Transform& model_to_printer);
// How many tips the edits added, moved and deleted.
struct EditCounts {
    std::size_t added = 0, moved = 0, deleted = 0, changed = 0;  // changed: resized or pinned
};
EditCounts countEdits(const std::vector<TipEdit>& edits);

// The stages the running card lists, and which one a progress report is in.
const std::vector<std::string>& supportStages();
int supportStage(const std::string& engine_stage, double fraction);

// Why a tip is where it is, in a sentence or two, and its numbers.
std::string tipKind(const fruit::SupportPoint& p);
std::string tipWhy(const fruit::SupportPoint& p);
std::string tipNumbers(const fruit::SupportPoint& p, const fruit::SupportConfig& cfg, const fruit::TreeConfig& tc);

}  // namespace gui
