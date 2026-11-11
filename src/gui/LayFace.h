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

// Any spot on a part, for On plate (as Select Base in other slicers): a face it
// can rest on, another flat face, or a spot on a curved surface. Turning
// `normal` straight down makes that surface face the plate.
struct SurfacePick {
    enum class Kind { None, Resting, Flat, Curved };
    Kind kind = Kind::None;
    int region = -1;           // Resting: an index into flatFaces().faces; Flat: into flatFaces().others
    int triangle = -1;         // the triangle there (the part's own mesh)
    fruit::Vec3 point;         // where, in printer coordinates
    fruit::Vec3 normal;        // outward there, in the part's own coordinates: the flat face's, or the triangle's
    std::string name;          // a flat face's name by where it points now ("Back", "Top"...); "" when curved
    double area = 0;           // a flat face's area on the plate, mm²
    bool facing_down = false;  // it already faces the plate
};
// The surface of `part` at its own `triangle`, at `point` (printer coordinates).
SurfacePick surfaceAt(const Part& part, int triangle, const fruit::Vec3& point);

}  // namespace gui
