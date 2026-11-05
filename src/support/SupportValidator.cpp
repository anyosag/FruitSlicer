#include "support/SupportValidator.h"

#include "layers/IslandGraph.h"
#include "layers/LayerStack.h"

namespace fruit {

ValidationReport validateSupported(const TriangleMesh& model, const TriangleMesh& supports, double layer_height,
                                   double link_grow, double small_area, double pixel_area) {
    TriangleMesh all = model;
    all.append(supports);
    const LayerStack ls = sliceModel(all, layer_height);
    const IslandGraph g(ls, link_grow);
    ValidationReport r;
    r.layers = ls.size();
    if (ls.size()) r.first_layer_area = area(ls.slices[0]);
    for (int id : g.islands()) {
        const LayerPart& p = g.part(id);
        if (p.layer == 0) {
            ++r.first_layer_islands;
            continue;
        }
        if (p.area < pixel_area) ++r.subpixel;
        else if (p.area < small_area) ++r.small_floating;
        else r.floating.push_back({p.layer, ls.sample_z[p.layer], p.area, p.box.center()});
    }
    return r;
}

}  // namespace fruit
