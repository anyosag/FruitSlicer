// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// On plate: the flat faces a part can be laid on, named as the panel and the
// view show them. No UI here.

#include <string>
#include <vector>

#include "Parts.h"

namespace gui {

struct RestingFace {
    int region = -1;       // its index in part.flatFaces().faces
    std::string name;      // "Back", "Left side", "Slanted face 2"... where it points now
    double area = 0;       // mm², at the part's size on the plate
    fruit::Vec3 dir;       // where it points, in printer directions
    bool on_plate = false;  // it already lies on the plate
};

// The faces `part` can rest on, largest first, as it is placed now.
std::vector<RestingFace> restingFaces(const Part& part);

}  // namespace gui
