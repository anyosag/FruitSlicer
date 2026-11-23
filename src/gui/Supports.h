// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The Supports step's work, off the UI thread: plan tips, build the tree,
// check the result, and say what needs a look. No UI here.

#include <array>
#include <cstddef>
#include <limits>
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
#include "support/DetailMap.h"
#include "support/PointGenerator.h"
#include "support/SupportCheck.h"
#include "support/SupportConfig.h"
#include "support/SupportSettings.h"
#include "support/SupportTree.h"
#include "support/SupportValidator.h"

namespace gui {

class Document;

// The quick support controls: How many (five stops, 1 to 5), Tip size and
// Look further. How many's middle stop is the resin's own values (FruitSlicer's
// starting ones until a resin measures them): a job left there plans what it
// always did. Trunks and braces has five stops too, as a row on the Advanced
// page, the second FruitSlicer's own. Hold is from jobs saved with the three
// sliders these replaced: nothing sets it now.
inline constexpr int kQuickStops = 5;
inline constexpr int kHowManyCentre = 3, kSturdyDefault = 2, kHoldDefault = 3;

// What the Supports step lets you choose before generating.
struct SupportChoices {
    // Tip size, a tip class: 1 = 0.3 mm, 2 = 0.4 mm (default), 3 = 0.6 mm (0 = 0.2 mm, in old jobs only).
    // The sizes for spots in mid-air and near edges sit as many sizes above or below as it is from the resin's.
    int usual_tip = 2;
    double density = 1.0;  // 1 = normal spacing; up to 2 = closer together (old jobs; How many adds to it)
    // The job's raft, from the Raft page (Least resin's unless chosen).
    fruit::RaftSpec raft;
    // The preset it came from. A label only: two jobs with the same raft are the same whatever it is called,
    // so renaming a preset leaves supports made with it up to date.
    struct PresetName {
        std::string name = fruit::kDefaultRaftPreset;
        bool operator==(const PresetName&) const { return true; }
    } raft_preset;
    bool merge = true;     // neighbouring supports join into trunks (old jobs; Trunks and braces can turn it off too)
    int how_many = kHowManyCentre;  // How many: 1 (Fewest) to 5 (Most)
    bool look_further = false;      // a support looks further for a way down before it stands on the model
    int hold = kHoldDefault;        // old jobs: the Hold slider's end stops scaled how much a tip is counted on to hold
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

// A part's own support values, set on the Supports panel with that part
// picked: each one here is the part's instead of the job's (All parts). A
// value equal to the job's is not kept (tidy), so the part reads as
// following the job again. Lift and Raft / On the plate are the part's
// placement (Document: Placement::elevation, on_plate).
struct PartChoices {
    std::optional<int> how_many;
    std::optional<int> usual_tip;
    std::optional<bool> look_further;
    std::map<std::string, double> advanced;  // Advanced rows the part sets itself, over the job's
    std::optional<fruit::RaftSpec> raft;     // its raft's settings (on a shared raft: every part on it the same)
    SupportChoices::PresetName raft_preset;  // with `raft`: the preset it came from (a label)
    bool empty() const { return !how_many && !usual_tip && !look_further && advanced.empty() && !raft; }
    bool operator==(const PartChoices&) const = default;
};

// What supports are made from besides the parts themselves: the job's
// choices (All parts), each part's own, and the parts that share a raft.
struct SupportSetup {
    SupportChoices choices;                // All parts
    std::map<int, PartChoices> own;        // by part id
    std::vector<std::vector<int>> shared;  // parts that share a raft by choice: ids, a list per raft
    // The part's choices: the job's, with the part's own over them.
    SupportChoices forPart(int part) const;
    bool operator==(const SupportSetup&) const = default;
};
// Drops a part's own values that are the job's, and the parts left with none.
void tidy(SupportSetup& s);
// The same with every How many at the centre (what tip counts are made for:
// they count every stop).
SupportSetup centred(SupportSetup s);

// A raft and the parts on it.
struct RaftGroup {
    std::vector<int> parts;     // ids, in the job's order
    fruit::RaftSpec spec;       // its settings: its first part's
    std::string preset;         // the preset they came from (a label)
    bool touching = false;      // joined because their rafts would meet, not by choice (made supports only)
    bool operator==(const RaftGroup&) const = default;
};
// The rafts as chosen: each part not on the plate on a raft of its own, or on
// the one it shares by choice (SupportSetup::shared), in the job's order. Rafts
// that would meet are joined when the supports are made (SupportResult::rafts).
std::vector<RaftGroup> chosenRafts(const Document& doc, const SupportSetup& s);
// Rafts nearer each other than this, mm, print joined: the first layers are
// cured much longer and come out wider than drawn. ASSUMPTION (0.5 mm), as
// TreeConfig::foot_clearance; not measured.
inline constexpr double kRaftsMeet = 0.5;
// The lift, mm: the slider's stops go from 1 to 10; one of your own (Custom…) up to kMaxLift, to 0.1 mm.
inline constexpr double kMinLift = 1, kMaxLift = 50;

// Tip sizes are chosen as FruitSlicer's own classes (0.2, 0.3, 0.4, 0.6 mm,
// by index). A resin profile may have measured other sizes: a choice then
// means the class nearest that size, and a class shows as the nearest one.
int nearestTipClass(const fruit::SupportConfig& cfg, int standard_class);
// The usual tip (1 = 0.3, 2 = 0.4, 3 = 0.6 mm) a resin profile measured, or the default.
int usualTipFor(const nlohmann::json& profile);
int standardTipClass(const fruit::SupportConfig& cfg, int cls);

// ---- Quick support: how many tips, how big, and how far a support looks for a way down ----
// The controls are named for what they set, not for an outcome: the planner
// has no "hold" or "amount" to turn, only rules that place tips, and which
// rule decides depends on the part.

// How many. The stops move the rules together, so that each one changes
// something on most parts: the lean past which a face needs tips (and, towards
// More, the short-ledge rule with it: a ledge of 1 mm over 1 mm is itself 45°,
// so a lower angle alone gives almost nothing), the spacing (More and Most)
// and how close a tip sits to a free fresh edge (Fewer and Fewest).
// The stop's word: "Fewest" ... "Most"; the centre's says whose values they are.
const char* howManyWord(int stop, const SupportChoices& c);
// What `stop` sets, with the real values, the other choices as `c` has them.
std::string howManyCaption(int stop, const SupportChoices& c);
double howManyAngleShift(int stop);  // degrees added to the "faces that need tips" angle
double howManyDensity(int stop);     // times the job's tip spacing
double howManyEdgeReach(int stop);   // times "a tip within" of a free fresh edge
// Whether the centre's values are a resin's measured ones (else FruitSlicer's starting values).
bool howManyMeasured(const SupportChoices& c);
// Tip size: what the other two sizes are with it, in words.
std::string tipSizeCaption(const SupportChoices& c);
// Look further: a blocked support walks up to this far for a way down, mm,
// before it stands on the model. Long slanted routes are soft: not printed yet.
inline constexpr double kLookFurtherRoute = 30.0;
std::string lookFurtherCaption(const SupportChoices& c);
// Trunks and braces: the key of its row on the Advanced page (a stop, 1 to
// 5: presets over the trunk and brace values, which rows of their own override).
inline constexpr const char* kSturdyKey = "sturdiness";
// Sets the trunk and brace values for the stop on `tc`; the stop those values
// are, as far as they tell; and what `c` has, in words.
void applySturdiness(int stop, fruit::TreeConfig& tc);
int sturdinessOf(const fruit::TreeConfig& tc);
std::string sturdinessCaption(const SupportChoices& c);
// Old jobs: how far Hold's end stops scale "how much a tip holds".
double quickCapacityScale(int hold_stop);
// Whether a quick control, or an older spacing, merge or hold choice, is off the resin's values.
bool quickChanged(const SupportChoices& c);
// Puts them back.
void resetQuick(SupportChoices& c);
// A job saved with the three sliders these controls replaced (Hold, Amount,
// Sturdiness, stops 1 to 5; 3, 3, 2 with none): the same planner and tree
// settings as it had, written as the usual tip and values of the job's own
// on the Advanced page. `c` holds the rest of the job's choices already.
void convertOldQuick(SupportChoices& c, int hold, int amount, int sturdy);

// A job saved before the Raft page: its raft from the Full switch and the raft
// values it set on the Advanced page (taken out of `advanced`, where no row
// reads them now), so it builds as it did.
fruit::RaftSpec legacyJobRaft(bool full, std::map<std::string, double>& advanced);

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
// How suction cups are graded with `c`'s Advanced values.
fruit::CupLimits cupLimitsOf(const SupportChoices& c);
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
// values and under the quick controls; a job's own advanced values win.
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
// The colour the slots and holes kept clear are drawn in (the Why view and its legend).
inline constexpr unsigned kKeptClearColour = 0x46C2CB;

struct SupportCheck {
    bool ok = true;
    std::string title, detail;
};

// Something to look at, grouped by kind. Blocking ones stop saving.
struct SupportProblem {
    // Warn: worth a look, but not counted as a problem (a suction cup graded a warning).
    enum class Level { Blocks, Review, Warn, Note };
    Level level = Level::Review;
    std::string code;     // the engine's warning code
    std::string title;    // plain words
    std::string body;     // what it means and what to do
    std::string engine;   // the engine's first message, for the curious
    std::size_t count = 0;
    bool has_where = false;
    fruit::Vec3 where;    // printer coordinates of the first one
    // Suction cups and resin traps: each one's warning, so one you accept
    // on the Hollow screen moves to "Hollows you accepted" (problemsNow).
    std::vector<fruit::Warning> hollows;
};

// The tip planner's part: its own tips and what it warned about. Given hand
// edits, it plans around them: your tips are pinned (and left out of
// `points`, which applyEdits puts back) and its own are kept out of the zones.
struct SupportPlan {
    std::vector<fruit::SupportPoint> points;
    std::vector<fruit::Warning> warnings;
    std::vector<TipEdit> edits;  // the hand edits it planned around
    std::shared_ptr<const HollowPlan> hollow;  // the hollowed or drilled model, when the Hollow screen changed it
    // With no raft: the counts of held layers these tips are right for (SupportResult::held_min).
    int held_min = 0, held_max = std::numeric_limits<int>::max();
};
// Each part's plan, by part id.
using JobPlan = std::map<int, SupportPlan>;

// What the replay-and-strengthen loop did (lowerPeak). rounds 0: it didn't
// run, or found nothing to strengthen.
struct PeakFix {
    bool ran = false;
    int rounds = 0;           // rounds kept
    double weaker = 1;        // the resin was taken as this many times weaker than its values (peakWeaker)
    int replays = 0;          // replays each check made
    // What it aims at, with the resin that much weaker: the chance that part of the model (1 mm³ or more) is not
    // on the finished print, and how much of it to expect lost per print (mm³), as first made and at the end.
    double lost_before = 0, lost_after = 0;
    double mm3_before = 0, mm3_after = 0;
    std::size_t enlarged = 0; // tips made a size bigger
    std::size_t added = 0;    // tips added beside them, or where a fresh skin or a mid-air piece was short of hold
    // Places that may cost part of the model that more tips don't fix: a thin
    // place in the model, one bent by an off-centre pull, or a support's own rod.
    std::size_t left = 0;
    double seconds = 0;
};

struct SupportResult {
    SupportChoices choices;  // the job's (All parts): setup.choices
    SupportSetup setup;      // what it was made from
    // The hollowed or drilled model the tips were planned on (null: the
    // model as it is). The layers and the print file use the same.
    std::shared_ptr<const HollowPlan> hollow;
    fruit::SupportConfig cfg;
    fruit::TreeConfig tree_cfg;
    std::vector<fruit::SupportPoint> points;
    fruit::SupportTree tree;
    // The slots and holes no support went into (and the space under their
    // mouths), in the whole job; null when not worked out (an older job).
    std::shared_ptr<const fruit::DetailMap> details;
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
    // With no raft: the planner's tips are the same while the bottom layers
    // print at least `held_min` and at most `held_max` of its layers of the
    // model (plateHeldLayers). Any count when nothing near the plate has an
    // underside there.
    int held_min = 0, held_max = std::numeric_limits<int>::max();
    double seconds = 0;
    // What each part got, in the job's order.
    struct PartMade {
        int part = -1;                     // its id
        std::size_t first = 0, tips = 0;   // its tips: points[first, first + tips)
        std::size_t trunks = 0;
        double ml = 0;                     // its supports, and its raft when it has one of its own
        int raft = -1;                     // index into rafts (-1: it stands on the plate)
        fruit::SupportConfig cfg;          // its settings
        fruit::TreeConfig tree_cfg;
    };
    std::vector<PartMade> parts;
    // The rafts as made: those that would meet are one (RaftGroup::touching).
    struct RaftMade : RaftGroup {
        double top = 0;                   // its thickness, mm
        int pieces = 1;                   // separate pieces on the plate
        fruit::TreeConfig tree_cfg;       // the settings it was built with
        std::vector<std::size_t> pillars;  // its trunks: indexes into tree.pillars
    };
    std::vector<RaftMade> rafts;
    JobPlan plans;  // each part's tips as planned, before hand edits (planned is them all, in the job's order)
    PeakFix peak;   // what the replay-and-strengthen loop did, when it ran (generate's lower_peak)

