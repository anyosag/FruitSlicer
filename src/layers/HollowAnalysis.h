// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Hollow regions of a sliced model: suction cups and resin traps.
//
// A hollow is a hole inside a layer's polygons. Hollows overlapping between
// adjacent layers form one component. Following the two-pass idea of
// UVtools' resin-trap / suction-cup detection (IssueManager), but on
// polygons instead of pixels:
//
//   open below (towards the plate, printed first) → resin drains: fine;
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

struct HollowComponent {
    HollowKind kind = HollowKind::Drains;
    int first_layer = 0, last_layer = 0;
    double volume = 0;      // mm³ (Σ area × layer height)
    double max_area = 0;    // mm²
    Vec2 center;            // area-weighted centroid
};

struct HollowReport {
    std::vector<HollowComponent> components;  // only cups and traps above the size limits
    // Per layer, the hollow regions belonging to resin traps (for skipping
    // supports on cavity ceilings).
    std::vector<Paths> trap_regions;
    // Per layer, the hollow regions belonging to suction cups.
    std::vector<Paths> cup_regions;
};

HollowReport analyzeHollows(const LayerStack& layers, double min_area = 0.5, double min_volume = 1.0);

}  // namespace fruit
