// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// One place that turns "resin profile + what the user chose" into the
// generator and tree settings, in a fixed order: built-in defaults, then the
// resin profile, then explicit choices.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/SupportConfig.h"
#include "support/SupportTree.h"

namespace fruit {

// Side of the build plate where the tilting vat is hinged.
enum class HingeSide { PlusX, MinusX, PlusY, MinusY };

std::optional<HingeSide> parseHinge(std::string_view s);  // "+x", "-x", "+y", "-y"
const char* hingeName(HingeSide h);

struct SupportChoices {
    double layer_height = 0.05;
    double density = 1.0;                // > 1: denser tips
    double critical_angle = 0;           // > 0 overrides profile and default
    std::optional<HingeSide> hinge;      // extra hold away from the hinge
    double peel_bias = -1;               // < 0: 0.25 when a hinge is set
    bool on_plate = false;               // no lift, no raft
};

// Applies `profile` (may be null) and `choices` to `cfg` (and `tree`, when
// given). Returns the profile's notes (e.g. a layer height mismatch).
// Throws std::invalid_argument for a peel bias without a hinge side.
std::vector<std::string> resolveSettings(const nlohmann::json* profile, const SupportChoices& choices,
                                         SupportConfig& cfg, TreeConfig* tree);

// The raft must be at least as thick as the long-exposure layers, so their
// overcure never reaches the tips.
double minRaftThickness(int bottom_layers, int transition_layers, double layer_height);

}  // namespace fruit
