// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Script.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "App.h"
#include "Capture.h"
#include "Document.h"
#include "Log.h"
#include "mesh/StlIO.h"

#include <miniz.h>

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

std::filesystem::path demoDir() {
    const auto dir = std::filesystem::temp_directory_path() / "fruitslicer-demo";
    std::filesystem::create_directories(dir);
    return dir;
}

// A 3MF package holding `m` (geometry only), to exercise the 3MF reader.
void writeDemo3mf(const std::string& path, const fruit::TriangleMesh& m) {
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<model unit=\"millimeter\" "
        "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\"><resources><object id=\"1\" "
        "type=\"model\"><mesh><vertices>";
    char b[160];
    for (const fruit::Vec3& v : m.vertices) {
        std::snprintf(b, sizeof b, "<vertex x=\"%.6f\" y=\"%.6f\" z=\"%.6f\"/>", v.x, v.y, v.z);
        xml += b;
    }
    xml += "</vertices><triangles>";
    for (const fruit::Face& f : m.faces) {
        std::snprintf(b, sizeof b, "<triangle v1=\"%d\" v2=\"%d\" v3=\"%d\"/>", f[0], f[1], f[2]);
        xml += b;
    }
    xml += "</triangles></mesh></object></resources><build><item objectid=\"1\"/></build></model>";
    const std::string rels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Relationships "
        "xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship "
        "Target=\"/3D/3dmodel.model\" Id=\"rel0\" "
        "Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/></Relationships>";
    mz_zip_archive z{};
    std::filesystem::remove(path);
    if (!mz_zip_writer_init_file(&z, path.c_str(), 0)) throw std::runtime_error("cannot write " + path);
    mz_zip_writer_add_mem(&z, "_rels/.rels", rels.data(), rels.size(), MZ_DEFAULT_COMPRESSION);
    mz_zip_writer_add_mem(&z, "3D/3dmodel.model", xml.data(), xml.size(), MZ_DEFAULT_COMPRESSION);
    mz_zip_writer_finalize_archive(&z);
    mz_zip_writer_end(&z);
}

// "@demo": the bracket; "@demo-big": six times larger (too deep for the
// plate); "@demo-3mf": the bracket as a 3MF file.
std::string demoPath(const std::string& which) {
    fruit::TriangleMesh m = demoModel();
    if (which == "@demo-big") {
        fruit::Transform t;
        for (auto& row : t.m)
            for (double& v : row) v *= 6;
        m.transform(t);
        const auto p = (demoDir() / "big bracket.stl").string();
        fruit::writeStl(p, m);
        return p;
    }
    if (which == "@demo-3mf") {
        const auto p = (demoDir() / "bracket.3mf").string();
        writeDemo3mf(p, m);
        return p;
    }
    const auto p = (demoDir() / "bracket.stl").string();
    fruit::writeStl(p, m);
    return p;
}

Step stepFrom(const std::string& s) {
    static const char* names[] = {"model", "orient", "supports", "layers", "print"};
    for (int i = 0; i < 5; ++i)
        if (s == names[i]) return static_cast<Step>(i);
    throw std::runtime_error("unknown step " + s);
}

Side sideFrom(const std::string& s) {
    static const char* names[] = {"front", "back", "left", "right", "top", "bottom"};
    for (int i = 0; i < 6; ++i)
        if (s == names[i]) return static_cast<Side>(i);
    throw std::runtime_error("unknown side " + s);
}

SizeChoice sizeFrom(const std::string& s) {
    if (s == "full") return SizeChoice::Full;
    if (s == "largest") return SizeChoice::Largest;
    if (s == "custom") return SizeChoice::Custom;
    if (s == "inches") return SizeChoice::Inches;
    throw std::runtime_error("unknown size choice " + s);
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
            if (!p.empty() && p[0] == '@') p = demoPath(p);
            app.openPath(p);
        } else if (step.contains("health")) {
            const std::string what = step["health"].get<std::string>();
            if (what == "place") app.placeChecked();
            else if (what == "cancel") app.cancelChecked();
            else app.chooseSize(sizeFrom(what), step.value("percent", 0.0));
        } else if (step.contains("scale")) {
            app.setScalePercent(step["scale"].get<double>());
        } else if (step.contains("rotate")) {
            const auto& r = step["rotate"];
            app.setRotation({r[0].get<double>(), r[1].get<double>(), r[2].get<double>()});
        } else if (step.contains("sits")) {
            app.setOnPlate(step["sits"].get<std::string>() == "flat");
        } else if (step.contains("fit")) {
            app.scaleToFit();
        } else if (step.contains("undo")) {
            for (int i = 0, n = step["undo"].get<int>(); i < n; ++i) app.undo();
        } else if (step.contains("redo")) {
            for (int i = 0, n = step["redo"].get<int>(); i < n; ++i) app.redo();
        } else if (step.contains("step")) {
            app.goToStep(stepFrom(step["step"].get<std::string>()));
        } else if (step.contains("side")) {
            app.toggleSide(sideFrom(step["side"].get<std::string>()));
        } else if (step.contains("orient")) {
            const std::string what = step["orient"].get<std::string>();
            if (what == "find") app.findOrientation();
            else if (what == "settings") app.showOrientSettings();
            else throw std::runtime_error("unknown orient step " + what);
        } else if (step.contains("use")) {
            app.useOrientation(step["use"].get<int>() - 1);  // 1 = option 1; 0 keeps how it sits
        } else if (step.contains("shortcuts")) {
            app.showShortcuts(step["shortcuts"].get<bool>());
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
