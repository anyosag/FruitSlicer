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
        cfg.peel_axis = peelAxis(*c.hinge);
        // Yours, else the resin's measured one (the tips plate in two sets), else 25 %, an assumption.
        cfg.peel_bias = c.peel_bias >= 0                                 ? c.peel_bias
                        : profile && profile->contains("peel_bias") ? profile->at("peel_bias").get<double>()
                                                                         : kAssumedPeelBias;
    } else if (c.peel_bias >= 0) {
        throw std::invalid_argument("a peel bias needs the hinge side");
    }
    if (c.on_plate) {
        cfg.on_plate = true;
        if (tree) tree->raft = RaftStyle::None;
    }
    return notes;
}

Vec2 peelAxis(HingeSide h) {
    switch (h) {
        case HingeSide::PlusX: return {-1, 0};
        case HingeSide::MinusX: return {1, 0};
        case HingeSide::PlusY: return {0, -1};
        case HingeSide::MinusY: return {0, 1};
    }
    return {0, 0};
}

double minRaftThickness(int bottom_layers, int transition_layers, double layer_height) {
    return std::max(0, bottom_layers + transition_layers) * layer_height;
}

}  // namespace fruit
