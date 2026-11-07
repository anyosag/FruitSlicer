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
    fruit::exportGoo(fruit::makeBox({-2, -2, 0}, {2, 2, 0.3}), setup.header, setup.grid, fruit::GooExportOptions{}, p);
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
    if (s == "calibrate") return Screen::Calibrate;
    if (s == "orientation_check") return Screen::OrientationCheck;
    if (s == "printer") return Screen::Printer;
    if (s == "settings") return Screen::Settings;
    if (s == "setup") return Screen::Setup;
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
        } else if (step.contains("supports")) {
            const std::string what = step["supports"].get<std::string>();
            if (what != "generate") throw std::runtime_error("unknown supports step " + what);
            app.generateSupports();
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
