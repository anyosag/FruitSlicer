// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "geometry/Vec.h"

namespace fruit {

enum class PointCategory { Island, Overhang, Capacity, Stabilization, Manual };

const char* categoryName(PointCategory c);
PointCategory categoryFromName(const std::string& s);

struct SupportPoint {
    uint64_t id = 0;        // stable: hash of quantised position + category
    Vec3 pos;               // on the model surface (printer coordinates)
    Vec3 normal;            // outward surface normal at pos
    double tip_diameter = 0.4;
    PointCategory category = PointCategory::Overhang;
    bool pinned = false;    // never pruned (island minimum, manual, locked)
    int layer = 0;
    int part = -1;          // IslandGraph part the tip touches
    double cost = 1.0;      // placement cost (cosmetic / edge penalties)
    // One size up from what the planner gives its kind, so that it can carry its own share of the pull.
    bool enlarged = false;
};

uint64_t stablePointId(const Vec3& pos, PointCategory c);

// A tip whose support stands on the model instead of the raft, and where its
// foot is. Such a support ties two places on the print together: it holds a
// fresh ceiling up, but adds nothing to what holds the print on the raft.
struct TiedTip {
    SupportPoint tip;
    Vec3 foot;
};

struct Warning {
    std::string code;     // stable key, e.g. "peak_area", "flat_face", "capacity_deficit", "tiny_islands_skipped"
    std::string message;
    int layer = -1;
    Vec3 where;
};

}  // namespace fruit