    std::size_t count(SupportProblem::Level l) const;
    const PartMade* partOfTip(std::size_t i) const;  // null: no such tip
    const PartMade* partMade(int id) const;
    double thinnestRaft() const;  // the thinnest raft's thickness; 0 with none
    bool heldLayersFit(int held) const { return held_min <= held && held <= held_max; }
};

// Raft `raft` of the result alone (its trunks and its thickness): for the
// Raft page's preview. Empty when there is no such raft.
fruit::SupportTree raftTreeOf(const SupportResult& r, std::size_t raft);

// The planner's settings for this placement and these choices.
fruit::SupportConfig supportConfig(const fruit::Placement& p, const SupportChoices& c);
fruit::TreeConfig treeConfig(const SupportChoices& c);

// Plans tips, builds the trees and rafts and checks the result. Each part
// gets its tips and its supports with its own choices (SupportSetup::forPart),
// on the plate or on a raft: its own, or one it shares (chosenRafts; rafts
// that would meet are joined). A job of one part is made as it always was.
// `visible` are the sides people will see, as printer directions
// (Document::visibleInPrinter). Progress: tips up to 0.55, the trees up to
// 0.85, the check after (stage "check").
// `lower_peak`: then replay the print and strengthen it where part of the
// model may be lost (lowerPeak), as the app makes supports.
SupportResult generate(const Document& doc, const SupportSetup& s, const std::vector<fruit::Vec3>& visible,
                       const fruit::JobControl& ctl, const std::vector<TipEdit>& edits = {}, bool lower_peak = false);
// The same with `c` for every part, all on one raft (as jobs were made before
// parts had their own): tests, and the orientation search's one part.
SupportResult generate(const Document& doc, const SupportChoices& c, const std::vector<fruit::Vec3>& visible,
                       const fruit::JobControl& ctl, const std::vector<TipEdit>& edits = {});
// Every part with `c`, on one raft.
SupportSetup oneRaft(const Document& doc, const SupportChoices& c);

// With no raft the planner gives no tips to what starts within `held_mm` of
// the plate (what the bottom layers print of the model, modelInBottomLayers):
// how many of its layers that is. Supports planned for another count would
// come out differently, so they no longer count.
int plateHeldLayers(double held_mm);
// The planner's tips for the whole job as one model, with `c`.
SupportPlan planTips(const Document& doc, const SupportChoices& c, const std::vector<fruit::Vec3>& visible,
                     const fruit::JobControl& ctl, const std::vector<TipEdit>& edits = {});
// Each part's tips, with its own choices and raft. `known`: plans made
// already for some parts (for these choices), not made again.
JobPlan planJob(const Document& doc, const SupportSetup& s, const std::vector<fruit::Vec3>& visible,
                const fruit::JobControl& ctl, const std::vector<TipEdit>& edits = {}, const JobPlan* known = nullptr);
// How many tips each stop of How many (1 to 5) would give each part, the
// part's other choices as they are: what generate() would end with, hand
// edits included, without building the supports. `known`: plans made already
// at a part's own stop (planJob's), not made again; `plans`: every part's plan
// at every stop.
using TipCounts = std::array<std::size_t, kQuickStops>;
std::map<int, TipCounts> partTipCounts(const Document& doc, const SupportSetup& s,
                                       const std::vector<fruit::Vec3>& visible, const fruit::JobControl& ctl,
                                       const std::vector<TipEdit>& edits = {}, const JobPlan* known = nullptr,
                                       std::map<int, std::array<SupportPlan, kQuickStops>>* plans = nullptr);
// The job's tips at each stop of All parts' How many: a part with a How many
// of its own keeps its own stop.
TipCounts jobTipCounts(const std::map<int, TipCounts>& counts, const SupportSetup& s);
// The plans as made (before their hand edits), from the supports finished from them.
JobPlan planOf(const SupportResult& r);
// One plan for the whole job (an older job's, or planTips'), split by the
// part each tip is on.
JobPlan splitPlan(const Document& doc, SupportPlan plan);
// What stays the same from one rebuild of a job's supports to the next while
// only its tips change, as in lowerPeak's rounds and hand edits: the same job
// and setup. Kept by finishWithEdits when it is given one; only for that.
struct RebuildCache {
    std::shared_ptr<const fruit::DetailMap> details;  // the job's slots and holes
    // The job's model, indexed (the index points into this copy), and its
    // sharp edges: for the check after making.
    std::shared_ptr<const fruit::TriangleMesh> model;
    std::shared_ptr<const fruit::AabbTree> model_index;
    std::shared_ptr<const fruit::SharpEdgeIndex> model_edges;
    // By part id: the tips its tree was last built from (hand edits applied),
    // the raft it stood on, and what was built (the tips as built too). Then
    // what was worked out from that tree, once it was: kept while the tree is.
    struct Tree {
        bool made = false;
        int version = 0;  // which tree it is (RebuildCache::made when it was made)
        std::vector<fruit::SupportPoint> from, built;
        double raft_thickness = 0;
        fruit::SupportTree tree;
        SupportChoices with;                          // the part's settings for what follows
        std::optional<fruit::GapCheck> gaps;          // the check after making
        std::shared_ptr<const fruit::TriangleMesh> mesh;  // its supports' mesh
        bool mesh_raft = false;                       // made with its raft (not a shared one)
        bool checked = false;                         // the two below
        fruit::DropCheck drops;                       // what the tips no support reached leave unheld
        std::size_t short_from_raft = 0;              // layers short of hold from the raft
    };
    std::map<int, Tree> trees;
    int made = 0;  // trees made so far
    // By the parts on a raft, for their trees as kept (`trees`, their versions): its outline (for
    // rafts that meet, with the first part's settings), the shared raft under
    // them, and its pieces through its height (with the raft's settings).
    struct Raft {
        std::vector<int> trees;
        std::optional<fruit::Paths> meet;
        std::vector<SupportChoices> with;  // its parts' settings for what follows
        std::optional<fruit::SupportTree> under;
        std::shared_ptr<const fruit::TriangleMesh> under_mesh;
        std::optional<fruit::Paths> foot;
        int pieces = 0;
    };
    std::map<std::vector<int>, Raft> rafts;
};
// The planners' tips with hand edits applied, then the trees, the rafts, the
// checks and the problems (keeps both). Used when a saved job comes back with
// its tips, and after a hand edit. The parts' trees are built side by side;
// with `cache`, a part whose tips are as they were last time keeps its tree.
SupportResult finishWithEdits(const Document& doc, const SupportSetup& s, JobPlan plan,
                              const std::vector<TipEdit>& edits, const fruit::JobControl& ctl,
                              RebuildCache* cache = nullptr);
// The same for one plan and `c` for every part, all on one raft.
SupportResult finishWithEdits(const Document& doc, const SupportChoices& c, SupportPlan plan,
                              const std::vector<TipEdit>& edits, const fruit::JobControl& ctl);
// Replays the print made as `r` (checkRisk, the resin peakWeaker() times
// weaker than its values) and strengthens it where part of the model goes
// with what lets go, round after round, rebuilding each time. A tip that part
// of the model went with in the replays, most first, is made a size bigger
// (where the bigger contact still sits on its face), or, already bigger or at
// the biggest size, gets a neighbour on its own piece; a fresh skin likely to
// tear over enough of the model to count, or a mid-air piece, gets tips there.
// A tip that may let go while its neighbours keep the model is left as it is.
// A round is kept only when the chance of losing part of the model, or how
// much of it to expect lost, comes down; it stops when that chance is under
// kPeakChance, a round brings nothing, or after kPeakRounds. The tips it adds
// are the planner's (in `plans`, saved with the job; a hand-deleted tip stays
// deleted). Places more tips don't fix (a thin place, bending, a rod) are
// counted, not changed. Progress: "replay|…" for the first replay, then for
// round n "round n|pick", "round n|build <tips added> <made bigger>|…",
// "round n|replay|…" and, built again without the tips that made it worse,
// "round n|again|…"; each round as much of the bar as the most rounds share it.
SupportResult lowerPeak(const Document& doc, const SupportSetup& s, JobPlan plans, const std::vector<TipEdit>& edits,
                        SupportResult r, const fruit::JobControl& ctl);
inline constexpr int kPeakRounds = 6;
// What the loop did, in a line for the Supports panel ("" when it didn't run).
std::string peakLine(const PeakFix& f);
// The chance of losing part of the model the loop aims below.
inline constexpr double kPeakChance = 0.01;
// How much weaker than its starting values the loop takes a resin whose tip
// strengths are borrowed (nothing measured them): one step of the tips
// plate's loads (fruit::kOneLoadStep), the least the plate tells apart.
inline constexpr double kPeakWeaker = 1.41;
// What the loop takes: kPeakWeaker with borrowed tip strengths, 1 with
// measured ones. A measured tip is trusted with 70 % of where half let go
// (1 / 0.7, about that same step): a step weaker again would count that
// margin twice, and every tip loaded as far as it is trusted would be as
// likely to let go as not.
double peakWeaker(const fruit::SupportConfig& planner);

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
// The tips a rebuild of `made` with `edits` starts from, as finishWithEdits
// has them: part by part in the job's order, the planner's (`made`'s plans)
// with the edits applied, each tip as it was built in `made` where it is
// there. With the part (an id) each is on. What the view shows of an edit at
// once, while the supports are made for it (when the plan still fits).
struct EditedTips {
    std::vector<fruit::SupportPoint> points;
    std::vector<int> parts;  // by tip: its part's id
};
EditedTips editedTips(const Document& doc, const SupportResult& made, const std::vector<TipEdit>& edits);
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

// How Generating supports shares its bar out: the plan to kPlanShare of the
// making, then the trees, their shapes and the checks; with the loop that
// strengthens tips (lowerPeak), the making to kMadeShare and the loop the
// rest. Each part's progress says which it is: "plan|…", "make|…" (its
// checks "make|check|…"), "strengthen|…"; with more than one part, the plan
// for each "plan|part i/n|…".
inline constexpr double kPlanShare = 0.55;
inline constexpr double kMadeShare = 0.6;
// The stages the running card lists, and which one a progress report is in
// (by its labels; a bare engine stage by its name).
const std::vector<std::string>& supportStages();
int supportStage(const std::string& stage);
// What the running card says under them, from the same report: the part being
// planned, the strengthening loop's round and what it is doing ("" for none).
std::string supportsDoing(const std::string& stage);

// Why a tip is where it is, in a sentence or two, and its numbers.
std::string tipKind(const fruit::SupportPoint& p);
std::string tipWhy(const fruit::SupportPoint& p);
std::string tipNumbers(const fruit::SupportPoint& p, const fruit::SupportConfig& cfg, const fruit::TreeConfig& tc);

// r.problems as they stand now: the hollows accepted on the Hollow screen
// since the supports were made moved to a note of their own, and each
// suction cup's card with its numbers.
std::vector<SupportProblem> problemsNow(const Document& doc, const SupportResult& r);
std::size_t countOf(const std::vector<SupportProblem>& problems, SupportProblem::Level l);

}  // namespace gui
