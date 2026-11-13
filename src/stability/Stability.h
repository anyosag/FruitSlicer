// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Unstable areas: what each layer's pull does to the print so far, and how
// likely each 3D pixel (one printer pixel across, one layer tall) is not to
// print.
//
// The print is replayed from the plate up. For every layer:
//
//  1. the pull on it: its lit area (more away from the vat's hinge when that
//     side is known, plus the suction of any cup open in that layer);
//  2. that pull is passed through the print so far. Every layer part of the
//     model is a stiff sheet, joined to the parts under it over the area they
//     share; a support is a rod that pulls only along its length, and a tip
//     is a pin that cannot bend. Thin, fresh model is held by the tips near
//     each patch of it. Anything thicker shares its pull among all that
//     holds it, by stiffness, through every joint of the model at once: so a
//     pull also finds its way round through a joint at the side. That gives
//     the force and the bending at every joint between two layers, and the
//     pull on every tip;
//  3. the stress at each joint is set against what the resin stands. Every
//     joint keeps the worst it ever sees ("how close to breaking").
//
// A weakest-link curve turns "how close" into a chance of breaking. Then the
// print is replayed many times with every strength drawn from that curve:
// what breaks hands its load to what else holds the same piece, and whatever
// is left hanging from nothing is lost. A 3D pixel's chance of not printing
// is the share of replays in which it was lost, joined with the chance that
// its own fresh skin never formed.
//
// Everything is in the planner's own units: a load is "mm² of layer", and a
// strength is how many mm² of layer a piece of resin holds. The pull's real
// size in newtons never enters.
//
// What it assumes, and does not know:
//  - the pull is in proportion to lit area (published for other machines);
//  - nothing holds a piece of the model sideways, so an off-centre pull bends
//    it in full. The film may hold the newest layer sideways; then the
//    bending here is too high. It is an upper limit;
//  - supports are held sideways by the model they carry, so they do not bend;
//  - how thick a slab still is "thin" (`slab`), and how far a patch of it
//    looks for tips (`share_reach`), are reasoned, not measured;
//  - the sideways pull of the tilt, sway, curl from shrinkage and bending
//    inside thin flexible sheets are left out;
//  - the strengths are the planner's borrowed ones until a resin's test
//    plates are fitted.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/JobControl.h"
#include "geometry/Polygon.h"
#include "layers/HollowAnalysis.h"
#include "layers/IslandGraph.h"
#include "layers/LayerStack.h"
#include "support/SupportConfig.h"

namespace fruit {

// A contact smaller than this (mm²: under that of a 0.2 mm tip, 0.031 mm²)
// is a sliver where two outlines barely meet. One with nothing at risk above
// it may snap and nothing comes of it: it is not listed as a weak place.
inline constexpr double kSliverContact = 0.02;

// Less of the model than this (mm³) going with a joint that lets go is not
// counted as losing any of it.
inline constexpr double kCountsAsLost = 0.1;

// Where a support's tip touches the model, and how big the tip is.
struct TipContact {
    Vec3 at;
    double diameter = 0.4;  // mm
    bool foot = false;      // turned upside down: the tip a support stands on the model with
};

struct StabilityConfig {
    // What a tip holds, in mm² of layer by tip diameter: the planner's table
    // (the resin's measured one, where a plate was fitted).
    CapacityModel capacity;
    // That table is the load a tip is trusted with: this share of the load at
    // which half of such tips let go. 0.7 for a fitted table (as `fruitslicer
    // fit` writes it). The built-in table is borrowed and has no such load
    // behind it; stabilityConfig() takes its numbers as a third of it.
    double usable_fraction = 0.7;
    // The print's own resin across layers, per mm², as a multiple of a tip
    // contact's per mm². Not measured: 1 until neck coupons say otherwise.
    double bulk_factor = 1.0;
    // How sharply "holds" turns into "breaks" (the Weibull modulus). Published
    // for resin that was not post-cured: 5 to 11.
    double steepness = 7.0;

    // The pull. More on the side away from the hinge, as the planner has it.
    double peel_bias = 0.0;
    Vec2 peel_axis{0, 0};
    Vec2 plate_half{105.84, 59.185};
    // 0: pull in proportion to area alone. 1: thin and spiky outlines pull
    // less, as Pan et al. (2017) measured on another vat floor. Off until
    // measured here: the planner's tip table has no such term.
    double shape_effect = 0.0;
    // Suction over a cup's opening, as a multiple of the pull on the same
    // area of resin. The air sets a limit of 101 kPa, 2 to 8 times the
    // published pulls; the real value is not known.
    double cup_pull = 4.0;

