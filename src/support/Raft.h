// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The raft's settings (what the Raft page sets), the built-in presets, and
// building a raft from them under a support tree.
//
// Least resin and Plain base are the Pads and Full rafts FruitSlicer has
// always made. Settings that match them, in a preset of the user's own too,
// build through that same code, so their print files stay as they were.
// Anything else is built here, one 0.1 mm layer at a time: a tapered side,
// a rounded lip, windows at the plate, rounded edges, bars and a frame.

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "geometry/Polygon.h"
#include "geometry/Vec.h"
#include "mesh/TriangleMesh.h"

namespace fruit {

struct SupportTree;
struct TreeConfig;

enum class RaftKind { Pads, Full };
// How many windows: none, about every 30 or 18 mm along the edge, or one at every lobe tip.
enum class RaftWindows { None, Few, Some, EveryTip };
// Fewest: each pad joined to its nearest neighbours, the fewest bars that make one piece. Grid: every neighbour too.
enum class PadJoin { Fewest, Grid };

// Least resin's pads: 0.4 mm wider at the plate than at their 1 mm top (180° − atan 2.5).
inline constexpr double kPadsSideAngle = 111.80140948635182;
// Pads with frame's pads: 0.4 mm narrower at the plate than at their 1 mm top (atan 2.5).
inline constexpr double kFramedPadsSideAngle = 68.198590513648185;

struct RaftSpec {
    RaftKind kind = RaftKind::Pads;
    double reach = 0.4;                // the top edge, mm past each trunk's foot
    double thickness = 1.0;            // mm; raised to the printer's long-exposure layers if thinner
    double side_angle = kPadsSideAngle;  // degrees from the plate, outside: < 90 the base tucks in, > 90 it sticks out
    bool rounded = false;              // every edge rounded but the ones on the plate
    double gap_fill = 10.0;            // Full, and the frame: gaps narrower than this are filled in
    double lip_height = 0.0;           // Full, or the frame: a rounded rim round the top, 0 = none
    RaftWindows windows = RaftWindows::None;  // Full, or the frame
    double window_pct = 60;            // window height, % of the thickness (the frame's height)
    double bar_width = 1.2;            // Pads
    double bar_pct = 80;               // Pads: bar height, % of the thickness
    PadJoin join = PadJoin::Fewest;    // Pads
    bool frame = false;                // Pads: a frame round the outside
    double frame_width = 3.6, frame_gap = 1.0, frame_height = 2.0, frame_angle = 60;
    bool operator==(const RaftSpec&) const = default;
};

inline constexpr double kWindowWidth = 2.5;  // mm, across
inline constexpr double kWindowDepth = 1.8;  // mm in from the base edge
inline constexpr double kMinWindowRoof = 0.5;  // mm of raft a window must leave above it
// Window spacing along the edge, mm (0: one at every lobe tip; < 0: none).
double windowSpacing(RaftWindows w);
// How far the base on the plate sits inside the top edge (negative: outside), mm.
double sideInset(double height, double angle_deg);
// The lip's width for its height, mm.
double lipWidth(double lip_height);
// The window's height, mm.
double windowHeight(const RaftSpec& s);

struct RaftPreset {
    std::string name;
    std::string about;  // one line, for the presets list
    RaftSpec spec;
};
const std::vector<RaftPreset>& builtInRaftPresets();
const RaftPreset* builtInRaftPreset(const std::string& name);
inline constexpr const char* kDefaultRaftPreset = "Least resin";
// Least resin's settings: the raft every job had before the Raft page.
RaftSpec leastResinSpec();
RaftSpec plainBaseSpec();

nlohmann::json raftSpecToJson(const RaftSpec& s);
// Missing keys read as Least resin's.
RaftSpec raftSpecFromJson(const nlohmann::json& j);

// A job saved before the Raft page: its raft from the Full switch and the
// values it set on the Advanced page (thickness, Full raft edge and gaps,
// link width), so it builds exactly as it did.
RaftSpec legacyRaftSpec(bool full, const TreeConfig& tc);

// Sets `tc` for the raft (unless it has none): the old Pads or Full fields when
// the settings are theirs, else `tc.raft_spec`.
void applyRaftSpec(const RaftSpec& s, TreeConfig& tc);
// How far the raft reaches past a trunk's foot at its widest, mm.
double raftPastFoot(const TreeConfig& tc);

// The edge's shape across it, (d, z): d in from the top edge (pads: from the
// pad's top edge; with a frame, the frame's outer edge), from the plate up and
// over the lip; a frame's ends back down its inner wall. For drawing the cut.
std::vector<Vec2> raftEdgeProfile(const RaftSpec& s, double height);

// Where a window goes: at the tip of a lobe, pointing out.
struct RaftWindow {
    Vec2 trunk;   // the trunk the lobe is round
    Vec2 dir;     // outward, unit
    double radius = 0;  // the lobe's top edge from the trunk, mm
};

// What the built raft is, for the Raft page's checks and preview.
struct RaftShape {
    Paths top;      // the top edge (Full) or the frame's outer edge; empty for plain pads
    Paths base;     // what touches the plate
    std::vector<RaftWindow> windows;
    std::vector<Vec2> trunks;
    std::vector<Paths> layers;  // each layer, from the plate up
    double layer_dz = 0;        // how thick each is, mm
    // Separate pieces: parts that don't join anywhere up its height. (Its
    // footprint can be in more: a sloped side parts lobes at the plate that
    // join above it.)
    int pieces = 0;
    double volume = 0;      // mm³, the raft alone
    double height = 0;      // its top, lip included, mm
};

// Builds the raft `tc.raft_spec` describes under `tree`; `shape`, when given, gets what it is.
void appendSpecRaft(TriangleMesh& m, const SupportTree& tree, const TreeConfig& tc, RaftShape* shape = nullptr);
// What a spec raft would be under `tree`, without the mesh.
RaftShape specRaftShape(const SupportTree& tree, const TreeConfig& tc);
// Where the windows cut the raft's edge, seen from above: each one's shape at
// the plate, from the edge in, within the top edge. For drawing them.
Paths raftWindowNotches(const RaftShape& shape, const RaftSpec& s, double raft_top);

}  // namespace fruit
