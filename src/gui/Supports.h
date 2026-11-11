// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The Supports step's work, off the UI thread: plan tips, build the tree,
// check the result, and say what needs a look. No UI here.

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "Hollows.h"
#include "Parts.h"
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

// The quick support sliders have five stops each (1 to 5). The middle stop of
// Hold and Amount, and stop 2 of Sturdiness, are FruitSlicer's own values (the
// resin's, where it measured them): a job left there plans what it always did.
inline constexpr int kQuickStops = 5;
inline constexpr int kHoldDefault = 3, kAmountDefault = 3, kSturdyDefault = 2;

// What the Supports step lets you choose before generating.
struct SupportChoices {
    int usual_tip = 2;     // tip class: 1 = 0.3 mm, 2 = 0.4 mm (default), 3 = 0.6 mm: the resin's usual tip
    double density = 1.0;  // 1 = normal spacing; up to 2 = closer together (old jobs; the Amount slider adds to it)
    bool merge = true;     // neighbouring supports join into trunks (old jobs; Sturdiness can turn it off too)
    // The quick support sliders' stops (see applyQuick).
    int hold = kHoldDefault, amount = kAmountDefault, sturdy = kSturdyDefault;
    // Values set on the advanced page, by field key (see advancedSettings());
    // every other value is FruitSlicer's own.
    std::map<std::string, double> advanced;
    // The vat's hinge side, in plate coordinates: extra hold on the far side.
    // Set by the app from the printer's settings once the orientation is confirmed.
    std::optional<fruit::HingeSide> hinge;
    // The resin the job is printed in, and a copy of the support values
    // measured for it (a fitted profile; null: FruitSlicer's own), so a saved
    // job keeps its values whatever happens to the resin later.
    std::string resin;
    nlohmann::json profile;
    bool operator==(const SupportChoices&) const = default;
};

// Tip sizes are chosen as FruitSlicer's own classes (0.2, 0.3, 0.4, 0.6 mm,
// by index). A resin profile may have measured other sizes: a choice then
// means the class nearest that size, and a class shows as the nearest one.
int nearestTipClass(const fruit::SupportConfig& cfg, int standard_class);
// The usual tip (1 = 0.3, 2 = 0.4, 3 = 0.6 mm) a resin profile measured, or the default.
int usualTipFor(const nlohmann::json& profile);
int standardTipClass(const fruit::SupportConfig& cfg, int cls);

// ---- Quick support: three sliders over the planner's own settings ----

enum class QuickSlider { Hold, Amount, Sturdiness };
// What one slider shows: its label, the words at its ends, the word and the
// caption for its stop (what that stop sets, with the real values).
struct QuickInfo {
    const char* label;
    const char* left;
    const char* right;
    std::string word;
    std::string caption;
    int stop = 1;
    int normal = 1;  // the resin's stop
};
int& quickStop(SupportChoices& c, QuickSlider s);
int quickStop(const SupportChoices& c, QuickSlider s);
const char* quickWord(QuickSlider s, int stop);  // stop 1..5
// The slider as `c` has it (stop clamped to 1..5), with the caption of that stop.
QuickInfo quickInfo(QuickSlider s, const SupportChoices& c);
// The caption for `stop` of slider `s`, the other sliders as `c` has them.
std::string quickCaption(QuickSlider s, int stop, const SupportChoices& c);
// What the sliders set. The usual tip class (0 = 0.2 ... 3 = 0.6 mm) for Hold:
// the resin's usual tip, one class either way across the stops (a usual tip
// set on the advanced page stays); how far to scale "how much a tip holds"
// (Hold's ends); the lean angle's shift and the spacing for Amount.
int quickTipClass(const SupportChoices& c);
double quickCapacityScale(int hold_stop);
double quickAngleShift(int amount_stop);  // degrees added to the "faces that need tips" angle
double quickDensity(int amount_stop);     // times the job's tip spacing
// Sets the trunk and brace values for the Sturdiness stop on `tc`.
void applySturdiness(int stop, fruit::TreeConfig& tc);
// Whether any slider, or the older spacing and merge choices, is off the resin's values.
bool quickChanged(const SupportChoices& c);
// Puts the sliders (and the older spacing and merge choices) back.
void resetQuick(SupportChoices& c);

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
// The resin profile key that measures a row's value ("" if none measures it).
const char* measuredKey(const AdvancedSetting& s);
// Whether the job's resin measured the row's value (and the job keeps it).
bool measuredByResin(const AdvancedSetting& s, const SupportChoices& c);
// A row's value as text, as the fields show it ("11.1 mm²", "1 tip ≤ 1.7 · 2 ≤ 6.0 mm").
std::string advancedValueText(const AdvancedSetting& s, const fruit::SupportConfig& cfg, const fruit::TreeConfig& tc);

