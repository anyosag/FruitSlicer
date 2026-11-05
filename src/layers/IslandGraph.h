// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Connected parts per layer, linked to the parts they rest on in the layer
// below. A part with no parent is an island: it would print onto the film
// with nothing holding it, so it must receive support.
//
// Two parts are linked when the upper one overlaps the lower one grown by
// `link_grow` (≈ lateral cure width). A feature of thickness d at angle θ
// from horizontal stops overlapping between layers once d < h·cos θ (e.g. a
// 0.06 mm wire at 20° with 0.1 mm layers); growing the parent keeps such
// features from being misread as a new island on every layer.

#include <vector>

#include "geometry/Polygon.h"
#include "layers/LayerStack.h"

namespace fruit {

struct LayerPart {
    int layer = 0;
    ExPolygon shape;
    BBox2 box;
    double area = 0;  // mm²
    std::vector<int> parents;   // part ids in layer - 1
    std::vector<int> children;  // part ids in layer + 1
};

class IslandGraph {
public:
    IslandGraph() = default;
    IslandGraph(const LayerStack& layers, double link_grow_mm);

    const std::vector<LayerPart>& parts() const { return parts_; }
    const LayerPart& part(int id) const { return parts_[id]; }
    const std::vector<int>& partsInLayer(size_t layer) const { return by_layer_[layer]; }
    size_t layerCount() const { return by_layer_.size(); }

    std::vector<int> islands() const;  // parts without parents, by layer then position
    bool isIsland(int id) const { return parts_[id].parents.empty(); }

private:
    std::vector<LayerPart> parts_;
    std::vector<std::vector<int>> by_layer_;
};

}  // namespace fruit
