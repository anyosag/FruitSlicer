// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The Layers step with adaptive layers: it shows the print file's own layers
// (see FileLayer), which are not the 0.05 mm layers the checks ran on. These
// turn heights into layers and back, and keep the layer shown in both kinds
// at one height. No UI.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "Print.h"

namespace gui {

// The file layer containing height `z` (mm above the plate); below the first
// is the first, above the last is the last. -1 when there are none.
inline int fileLayerAt(const std::vector<FileLayer>& v, double z) {
    if (v.empty()) return -1;
    // The first layer whose top is above z.
    const auto it = std::upper_bound(v.begin(), v.end(), z, [](double h, const FileLayer& l) { return h < l.z1; });
    return it == v.end() ? static_cast<int>(v.size()) - 1 : static_cast<int>(it - v.begin());
}

// The 0.05 mm layer (of `count`, `height` thick) containing height `z`, held to the first and last.
inline int thinLayerAt(double z, double height, int count) {
    if (count <= 0 || height <= 0) return 0;
    const double i = std::floor(z / height + 1e-9);
    return static_cast<int>(std::clamp(i, 0.0, static_cast<double>(count - 1)));
}

// The mid-height of the 0.05 mm layer `index`.
inline double thinLayerMid(int index, double height) { return (static_cast<double>(index) + 0.5) * height; }

// The layer shown, in both kinds: a file layer and the 0.05 mm layer at its
// mid-height.
struct LayerPick {
    int file = 0;
    int thin = 0;
};

// Shows file layer `file` (held to the layers there are): the 0.05 mm layer at its mid-height goes with it.
inline LayerPick pickFileLayer(const std::vector<FileLayer>& v, int file, double thin_height, int thin_count) {
    LayerPick p;
    if (v.empty()) return p;
    p.file = std::clamp(file, 0, static_cast<int>(v.size()) - 1);
    p.thin = thinLayerAt(v[static_cast<std::size_t>(p.file)].mid(), thin_height, thin_count);
    return p;
}

// Shows the file layer at the height `z`.
inline LayerPick pickFileLayerAtHeight(const std::vector<FileLayer>& v, double z, double thin_height, int thin_count) {
    return pickFileLayer(v, fileLayerAt(v, z), thin_height, thin_count);
}

// The file layer shown when the 0.05 mm layer `thin` is (as `LayerState::current`
// says) and `kept` is the file layer last picked (-1: none). A kept file layer
// stands while the 0.05 mm layer still is the one at its mid-height; otherwise
// (the layer was moved without it: outside the adaptive view, or by "go to the
// biggest layer") it is the file layer at the height of the 0.05 mm layer.
inline LayerPick shownFileLayer(const std::vector<FileLayer>& v, int kept, int thin, double thin_height, int thin_count) {
    if (v.empty()) return {};
    if (kept >= 0 && kept < static_cast<int>(v.size())) {
        const LayerPick p = pickFileLayer(v, kept, thin_height, thin_count);
        if (p.thin == thin) return p;
    }
    return pickFileLayerAtHeight(v, thinLayerMid(thin, thin_height), thin_height, thin_count);
}

}  // namespace gui