// ---- A resin's own support values, set on the Resins page ----
// Kept in the resin's profile under this key, by advanced field key, so a
// job that copies the profile keeps them. They apply over the measured
// values and under the quick sliders; a job's own advanced values win.
inline constexpr const char* kResinOwnKey = "yours";
// The settings a job on a resin with this profile starts with (its usual tip too).
SupportChoices resinChoices(const nlohmann::json& profile);
// Applies the resin's own values (all but the usual tip, which a job keeps in usual_tip).
void applyResinValues(const nlohmann::json& profile, fruit::SupportConfig& cfg, fruit::TreeConfig& tc);
// Whether the resin sets one of the row's values itself.
bool resinChanged(const AdvancedSetting& s, const nlohmann::json& profile);
// The profile with one of the resin's own values set (clamped to its field's
// range). A value equal to the resin's measured one, or FruitSlicer's own, is
// not kept: the row reads as it did before.
nlohmann::json withResinValue(const nlohmann::json& profile, const std::string& key, double value);
// The profile without the resin's own values for a row.
nlohmann::json withoutResinRow(const nlohmann::json& profile, const AdvancedSetting& s);
// The row the Resins page lists under `label` (an advanced row's label).
const AdvancedSetting* advancedRow(const std::string& label);

// A change made by hand to the tips, in its part's own coordinates so it
// survives turning, resizing or moving that part. Between plans an edit applies on top of the
// planner's tips; when the tips are planned again (regenerating, a turned
// model) the planner works around them: your tips are pinned and its own are
// kept out of the no-tip zones. Each tip has at most one edit: changing a tip
// of your own changes the edit that made it (see recordMove and the others).
struct TipEdit {
    enum class Kind { Add, Move, Delete, Resize, Pin, Zone };
    Kind kind = Kind::Add;
    fruit::Vec3 at;         // the tip (Move: where from); a zone's centre
    fruit::Vec3 to;         // Move: where to
    fruit::Vec3 normal;     // the surface's outward normal where the tip ends up
    double diameter = 0.4;  // the tip's size after the edit
    double radius = 0;      // Delete, Move, Zone: the planner's tips are kept this far from `at`
    int part = 0;           // the part it is on (an id): `at`, `to` and `normal` are in its coordinates
    bool operator==(const TipEdit&) const = default;
};
// mm, radius, where a deleted or moved tip was: just under the planner's 1 mm
// sample spacing, so a neighbouring tip stays and a new plan's tip is at least
// one sample away.
inline constexpr double kDeletedZone = 0.9;
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
    // The hollowed or drilled model the tips were planned on (null: the
    // model as it is). The layers and the print file use the same.
    std::shared_ptr<const HollowPlan> hollow;
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
    std::vector<fruit::SupportPoint> planned;   // the planner's own tips, before hand edits
    std::vector<TipEdit> plan_edits;            // the hand edits the planner worked around
    std::vector<TipEdit> edits;                 // the hand edits `points` has
    double seconds = 0;

    std::size_t count(SupportProblem::Level l) const;
};

// The planner's settings for this placement and these choices.
fruit::SupportConfig supportConfig(const fruit::Placement& p, const SupportChoices& c);
fruit::TreeConfig treeConfig(const SupportChoices& c);

// Plans tips, builds the tree and checks the result, for every part of the
// job together. `visible` are the sides people will see, as printer
// directions (Document::visibleInPrinter). Progress: tips up to 0.55, the
// tree up to 0.85, the check after (stage "check").
SupportResult generate(const Document& doc, const SupportChoices& c, const std::vector<fruit::Vec3>& visible,
                       const fruit::JobControl& ctl, const std::vector<TipEdit>& edits = {});

