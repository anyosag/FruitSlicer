// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Scrolling over the Layers step's chart moves the layer shown. A Mac
// trackpad sends small fractions many times a second, a wheel whole units: the
// fractions add up until they make a whole layer.

#include <algorithm>
#include <cmath>

namespace gui {

struct LayerScrollStep {
    int layer = 0;       // the layer to show (0-based)
    double carry = 0;    // what has not yet made a whole layer, in layers
};

// One wheel unit moves about 1 % of the `count` layers, at least one. `units`
// is io.MouseWheelH - io.MouseWheel: on a Mac trackpad with natural scrolling,
// swiping right and swiping up both give positive units and move to higher
// layers (a mouse wheel turned towards you does too). `carry`
// is what the last call left over. The result stays within the first and last
// layer; the leftover is dropped at either end, so no scrolling is owed there.
inline LayerScrollStep scrollLayer(int layer, int count, double units, double carry) {
    LayerScrollStep r;
    const int last = std::max(0, count - 1);
    r.layer = std::clamp(layer, 0, last);
    if (count <= 1 || units == 0 || !std::isfinite(units)) {
        r.carry = (count <= 1 || !std::isfinite(units)) ? 0 : carry;
        return r;
    }
    const double per_unit = std::max(1.0, 0.01 * count);
    const double pending = carry + units * per_unit;
    // (a hair over, so ten tenths make one even though they add up to 0.99999...)
    const double whole = std::trunc(pending + (pending >= 0 ? 1e-9 : -1e-9));
    const double target = static_cast<double>(r.layer) + whole;
    r.layer = static_cast<int>(std::clamp(target, 0.0, static_cast<double>(last)));
    r.carry = (target < 0 || target > last) ? 0 : pending - whole;
    return r;
}

}  // namespace gui
