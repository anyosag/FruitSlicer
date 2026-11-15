// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Nesting: lay parts out on the plate by their real outlines seen from above
// (a ring's middle, an L's corner are room), with their supports' feet and
// rafts, turned only about the vertical so each part stands as it did.
//
// Each part's outline is drawn on a grid (`NestOptions::cell`) at every turn
// it may take, grown by half the gap. Parts then go on one at a time, each in
// the first free spot scanning from one side of the plate (bottom-left fill),
// at whichever turn gets it furthest in. Many such runs (orders of the parts,
// sides to fill from, how much turning) are made side by side on every core,
// each layout is moved as a whole to centre its load or to bring its big
// layers to the hinge, and every layout is scored:
//  - how many parts fit;
//  - the rectangle it takes;
//  - how far its load (the parts' volume) is from the plate's middle: a plate
//    loaded to one side is pulled harder there at every peel;
//  - with the vat's hinge side known, how far its big layers are from the
//    hinge (the peel pulls hardest on the side away from it);
//  - how many parts were turned.
// pickLayouts() keeps the best four, each the best at something.
//
// Deterministic: the same parts and options always give the same layouts,
// whatever the number of threads.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/JobControl.h"
#include "geometry/Vec.h"
#include "mesh/TriangleMesh.h"

namespace fruit::ops {

// A round foot or pad (centre, radius), printer coordinates, mm.
struct NestDisk {
    Vec2 at;
    double radius = 0;
};

// One part as it stands now, printer coordinates.
struct NestPart {
    const TriangleMesh* mesh = nullptr;  // placed; only X and Y are used
    std::vector<NestDisk> disks;         // its supports' feet and raft pads, where known
    double grow = 0;                     // room its raft takes past its outline where its supports are not known
    double load = 0;                     // its volume, mm³ (balance)
    Vec2 load_at;                        // where that is centred
    double big_layer = 0;                // its biggest layer, mm² (how much a gentle peel matters to it; 0: its outline's area)
    bool may_turn = true;
};

struct NestOptions {
    double plate_width = 0, plate_depth = 0;  // X, Y, mm
    double edge_margin = 0;                   // least distance from an outline to the plate's edge
    double gap = 2.0;                         // least distance between two parts' outlines (supports and rafts included)
    double cell = 0.5;                        // grid, mm
    bool turn = true;                         // may turn parts about the vertical
    double angle_step = 10;                   // degrees between the turns tried when free
    // Towards the vat's hinge, from the plate's middle (a unit vector along
    // X or Y), when known: the peel pulls hardest on the side away from it.
    std::optional<Vec2> hinge;
    unsigned threads = 0;  // 0: all cores
};

struct NestPlacement {
    bool placed = false;
    double turn_deg = 0;  // about the vertical, counter-clockwise seen from above, added to the part's own
    Vec2 offset;          // where the middle of its mesh's rectangle goes (Placement::offset)
};

struct NestLayout {
    std::vector<NestPlacement> parts;  // same order as the input
    int placed = 0, turned = 0;
    double width = 0, depth = 0;  // the rectangle all the placed outlines take, mm
    double balance = 0;           // load centre's distance from the plate's middle, mm
    double peel = -1;             // big layers' mean distance from the hinge, as a share of the plate (-1: hinge unknown)
    std::string how;              // which run made it (for the log and tests)
    // What it covers on the plate, on the grid: two layouts with the same
    // are the same to look at (alike parts swapped, a round part turned).
    std::uint64_t covers = 0;
    double area() const { return width * depth; }
};

// Every distinct layout the runs made. Progress: outlines up to 0.3, runs after.
std::vector<NestLayout> nestLayouts(const std::vector<NestPart>& parts, const NestOptions& options,
                                    const JobControl& ctl = JobControl::none());

struct NestPick {
    // What it is best at: "Recommended", "Most compact", "Balanced on the
    // plate", "Gentlest peel", "Fewest turns"; or "Another way".
    std::string title;
    std::vector<std::string> also;  // what else it is best at
    NestLayout layout;
};
// Up to four, all different to look at, each fitting as many parts as any
// layout does: the best overall, the most compact, the best balanced and the
// gentlest peel (the fewest turns when the hinge is not known). A layout best
// at more than one of these is one pick (`also`), one on which they all
// score alike is left out, and the room left goes to the next best overall
// ("Another way").
std::vector<NestPick> pickLayouts(const std::vector<NestLayout>& layouts);

}  // namespace fruit::ops
