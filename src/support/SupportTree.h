// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Support structure from support points (milestone M2, phase-1 routing).
//
//   point --pinhead--> junction --(bridge ≤ 45°)--> pillar --> base cone --> raft
//
// A support touches the model at its tip and nowhere else: shafts, bridges,
// braces and feet keep a gap to it (a support that stands on the model does so
// on a second tip). Heads follow the surface normal clamped to 45° from
// vertical, searching other directions when the head or its drop would hit
// the model. Up to
// `max_heads_per_pillar` heads share a vertical pillar through 45° bridges;
// a head with no clear drop and no reachable pillar stands on the model
// below, however far down that is, on a reversed pinhead as wide as its own
// tip ("anchor", reported because it marks an upward surface): a spot is
// never left without a support for want of a way to the raft. A head whose
// support would stand where another already stands on the model joins that
// one, and the foot they share is as wide as their tips together. Where the
// model is too close under a tip for any head, a short column joins the two.
// Pillars taller than 15 mm are cross-braced, those on the raft with each
// other and those on the model with each other, but never within the flex
// zone below the heads, so tips bend and snap at the part (Formlabs'
// light-touch idea). The raft is one pad per pillar joined by bars, so the
// whole support comes off in one piece. A tip nearer the plate than a head
// is long stands on the plate on its head alone: nothing reaches below z = 0.

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/JobControl.h"
#include "mesh/AabbTree.h"
#include "geometry/Polygon.h"
#include "mesh/TriangleMesh.h"
#include "support/Raft.h"
#include "support/SupportGeometry.h"
#include "support/SupportPoint.h"

