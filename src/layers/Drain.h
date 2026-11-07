// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Drain holes: openings from a hollowed model's cavities to the outside, so
// the resin inside runs out and no peel pulls against a sealed pocket.
//
// A hole is kept in model coordinates, so it turns with the model: the point
// where it enters the surface and the way it runs. Placed in the printer's
// frame, it is drilled from just outside that point, through the solid, and
// on into the first cavity it reaches, by its own diameter or half the
// cavity's depth along it if that is less, so it can't break through the
// wall on the far side. A hole that leaves the model, or runs into no
// cavity, is not drilled.
//
// A cavity drains when it opens towards the plate (printed first): its
// lowest layer reaches the outside, which is what analyzeHollows calls
// draining (its other layers then open into ones below). A hole level with
// or under a cavity's lowest point does that; one higher up leaves a cup
// beneath it. suggestDrainHoles puts a drain hole at each cavity's lowest
// point, along the shortest way out that runs level or down, and an air
// hole at its highest, along the shortest way out that runs level or up.

#include <vector>

#include "geometry/Polygon.h"
#include "geometry/Transform.h"
#include "layers/Hollow.h"
#include "layers/LayerStack.h"
#include "mesh/TriangleMesh.h"

namespace fruit {

struct DrainHole {
    Vec3 at;              // where it enters the surface (model coordinates)
    Vec3 axis;            // the way it runs into the model (model coordinates)
    double diameter = 3;  // mm
    // An air hole near a cavity's top lets air in while the resin runs out
    // of the drain hole at its bottom, so it doesn't hold back like a
    // finger over a straw. Drilled the same way.
    bool air = false;
    bool operator==(const DrainHole&) const = default;
};

// A hole as drilled, in printer coordinates.
struct PlacedHole {
    Vec3 from, to;       // the drilled length of its axis
    double radius = 0;
    int cavity = -1;     // the cavity it opens into; -1 if none (then it isn't drilled)
};

// Where `holes` run once the model is placed with `to_printer`, through the
// slices `solid` hollowed as `hollow`.
std::vector<PlacedHole> placeHoles(const std::vector<DrainHole>& holes, const Transform& to_printer,
                                   const LayerStack& solid, const HollowResult& hollow);

// The slices with the interior taken out and the holes drilled.
LayerStack drilledLayers(const LayerStack& solid, const HollowResult& hollow, const std::vector<PlacedHole>& holes);

// For each of `hollow`'s cavities, whether it drains in `drilled`.
std::vector<char> cavitiesDrain(const LayerStack& drilled, const HollowResult& hollow);

// Two holes `diameter` wide for each cavity: a drain hole at its lowest
// point, along the shortest way out that runs level or down, and an air hole
// at its highest, along the shortest way out that runs level or up. `placed`
// is the model as placed with `to_printer`; the holes come back in model
// coordinates, a cavity's drain hole first.
std::vector<DrainHole> suggestDrainHoles(const TriangleMesh& placed, const Transform& to_printer,
                                         const LayerStack& solid, const HollowResult& hollow, double diameter = 3);

// What a hole of radius `r` along a → b takes out of the layer between z0
// and z1, seen from above: a convex polygon (empty if it misses the layer).
// The hole is a 32-sided prism around the circle, so it is never narrower.
// Exposed for testing.
Path holeFootprint(const Vec3& a, const Vec3& b, double r, double z0, double z1);

}  // namespace fruit
