#include "layers/IslandGraph.h"

#include <algorithm>

namespace fruit {

IslandGraph::IslandGraph(const LayerStack& layers, double link_grow_mm) {
    by_layer_.resize(layers.size());
    std::vector<Paths> grown_prev;  // grown shapes of the previous layer's parts
    for (size_t L = 0; L < layers.size(); ++L) {
        ExPolygons ex = toExPolygons(layers.slices[L]);
        // Deterministic order: by bbox min x, then y, then area.
        std::sort(ex.begin(), ex.end(), [](const ExPolygon& a, const ExPolygon& b) {
            const BBox2 ba = a.bbox(), bb = b.bbox();
            if (ba.min.x != bb.min.x) return ba.min.x < bb.min.x;
            if (ba.min.y != bb.min.y) return ba.min.y < bb.min.y;
            return a.area() < b.area();
        });
        std::vector<Paths> grown_cur;
        for (auto& e : ex) {
            LayerPart p;
            p.layer = static_cast<int>(L);
            p.box = e.bbox();
            p.area = e.area();
            p.shape = std::move(e);
            const int id = static_cast<int>(parts_.size());
            if (L > 0) {
                const auto& prev = by_layer_[L - 1];
                const Paths mine = p.shape.toPaths();
                for (size_t k = 0; k < prev.size(); ++k) {
                    LayerPart& q = parts_[prev[k]];
                    if (!p.box.overlaps(q.box, link_grow_mm)) continue;
                    const Paths inter = intersection(mine, grown_prev[k]);
                    if (std::abs(area(inter)) > 1e-8) {
                        p.parents.push_back(prev[k]);
                        q.children.push_back(id);
                    }
                }
            }
            grown_cur.push_back(offset(p.shape.toPaths(), link_grow_mm));
            parts_.push_back(std::move(p));
            by_layer_[L].push_back(id);
        }
        grown_prev = std::move(grown_cur);
    }
}

std::vector<int> IslandGraph::islands() const {
    std::vector<int> out;
    for (size_t i = 0; i < parts_.size(); ++i)
        if (parts_[i].parents.empty()) out.push_back(static_cast<int>(i));
    return out;
}

}  // namespace fruit
