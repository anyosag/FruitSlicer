#pragma once
// A model sliced into uniform layers. Layer i spans
// [i*h, (i+1)*h] above the build plate and is sampled at its mid-height.

#include <vector>

#include "geometry/Polygon.h"
#include "mesh/TriangleMesh.h"
#include "slice/MeshSlicer.h"

namespace fruit {

struct LayerStack {
    double layer_height = 0.05;
    std::vector<double> sample_z;   // mid-height of each layer
    std::vector<Paths> slices;      // unioned polygons per layer

    size_t size() const { return slices.size(); }
    double layerBottom(size_t i) const { return static_cast<double>(i) * layer_height; }
    double layerTop(size_t i) const { return static_cast<double>(i + 1) * layer_height; }
    // Index of the layer containing height z (clamped to >= 0).
    size_t layerAt(double z) const;
};

// Slice `mesh` (already placed in printer coordinates, Z >= 0) from the plate up.
LayerStack sliceModel(const TriangleMesh& mesh, double layer_height, SliceStats* stats = nullptr);

}  // namespace fruit
