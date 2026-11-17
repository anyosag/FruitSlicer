// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Details supports keep out of: slots, holes and channels in the model
// narrower than SupportConfig::detail_width with the model on both sides, and
// the space just below where such a slot opens downwards (its mouth), so that
// heads from either side can't meet under it and close it. On DishRack6.goo,
// tips on the edges of a coverslip tray's 0.6 mm slots and their heads under
// them sealed the slots, and resin was trapped in them.

#include <vector>

#include "core/JobControl.h"
#include "geometry/Polygon.h"
#include "layers/LayerStack.h"
#include "mesh/TriangleMesh.h"

namespace fruit {

struct DetailRegion {
    ExPolygon shape;  // in plan (mm)
    BBox2 box;
    double z0 = 0, z1 = 0;  // its height range (mm)
    bool mouth = false;     // the space below a slot's opening, not the slot itself
};

class DetailMap {
public:
    DetailMap() = default;
    // From the model's layers. `width`: slots narrower than this count;
    // `depth`: how far below a slot's mouth is kept clear.
    static DetailMap build(const LayerStack& layers, double width, double depth,
                           const JobControl& ctl = JobControl::none());

    bool empty() const { return regions_.empty(); }
    const std::vector<DetailRegion>& regions() const { return regions_; }
    // The slots themselves, without the mouths below them.
    std::size_t slotCount() const;
    // Whether a disc of radius `r` around `p` (in plan, at height p.z)
    // reaches into a detail.
    bool reaches(const Vec3& p, double r) const;
    // The regions as solid prisms (printer coordinates), for the 3D view.
    TriangleMesh mesh(bool with_mouths = false) const;

private:
    std::vector<DetailRegion> regions_;
    double z_base_ = 0;
    std::vector<std::vector<int>> by_z_;  // 1 mm bands of height → regions in them
    void index();
};

}  // namespace fruit
