// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Arrange's layouts for a job (ops/Nest.h): each part's outline seen from
// above with its supports' feet and raft, the search for layouts, the four
// picks with a picture each, using one, and moving the supports with the
// parts instead of planning them again. No UI here.

#include <string>
#include <vector>

#include "JobPicture.h"
#include "PartsTools.h"
#include "Supports.h"
#include "core/JobControl.h"
#include "ops/Nest.h"

namespace gui {

class Document;
class PartsEdit;

struct LayoutChoice {
    std::string title;              // "Recommended", "Most compact", ... or "Another way"
    std::vector<std::string> also;  // what else it is best at
    fruit::ops::NestLayout layout;
    JobPicture picture;  // the plate from above, each part in its own shade
};

struct LayoutSearch {
    std::vector<LayoutChoice> choices;  // up to four
    std::vector<int> part_ids;          // the parts, in the layouts' order
    int revision = -1;                  // the job as it was searched
    std::size_t candidates = 0;         // different layouts the runs made
    bool hinge = false;                 // the vat's hinge side was known
    bool supports = false;              // the parts' supports were made for them as they are, and laid out with them
    double gap = 0;
    double seconds = 0;
};

// Every part laid out, `gap` mm between their outlines (their supports and
// rafts included), turned about the vertical only when `turn`. Progress:
// the search, then the pictures.
LayoutSearch findLayouts(const Document& doc, double gap, bool turn, const fruit::JobControl& ctl);

// The parts where `choice` puts them: each turned about the vertical by its
// turn and moved; one with no room stays where it is.
ToolReport applyLayout(const Document& doc, PartsEdit& e, const LayoutSearch& s, int choice);

// The supports of `before`, moved with its parts to where `after` has them
// (moved and turned about the vertical, nothing else changed): the planner's
// tips go with their part, so the supports can be built again from them
// without planning. Not when a part changed otherwise, or when a tip now
// stands where the peel pulls harder (further from the vat's hinge) than
// within kMovedTipsSlack of where it was planned.
inline constexpr double kMovedTipsSlack = 0.02;  // an allowance of ours, not measured
struct MovedTips {
    bool ok = false;
    std::string why;   // when not: why they need making again
    SupportPlan plan;  // when ok: the planner's tips, moved
};
MovedTips moveTips(const Document& before, const Document& after, const SupportResult& r);

}  // namespace gui
