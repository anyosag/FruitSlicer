// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Scripted runs for screenshots and UI tests. A script is JSON:
//
//   {"size": [1440, 900], "scale": 1,
//    "steps": [{"screen": "home"}, {"shot": "home"},
//              {"open": "@demo"}, {"wait": "jobs"}, {"view": "angled"}, {"shot": "model"}]}
//
// Steps drive the app through the same commands its buttons use. "@demo"
// opens a built-in test model. Shots are written as <dir>/<name>.png once
// two consecutive frames are identical.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace gui {

class App;

class Script {
public:
    explicit Script(const std::string& path);
    int width() const { return width_; }
    int height() const { return height_; }
    float scale() const { return scale_; }

    // `render` draws one whole frame offscreen and returns its RGBA pixels.
    // Returns the process exit code.
    int run(App& app, const std::string& shot_dir, const std::function<std::vector<uint8_t>()>& render);

private:
    nlohmann::json steps_;
    int width_ = 1440, height_ = 900;
    float scale_ = 1;
};

}  // namespace gui
