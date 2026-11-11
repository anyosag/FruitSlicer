// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Script.h"

#include <algorithm>
#include <chrono>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "App.h"
#include "Capture.h"
#include "Document.h"
#include "Log.h"
#include "ShapeTools.h"
#include "calibration/CalibrationFit.h"
#include "calibration/CalibrationPlates.h"
#include "formats/GooExport.h"
#include "formats/GooSetup.h"
#include "mesh/Primitives.h"
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
// plate); "@demo-3mf": the bracket as a 3MF file; "@demo-hollow": a 30 mm
// ball with a sealed hollow inside, under a 2 mm wall; "@demo-block": a plain 40 × 24 × 14 mm
// block; "@demo-bar": a
// 13.4 × 154 × 10 mm rack bar with posts, lying along Y: deeper than the
// plate, but it fits turned a quarter.
std::string demoPath(const std::string& which) {
    // Brackets with faults for the repair: see demoFaultyModel.
    for (const char* kind : {"worn", "open", "loose"})
        if (which == std::string("@demo-") + kind) {
            const auto p = (demoDir() / (std::string(kind) + (kind == std::string("loose") ? " cube.stl" : " bracket.stl"))).string();
            fruit::writeStl(p, demoFaultyModel(kind));
            return p;
        }
    if (which == "@demo-hollow") {
        fruit::TriangleMesh m = fruit::makeSphere({0, 0, 15}, 15, 96);
        fruit::TriangleMesh v = fruit::makeSphere({0, 0, 15}, 13, 96);
        for (auto& f : v.faces) std::swap(f[1], f[2]);  // facing in: a void
        m.append(v);
        const auto p = (demoDir() / "sphere.stl").string();
        fruit::writeStl(p, m);
        return p;
    }
    if (which == "@demo-block") {  // a plain 40 × 24 × 14 mm block: one clean solid for Cut, Label and Combine
        const auto p = (demoDir() / "block.stl").string();
        fruit::writeStl(p, fruit::makeBox({0, 0, 0}, {40, 24, 14}));
        return p;
    }
    if (which == "@demo-bar") {
        fruit::TriangleMesh m = fruit::makeBox({0, 0, 0}, {13.4, 154, 4});
        for (int i = 0; i < 9; ++i) {
            const double y = 6 + i * 17.5;  // posts overlap the bar, so every shell stays closed
            m.append(fruit::makeBox({3, y, 3.5}, {10.4, y + 6, 10}));
        }
        const auto p = (demoDir() / "rack bar.stl").string();
        fruit::writeStl(p, m);
        return p;
    }
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

// "@demo-profile": a resin profile from `fruitslicer fit`, fitted to a tips
// plate scored as a resin a little weaker than the borrowed values would be.
std::string demoProfile() {
    const fruit::CalibrationPlate plate =
        fruit::tipsPlate(fruit::TipsPlateOptions{}, fruit::PrinterProfile::saturn4Ultra16k());
    fruit::PlateResults res;
    res.kind = plate.kind;
    res.settings = plate.settings;
    for (const auto& c : plate.coupons) {
        fruit::CouponResult r;
        r.id = c.id;
        r.params = c.params;
        r.recorded = true;
        const double d = c.params["tip_diameter"].get<double>(), load = c.params["load_per_tip_mm2"].get<double>();
        r.ok = load <= 0.8 * fruit::CapacityModel{}.capacity(d);
        r.scar = r.ok ? (d >= 0.6 ? 4 : d >= 0.4 ? 2 : 1) : 0;
        res.coupons.push_back(r);
    }
    fruit::FitOptions o;
    o.resin = "Demo resin";
    const auto p = (demoDir() / "demo resin.json").string();
    std::ofstream(p) << fruit::fitProfile({res}, o).dump(2) << "\n";
    return p;
}

// How the demo scorer finds a coupon: the tips hold up to 80 % of the
// borrowed capacity, and let go partly up to all of it; steep enough or
// close enough tips hold elsewhere.
App::Outcome demoOutcome(const std::string& kind, const nlohmann::json& p) {
    if (kind == "tips") {
        const double d = p["tip_diameter"].get<double>(), load = p["load_per_tip_mm2"].get<double>();
        const double cap = fruit::CapacityModel{}.capacity(d);
        return load <= 0.8 * cap ? App::Outcome::Held : load <= cap ? App::Outcome::Partly : App::Outcome::FellOff;
    }
    if (kind == "spacing") {
        const double a = p["angle_deg"].get<double>();
        if (p["kind"] == "wedge") return a <= 45 ? App::Outcome::Held : a <= 55 ? App::Outcome::Partly : App::Outcome::FellOff;
        const double s = p["spacing_mm"].get<double>();
        return s <= 2 + a / 15 ? App::Outcome::Held : s <= 3 + a / 15 ? App::Outcome::Partly : App::Outcome::FellOff;
    }
    const double D = p["island_diameter"].get<double>(), k = p["tips"].get<double>();
    return D <= 1.7 * k ? App::Outcome::Held : App::Outcome::FellOff;
}

int demoMark(const nlohmann::json& p) {
    const double d = p.value("tip_diameter", 0.4);
    return d >= 0.6 ? 4 : d >= 0.4 ? 2 : 1;
}

// "@demo" for score_import: a sheet filled in by hand for `p`, as
// `fruitslicer calibrate` writes them (settings columns and all), with its
// manifest beside it. Every seventh coupon fell off.
std::string demoFilledSheet(const fruit::CalibrationPlate& p) {
    const std::string base = (demoDir() / ("by hand " + p.kind + ".stl")).string();
    std::ofstream(base + ".json") << p.manifest().dump(2) << "\n";
    std::istringstream in(p.csvTemplate());
    std::ostringstream out;
    std::string line;
    std::getline(in, line);
    out << line << "\n";
    for (int i = 1; std::getline(in, line); ++i) {
        line.resize(line.size() - 3);  // the empty result, scar and notes
        out << line << (i % 7 == 0 ? ",failed,,\"came off, film clean\"" : ",ok,2,") << "\n";
    }
    std::ofstream(base + ".results.csv") << out.str();
    return base + ".results.csv";
}

// "@demo-sample": a print file like one SatelLite saves, with 0.03 mm
// layers and its own exposures, for the Print step's settings.
std::string demoSample() {
    const auto p = (demoDir() / "sample 0.03 mm.goo").string();
    fruit::GooPrintRequest r;
    r.layer_height = 0.03;
    r.exposure = 1.5;
    r.bottom_exposure = 27.5;
    r.bottom_layers = 6;
    r.transition_layers = 6;
    const fruit::GooPrintSetup setup = fruit::buildGooSetup(fruit::PrinterProfile::saturn4Ultra16k(), r);
    fruit::GooExportOptions o;
    o.file_time = "2026-11-05 15:43:00";  // the same every run
    fruit::exportGoo(fruit::makeBox({-2, -2, 0}, {2, 2, 0.3}), setup.header, setup.grid, o, p);
    return p;
}

// "@demo-sample-2": the same box as "@demo-sample", with other exposures
// (2 s, bottom 30 s), for comparing two files.
std::string demoSample2() {
    const auto p = (demoDir() / "sample 2 s.goo").string();
    fruit::GooPrintRequest r;
    r.layer_height = 0.03;
    r.exposure = 2.0;
    r.bottom_exposure = 30;
    r.bottom_layers = 6;
    r.transition_layers = 6;
    const fruit::GooPrintSetup setup = fruit::buildGooSetup(fruit::PrinterProfile::saturn4Ultra16k(), r);
    fruit::GooExportOptions o;
    o.file_time = "2026-11-05 15:43:00";  // the same every run
    fruit::exportGoo(fruit::makeBox({-2, -2, 0}, {2, 2, 0.3}), setup.header, setup.grid, o, p);
    return p;
}

Step stepFrom(const std::string& s) {
    static const char* names[] = {"model", "orient", "supports", "layers", "print"};
    for (int i = 0; i < 5; ++i)
        if (s == names[i]) return static_cast<Step>(i);
    throw std::runtime_error("unknown step " + s);
}

Tool toolFrom(const std::string& s) {
    if (s == "mirror") return Tool::Mirror;
    if (s == "measure") return Tool::Measure;
    if (s == "duplicate") return Tool::Duplicate;
    if (s == "arrange") return Tool::Arrange;
    if (s == "merge") return Tool::Merge;
    if (s == "split") return Tool::Split;
    if (s == "label") return Tool::Label;
    if (s == "combine") return Tool::Combine;
    if (s == "cut") return Tool::Cut;
    static const char* names[] = {"none", "on_plate", "move", "rotate", "scale", "supports", "hollow", "repair"};
    for (int i = 0; i < 8; ++i)
        if (s == names[i]) return static_cast<Tool>(i);
    throw std::runtime_error("unknown tool " + s);
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
    if (s == "calibrate") return Screen::Calibrate;
    if (s == "orientation_check") return Screen::OrientationCheck;
    if (s == "printer") return Screen::Printer;
    if (s == "settings") return Screen::Settings;
    if (s == "setup") return Screen::Setup;
    if (s == "resins") return Screen::Resins;
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
            if (p == "@demo-job") p = (demoDir() / "bracket.fruitjob").string();
            else if (!p.empty() && p[0] == '@') p = demoPath(p);
            app.openPath(p);
        } else if (step.contains("add_part")) {
            // Another model onto the plate (then the health dialog, as for open).
            std::string p = step["add_part"].get<std::string>();
            if (!p.empty() && p[0] == '@') p = demoPath(p);
            app.addPartFrom(p);
        } else if (step.contains("select_part") || step.contains("remove_part")) {
            const bool select = step.contains("select_part");
            const int i = step[select ? "select_part" : "remove_part"].get<int>() - 1;  // 1 = the first
            const auto& doc = app.document();
            if (!doc || i < 0 || i >= static_cast<int>(doc->partCount()))
                throw std::runtime_error("select_part/remove_part: no such part");
            const int id = doc->parts()[static_cast<std::size_t>(i)].id();
            if (select) app.selectPart(id);
            else app.removePart(id);
        } else if (step.contains("select_at")) {
            // [x, y, z, dx, dy, dz]: a click in the 3D view along that ray (printer coordinates) selects the part it meets.
            const auto& a = step["select_at"];
            const int id = app.pickPart({a[0].get<double>(), a[1].get<double>(), a[2].get<double>()},
                                        {a[3].get<double>(), a[4].get<double>(), a[5].get<double>()});
            if (id < 0) throw std::runtime_error("select_at: meets no part");
            app.selectPart(id);
        } else if (step.contains("save_job")) {
            std::string p = step["save_job"].get<std::string>();
            if (p == "@demo") p = (demoDir() / "bracket.fruitjob").string();
            app.saveJobTo(p);
        } else if (step.contains("unsaved")) {
            const std::string a = step["unsaved"].get<std::string>();
            app.answerUnsaved(a == "save"      ? App::UnsavedAnswer::Save
                              : a == "discard" ? App::UnsavedAnswer::DontSave
                                               : App::UnsavedAnswer::Cancel);
        } else if (step.contains("relink")) {
            const std::string a = step["relink"].get<std::string>();
            if (a == "copy") app.relinkOpen();
            else if (a == "cancel") app.relinkCancel();
            else app.relinkTo(a == "@demo" ? demoPath("@demo") : a);
        } else if (step.contains("remove")) {
            // Test set-up: "@demo" removes the demo bracket's STL (to make a saved job's model go missing).
            std::string p = step["remove"].get<std::string>();
            if (p == "@demo") p = (demoDir() / "bracket.stl").string();
            std::error_code ec;
            std::filesystem::remove(p, ec);
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
        } else if (step.contains("move")) {
            const auto& m = step["move"];
            app.setPosition({m[0].get<double>(), m[1].get<double>()});
        } else if (step.contains("nudge")) {
            const auto& m = step["nudge"];
            app.nudge(m[0].get<double>(), m[1].get<double>());
        } else if (step.contains("centre")) {
            app.centreModel();
        } else if (step.contains("turn")) {
            app.turnQuarter();
        } else if (step.contains("slide_on")) {
            app.slideOntoPlate();
        } else if (step.contains("drag")) {
            // ["move_x" | "move_y" | "turn_x" | "turn_y" | "turn_z", amount (mm or degrees), snap (⇧)]
            const auto& d = step["drag"];
            const std::string h = d[0].get<std::string>();
            const Handle handle = h == "move_x"   ? Handle::MoveX
                                  : h == "move_y" ? Handle::MoveY
                                  : h == "turn_x" ? Handle::TurnX
                                  : h == "turn_y" ? Handle::TurnY
                                  : h == "turn_z" ? Handle::TurnZ
                                                  : Handle::None;
            if (handle == Handle::None) throw std::runtime_error("unknown handle " + h);
            app.dragHandleBy(handle, d[1].get<double>(), d.size() > 2 && d[2].get<bool>());
        } else if (step.contains("undo")) {
            for (int i = 0, n = step["undo"].get<int>(); i < n; ++i) app.undo();
        } else if (step.contains("redo")) {
            for (int i = 0, n = step["redo"].get<int>(); i < n; ++i) app.redo();
        } else if (step.contains("step")) {
            app.goToStep(stepFrom(step["step"].get<std::string>()));
        } else if (step.contains("tool")) {
            app.openTool(toolFrom(step["tool"].get<std::string>()));
        } else if (step.contains("mirror")) {
            const std::string a = step["mirror"].get<std::string>();
            if (a != "x" && a != "y" && a != "z") throw std::runtime_error("unknown mirror axis " + a);
            app.mirrorModel(a == "x" ? 0 : a == "y" ? 1 : 2);
        } else if (step.contains("duplicate")) {
            app.partsTools().copies = std::clamp(step["duplicate"].get<int>(), 1, kMaxCopies);
            app.duplicateParts();
        } else if (step.contains("arrange")) {
            if (step["arrange"].is_number()) app.partsTools().arrange_gap = step["arrange"].get<double>();
            app.arrangeAll();
        } else if (step.contains("split")) {
            app.splitSelected();
        } else if (step.contains("merge_parts")) {
            // {"merge_parts": "join" | "fuse"}; {"merge_pick": [n, ...]} (1-based) first leaves the others out
            const std::string how = step["merge_parts"].get<std::string>();
            if (how != "join" && how != "fuse") throw std::runtime_error("unknown merge " + how);
            app.mergeParts(how == "fuse");
        } else if (step.contains("merge_pick")) {
            if (!app.document()) throw std::runtime_error("merge_pick: no job");
            std::vector<int> on;
            for (const auto& n : step["merge_pick"]) {
                const int i = n.get<int>() - 1;
                if (i < 0 || i >= static_cast<int>(app.document()->partCount())) throw std::runtime_error("merge_pick: no such part");
                on.push_back(app.document()->parts()[static_cast<std::size_t>(i)].id());
            }
            for (const Part& p : app.document()->parts())
                app.mergePick(p.id(), std::find(on.begin(), on.end(), p.id()) != on.end());
        } else if (step.contains("collisions")) {
            // {"collisions": "wait"} lets the background check finish; {"collisions": mm} sets the least gap
            if (step["collisions"].is_number()) app.collisionGap() = step["collisions"].get<double>();
            else app.finishCollisions();
        } else if (step.contains("measure")) {
            // Points on the plate; each is brought onto the surface along the line to the middle of the
            // part whose middle is nearest it (with one part, the model's middle).
            if (step["measure"].is_string()) {
                app.clearMeasure();
            } else if (app.document()) {
                for (const auto& q : step["measure"]) {
                    const fruit::Vec3 from{q[0].get<double>(), q[1].get<double>(), q[2].get<double>()};
                    fruit::Vec3 mid = app.document()->placedBox().center();
                    double best = -1;
                    for (const Part& part : app.document()->parts()) {
                        const fruit::Vec3 c = part.box().center(), d = c - from;
                        const double dd = d.x * d.x + d.y * d.y + d.z * d.z;
                        if (app.document()->partCount() > 1 && (best < 0 || dd < best)) best = dd, mid = c;
                    }
                    fruit::Vec3 at, n;
                    if (!app.pickModel(from, mid - from, &at, &n)) throw std::runtime_error("measure: no model there");
                    app.measureClick(at);
                }
            }
        } else if (step.contains("close_tool")) {
            app.closeToolPanel();
        } else if (step.contains("job_tab")) {
            const std::string tab = step["job_tab"].get<std::string>();
            app.jobTab() = tab == "collisions" ? 2 : tab == "hollows" ? 1 : 0;
        } else if (step.contains("side")) {
            app.toggleSide(sideFrom(step["side"].get<std::string>()));
        } else if (step.contains("orient")) {
            const std::string what = step["orient"].get<std::string>();
            if (what == "find") app.findOrientation();
            else if (what == "settings") app.showOrientSettings();
            else throw std::runtime_error("unknown orient step " + what);
        } else if (step.contains("hollow")) {
            const std::string what = step["hollow"].get<std::string>();
            if (what == "open") app.showHollow();
            else if (what == "apply") app.applyHollow();
            else throw std::runtime_error("unknown hollow step " + what);
        } else if (step.contains("hollow_fix") || step.contains("hollow_it") || step.contains("hole_place") ||
                   step.contains("hole_diameter")) {
            const auto& doc = app.document();
            if (!doc) throw std::runtime_error("hollow choices: no model open");
            HollowChoices c = doc->hollow.choices;
            if (step.contains("hollow_fix")) {
                const std::string f = step["hollow_fix"].get<std::string>();
                c.hollow = false;
                c.fix = f == "fill" ? HollowChoices::Fix::Fill
                        : f == "leave" ? HollowChoices::Fix::Leave
                                       : HollowChoices::Fix::Holes;
            }
            if (step.contains("hollow_it")) c.hollow = step["hollow_it"].get<bool>();
            c.wall = step.value("wall", c.wall);
            c.ceiling_lean = step.value("lean", c.ceiling_lean);
            if (step.contains("hole_place")) c.auto_holes = step["hole_place"].get<std::string>() != "hand";
            c.hole_diameter = step.value("hole_diameter", c.hole_diameter);
            app.setHollowChoices(c);
        } else if (step.contains("hole_ray")) {
            // [x, y, z, dx, dy, dz]: from (x, y, z) mm off the middle of the placed model, along (dx, dy, dz);
            // a hole goes in where that first meets the model, as a click there would put it.
            const auto& a = step["hole_ray"];
            const auto& doc = app.document();
            if (!doc) throw std::runtime_error("hole_ray: no model open");
            const fruit::BBox3 b = doc->part().box();  // the selected part's
            const fruit::Vec3 mid = (b.min + b.max) * 0.5;
            fruit::Vec3 pos, n;
            if (!app.pickModel({mid.x + a[0].get<double>(), mid.y + a[1].get<double>(), mid.z + a[2].get<double>()},
                               fruit::normalized({a[3].get<double>(), a[4].get<double>(), a[5].get<double>()}), &pos, &n))
                throw std::runtime_error("hole_ray: misses the model");
            app.addHoleAt(pos, n);
        } else if (step.contains("cut")) {
            app.setCutAway(step["cut"].get<bool>());
        } else if (step.contains("hole_remove")) {
            app.removeHole(step["hole_remove"].get<int>() - 1);  // 1 = the first
        } else if (step.contains("use")) {
            app.useOrientation(step["use"].get<int>() - 1);  // 1 = option 1; 0 keeps how it sits
        } else if (step.contains("supports")) {
            const std::string what = step["supports"].get<std::string>();
            if (what == "now") {
                app.supportsNow();  // the accent button from any place
            } else {
                if (what != "generate") throw std::runtime_error("unknown supports step " + what);
                app.generateSupports();
            }
        } else if (step.contains("tip_size") || step.contains("spacing") || step.contains("merge")) {
            SupportChoices c = app.document() ? app.document()->supports.choices : SupportChoices{};
            if (step.contains("tip_size")) {
                const double d = step["tip_size"].get<double>();
                c.usual_tip = d < 0.35 ? 1 : (d < 0.5 ? 2 : 3);
            }
            if (step.contains("spacing")) c.density = step["spacing"].get<double>();
            if (step.contains("merge")) c.merge = step["merge"].get<bool>();
            app.setSupportChoices(c);
        } else if (step.contains("supports_view")) {
            const std::string v = step["supports_view"].get<std::string>();
            app.setSupportsView(v == "why" ? SupportsView::Why : v == "tips" ? SupportsView::TipsOnly : SupportsView::Supports);
        } else if (step.contains("select_tip")) {
            app.selectTip(step["select_tip"].get<int>());
        } else if (step.contains("layer")) {
            app.setLayer(step["layer"].get<int>() - 1);  // 1-based, as shown
        } else if (step.contains("show_below")) {
            app.setShowBelow(step["show_below"].get<bool>());
        } else if (step.contains("problems")) {
            app.showProblems(step["problems"].get<bool>());
        } else if (step.contains("advanced")) {
            const std::string what = step["advanced"].get<std::string>();
            if (what == "open") app.showAdvanced(true);
            else if (what == "close") app.showAdvanced(false);
            else if (what == "reset") app.resetAdvanced();
            else if (what == "apply") app.applyAdvanced();
            else throw std::runtime_error("unknown advanced step " + what);
        } else if (step.contains("setting")) {
            const auto& kv = step["setting"];
            app.setAdvanced(kv[0].get<std::string>(), kv[1].get<double>());
        } else if (step.contains("exposures")) {
            const auto& e = step["exposures"];
            app.setExposures(e[0].get<double>(), e[1].get<double>());
        } else if (step.contains("start_setup")) {
            app.startSetup();
        } else if (step.contains("setup_step")) {
            app.setupStep() = step["setup_step"].get<int>();
        } else if (step.contains("default_sample")) {
            // Setup's print settings, from a file (refused if it is for another printer).
            std::string p = step["default_sample"].get<std::string>();
            if (p == "@demo-sample") p = demoSample();
            app.useDefaultSample(p);
        } else if (step.contains("typed_defaults")) {
            const auto& t = step["typed_defaults"];
            App::TypedDefaults& d = app.typedDefaults();
            d.exposure = t[0].get<double>();
            d.bottom_exposure = t[1].get<double>();
            d.bottom_layers = t[2].get<int>();
            d.transition_layers = t[3].get<int>();
            app.setupTyped() = true;
        } else if (step.contains("setup_typed")) {
            app.setupTyped() = step["setup_typed"].get<bool>();
        } else if (step.contains("resin_import")) {
            std::string p = step["resin_import"].get<std::string>();
            if (p == "@demo-profile") p = demoProfile();
            app.importResinFile(p);
        } else if (step.contains("resin_new_from")) {
            std::string p = step["resin_new_from"].get<std::string>();
            if (p == "@demo-sample") p = demoSample();
            app.newResinFromGooFile(p);
        } else if (step.contains("resin_select") || step.contains("job_resin") || step.contains("new_job_resin") ||
                   step.contains("resin_delete")) {
            const char* key = step.contains("resin_select") ? "resin_select"
                              : step.contains("job_resin")   ? "job_resin"
                              : step.contains("resin_delete") ? "resin_delete"
                                                              : "new_job_resin";
            const std::string name = step[key].get<std::string>();
            std::string id;
            for (const Resin& r : app.resins())
                if (r.name == name) id = r.id;
            if (id.empty()) throw std::runtime_error("no resin called " + name);
            if (std::string(key) == "resin_select") app.resinSelected() = id;
            else if (std::string(key) == "job_resin") app.useResinForJob(id);
            else if (std::string(key) == "resin_delete") app.resinToDelete() = id;
            else app.setNewJobResin(id);
        } else if (step.contains("resin_delete_confirm")) {
            app.deleteResin(app.resinToDelete());
        } else if (step.contains("resin_menu")) {
            app.resinMenu() = step["resin_menu"].get<bool>();
        } else if (step.contains("plate")) {
            const std::string kind = step["plate"].get<std::string>();
            if (!isPlateKind(kind)) throw std::runtime_error("no plate called " + kind);
            app.openPlate(kind);
        } else if (step.contains("plate_select")) {
            app.plateSelected() = step["plate_select"].get<int>();
        } else if (step.contains("plate_save_to")) {
            // "@demo": <kind>.goo in the demo folder, "@demo-stl": <kind>.stl, as the Save buttons would.
            std::string p = step["plate_save_to"].get<std::string>();
            if (p == "@demo") p = (demoDir() / (app.plateKind() + ".goo")).string();
            else if (p == "@demo-stl") p = (demoDir() / (app.plateKind() + ".stl")).string();
            app.savePlateTo(p);
        } else if (step.contains("score")) {
            const std::string kind = step["score"].get<std::string>();
            if (!isPlateKind(kind)) throw std::runtime_error("no plate called " + kind);
            app.openScores(kind);
        } else if (step.contains("score_select")) {
            app.scoreSelected() = step["score_select"].get<int>();
        } else if (step.contains("score_demo")) {
            // The first n coupons scored as by someone holding a resin a
            // little weaker than the borrowed values (as "@demo-profile").
            const int n = step["score_demo"].get<int>();
            if (!app.scores()) throw std::runtime_error("score_demo: no scores open");
            const std::vector<fruit::CouponResult> cs = app.scores()->coupons;
            for (int i = 0; i < n && i < static_cast<int>(cs.size()); ++i) {
                const auto& c = cs[static_cast<std::size_t>(i)];
                app.setScore(c.id, demoOutcome(app.plateKind(), c.params), demoMark(c.params), "");
                app.scoreSelected() = nextToScore(*app.scores(), c.id);  // as Save and go to the next coupon does
            }
        } else if (step.contains("fit_page")) {
            const std::string a = step["fit_page"].get<std::string>();
            if (a == "open") app.openFit();
            else if (a == "use") app.useFit();
            else throw std::runtime_error("unknown fit_page step " + a);
        } else if (step.contains("ab")) {
            if (step["ab"].get<std::string>() != "open") throw std::runtime_error("unknown ab step");
            app.openABSetup();
        } else if (step.contains("tools")) {
            const std::string t = step["tools"].get<std::string>();
            app.setScreen(Screen::Tools);
            app.toolsTab() = t == "compare" ? 1 : 0;
        } else if (step.contains("goo_inspect")) {
            std::string p = step["goo_inspect"].get<std::string>();
            if (p == "@demo-sample") p = demoSample();
            else if (p == "@demo-sample-2") p = demoSample2();
            app.inspectGooFile(p);
        } else if (step.contains("goo_check")) {
            app.checkGoo();
        } else if (step.contains("goo_compare")) {
            // [A, B]: "@demo-sample", "@demo-sample-2" (other exposures), or paths.
            for (int side = 0; side < 2; ++side) {
                std::string p = step["goo_compare"].at(static_cast<std::size_t>(side)).get<std::string>();
                if (p == "@demo-sample") p = demoSample();
                else if (p == "@demo-sample-2") p = demoSample2();
                app.setGooToCompare(side, p);
            }
        } else if (step.contains("ab_results")) {
            app.openABResults();
        } else if (step.contains("ab_select")) {
            app.abSelected() = step["ab_select"].get<int>();
        } else if (step.contains("ab_reveal")) {
            app.abRevealed() = step["ab_reveal"].get<bool>();
        } else if (step.contains("ab_score_demo")) {
            // The first n copies scored as by someone whose A copies (FruitSlicer's)
            // marked less and cleaned up faster; the last B copy only partly printed.
            const int n = step["ab_score_demo"].get<int>();
            const nlohmann::json m = app.abManifest();
            if (m.is_null()) throw std::runtime_error("ab_score_demo: no A/B test open");
            int i = 0, b_left = 0;
            for (const auto& c : m["copies"]) b_left += c["arm"] == "B" ? 1 : 0;
            for (const auto& c : m["copies"]) {
                if (i++ >= n) break;
                const int id = c["id"].get<int>();
                if (c["arm"] == "A") {
                    app.setABScore(id, App::Outcome::Held, 1 + id % 2, 40 + 3 * id, "");
                } else if (--b_left == 0) {
                    app.setABScore(id, App::Outcome::Partly, 4, 120, "one arm came off");
                } else {
                    app.setABScore(id, App::Outcome::Held, 3, 85 + 2 * id, "");
                }
            }
        } else if (step.contains("ab_copies")) {
            app.setABCopies(step["ab_copies"].get<int>());
        } else if (step.contains("ab_save_to")) {
            // "@demo": "<job> AB_A.stl" (and _B) in the demo folder, as the Save button would.
            std::string p = step["ab_save_to"].get<std::string>();
            if (p == "@demo" && app.document()) p = (demoDir() / (app.document()->name + " AB_A.stl")).string();
            app.saveABTo(p);
        } else if (step.contains("fit_table")) {
            app.fitTable() = step["fit_table"].get<bool>();
        } else if (step.contains("fit_save_to")) {
            // "@demo": the profile as "fitted profile.json" in the demo folder.
            std::string p = step["fit_save_to"].get<std::string>();
            if (p == "@demo") p = (demoDir() / "fitted profile.json").string();
            app.saveFitTo(p);
        } else if (step.contains("score_mirrored")) {
            app.scoreMirrored() = step["score_mirrored"].get<bool>();
        } else if (step.contains("score_import")) {
            std::string p = step["score_import"].get<std::string>();
            if (p == "@demo") p = demoFilledSheet(app.plate());
            app.importScoresFile(p);
        } else if (step.contains("licence")) {
            app.licenceOpen() = step["licence"].get<std::string>();
        } else if (step.contains("prefs")) {
            const auto& j = step["prefs"];
            Preferences q = app.preferences();
            q.smooth_view = j.value("smooth_view", q.smooth_view);
            q.cores = j.value("cores", q.cores);
            q.goo_to_drive = j.value("goo_to_drive", q.goo_to_drive);
            app.setPreferences(q);
        } else if (step.contains("sample")) {
            std::string p = step["sample"].get<std::string>();
            if (p == "@demo-sample") p = demoSample();
            app.useSample(p);
        } else if (step.contains("drive")) {
            // A stand-in USB drive: a folder under the demo directory.
            platform::Volume v;
            v.name = step["drive"].get<std::string>();
            v.path = (demoDir() / v.name).string();
            v.format = "MS-DOS (FAT32)";
            v.free_bytes = 15'500'000'000ULL;
            std::filesystem::create_directories(v.path);
            app.drives().push_back(v);
            app.driveChoice() = static_cast<int>(app.drives().size()) - 1;
        } else if (step.contains("save")) {
            app.savePrint();  // first time: the orientation question
        } else if (step.contains("save_to")) {
            std::string p = step["save_to"].get<std::string>();
            if (p == "@demo" && app.document()) p = (demoDir() / (app.document()->name + ".goo")).string();
            app.savePrintTo(p, "");
        } else if (step.contains("orientation_piece")) {
            // "@demo": f.stl in the demo folder, as the Save f.stl… button would.
            std::string p = step["orientation_piece"].get<std::string>();
            if (p == "@demo") p = (demoDir() / "f.stl").string();
            app.saveOrientationPieceTo(p);
        } else if (step.contains("orientation_sample")) {
            // "@stand-in-flip-y" (or -flip-x, -as-is): a stand-in for SatelLite's slice of the F.
            std::string p = step["orientation_sample"].get<std::string>();
            if (p.rfind("@stand-in", 0) == 0) {
                const bool fx = p.find("flip-x") != std::string::npos, fy = p.find("flip-y") != std::string::npos;
                p = (demoDir() / "f_satellite.goo").string();
                writeStandInSatelliteF(p, fx, fy);
            }
            app.checkOrientationWith(p);
        } else if (step.contains("orientation_fix")) {
            app.useOrientationFix();
        } else if (step.contains("hinge")) {
            const std::string h = step["hinge"].get<std::string>();
            if (h == "none") app.setHinge(std::nullopt);
            else if (h == "back") app.setHinge(HingeChoice::Back);
            else if (h == "left") app.setHinge(HingeChoice::Left);
            else if (h == "right") app.setHinge(HingeChoice::Right);
            else if (h == "front") app.setHinge(HingeChoice::Front);
            else throw std::runtime_error("unknown hinge side " + h);
        } else if (step.contains("edit_tips")) {
            app.editTips(step["edit_tips"].get<bool>());
        } else if (step.contains("tip_tool")) {
            const std::string t = step["tip_tool"].get<std::string>();
            app.tipTool() = t == "add" ? App::TipTool::Add : t == "zone" ? App::TipTool::Zone : App::TipTool::Select;
        } else if (step.contains("tip_add_near") || step.contains("tip_zone_near") || step.contains("tip_move_near")) {
            // [tip, dx, dy]: straight up from below the plate, (dx, dy) mm from that tip; move: the tip goes there.
            const char* key = step.contains("tip_add_near") ? "tip_add_near"
                              : step.contains("tip_zone_near") ? "tip_zone_near" : "tip_move_near";
            const auto& a = step[key];
            const int i = a[0].get<int>();
            const auto& doc = app.document();
            if (!doc || !doc->supports.result || i < 0 || i >= static_cast<int>(doc->supports.result->points.size()))
                throw std::runtime_error(std::string(key) + ": no such tip");
            const fruit::Vec3 p = doc->supports.result->points[static_cast<std::size_t>(i)].pos;
            fruit::Vec3 pos, n;
            if (!app.pickModel({p.x + a[1].get<double>(), p.y + a[2].get<double>(), -10}, {0, 0, 1}, &pos, &n))
                throw std::runtime_error(std::string(key) + ": nothing above that point");
            if (std::string(key) == "tip_add_near") app.addTipAt(pos, n);
            else if (std::string(key) == "tip_zone_near") app.addZoneAt(pos);
            else app.moveTip(i, pos, n);
        } else if (step.contains("tip_select")) {
            app.selectTip(step["tip_select"].get<int>());
        } else if (step.contains("tip_delete")) {
            app.deleteTip(step["tip_delete"].get<int>());
        } else if (step.contains("tip_resize")) {
            app.resizeTip(step["tip_resize"][0].get<int>(), step["tip_resize"][1].get<double>());
        } else if (step.contains("tip_pin")) {
            app.pinTip(step["tip_pin"].get<int>());
        } else if (step.contains("tip_undo")) {
            const auto& u = step["tip_undo"];
            if (u.is_string() && u.get<std::string>() == "all") app.undoAllTipEdits();
            else app.undoTipEdit(u.get<int>());
        } else if (step.contains("answer")) {
            // The "save before checking the image orientation?" question.
            const std::string a = step["answer"].get<std::string>();
            if (a == "save_anyway") app.savePrintAfterAsking();
            else if (a == "cancel") app.orientationAsk() = false;
            else throw std::runtime_error("unknown answer " + a);
        } else if (step.contains("save_target")) {
            app.sendOverWifi() = step["save_target"].get<std::string>() == "wifi";
        } else if (step.contains("mock_printer")) {
            app.useMockPrinter(step["mock_printer"].is_string() && step["mock_printer"].get<std::string>() == "silent");
        } else if (step.contains("search")) {
            app.findPrinters();
        } else if (step.contains("printer")) {
            app.send().chosen = step["printer"].get<int>() - 1;
        } else if (step.contains("start_print")) {
            app.send().start_print = step["start_print"].get<bool>();
        } else if (step.contains("send")) {
            if (step["send"].get<std::string>() == "close") app.closeSend();
            else app.sendToPrinter();
        } else if (step.contains("saved")) {
            if (step["saved"].get<std::string>() != "back") throw std::runtime_error("unknown saved step");
            app.dismissSaved();
        } else if (step.contains("shortcuts")) {
            app.showShortcuts(step["shortcuts"].get<bool>());
        } else if (step.contains("cut_plane") || step.contains("cut_keep") || step.contains("cut_apply")) {
            // The Cut tool: the plane ["z" | "x" | "y", mm], what to keep, and applying it (then "wait": "jobs").
            CutSettings& c = app.shape().cut;
            if (step.contains("cut_plane")) {
                const auto& a = step["cut_plane"];
                const std::string axis = a[0].get<std::string>();
                if (axis != "z" && axis != "x" && axis != "y") throw std::runtime_error("unknown cut axis " + axis);
                c.axis = axis == "z" ? CutAxis::Z : axis == "x" ? CutAxis::X : CutAxis::Y;
                if (app.document()) cutFit(c, app.document()->part());
                c.position = a[1].get<double>();
            } else if (step.contains("cut_keep")) {
                const std::string k = step["cut_keep"].get<std::string>();
                if (k != "both" && k != "above" && k != "below") throw std::runtime_error("unknown cut_keep " + k);
                c.keep = k == "both" ? CutKeep::Both : k == "above" ? CutKeep::Above : CutKeep::Below;
            } else {
                app.cutApply();
            }
        } else if (step.contains("label_set") || step.contains("label_at") || step.contains("label_apply")) {
            // The Label tool: its fields, a click on the part along a ray (printer coordinates), and applying it.
            LabelSettings& l = app.shape().label;
            if (step.contains("label_set")) {
                const auto& a = step["label_set"];
                if (a.contains("text")) l.text = a["text"].get<std::string>();
                if (a.contains("height")) l.height = a["height"].get<double>();
                if (a.contains("depth")) l.depth = a["depth"].get<double>();
                if (a.contains("engrave")) l.engrave = a["engrave"].get<bool>();
            } else if (step.contains("label_at")) {
                const auto& a = step["label_at"];
                fruit::Vec3 at, n;
                if (!app.pickModel({a[0].get<double>(), a[1].get<double>(), a[2].get<double>()},
                                   {a[3].get<double>(), a[4].get<double>(), a[5].get<double>()}, &at, &n))
                    throw std::runtime_error("label_at: meets no part");
                app.labelPick(at, n);
            } else {
                app.labelApply();
            }
        } else if (step.contains("combine_with") || step.contains("combine_mode") || step.contains("combine_apply")) {
            // The Combine tool: the second part (1-based, in the Parts list's order), the mode, and applying it.
            if (step.contains("combine_with")) {
                const int i = step["combine_with"].get<int>() - 1;
                const auto& doc = app.document();
                if (!doc || i < 0 || i >= static_cast<int>(doc->partCount())) throw std::runtime_error("combine_with: no such part");
                app.combinePick(doc->parts()[static_cast<std::size_t>(i)].id());
            } else if (step.contains("combine_mode")) {
                const std::string m = step["combine_mode"].get<std::string>();
                if (m != "join" && m != "subtract" && m != "overlap") throw std::runtime_error("unknown combine_mode " + m);
                app.shape().combine.mode = m == "join" ? CombineMode::Join : m == "subtract" ? CombineMode::Subtract : CombineMode::Overlap;
            } else {
                app.combineApply();
            }
        } else if (step.contains("busy")) {
            // A stand-in for slow work, to see its progress card: it runs, 40% done, until {"cancel_job": true}.
            app.jobs().start(
                step["busy"].get<std::string>(),
                [](const fruit::JobControl& ctl) {
                    ctl.report("", 0.4);
                    while (!ctl.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    ctl.check();
                },
                [](std::exception_ptr) {});
        } else if (step.contains("cancel_job")) {
            app.jobs().cancel();
            app.jobs().wait();
        } else if (step.contains("wait")) {
            app.jobs().wait();
        } else if (step.contains("view")) {
            const std::string v = step["view"].get<std::string>();
            if (v == "top") {  // from above, the whole plate's job in view
                app.camera().lookFrom({0, -0.18f, 1});  // as steep as the camera goes, the plate square on
                app.fitView();
            } else {
                app.setView(viewFrom(v));
            }
        } else if (step.contains("show_patch")) {
            // As the Model step's "Show the patch" link.
            const auto& doc = app.document();
            bool patched = false;
            if (doc)
                for (const Part& part : doc->parts()) patched = patched || part.check().repair.patch_faces > 0;
            if (!patched) throw std::runtime_error("show_patch: nothing was patched");
            app.showPatch();
        } else if (step.contains("frames")) {
            for (int i = 0, n = step["frames"].get<int>(); i < n; ++i) render();
        } else if (step.contains("shot")) {
            const std::string name = step["shot"].get<std::string>();
            // The Collisions check the app starts by itself with several parts
            // is let finish, so the picture doesn't depend on how fast it was.
            app.settleCollisions();
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
