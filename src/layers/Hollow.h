// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Hollowing a sliced model: the interior that can go while a shell at least
// `wall` thick stays everywhere, layer by layer.
//
// A point is interior when a ball of radius `wall` around it lies inside the
// solid: the solid eroded by the ball. Across layers the ball is replaced by
// a stack of N cylinders whose union contains it (eroding by a superset only
// thickens the walls):
//
//   cylinder k of N: radius wall·√(1 − (k−1)/N), half-height wall·√(k/N)
//
// Their corners lie on a sphere of radius wall·√(1 + 1/N), so walls come out
// between `wall` and wall·√(1 + 1/N) thick at any slope (1.08× at N = 6),
// plus up to a layer where a cylinder's half-height is rounded up to whole
// layers. Eroding by a cylinder is eroding, by its radius, the intersection
// of the slices within its half-height. The first cylinder's sliding-window
// intersections cost three polygon operations per layer (van Herk /
// Gil-Werman); each taller window after it is two of the last, one
// operation. Layers beyond the stack count as empty, so floors and ceilings
// keep the wall too.
//
// Scanned models slice into outlines of many tiny edges, and the interior
// needs no detail finer than a pixel: outlines are simplified to 5 µm and
// the erosions go far enough further to make up for it, so walls are never
// thinner than `wall` (across any gap wider than 10 µm) and at most a few
// hundredths of a millimetre thicker.
//
// Then three steps, each only taking more away from the interior, so walls
// only get thicker:
//
// - An opening drops interior wisps narrower than `opening`.
// - Ceilings that hold themselves up. A cavity's ceiling is printed over
//   liquid resin and can't be given tips, so like any face the generator
//   leaves alone it may lean at most `roof_lean` from vertical: each ceiling
//   layer within d = layer height · tan(roof_lean) of the solid under it.
//   Working down from the top, a cavity keeps what lies within d of the
//   cavity above, plus, where its ceiling meets the walls, what lies deep
//   enough inside them (an erosion that shrinks by d a layer going down):
//   together a hipped roof rising from the walls to a thin ridge, joining
//   any cavity that carries on upwards. Solid that no wall holds (the floor
//   inside a tube standing on a hollow base) goes straight down to the
//   cavity's floor, and so does anything still left hanging where two slopes
//   meet, when wider than half a millimetre; narrower bits print as small
//   overhangs. This costs volume: a spherical cavity keeps its lower half
//   and a cone over it, three quarters of it.
// - Cavities smaller than `min_volume` stay solid: each needs a drain hole
//   of its own, more trouble than the resin it saves.

#include <cstdint>
#include <vector>

#include "core/JobControl.h"
#include "geometry/Polygon.h"
#include "layers/LayerStack.h"

namespace fruit {

struct HollowOptions {
    double wall = 2.0;         // mm, the thinnest the shell may be
    int cylinders = 6;         // more: walls nearer `wall`, slower
    double opening = 0.3;      // mm: interior wisps narrower than this are dropped
    double roof_lean = 45;     // degrees from vertical a ceiling may lean (the generator's
                               // critical angle); 90 leaves ceilings flat
    double min_volume = 20;    // mm³: smaller cavities stay solid
    unsigned threads = 0;      // 0 = all cores
};

// One enclosed cavity: interior pieces that overlap from layer to layer.
struct Cavity {
    int first_layer = 0, last_layer = 0;
    double volume = 0;      // mm³
    bool opens_up = false;  // closedHollows: open towards the film, a suction cup (else sealed, a resin trap)
};

// A piece of one layer's interior: paths [begin, begin + count) of that
// layer's `interior`, its outline first and then its holes.
struct InteriorPiece {
    uint32_t begin = 0, count = 0;
    int cavity = 0;  // index into `cavities`
};

struct HollowResult {
    std::vector<Paths> interior;   // per layer, what is removed (empty where nothing is)
    std::vector<std::vector<InteriorPiece>> pieces;  // per layer, which cavity each piece belongs to
    double volume = 0;             // mm³ removed
    std::vector<Cavity> cavities;  // lowest first

    // The cavity that point `p` of layer `layer` lies in, or -1.
    int cavityAt(size_t layer, const Vec2& p) const;
    // A piece's outline and holes.
    Paths piece(size_t layer, const InteriorPiece& k) const;
};

// Reports stage "hollow". Throws Cancelled when cancelled, and
// std::invalid_argument for a wall or roof lean that isn't above 0.
HollowResult hollowInterior(const LayerStack& solid, const HollowOptions& o,
                            const JobControl& ctl = JobControl::none());

// The slices with the interior taken out.
LayerStack hollowedLayers(const LayerStack& solid, const HollowResult& h);

// A model's own hollows that hold resin (analyzeHollows' suction cups and
// resin traps), as cavities of their own: drain holes are suggested,
// drilled and checked for them as for a hollowed model's. Their interior is
// already empty in `layers`.
HollowResult closedHollows(const LayerStack& layers, double min_volume = 1.0);

// The slices with `h`'s interior filled: printed solid.
LayerStack filledLayers(const LayerStack& layers, const HollowResult& h);

// Sliding-window intersection, for any sequence of polygon sets: out[i] is
// the intersection of in[i − m .. i + m], empty where the window reaches
// past either end. Exposed for testing.
std::vector<Paths> windowIntersection(const std::vector<Paths>& in, int m, unsigned threads = 0,
                                      const JobControl& ctl = JobControl::none());

}  // namespace fruit