    // A thin slab is held patch by patch by the tips near each patch; a
    // thick piece shares a pull among all that holds it, by stiffness. Where
    // the model under a patch is `slab` mm thick, half of the patch's pull is
    // given outright to the tips near it (all of it at one layer, a fifth at
    // twice `slab`). Not measured: an assumption.
    double slab = 2.0;
    // How far "near" reaches, mm, where the model under the patch is 1 mm
    // thick; it grows with the 3/4 power of that thickness (a plate on
    // springs). 1.8 mm is for 1 mm supports about 6 mm tall at the planner's
    // spacing; it changes only with the fourth root of those. A very large
    // value gives every pull outright to all the tips of its piece at once,
    // as to the legs of a stiff table.
    double share_reach = 1.8;

    // A fresh skin (one layer hanging past what holds it) tears or sags in
    // half the cases at this reach, mm.
    double skin_half_reach = 3.82;

    double link_grow = 0.02;    // as the planner links layers (mm)
    double speck_area = 0.12;   // unheld parts smaller than this are specks (mm²)
    double pixel_area = 2.7e-4; // one printer pixel (mm²): a part smaller than this is not printed

    int replays = 500;
    std::uint64_t seed = 1;
    unsigned threads = 0;  // 0 = all cores
};

// The planner's settings as this model uses them: the same tip table, hinge
// and layer linking, and the reach from the planner's tip reach.
StabilityConfig stabilityConfig(const SupportConfig& cfg);

// The chance that a piece loaded to `closeness` breaks (1 = half of them do).
double breakChance(double closeness, double steepness);

struct StabilityInput {
    // Model, supports and raft together, as the printer cuts them. Required.
    const LayerStack* print = nullptr;
    // The model alone on the same layers. With it the supports are what of
    // each layer lies outside the model, and a tip is where a support lies
    // under the model. Without it the whole print is treated as the model.
    const LayerStack* model = nullptr;
    // The supports and raft alone on the same layers, whole (the part of each
    // tip sunk into the model included). Without them they are taken as what
    // of `print` lies outside the model, which thins each tip where it meets
    // the model.
    const LayerStack* supports = nullptr;
    // Where support tips touch the model: under it, and (turned upside down)
    // where a support stands on it. Without them a tip is sized from the
    // area it shares with the model.
    std::vector<TipContact> tips;
    // The model's parts, if already worked out (for `model`, linked with
    // `link_grow`, as the tip planner returns them); otherwise worked out here.
    std::shared_ptr<const IslandGraph> model_graph;
    // The print's cups, if already worked out; otherwise worked out here.
    const HollowReport* hollows = nullptr;
};

enum class RiskCause : std::uint8_t {
    None,
    Unheld,   // nothing held it when it printed, or only a sliver of contact does
    Skin,     // a fresh skin too far from what holds it
    Tip,      // a support tip pulled harder than it holds
    Neck,     // a thin cross-section pulled apart
    Bend,     // a cross-section bent by an off-centre pull
    Support,  // a support rod pulled apart
};
const char* causeName(RiskCause c);

// Where one part is: the model's, or the supports' outside the model.
struct PrintPart {
    int layer = 0;
    BBox2 box;
    double area = 0;                   // mm²
    const ExPolygon* shape = nullptr;  // kept alive by the result
};

// One part of one layer, and the joint it rests on.
struct PartRisk {
    float closeness = 0;     // worst "how close to breaking" its joint ever sees (1 = half break)
    float load = 0;          // the pull through that joint then, mm² of layer
    float stress = 0;        // the peak stress in it then, as a multiple of the pull per mm²
    float break_chance = 0;  // chance that a break starts at that joint
    float lost_chance = 0;   // share of replays in which the part was not on the finished print
    std::int32_t at_layer = -1;  // the layer that was printing when it was closest
    Vec2 joint;              // where that joint is
    float joint_area = 0;    // mm² of it
    RiskCause cause = RiskCause::None;  // what the closeness comes from
    bool held = true;        // false: nothing held it when it printed
    bool model = true;       // false: part of the supports or the raft
};

// Where, in one part's layer, a fresh skin has at least this chance of not forming.
struct SkinBand {
    int part = -1;
    float chance = 0;
    Paths region;
};

// A place worth looking at.
struct WeakSpot {
    RiskCause cause = RiskCause::None;
    Vec3 at;                 // printer coordinates
    int layer = 0;           // the joint's layer
    int part = -1;
    double chance = 0;       // that a break starts here (Unheld, Skin: that it does not print)
    double lost_chance = 0;  // that the part half a millimetre above it is not on the finished print
    double closeness = 0;
    double area = 0;         // mm²: the joint, the skin past its reach, or the unheld part
    double load = 0;         // mm² of layer pulling on it at its worst
    double near = 0;         // a tip: how much of that was thin, fresh model near it (the rest is its share of the piece)
    double holds = 0;        // mm² of layer at which half of such joints break
    // mm³ of the model that goes with it: the average over the replays in
    // which this joint let go (0 where it is still held another way); all of
    // a part that starts in mid-air; 0 for a skin.
    double lost_volume = 0;
    int at_layer = -1;       // the layer printing at its worst
    int count = 1;           // joints of the same kind within reach that this one stands for
};

// One support tip that touches the model, and the most it is ever pulled by.
struct TipRisk {
    Vec3 at;                 // where it touches the model, printer coordinates
    int contact = -1;        // its place in StabilityInput::tips; -1 for a tip nobody told us about
    bool foot = false;       // turned upside down: a support stands on the model with it
    double chance = 0;       // that it lets go
    double lost_chance = 0;  // that the part half a millimetre above it is not on the finished print
    double lost_volume = 0;  // mm³ of the model that goes with it, as WeakSpot::lost_volume
    // The same averaged over every replay: what it costs per print. When the
    // tips let go one after another, the model falls with whichever went
    // last, so over many replays every one of them carries a little of it.
    double lost_per_print = 0;
    double closeness = 0;
    double area = 0;         // mm² it touches the model with
    double load = 0;         // mm² of layer pulling on it at its worst
    double near = 0;         // how much of that was thin, fresh model near it
    double holds = 0;        // mm² of layer at which half of such tips let go
    int at_layer = -1;       // the layer printing at its worst (-1: never pulled)
};

struct StabilityStats {
    std::size_t layers = 0, parts = 0, model_parts = 0;
    std::size_t tips = 0;             // tips that touch the model: under it, or where a support stands on it
    std::size_t unheld = 0;           // model parts that start in mid-air, specks left out
    std::size_t unheld_specks = 0;
    std::size_t slivers = 0;          // overloaded joints under 0.02 mm² with nothing at risk above: not listed
    double model_volume = 0;          // mm³
    // Model volume whose chance of not printing is at least 1, 5, 20 and 50 %.
    double at_risk[4] = {0, 0, 0, 0};
    double expected_loss = 0;         // mm³: volume × chance, summed
    // The share of replays in which part of the model (kCountsAsLost or
    // more) went with tips that let go, and how much on average in those.
    double tips_cost_chance = 0;
    double tips_cost_volume = 0;      // mm³
    double whole_chance = 1;          // share of replays in which less than 1 mm³ of the model was lost
    double worst_closeness = 0;
    int replays = 0;
    double seconds = 0;
};

struct StabilityResult {
    std::shared_ptr<const IslandGraph> model_graph, support_graph;  // own the parts' shapes
    double layer_height = 0.05;
    std::vector<PartRisk> parts;                // every part of the print
    std::vector<PrintPart> where;               // by the same index
    std::vector<std::vector<int>> in_layer;     // part indices, layer by layer
    std::vector<std::vector<SkinBand>> skins;  // by layer, rising chance within a part
    std::vector<WeakSpot> spots;               // what would lose part of the print first, then the likeliest
    std::vector<TipRisk> tips;                 // every tip that touches the model, lowest first
    std::vector<float> closeness_by_layer;     // the weakest joint while each layer prints
    StabilityStats stats;

    // The part of `layer` at `xy`, or -1.
    int partAt(std::size_t layer, const Vec2& xy) const;
    // The chance that the 3D pixel at `xy` in `layer` does not print: lost
    // with what it hangs from, or its own skin never formed. 0 off the print.
    double chanceAt(std::size_t layer, const Vec2& xy) const;
    // The chance that a break starts at the joint under that 3D pixel.
    double breakAt(std::size_t layer, const Vec2& xy) const;
    // The same for a whole part, anywhere in it (its skins left out).
    double partChance(int part) const;
};

// Reports stages "parts" (unless given), "stress", "skins" and "replays".
StabilityResult analyzeStability(const StabilityInput& in, const StabilityConfig& cfg,
                                 const JobControl& ctl = JobControl::none());

}  // namespace fruit
