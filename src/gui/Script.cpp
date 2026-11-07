// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Script.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "App.h"
#include "Capture.h"
#include "Document.h"
#include "Log.h"
#include "mesh/StlIO.h"

namespace gui {

Script::Script(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open script " + path);
    const nlohmann::json j = nlohmann::json::parse(in);
    if (j.contains("size")) {
        width_ = j["size"][0].get<int>();
        height_ = j["size"][1].get<int>();
    }
    scale_ = j.value("scale", 1.0f);
    steps_ = j.at("steps");
}

namespace {

std::string demoPath() {
    const auto dir = std::filesystem::temp_directory_path() / "fruitslicer-demo";
    std::filesystem::create_directories(dir);
    const auto p = (dir / "bracket.stl").string();
    fruit::writeStl(p, demoModel());
    return p;
}

ViewPreset viewFrom(const std::string& s) {
    if (s == "front") return ViewPreset::Front;
    if (s == "side") return ViewPreset::Side;
    if (s == "below") return ViewPreset::Below;
    if (s == "angled") return ViewPreset::Angled;
    throw std::runtime_error("unknown view " + s);
}

Screen screenFrom(const std::string& s) {
    if (s == "home") return Screen::Home;
    if (s == "job" || s == "model") return Screen::Job;
    if (s == "about") return Screen::About;
    throw std::runtime_error("unknown screen " + s);
}

}  // namespace

int Script::run(App& app, const std::string& shot_dir, const std::function<std::vector<uint8_t>()>& render) {
    if (!shot_dir.empty()) std::filesystem::create_directories(shot_dir);
    const int pw = static_cast<int>(width_ * scale_), ph = static_cast<int>(height_ * scale_);
    for (const auto& step : steps_) {
        log::info("script: " + step.dump());
        if (step.contains("screen")) {
            app.setScreen(screenFrom(step["screen"].get<std::string>()));
        } else if (step.contains("open")) {
            std::string p = step["open"].get<std::string>();
            if (p == "@demo") p = demoPath();
            app.openPath(p);
        } else if (step.contains("wait")) {
            app.jobs().wait();
        } else if (step.contains("view")) {
            app.setView(viewFrom(step["view"].get<std::string>()));
        } else if (step.contains("frames")) {
            for (int i = 0, n = step["frames"].get<int>(); i < n; ++i) render();
        } else if (step.contains("shot")) {
            const std::string name = step["shot"].get<std::string>();
            // Render until the picture stops changing (layout settles over a
            // frame or two), at most 12 frames.
            std::vector<uint8_t> prev = render(), cur;
            for (int i = 0; i < 12; ++i) {
                cur = render();
                if (cur == prev) break;
                prev.swap(cur);
            }
            if (!shot_dir.empty()) {
                const std::string path = (std::filesystem::path(shot_dir) / (name + ".png")).string();
                writePngRgba(path, pw, ph, cur);
                log::info("script: wrote " + path);
            }
        } else {
            log::error("script: unknown step " + step.dump());
            return 2;
        }
    }
    return 0;
}

}  // namespace gui
