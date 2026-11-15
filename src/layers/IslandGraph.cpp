// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "layers/IslandGraph.h"

#include <algorithm>

#include "core/Parallel.h"

namespace fruit {

namespace {

// A layer's parts, in IslandGraph's order: by bbox min x, then y, then area.
std::vector<LayerPart> partsOf(const LayerStack& layers, size_t L) {
    ExPolygons ex = toExPolygons(layers.slices[L]);
    std::sort(ex.begin(), ex.end(), [](const ExPolygon& a, const ExPolygon& b) {
        const BBox2 ba = a.bbox(), bb = b.bbox();
        if (ba.min.x != bb.min.x) return ba.min.x < bb.min.x;
        if (ba.min.y != bb.min.y) return ba.min.y < bb.min.y;
        return a.area() < b.area();
    });
    std::vector<LayerPart> out;
    out.reserve(ex.size());
    for (auto& e : ex) {
        LayerPart p;
        p.layer = static_cast<int>(L);
        p.box = e.bbox();
        p.area = e.area();
        p.shape = std::move(e);
        out.push_back(std::move(p));
    }
    return out;
}

}  // namespace

IslandGraph::IslandGraph(const LayerStack& layers, double link_grow_mm, unsigned threads) {
    // Each layer's parts, and each layer's links to the one below, are worked
    // out on their own (in parallel); ids and the order of every list are as
    // one pass from the bottom up would give them.
    const size_t nl = layers.size();
    by_layer_.resize(nl);
    std::vector<std::vector<LayerPart>> per(nl);
    std::vector<std::vector<Paths>> grown(nl);  // each part grown by link_grow_mm, for the layer above
    parallelFor(nl, threads, [&](size_t L) {
        per[L] = partsOf(layers, L);
        grown[L].reserve(per[L].size());
        for (const LayerPart& p : per[L]) grown[L].push_back(offset(p.shape.toPaths(), link_grow_mm));
    });
    std::vector<int> first(nl + 1, 0);
    for (size_t L = 0; L < nl; ++L) first[L + 1] = first[L] + static_cast<int>(per[L].size());
    // Parents: a part rests on each part below whose grown shape it overlaps.
    parallelFor(nl, threads, [&](size_t L) {
        if (L == 0) return;
        for (LayerPart& p : per[L]) {
            const Paths mine = p.shape.toPaths();
            for (size_t k = 0; k < per[L - 1].size(); ++k) {
                if (!p.box.overlaps(per[L - 1][k].box, link_grow_mm)) continue;
                const Paths inter = intersection(mine, grown[L - 1][k]);
                if (std::abs(area(inter)) > 1e-8) p.parents.push_back(first[L - 1] + static_cast<int>(k));
            }
        }
    });
    grown.clear();
    // Children, from the layer above, in its parts' order.
    parallelFor(nl, threads, [&](size_t L) {
        if (L == 0) return;
        for (size_t i = 0; i < per[L].size(); ++i)
            for (int q : per[L][i].parents)
                per[L - 1][static_cast<size_t>(q - first[L - 1])].children.push_back(first[L] + static_cast<int>(i));
    });
    parts_.reserve(static_cast<size_t>(first[nl]));
    for (size_t L = 0; L < nl; ++L) {
        by_layer_[L].reserve(per[L].size());
        for (LayerPart& p : per[L]) {
            by_layer_[L].push_back(static_cast<int>(parts_.size()));
            parts_.push_back(std::move(p));
        }
    }
}

std::vector<LayerPart> findIslands(const LayerStack& layers, double link_grow_mm, unsigned threads) {
    const size_t nl = layers.size();
    std::vector<std::vector<LayerPart>> per(nl);
    parallelFor(nl, threads, [&](size_t L) { per[L] = partsOf(layers, L); });
    // A part rests on the first part below whose grown shape it overlaps (the
    // graph's test); a part below is grown only when something above might.
    std::vector<std::vector<char>> held(nl);
    parallelFor(nl, threads, [&](size_t L) {
        held[L].assign(per[L].size(), 0);
        if (L == 0) return;
        const std::vector<LayerPart>& below = per[L - 1];
        std::vector<Paths> grown(below.size());
        std::vector<char> made(below.size(), 0);
        for (size_t i = 0; i < per[L].size(); ++i) {
            const LayerPart& p = per[L][i];
            Paths mine;
            for (size_t k = 0; k < below.size() && !held[L][i]; ++k) {
                if (!p.box.overlaps(below[k].box, link_grow_mm)) continue;
                if (!made[k]) {
                    grown[k] = offset(below[k].shape.toPaths(), link_grow_mm);
                    made[k] = 1;
                }
                if (mine.empty()) mine = p.shape.toPaths();
                if (std::abs(area(intersection(mine, grown[k]))) > 1e-8) held[L][i] = 1;
            }
        }
    });
    std::vector<LayerPart> out;
    for (size_t L = 0; L < nl; ++L)
        for (size_t i = 0; i < per[L].size(); ++i)
            if (!held[L][i]) out.push_back(std::move(per[L][i]));
    return out;
}

std::vector<int> IslandGraph::islands() const {
    std::vector<int> out;
    for (size_t i = 0; i < parts_.size(); ++i)
        if (parts_[i].parents.empty()) out.push_back(static_cast<int>(i));
    return out;
}

}  // namespace fruit
