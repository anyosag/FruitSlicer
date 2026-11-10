// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The Collisions check: every pair of parts, as they are placed, tested for
// overlapping or sitting closer than a least gap. Runs in the background
// (App::maintain) for the job's current revision; no UI here.

#include <string>
#include <vector>

#include "Document.h"
#include "core/JobControl.h"

namespace gui {

inline constexpr double kMinPartGap = 2.0;  // mm: closer than this is reported

struct CollisionPair {
    int a = 0, b = 0;      // the two parts (ids), a before b in the job
    bool overlaps = false;  // they share volume (they print as one lump)
    double overlap_mm3 = 0;
    double gap = 0;         // when they do not overlap: the least distance between them, mm (0: touching)
};

struct PartNotChecked {
    int part = 0;
    std::string why;  // in words
};

struct CollisionReport {
    int revision = -1;       // the job's revision it was made for
    double min_gap = 0;      // mm
    int parts = 0;           // parts in the job then
    int pairs_checked = 0;   // pairs of parts looked at (those far apart count)
    std::vector<CollisionPair> problems;  // the pairs that overlap or are closer than min_gap
    std::vector<PartNotChecked> not_checked;  // parts left out, with the reason

    bool clear() const { return problems.empty() && not_checked.empty(); }
    int overlapping() const;  // problems that overlap
};

// Checks every pair of parts in `doc` (placed, in printer coordinates). Throws
// fruit::Cancelled when `ctl` is cancelled. Uses the placed meshes: a copy of
// the document made on the UI thread has them ready.
CollisionReport checkCollisions(const Document& doc, double min_gap, const fruit::JobControl& ctl);

// The words for a pair: "overlap by 12 mm³", "are 0.8 mm apart", "touch".
std::string describePair(const CollisionPair& p);

}  // namespace gui
