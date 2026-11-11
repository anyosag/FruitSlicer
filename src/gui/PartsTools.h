// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The parts tools' logic: Duplicate, Arrange, Split and Merge. Each one reads
// the job and changes a working copy of its parts (PartsEdit), so the app can
// make it one undo step through App::editParts. No UI here.
//
// Every function says plainly in its report what it did and what it did not.

#include <memory>
#include <string>
#include <vector>

#include "Document.h"
#include "Parts.h"

namespace gui {

inline constexpr int kMaxCopies = 24;  // Duplicate: the most copies in one go
inline constexpr int kMaxPieces = 50;  // Split: the most parts one part is split into

// What a parts tool did.
struct ToolReport {
    bool changed = false;  // the parts were changed
    int wanted = 0;        // how many it was asked to make or place
    int done = 0;          // how many it did
    std::string message;   // in words, for the panel
};

// Copies of the selected part. The other parts stay where they are: the
// copies go in the room beside them (found with ops::arrange), `gap` mm from
// everything. A copy that has no room is not made.
ToolReport duplicatePart(const Document& doc, PartsEdit& e, int copies, double gap);

// Every part laid out on the plate, `gap` mm apart (ops::arrange), the whole
// layout centred on the plate. A part that has no room stays where it is.
ToolReport arrangeParts(const Document& doc, PartsEdit& e, double gap);

// How many shells a part has (what Split would make of it), counted once per
// mesh and kept.
int shellsOf(const Part& part);
// Splits a part into its shells, each its own part standing where it did.
ToolReport splitPart(const Document& doc, PartsEdit& e, int part_id);

// Joins the parts `ids` (at least two) into one. `fuse` false: the shells
// stay as they are, only the part is one. `fuse` true: where they overlap
// they become one solid (Manifold's union); needs closed meshes.
ToolReport mergeParts(const Document& doc, PartsEdit& e, const std::vector<int>& ids, bool fuse);

// A name for a copy or a piece: `base` without a trailing " 2", then " 2",
// " 3"... until it is not in `taken`.
std::string numberedName(const std::string& base, const std::vector<std::string>& taken);

// The tools' settings, kept with the app while a job is open.
struct PartsToolState {
    int copies = 1;            // Duplicate
    double arrange_gap = kPartGap;  // Arrange, mm
    std::vector<int> merge_off;     // Merge: parts left out (ids); all others are joined
    ToolReport last;           // the last thing a tool did
    int last_tool = 0;         // which tool made it (a Tool value)
    int last_revision = -1;    // the job's revision just after it: the report shows until it changes
    std::weak_ptr<const Document> last_doc;  // the job it was for (sameJob: revisions repeat across jobs)
};

}  // namespace gui
