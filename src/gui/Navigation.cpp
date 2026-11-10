// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Navigation.h"

#include "Document.h"

namespace gui {

Need needFor(const Document& d, Step s) {
    switch (s) {
        case Step::Model: return Need::Nothing;
        case Step::Orient: return d.fit().fits_centred ? Need::Nothing : Need::Fit;  // the search turns it, keeps the size
        case Step::Supports: return d.fit().fits ? Need::Nothing : Need::Fit;
        case Step::Layers:
        case Step::Print: {
            if (!d.fit().fits) return Need::Fit;
            if (!d.supports.result) return Need::Supports;
            if (!d.supports.fresh(d.revision(), d.supportSettings())) return Need::FreshSupports;
            if (s == Step::Print && !d.layers.fresh(d.supports.result)) return Need::Layers;
            return Need::Nothing;
        }
    }
    return Need::Nothing;
}

Mark markFor(const Document& d, Step s) {
    const bool fresh = d.supports.result && d.supports.fresh(d.revision(), d.supportSettings());
    switch (s) {
        case Step::Model: return d.fit().fits ? Mark::None : Mark::Problem;
        case Step::Orient: return d.orient.decided ? Mark::Done : Mark::None;
        case Step::Supports: return fresh ? Mark::Done : Mark::None;
        case Step::Layers: return fresh && d.layers.fresh(d.supports.result) ? Mark::Done : Mark::None;
        case Step::Print:
            return fresh && d.print.saved && d.layers.result && d.print.saved_from == d.layers.result ? Mark::Done
                                                                                                      : Mark::None;
    }
    return Mark::None;
}

}  // namespace gui
