// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "layers/LayerStack.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "core/Parallel.h"

namespace fruit {

size_t LayerStack::layerAt(double z) const {
    if (z <= 0) return 0;
    return static_cast<size_t>(std::floor(z / layer_height));
}

LayerStack sliceModel(const TriangleMesh& mesh, double layer_height, SliceStats* stats, unsigned threads,
                      const JobControl& ctl) {
    LayerStack ls;
    ls.layer_height = layer_height;
    const double top = mesh.bbox().max.z;
    if (mesh.empty() || top <= 0) return ls;
    // Relative tolerance: heights read from print files are float32
    // (0.03 is stored as 0.0299999993), which must not add a layer.
    const auto n = static_cast<size_t>(std::ceil(top / layer_height - 1e-6));
    ls.sample_z.resize(n);
    for (size_t i = 0; i < n; ++i) ls.sample_z[i] = (static_cast<double>(i) + 0.5) * layer_height;
    ls.slices = sliceMesh(mesh, ls.sample_z, stats, threads, ctl);
    return ls;
}

LayerStack unionLayers(const LayerStack& a, const LayerStack& b, unsigned threads) {
    if (a.size() && b.size() && std::abs(a.layer_height - b.layer_height) > 1e-9)
        throw std::invalid_argument("layers of different heights can't be put together");
    LayerStack out;
    out.layer_height = a.size() ? a.layer_height : b.layer_height;
    const size_t n = std::max(a.size(), b.size());
    out.sample_z.resize(n);
    out.slices.resize(n);
    parallelFor(n, threads, [&](size_t i) {
        out.sample_z[i] = (static_cast<double>(i) + 0.5) * out.layer_height;
        const Paths none;
        const Paths& x = i < a.size() ? a.slices[i] : none;
        const Paths& y = i < b.size() ? b.slices[i] : none;
        out.slices[i] = x.empty() ? y : y.empty() ? x : unionOf(x, y);
    });
    return out;
}

}  // namespace fruit
