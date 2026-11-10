// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Collisions.h"

#include <algorithm>

#include "Format.h"
#include "ops/MeshOps.h"

namespace gui {

int CollisionReport::overlapping() const {
    int n = 0;
    for (const CollisionPair& p : problems) n += p.overlaps ? 1 : 0;
    return n;
}

CollisionReport checkCollisions(const Document& doc, double min_gap, const fruit::JobControl& ctl) {
    CollisionReport rep;
    rep.revision = doc.revision();
    rep.min_gap = min_gap;
    rep.parts = static_cast<int>(doc.partCount());
    const auto& parts = doc.parts();
    const std::size_t n = parts.size();
    std::vector<fruit::ops::Solid> solids(n);
    std::vector<bool> usable(n, false);
    for (std::size_t i = 0; i < n; ++i) {
        ctl.step("collisions", 0.3 * static_cast<double>(i) / static_cast<double>(n));
        solids[i] = fruit::ops::Solid(parts[i].placed());
        usable[i] = solids[i].valid();
        if (!usable[i]) {
            std::string why = solids[i].message();
            if (why.empty()) why = "is not a closed solid";
            rep.not_checked.push_back({parts[i].id(), why});
        }
    }
    const std::size_t total = n * (n - 1) / 2;
    std::size_t done = 0;
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j, ++done) {
            ctl.step("collisions", 0.3 + 0.7 * static_cast<double>(done) / static_cast<double>(std::max<std::size_t>(1, total)));
            if (!usable[i] || !usable[j]) continue;
            ++rep.pairs_checked;
            // Far apart by their boxes: nothing to look at.
            fruit::BBox3 a = parts[i].box();
            a.min = a.min - fruit::Vec3{min_gap, min_gap, min_gap};
            a.max = a.max + fruit::Vec3{min_gap, min_gap, min_gap};
            if (!a.overlaps(parts[j].box())) continue;
            const fruit::ops::CollisionResult c = fruit::ops::checkCollision(solids[i], solids[j], 1e-3, min_gap);
            if (!c.ok) {
                rep.not_checked.push_back({parts[j].id(), c.message});
                continue;
            }
            if (c.overlaps) rep.problems.push_back({parts[i].id(), parts[j].id(), true, c.overlap_volume, 0});
            else if (c.gap >= 0 && c.gap < min_gap) rep.problems.push_back({parts[i].id(), parts[j].id(), false, 0, c.gap});
        }
    }
    return rep;
}

std::string describePair(const CollisionPair& p) {
    if (p.overlaps) return strf("overlap by %.1f mm³", p.overlap_mm3);
    if (p.gap < 0.05) return "touch";
    return strf("are %.1f mm apart", p.gap);
}

}  // namespace gui
