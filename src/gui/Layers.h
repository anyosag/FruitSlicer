// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The Layers step's work, off the UI thread: cut the model and its supports
// into the printer's layers, measure them, and draw any one of them. No UI.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/JobControl.h"
#include "geometry/BBox.h"
#include "layers/HollowAnalysis.h"
#include "layers/LayerStack.h"
#include "mesh/TriangleMesh.h"
#include "printer/PrinterProfile.h"

namespace gui {

class Document;

struct LayerResult {
    std::shared_ptr<const fruit::LayerStack> layers;  // model and supports together, as the printer cuts them
    std::vector<double> area;        // lit area per layer, mm²
    std::size_t raft_layers = 0;     // layers up to the raft's top
    std::size_t biggest = 0;         // the biggest layer above the raft
    double resin_ml = 0;             // Σ area × layer height
    double top = 0;                  // top of the last layer, mm
    fruit::BBox3 bounds;             // model and supports, printer coordinates
    fruit::HollowReport hollows;     // suction cups and resin traps
    std::vector<std::size_t> marks;  // layers worth a look: where hollows start, the biggest layer
    double seconds = 0;

    std::size_t cups() const;
    std::size_t traps() const;
};

// Slices the placed model of `doc` together with `supports` (empty when it
// stands flat on the plate). `raft_top` is the raft's top, mm (0 for none).
// Progress: slicing up to 0.8 (stage "slice"), then "hollows".
LayerResult sliceLayers(const Document& doc, const fruit::TriangleMesh& supports, double raft_top,
                        const fruit::JobControl& ctl);

// One layer drawn for the screen.
struct LayerImage {
    int w = 0, h = 0;
    std::vector<std::uint8_t> lit;    // coverage 0–255, top row first
    std::vector<std::uint8_t> below;  // the layer below on the same grid, when asked
    double x_left = 0, y_top = 0;     // plate mm at the image's top-left corner
    double px = 0, py = 0;            // mm per image pixel
    int printer_pixels = 1;           // printer pixels per image pixel, along each axis
};

// Layer `i` over `crop` (plate XY, mm) in at most `max_w` × `max_h` pixels.
// Each image pixel is a whole number of printer pixels, so the image keeps
// the printer's 14 × 19 µm pixel shape; seen from above, not mirrored.
LayerImage drawLayer(const fruit::LayerStack& ls, std::size_t i, const fruit::BBox3& crop, int max_w, int max_h,
                     const fruit::PrinterProfile& printer, bool with_below);

}  // namespace gui
