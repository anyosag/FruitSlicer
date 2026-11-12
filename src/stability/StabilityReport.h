// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// A stability result written out: as JSON, and as a picture for eyeballing it
// without the app.

#include <cstdint>
#include <vector>

#include <nlohmann/json.hpp>

#include "stability/Stability.h"

namespace fruit {

// The totals, what they rest on, the places worth a look (at most
// `max_spots`), and how close the weakest joint was while each layer printed.
nlohmann::json stabilityJson(const StabilityResult& r, const StabilityConfig& cfg, std::size_t max_spots = 40);

// The print seen through, from the front (X across) and from the side (Y
// across), plate at the bottom. Top row: what is at risk, each part coloured
// by its chance of not printing. Bottom row: weak spots, each joint coloured
// by the chance that a break starts there, the listed ones marked with a
// square. Light grey: supports. Grey: model, under 1 %. Yellow 1 to 5 %,
// orange 5 to 20 %, red 20 to 50 %, dark red over 50 %. The same six colours
// are drawn as swatches in the bottom left corner, in that order.
// RGB, row-major, `width` × `height`.
std::vector<std::uint8_t> stabilityPicture(const StabilityResult& r, int& width, int& height, int max_size = 1600);

}  // namespace fruit
