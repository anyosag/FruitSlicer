// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "support/SupportSettings.h"

#include <stdexcept>

#include "calibration/CalibrationFit.h"

namespace fruit {

std::optional<HingeSide> parseHinge(std::string_view s) {
    if (s == "+x") return HingeSide::PlusX;
    if (s == "-x") return HingeSide::MinusX;
    if (s == "+y") return HingeSide::PlusY;
    if (s == "-y") return HingeSide::MinusY;
    return std::nullopt;
}

const char* hingeName(HingeSide h) {
    switch (h) {
        case HingeSide::PlusX: return "+x";
        case HingeSide::MinusX: return "-x";
        case HingeSide::PlusY: return "+y";
        case HingeSide::MinusY: return "-y";
    }
    return "?";
}

std::vector<std::string> resolveSettings(const nlohmann::json* profile, const SupportChoices& c, SupportConfig& cfg,
                                         TreeConfig* tree) {
    cfg.layer_height = c.layer_height;
    cfg.coverage.density = c.density;
    std::vector<std::string> notes;
    if (profile) applyProfile(*profile, cfg, tree, &notes);
    if (c.critical_angle > 0) cfg.critical_angle_deg = c.critical_angle;
    if (c.hinge) {
        // Peel stress is highest on the side away from the hinge (Wu 2016).
        switch (*c.hinge) {
            case HingeSide::PlusX: cfg.peel_axis = {-1, 0}; break;
            case HingeSide::MinusX: cfg.peel_axis = {1, 0}; break;
            case HingeSide::PlusY: cfg.peel_axis = {0, -1}; break;
            case HingeSide::MinusY: cfg.peel_axis = {0, 1}; break;
        }
        cfg.peel_bias = c.peel_bias >= 0 ? c.peel_bias : 0.25;
    } else if (c.peel_bias >= 0) {
        throw std::invalid_argument("a peel bias needs the hinge side");
    }
    if (c.on_plate) {
        cfg.on_plate = true;
        if (tree) tree->raft = RaftStyle::None;
    }
    return notes;
}

double minRaftThickness(int bottom_layers, int transition_layers, double layer_height) {
    return std::max(0, bottom_layers + transition_layers) * layer_height;
}

}  // namespace fruit
