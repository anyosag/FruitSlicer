// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// FruitSlicer's licence and those of the code and font built into the app,
// with their full texts (embedded at build time from the files the build
// uses, so they match what is inside).

#include <string_view>
#include <vector>

namespace gui {

struct Licence {
    const char* name;       // "SDL 3"
    const char* what;       // what FruitSlicer uses it for
    const char* licence;    // the licence's short name
    std::string_view text;  // the full text, as the project ships it
};

// FruitSlicer's own licence first, then each library and the typeface.
const std::vector<Licence>& licences();

}  // namespace gui