namespace fruit {

class DetailMap;

// Pads: a small pad under each trunk, joined by bars. Full: one solid slab
// under all the trunks with a sloped edge. None: no raft.
enum class RaftStyle { Pads, None, Full };

struct TreeConfig {
    double max_head_tilt_deg = 45.0;
    double head_length = 1.0;          // contact → junction cone length
    double flex_zone = 2.0;            // no bracing within this distance below a junction
    // The least gap a support keeps to the model, except at its tip: the gap
    // that fuses on the printer, plus a pixel (an edge drawn without
    // anti-aliasing can land half a pixel nearer, on each side). The resin's
    // gap test measures it (Resin::gap_test); this is the stand-in until it
    // is. It was 0.15 mm: on DishRack6.goo that let shafts stand 0.17-0.22
    // mm from the walls of hollow posts, and they printed glued to them; so
    // the stand-in is wider than that. ASSUMPTION, not measured.
    double clearance = 0.30;
    // A shaft goes where it has the most room, up to this gap: no more than
    // the first spot that keeps `clearance` (DishRack6: shafts 0.17 mm from a
    // post's wall where the middle of the post had 2.4 mm). Further than this
    // from the model a shaft is clear of every way it could stick to it: the
    // light that spills into a narrow gap and the sway of a long shaft (a few
    // tenths of a mm), and the liquid a narrow gap holds through the wash (in
    // a 9 mm deep post, any gap under about 0.7 mm).
    double wanted_gap = 1.0;
    // Slots, holes and the space under where they open downwards, that no
    // support enters (by half `clearance`, so heads from either side of a
    // slot stay that far apart). Null: none.
    std::shared_ptr<const DetailMap> details;
    // The gap a foot that stands on the plate itself keeps to the model. Wider
    // than `clearance`, because the first layers are cured much longer and
    // come out wider than drawn: a foot nearer than this may print joined to
    // the model. The app works it out from the job's exposures
    // (bottomLayerGap); 0.5 mm where they are not known, an ASSUMPTION. With
    // a raft the foot stands above those layers and keeps `clearance`.
    double foot_clearance = 0.5;
    double max_bridge_length = 5.0;    // horizontal reach of a 45° bridge to a pillar
    int max_heads_per_pillar = 3;
    double brace_min_height = 15.0;    // brace pillars taller than this
    double brace_max_distance = 10.0;  // to neighbours within this
    double brace_diameter = 0.6;
    double brace_step = 6.0;           // vertical spacing of brace levels
    double max_route_length = 15.0;    // diagonal search for a clear drop
    // Longest pillar allowed to stand on the model: any the printer can build.
    // (10 mm until 2026-11: on a hollow or a cap a quarter of the tips then got
    // no support at all.) 0: never stand on the model.
    double max_model_anchor_drop = 250.0;
    // A pillar stands on the model only where the face is no steeper than this
    // from level, so its foot sits on it instead of sliding down a wall.
    double max_anchor_slope_deg = 60.0;
    // Where the model is so close under a tip that no head fits (inside a
    // thread, between ribs, under raised text), a straight column as wide as
    // the tip joins the two, if the model is at most this far below. 0: none.
    double max_stub_length = 3.0;
    // Branching: pillars within reach merge where 45° branches from each meet,
    // below the flex zone; fewer, thicker trunks reach the raft.
    bool merge_pillars = true;
    double max_merge_distance = 8.0;
    int max_tips_per_trunk = 6;
    double max_trunk_diameter = 1.8;
    RaftStyle raft = RaftStyle::Pads;
    double raft_thickness = 1.0;       // raised to the printer's bottom+transition layers if thinner
    double base_diameter = 3.0;
    double base_height = 1.0;
    double pad_brim = 0.8;
    double link_width = 1.2;          // raft link bar width
    double full_raft_margin = 1.0;    // Full: the slab reaches this far past each pad's brim
    double full_raft_gap = 10.0;      // Full: gaps up to this wide between pads are filled in
    // A raft other than the old Pads and Full ones, from the Raft page's
    // settings (applyRaftSpec): built by appendSpecRaft. Empty: the fields above.
    std::optional<RaftSpec> raft_spec;
    // Measured (tip diameter, penetration) pairs from a resin profile; empty =
    // 0.5 × diameter clamped to [0.08, 0.2] mm.
    std::vector<std::pair<double, double>> penetration_table;
    // Breakaway necks measured on the resin's tips plate: (tip diameter, how
    // far out from the surface, width as a share of the tip's) for the tip
    // sizes whose necked heads left a smaller mark and held as well. Others
    // keep the plain head. Empty: none.
    std::vector<std::array<double, 3>> neck_table;
    GeometryQuality quality;
};

struct Head {
    int point = -1;
    Vec3 contact, dir, junction;
    PinheadSpec spec;
    // So close to the plate that a head and pillar would reach through it:
    // the head's cone runs down to the plate and stands on it, with no
    // pillar. `junction` is then the middle of its foot on the plate.
    bool on_plate = false;
    // No head fits: a short straight column to the model just below holds
    // the tip instead (SupportTree::stubs). `junction` is where it lands.
    bool stub = false;
    // At its planned size and place no head found a way down: built a size
    // or two smaller (spec.tip_diameter), or this far from where it was
    // planned (mm, along the surface; `contact` is where it touches).
    bool smaller = false;
    double moved = 0;
};

struct Pillar {
    Vec2 xy;
    double top = 0, bottom = 0;  // z range of the shaft
    double diameter = 1.0;
    // How wide its foot is where it stands on the raft or the plate: the
    // tree's base_diameter where there is room, less beside the model, so the
    // foot keeps its gap as the shaft does. 0: the tree's base_diameter.
    double base_diameter = 0;
    std::vector<int> heads;
    int bridges = 0;
    bool on_model = false;       // stands on the model (anchor) instead of the raft
    bool merged = false;         // ends in a branch into a trunk instead of reaching the raft
    int tips = 1;                // tips carried (for trunk limits)
    double lowest_attach = 1e9;  // lowest bridge attachment on the shaft (a merge must start below it)
    Vec3 anchor_contact, anchor_normal;
    // On the model: the diameter of the contact its foot makes there, the
    // tip's own, so the foot holds what the tip holds. Where several tips
    // share the pillar (their supports would have stood on the same spot), as
    // much contact as those tips have together.
    double foot_tip = 0.4;
};

struct Rod {  // bridges and braces
    Vec3 a, b;
    double diameter = 1.0;
};

struct SupportTree {
    std::vector<Head> heads;
    std::vector<Pillar> pillars;
    std::vector<Rod> bridges;
    std::vector<Rod> braces;
    std::vector<Rod> branches;   // pillar -> trunk diagonals
    std::vector<Rod> stubs;      // short columns where no head fits, tip (a) to the model below (b)
    std::vector<Rod> raft_links;
    double raft_top = 0;
    std::vector<Warning> warnings;
    size_t dropped_points = 0;
    // Tips built a size or two smaller than planned, or moved a little, because
    // at their own size and place no head found a way down (Head::smaller, moved).
    size_t smaller_heads = 0, moved_heads = 0;
    size_t braces_skipped_trap = 0;  // pillar pairs left unbraced because model passes between them