// The tip planner's part: its own tips and what it warned about. Given hand
// edits, it plans around them: your tips are pinned (and left out of
// `points`, which applyEdits puts back) and its own are kept out of the zones.
struct SupportPlan {
    std::vector<fruit::SupportPoint> points;
    std::vector<fruit::Warning> warnings;
    std::vector<TipEdit> edits;  // the hand edits it planned around
    std::shared_ptr<const HollowPlan> hollow;  // the hollowed or drilled model, when the Hollow screen changed it
};
SupportPlan planTips(const Document& doc, const SupportChoices& c, const std::vector<fruit::Vec3>& visible,
                     const fruit::JobControl& ctl, const std::vector<TipEdit>& edits = {});
// Everything after the tips: the tree, its checks and the problems. Used
// when a saved job comes back with its tips.
SupportResult finishSupports(const Document& doc, const SupportChoices& c, SupportPlan plan,
                             const fruit::JobControl& ctl);
// The planner's tips with hand edits applied, then the rest (keeps both).
SupportResult finishWithEdits(const Document& doc, const SupportChoices& c, SupportPlan plan,
                              const std::vector<TipEdit>& edits, const fruit::JobControl& ctl);

// `planned` (printer coordinates) with `edits` applied; each part is placed
// by its frame in `frames` (an edit whose part has none is left out). Each
// of your tips is added; a deleted, moved, resized or pinned tip of the
// planner's is taken away, and so is any of its tips inside a zone. Your own
// tips are never taken away by another edit.
std::vector<fruit::SupportPoint> applyEdits(std::vector<fruit::SupportPoint> planned, const std::vector<TipEdit>& edits,
                                            const PartFrames& frames);
// Whether tips planned around `plan_edits` still suit `edits`: every edit the
// planner worked around is still there. If one was undone or changed, the
// planner's tips have a hole or a zone that is no longer wanted, so the tips
// are planned again; otherwise the edits apply on top.
bool planStillFits(const std::vector<TipEdit>& plan_edits, const std::vector<TipEdit>& edits);
// Recording a change to `tip` (printer coordinates, as shown). A tip of the
// planner's gets an edit of its own; for a tip of yours the edit that made it
// changes (moving an added tip moves the addition, deleting it undoes it), so
// each edit can still be undone alone. The changed edit goes to the end of the
// list. `part` is the part the tip is on (a new edit is kept in its
// coordinates; an edit of yours stays on its own part). False when nothing
// changed.
bool recordMove(std::vector<TipEdit>& edits, const fruit::SupportPoint& tip, const fruit::Vec3& to,
                const fruit::Vec3& normal, const PartFrames& frames, int part = 0);
bool recordDelete(std::vector<TipEdit>& edits, const fruit::SupportPoint& tip, const PartFrames& frames, int part = 0);
bool recordResize(std::vector<TipEdit>& edits, const fruit::SupportPoint& tip, double diameter,
                  const PartFrames& frames, int part = 0);
bool recordPin(std::vector<TipEdit>& edits, const fruit::SupportPoint& tip, const PartFrames& frames, int part = 0);
// The edit as a line of "Your edits": a title and a detail.
std::pair<std::string, std::string> describeEdit(const TipEdit& e, const PartFrames& frames);
// How many of each kind of edit there are, and as words ("1 added, 2 no-tip
// zones"; only the kinds there are).
struct EditCounts {
    std::size_t added = 0, moved = 0, deleted = 0, resized = 0, pinned = 0, zones = 0;
};
EditCounts countEdits(const std::vector<TipEdit>& edits);
std::string describeCounts(const EditCounts& n);

// The stages the running card lists, and which one a progress report is in.
const std::vector<std::string>& supportStages();
int supportStage(const std::string& engine_stage, double fraction);

// Why a tip is where it is, in a sentence or two, and its numbers.
std::string tipKind(const fruit::SupportPoint& p);
std::string tipWhy(const fruit::SupportPoint& p);
std::string tipNumbers(const fruit::SupportPoint& p, const fruit::SupportConfig& cfg, const fruit::TreeConfig& tc);

}  // namespace gui
