// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The Orient step's work, done off the UI thread: how the model scores as it
// sits, the search for better ways to stand it, and a picture of each seen
// from below. No UI here, so it can be tested on its own.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/JobControl.h"
#include "geometry/Vec.h"
#include "mesh/Placement.h"
#include "mesh/TriangleMesh.h"
#include "support/SupportConfig.h"

namespace gui {

class Document;

// Sides of the model as the Front view shows them (printer frame).
enum class Side { Front, Back, Left, Right, Top, Bottom };
fruit::Vec3 sideDirection(Side s);  // outward unit vector: Front = -Y, Top = +Z, ...
const char* sideName(Side s);

struct OrientMetrics {
    std::size_t tips = 0;           // support tips the planner places
    std::size_t island_tips = 0;    // tips under spots that start in mid-air
    std::size_t cosmetic_tips = 0;  // tips on the sides picked as visible
    std::size_t short_of_hold = 0;  // layers bigger than their tips can hold
    double biggest_layer = 0;       // mm²
    double top = 0;                 // highest point above the plate, mm (lift included)
    std::size_t layers = 0;         // layers to print at the job's layer height
};

// The model seen from below, as an alpha mask (top row first).
struct Silhouette {
    int w = 0, h = 0;
    std::vector<std::uint8_t> alpha;
};

struct OrientOption {
    fruit::Vec3 rotation_deg;  // for Placement::rotation_deg
    OrientMetrics m;
    bool fits = true;  // on the plate as rotated (turned about the vertical if needed)
    Silhouette below;
    std::string summary;  // one or two sentences against how it sits now
};

struct OrientSearch {
    std::vector<OrientOption> options;  // better than now, best first, at most three
    OrientOption now;                   // as it sits
    int directions = 0;                 // build directions scored
    int finalists = 0;                  // planned with the full tip planner
    double seconds = 0;
};

// Layer height the app uses until resin profiles arrive.
inline constexpr double kLayerHeight = 0.05;

// The support planner's settings for this placement (built-in defaults).
fruit::SupportConfig supportConfig(const fruit::Placement& p);

// Visible sides picked in model coordinates, as printer-frame directions for
// the model placed per `p`.
std::vector<fruit::Vec3> visibleInPrinter(const std::vector<fruit::Vec3>& model_dirs, const fruit::Placement& p);

// Scores the model as `doc` places it.
OrientMetrics measure(const Document& doc, const std::vector<fruit::Vec3>& visible_model,
                      const fruit::JobControl& ctl);

// Scores how it sits, then searches for better build directions.
OrientSearch search(const Document& doc, const std::vector<fruit::Vec3>& visible_model, const fruit::JobControl& ctl);

// `placed` seen from below, mirrored as from underneath, `mm_per_px` per
// pixel, centred in a `w` × `h` mask (2 × 2 samples per pixel).
Silhouette silhouetteBelow(const fruit::TriangleMesh& placed, double mm_per_px, int w, int h);

// What `o` changes against `now`, in a sentence or two.
std::string summarize(const OrientMetrics& o, const OrientMetrics& now);

}  // namespace gui
