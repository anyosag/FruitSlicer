// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Hollow regions of a sliced model: suction cups and resin traps.
//
// A hollow is a hole inside a layer's polygons, less the islands standing in
// it (a groove round a solid block is the groove). Hollows overlapping
// between adjacent layers form one component. Following the two-pass idea of
// UVtools' resin-trap / suction-cup detection (IssueManager), but on
// polygons instead of pixels:
//
//   open below (towards the plate, printed first) → resin drains: fine
//                                                    (a hollow in the first
//                                                    layer is closed below:
//                                                    the plate seals it);
//   closed below, open above (towards the film)   → suction cup: every peel
//                                                    pulls against a sealed
//                                                    pocket (cupping);
//   closed both ways                              → resin trap: enclosed
//                                                    cavity, its ceiling can
//                                                    not be supported.

#include <vector>

#include "geometry/Polygon.h"
#include "layers/LayerStack.h"

namespace fruit {

enum class HollowKind { Drains, SuctionCup, ResinTrap };

// How hard a suction cup pulls, and where it could be let open. While a layer
// of the cup prints, its open area is sealed against the film by the walls
// round it, so each peel pulls on it much as on cured resin: `share` is how
// much more the layer pulls than its own cured area would.
struct CupMeasures {
    double open_area = 0;     // mm² facing the film, on the layer where `share` is largest
    double cured_around = 0;  // mm² cured on that layer inside the outline round the cup
    double share = 0;         // open_area / cured_around, the largest over the cup's layers
    int share_layer = -1;
    // Its biggest layer: twice its area over its edges' length (a groove's
    // width, a round cup's radius), how long that makes it, and the widest
    // circle in it.
    double width = 0;   // mm
    double length = 0;  // mm: area / width
    double across = 0;  // mm
    // The solid under it stands on the plate, or the plate is its floor, so
    // a hole down through its floor would end on the plate, which seals it.
    bool floor_on_plate = false;
    double wall = -1;  // mm: the thinnest wall from its biggest layer to the outside (-1: none found)
    Vec2 wall_at;      // the outside of that wall, where a notch would come out
    // Long and narrow: a groove or a channel rather than a cup.
    bool channel() const { return width > 0 && length >= 5 * width; }
};

// Where a suction cup's measures put it. The limits are assumptions, not
// measured on prints: the cup's open area under `note_below` of the layer
// cured round it is a note; from `problem_from` it is a problem; between, a
// warning, and a problem when it pulls on `deep_layers` layers or more
// (more peels, more chances to fail).
struct CupLimits {
    double note_below = 0.25;
    double problem_from = 1.0;
    int deep_layers = 300;
};
enum class CupGrade { Note, Warning, Problem };
CupGrade gradeCup(const CupMeasures& m, int layers, const CupLimits& limits = {});
const char* gradeName(CupGrade g);  // "note", "warning", "problem"

struct HollowComponent {
    HollowKind kind = HollowKind::Drains;
    int first_layer = 0, last_layer = 0;
    double volume = 0;      // mm³ (Σ area × layer height)
    double max_area = 0;    // mm²
    Vec2 center;            // area-weighted centroid
    BBox2 box;              // round all its layers
    // A point inside it (not on its edge), on layer `inside_layer`.
    Vec2 inside;
    int inside_layer = -1;
    CupMeasures cup;        // suction cups only
    int layers() const { return last_layer - first_layer + 1; }
};

struct HollowReport {
    std::vector<HollowComponent> components;  // only cups and traps above the size limits
    // Per layer, the hollow regions belonging to resin traps (for skipping
    // supports on cavity ceilings). Non-zero: the islands standing in a
    // hollow are clockwise holes of it.
    std::vector<Paths> trap_regions;
    // Per layer, the hollow regions belonging to suction cups.
    std::vector<Paths> cup_regions;
};

// On up to `threads` threads (0 = all cores); the report is the same however many.
HollowReport analyzeHollows(const LayerStack& layers, double min_area = 0.5, double min_volume = 1.0, unsigned threads = 0);
// The same report's regions alone (trap_regions, cup_regions; no components): where the cups and traps are, with
// none of their measures worked out, as the replay wants them for a whole print.
HollowReport hollowRegions(const LayerStack& layers, double min_area = 0.5, double min_volume = 1.0,
                           unsigned threads = 0);

}  // namespace fruit
