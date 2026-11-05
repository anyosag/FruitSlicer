// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "layers/HollowAnalysis.h"

#include <numeric>

namespace fruit {

namespace {

struct Hollow {
    int layer;
    Paths shape;  // positive (CCW) region
    BBox2 box;
    double area;
};

}  // namespace

HollowReport analyzeHollows(const LayerStack& layers, double min_area, double min_volume) {
    const size_t n = layers.size();
    HollowReport report;
    report.trap_regions.assign(n, {});
    std::vector<Hollow> hollows;
    std::vector<std::vector<int>> by_layer(n);
    std::vector<Paths> filled(n);  // material + enclosed holes (outer contours only)
    for (size_t L = 0; L < n; ++L) {
        Paths outers;
        for (const ExPolygon& e : toExPolygons(layers.slices[L])) {
            outers.push_back(e.contour);
            for (const Path& h : e.holes) {
                Path pos = h;
                std::reverse(pos.begin(), pos.end());
                const double a = area(pos);
                if (a < 1e-6) continue;
                by_layer[L].push_back(static_cast<int>(hollows.size()));
                hollows.push_back({static_cast<int>(L), {pos}, bbox(pos), a});
            }
        }
        filled[L] = unionNonZero(outers);
    }
    // Union-find over hollows overlapping between adjacent layers.
    std::vector<int> uf(hollows.size());
    std::iota(uf.begin(), uf.end(), 0);
    auto find = [&](int a) {
        while (uf[a] != a) a = uf[a] = uf[uf[a]];
        return a;
    };
    std::vector<char> open_below(hollows.size(), 0), open_above(hollows.size(), 0);
    auto outsideAt = [&](const Hollow& h, long L) {
        // Part of the hollow's footprint that is outside the model at layer L.
        if (L < 0 || L >= static_cast<long>(n)) return true;
        return std::abs(area(difference(h.shape, filled[L]))) > 1e-4;
    };
    for (size_t i = 0; i < hollows.size(); ++i) {
        const Hollow& h = hollows[i];
        open_below[i] = outsideAt(h, h.layer - 1);
        open_above[i] = outsideAt(h, h.layer + 1);
        if (h.layer + 1 < static_cast<int>(n))
            for (int j : by_layer[h.layer + 1]) {
                const Hollow& g = hollows[j];
                if (!h.box.overlaps(g.box)) continue;
                if (std::abs(area(intersection(h.shape, g.shape))) > 1e-4) uf[find(static_cast<int>(i))] = find(j);
            }
    }
    struct Acc {
        bool below = false, above = false;
        int first = 1 << 30, last = -1;
        double volume = 0, max_area = 0;
        Vec2 moment{};
        std::vector<int> members;
    };
    std::vector<Acc> acc(hollows.size());
    for (size_t i = 0; i < hollows.size(); ++i) {
        Acc& a = acc[find(static_cast<int>(i))];
        const Hollow& h = hollows[i];
        a.below = a.below || open_below[i];
        a.above = a.above || open_above[i];
        a.first = std::min(a.first, h.layer);
        a.last = std::max(a.last, h.layer);
        a.volume += h.area * layers.layer_height;
        a.max_area = std::max(a.max_area, h.area);
        a.moment += h.box.center() * h.area;
        a.members.push_back(static_cast<int>(i));
    }
    for (size_t r = 0; r < acc.size(); ++r) {
        const Acc& a = acc[r];
        if (a.members.empty() || a.below) continue;  // drains towards the plate
        if (a.max_area < min_area || a.volume < min_volume) continue;
        HollowComponent c;
        c.kind = a.above ? HollowKind::SuctionCup : HollowKind::ResinTrap;
        c.first_layer = a.first;
        c.last_layer = a.last;
        c.volume = a.volume;
        c.max_area = a.max_area;
        double total = 0;
        for (int m : a.members) total += hollows[m].area;
        c.center = a.moment / std::max(total, 1e-12);
        report.components.push_back(c);
        if (c.kind == HollowKind::ResinTrap)
            for (int m : a.members) {
                Paths& dst = report.trap_regions[hollows[m].layer];
                dst.insert(dst.end(), hollows[m].shape.begin(), hollows[m].shape.end());
            }
    }
    return report;
}

}  // namespace fruit
