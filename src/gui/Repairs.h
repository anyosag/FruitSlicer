// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// What loading a model repaired, in words: for the health check and the
// Model step. No UI.

#include <string>

#include "mesh/ModelCheck.h"

namespace gui {

struct RepairWords {
    enum class Level {
        None,      // nothing needed repair
        Repaired,  // closed, and nothing in it was a guess worth a look
        Check,     // closed, with a patch big enough to look at before printing
        Open,      // still not watertight
    };
    Level level = Level::None;
    std::string title;   // the health check's row: "Repaired: now watertight"
    std::string detail;  // what was done, and what to look at
    std::string brief;   // the Model step's card: "5 holes closed, 14 faces turned."
};

// A patch is worth a look when it is big: as a disc it would be over a
// tenth of the model's size across; or when patches make up over a
// twentieth of the model's faces. What closes a hole is the flattest patch
// that fits, a guess at what was meant.
RepairWords repairWords(const fruit::MeshCheck& c);

}  // namespace gui