    size_t trunks() const {  // pillars standing on the raft
        size_t n = 0;
        for (const auto& p : pillars) n += !p.on_model && !p.merged;
        return n;
    }

    // The supports and their raft; with `raft` false, the supports alone (each
    // trunk's foot as ever, but no pads, slab or bars under them).
    TriangleMesh mesh(const TreeConfig& cfg, bool raft = true) const;
    // The raft alone: what mesh() adds for it (pads, slab or the Raft page's
    // raft, and the bars that join it), under every trunk of this tree.
    TriangleMesh raftMesh(const TreeConfig& cfg) const;
    // What the raft covers on the plate (empty with none): to tell whether two
    // rafts would meet.
    Paths raftFootprint(const TreeConfig& cfg) const;

    // The Full raft's outline at height `inset` mm up its sloped edge (0 = on
    // the plate): the footprint of the pads, closed over gaps and pulled in
    // by `inset`. Empty for other raft styles.
    Paths fullRaftOutline(const TreeConfig& cfg, double inset = 0) const;
    // How far the sloped edge pulls in by the top of the slab.
    double fullRaftInset(const TreeConfig& cfg) const;
};

// Joins the trunks' feet along a minimum spanning tree (raft_links), so the
// raft under them is one piece: what the builder does for its own tree, for a
// tree put together from several (parts that share a raft).
void linkRaft(SupportTree& tree);
// `from` added at the end of `into`: its heads' points are `point_offset`
// further on (where its points are in the joined list), and its pillars'
// heads where its heads are now. The raft's top is the higher of the two.
void appendTree(SupportTree& into, const SupportTree& from, int point_offset);

// How deep a tip of diameter `tip_d` sinks into the part: the nearest
// measured value from the profile, or half the diameter within 0.08–0.2 mm.
double tipPenetration(double tip_d, const TreeConfig& cfg);

// The gap a support keeps to the model down where the bottom layers print,
// for a printer whose bottom layers get `light_ratio` times the light of the
// others: `gap`, widened as the light spilling into a narrow gap is, by the
// log of the light (gap ∝ ln(light / what sets the resin)). The others get
// about kSpillContrast times what sets the resin, counting what the next
// layers add (ASSUMPTION: a layer cures about twice its thickness, from the
// two exposures tested on the user's resin, 1.5 s at 0.03 mm and 2.5 s at
// 0.05 mm). 27.5 s bottom layers over 1.5 s ones: 0.30 mm becomes 0.70 mm.
// `light_ratio` <= 1 (or not known): `gap`.
inline constexpr double kSpillContrast = 9.0;
double bottomLayerGap(double gap, double light_ratio);

// Reports stage "tree".
SupportTree buildSupportTree(const TriangleMesh& model, const std::vector<SupportPoint>& points,
                             const TreeConfig& cfg, double min_raft_thickness,
                             const JobControl& ctl = JobControl::none());

// For the planner's rule on each tip's own load (PointGenOptions::support_lengths):
// how long the support under each of `points` would be if the tree were built
// for them now, in their order (mm). Only where it would stand on the model:
// from the tip down to its foot, and as much again as the narrow foot gives
// (a foot is a second tip; it gives like head_length × shaft / foot of plain
// shaft). 0 for a tip whose support reaches the raft or the plate, and for
// one that gets none.
std::vector<double> supportLengthsOnModel(const TriangleMesh& model, const std::vector<SupportPoint>& points,
                                          const TreeConfig& cfg, double min_raft_thickness,
                                          const JobControl& ctl = JobControl::none());

// `points` as passed to buildSupportTree, split into the tips that got a head
// and those the tree dropped (for checkDroppedTips).
void splitByTree(const SupportTree& tree, const std::vector<SupportPoint>& points, std::vector<SupportPoint>& built,
                 std::vector<SupportPoint>& dropped);

// The tips that got a head, split by what their support stands on: the raft
// or the plate (`from_raft`), or the model (`tied`, with where the foot is).
// Dropped tips are in neither. For layersShortOfRaftHold.
void splitByFooting(const SupportTree& tree, const std::vector<SupportPoint>& points,
                    std::vector<SupportPoint>& from_raft, std::vector<TiedTip>& tied);

}  // namespace fruit
