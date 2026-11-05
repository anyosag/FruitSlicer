#pragma once
// Final check on the printable result: slice model + supports together and
// make sure the only islands are on the first layer (the raft). Anything
// else would start printing in mid-air.

#include <vector>

#include "mesh/TriangleMesh.h"

namespace fruit {

struct ValidationReport {
    size_t layers = 0;
    size_t first_layer_islands = 0;  // raft pieces / bases on the plate
    struct Floating { int layer; double z; double area; Vec2 at; };
    std::vector<Floating> floating;  // islands above the first layer, larger than `small_area`
    size_t small_floating = 0;       // islands below `small_area` (absorbed / skipped by design)
    size_t subpixel = 0;             // slivers below one pixel's area: not printable, ignored
    double first_layer_area = 0;
};

// `small_area`: islands below this are expected (the point generator skips
// islands that merge with the model almost at once); larger ones are failures.
ValidationReport validateSupported(const TriangleMesh& model, const TriangleMesh& supports, double layer_height,
                                   double link_grow, double small_area = 0.12, double pixel_area = 2.7e-4);

}  // namespace fruit
