// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Navigation.h"

#include "Document.h"

namespace gui {

Need needFor(const Document& d, Step s) {
    switch (s) {
        case Step::Model: return Need::Nothing;
        case Step::Orient:  // the search turns the selected part, keeps the size
            return d.fit().fits_centred ? Need::Nothing : Need::Fit;
        case Step::Supports: return d.fits() ? Need::Nothing : Need::Fit;
        case Step::Layers:
        case Step::Print: {
            if (!d.fits()) return Need::Fit;
            if (!d.supports.result) return Need::Supports;
            if (!d.supports.fresh(d.revision(), d.supportSettings())) return Need::FreshSupports;
            if (s == Step::Print && !d.layersFresh()) return Need::Layers;
            return Need::Nothing;
        }
    }
    return Need::Nothing;
}

Mark markFor(const Document& d, Step s) {
    const bool fresh = d.supports.result && d.supports.fresh(d.revision(), d.supportSettings());
    switch (s) {
        case Step::Model: return d.fits() ? Mark::None : Mark::Problem;
        case Step::Orient: return d.orientDecided() ? Mark::Done : Mark::None;
        case Step::Supports: return fresh ? Mark::Done : Mark::None;
        case Step::Layers: return fresh && d.layersFresh() ? Mark::Done : Mark::None;
        case Step::Print:
            return fresh && d.print.saved && d.layers.result && d.print.saved_from == d.layers.result ? Mark::Done
                                                                                                      : Mark::None;
    }
    return Mark::None;
}

Tool toolShown(Step s, bool hollow_view, Tool model_tool) {
    switch (s) {
        case Step::Model: return model_tool == Tool::Rotate || model_tool == Tool::Supports || model_tool == Tool::Hollow
                                     ? Tool::None
                                     : model_tool;
        case Step::Orient: return hollow_view ? Tool::Hollow : Tool::Rotate;
        case Step::Supports: return Tool::Supports;
        case Step::Layers:
        case Step::Print: return Tool::None;
    }
    return Tool::None;
}

Step stepFor(Tool t) {
    switch (t) {
        case Tool::Rotate:
        case Tool::Hollow: return Step::Orient;
        case Tool::Supports: return Step::Supports;
        default: return Step::Model;
    }
}

MainButton mainButton(const Document* d) {
    if (!d) return {MainAction::OpenModel, true};
    const bool fits = d->fits();
    if (!d->supports.result) return {MainAction::MakeSupports, fits};
    if (!d->supports.fresh(d->revision(), d->supportSettings())) return {MainAction::RemakeSupports, fits};
    if (d->layersFresh()) return {MainAction::ShowLayers, true};
    return {MainAction::Slice, true};
}

const char* mainLabel(MainAction a) {
    switch (a) {
        case MainAction::OpenModel: return "Open a model";
        case MainAction::MakeSupports: return "Make supports";
        case MainAction::RemakeSupports: return "Remake supports";
        case MainAction::Slice: return "Slice";
        case MainAction::ShowLayers: return "Show the layers";
    }
    return "";
}

}  // namespace gui
