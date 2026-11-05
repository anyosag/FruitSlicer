#pragma once
// Support structure from support points (milestone M2, phase-1 routing).
//
//   point --pinhead--> junction --(bridge ≤ 45°)--> pillar --> base cone --> raft
//
// Heads follow the surface normal clamped to 45° from vertical, searching
// other directions when the head or its drop would hit the model. Up to
// `max_heads_per_pillar` heads share a vertical pillar through 45° bridges;
// a head with no clear drop and no reachable pillar lands on the model with
// a small reversed pinhead ("anchor", reported because it marks an upward
// surface). Pillars taller than 15 mm are cross-braced, but never within the
// flex zone below the heads, so tips bend and snap at the part (Formlabs'
// light-touch idea). The raft is one pad per pillar joined by bars, so the
// whole support comes off in one piece.

#include <string>
#include <vector>

#include "mesh/AabbTree.h"
#include "mesh/TriangleMesh.h"
#include "support/SupportGeometry.h"
#include "support/SupportPoint.h"

namespace fruit {

enum class RaftStyle { Pads, None };

struct TreeConfig {
    double max_head_tilt_deg = 45.0;
    double head_length = 1.0;          // contact → junction cone length
    double flex_zone = 2.0;            // no bracing within this distance below a junction
    double clearance = 0.15;           // min distance from support to model (except at the tip)
    double max_bridge_length = 5.0;    // horizontal reach of a 45° bridge to a pillar
    int max_heads_per_pillar = 3;
    double brace_min_height = 15.0;    // brace pillars taller than this
    double brace_max_distance = 10.0;  // to neighbours within this
    double brace_diameter = 0.6;
    double brace_step = 6.0;           // vertical spacing of brace levels
    double max_route_length = 15.0;    // diagonal search for a clear drop
    double max_model_anchor_drop = 10.0;  // longest pillar allowed to stand on the model
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
    GeometryQuality quality;
};

struct Head {
    int point = -1;
    Vec3 contact, dir, junction;
    PinheadSpec spec;
};

struct Pillar {
    Vec2 xy;
    double top = 0, bottom = 0;  // z range of the shaft
    double diameter = 1.0;
    std::vector<int> heads;
    int bridges = 0;
    bool on_model = false;       // stands on the model (anchor) instead of the raft
    bool merged = false;         // ends in a branch into a trunk instead of reaching the raft
    int tips = 1;                // tips carried (for trunk limits)
    double lowest_attach = 1e9;  // lowest bridge attachment on the shaft (a merge must start below it)
    Vec3 anchor_contact, anchor_normal;
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
    std::vector<Rod> raft_links;
    double raft_top = 0;
    std::vector<Warning> warnings;
    size_t dropped_points = 0;

    size_t trunks() const {  // pillars standing on the raft
        size_t n = 0;
        for (const auto& p : pillars) n += !p.on_model && !p.merged;
        return n;
    }

    TriangleMesh mesh(const TreeConfig& cfg) const;
};

SupportTree buildSupportTree(const TriangleMesh& model, const std::vector<SupportPoint>& points,
                             const TreeConfig& cfg, double min_raft_thickness);

}  // namespace fruit
