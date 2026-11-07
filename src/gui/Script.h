// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Scripted runs for screenshots and UI tests. A script is JSON:
//
//   {"size": [1440, 900], "scale": 1,
//    "steps": [{"screen": "home"}, {"shot": "home"},
//              {"open": "@demo"}, {"wait": "jobs"}, {"view": "angled"}, {"shot": "model"}]}
//
// Steps drive the app through the same commands its buttons use:
//   {"open": path | "@demo" | "@demo-big" | "@demo-3mf"}  built-in test models
//   {"wait": "jobs"}            finish background work
//   {"health": "full" | "largest" | "custom" (+ "percent") | "inches" | "place" | "cancel"}
//   {"scale": percent} {"rotate": [x, y, z]} {"sits": "raised" | "flat"} {"fit": true}
//   {"undo": n} {"redo": n} {"shortcuts": true | false}
//   {"step": "model" | "orient" | "supports" | "layers"} {"layer": n (1-based)} {"show_below": bool} {"side": "front" | "back" | "left" | "right" | "top" | "bottom"}
//   {"orient": "find" | "settings"} {"use": 1 | 2 | 3, or 0 to keep how it sits}
//   {"supports": "generate"} {"tip_size": 0.3 | 0.4 | 0.6} {"spacing": 1 to 2} {"merge": bool}
//   {"supports_view": "supports" | "tips" | "why"} {"select_tip": index} {"problems": bool}
//   {"frames": n} renders n frames (lets per-frame work, like background scoring, start)
//   {"screen": "home" | "job" | "about"} {"view": "front" | "side" | "angled" | "below"}
// Shots are written as <dir>/<name>.png once two consecutive frames are
// identical.

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
