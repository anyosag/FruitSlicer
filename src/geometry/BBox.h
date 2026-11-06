// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once

#include "geometry/Vec.h"

namespace fruit {

struct BBox3 {
    Vec3 min{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
             std::numeric_limits<double>::max()};
    Vec3 max{std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(),
             std::numeric_limits<double>::lowest()};

    bool empty() const { return min.x > max.x || min.y > max.y || min.z > max.z; }
    void extend(const Vec3& p) {
        min = fruit::min(min, p);
        max = fruit::max(max, p);
    }
    void extend(const BBox3& b) {
        if (b.empty()) return;
        extend(b.min);
        extend(b.max);
    }
    Vec3 size() const { return empty() ? Vec3{} : max - min; }
    Vec3 center() const { return (min + max) * 0.5; }
    bool overlaps(const BBox3& o) const {
        return !(o.min.x > max.x || o.max.x < min.x || o.min.y > max.y || o.max.y < min.y ||
                 o.min.z > max.z || o.max.z < min.z);
    }
    double surfaceArea() const {
        const Vec3 s = size();
        return 2.0 * (s.x * s.y + s.y * s.z + s.z * s.x);
    }
    int longestAxis() const {
        const Vec3 s = size();
        return (s.x >= s.y && s.x >= s.z) ? 0 : (s.y >= s.z ? 1 : 2);
    }
};

}  // namespace fruit
