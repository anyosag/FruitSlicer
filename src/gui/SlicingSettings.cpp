// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "SlicingSettings.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>

#include "Calibration.h"
#include "Document.h"
#include "Format.h"
#include "Hollows.h"
#include "Layers.h"
#include "Orient.h"
#include "RaftPresets.h"
#include "Resins.h"
#include "Supports.h"
#include "formats/GooExport.h"
#include "formats/GooSetup.h"
#include "layers/LayerStack.h"
#include "support/SupportSettings.h"
#include "mesh/TriangleMesh.h"

namespace gui {

namespace {

std::string trimmed(double v, const char* unit) { return strf("%g %s", std::round(v * 1000) / 1000, unit); }

std::string secondsText(double s) { return s > 0 ? strf("%g s", std::round(s * 100) / 100) : std::string(); }

// The header the print file would have with these settings.
fruit::GooHeader headerFor(const Document& doc, const PrintSettings& s) {
    return fruit::printHeader(doc.printer, printRequest(s, doc.printer.layer_height));
}

// The print settings rows' sources, as the Resins page has them.
std::map<std::string, PrintRow> rowsByKey(const Document& doc) {
    const PrintSettings& s = doc.print.settings;
    Resin r;
    r.sample_path = s.sample_path;
    r.print = s;
    std::map<std::string, PrintRow> out;
    for (PrintRow& row : printRows(r, s.sample.get(), doc.printer)) out[row.key] = std::move(row);
    return out;
}

ValueFrom fromRow(const PrintRow& r) {
    switch (r.source) {
        case PrintRow::Source::File: return ValueFrom::File;
        case PrintRow::Source::Yours: return ValueFrom::Yours;
        default: return ValueFrom::Default;
    }
}

std::string fileName(const std::string& path) { return std::filesystem::path(path).filename().string(); }

// The layers and light rows' values, from a header.
struct LightRows {
    std::string height, bottom, transition, exposure, power, waits, bottom_waits, lift;
};
LightRows lightRows(const fruit::GooHeader& h) {
    const fruit::GooPrintSettings& p = h.print;
    LightRows r;
    r.height = trimmed(fruit::gooLayerHeight(h), "mm");
    r.bottom = p.bottom_exposure > 0 ? strf("%u layers × %s", p.bottom_layers, secondsText(p.bottom_exposure).c_str())
                                     : strf("%u layers × no exposure yet", p.bottom_layers);
    if (p.transition_layers == 0) r.transition = "None";
    else if (p.bottom_exposure > 0 && p.exposure > 0)
        r.transition = strf("%u layers, %s down to %s", p.transition_layers, secondsText(p.bottom_exposure).c_str(),
                            secondsText(p.exposure).c_str());
    else r.transition = strf("%u layers", p.transition_layers);
    r.exposure = p.exposure > 0 ? secondsText(p.exposure) : "No exposure yet";
    r.power = p.pwm >= 255 ? std::string("255 of 255, full") : strf("%u of 255", p.pwm);
    r.waits = strf("%g s before lift · %g s after retract", p.wait_before_lift, p.wait_after_retract);
    r.bottom_waits = strf("%g s before lift · %g s after retract", p.bottom_wait_before_lift, p.bottom_wait_after_retract);
    r.lift = strf("%g mm", std::round(p.lift_distance * 1000) / 1000);
    return r;
}

// The job's resin by name ("My resin" when it has none).
std::string resinName(const Document& doc) {
    return doc.supports.choices.resin.empty() ? std::string("My resin") : doc.supports.choices.resin;
}

std::string orientationText(const PrinterSetup& s) {
    if (s.flip_x && s.flip_y) return "Flipped in X and Y";
    if (s.flip_x) return "Flipped in X";
    if (s.flip_y) return "Flipped in Y";
    return "No flip";
}

}  // namespace

const char* valueFromWord(ValueFrom f) {
    switch (f) {
        case ValueFrom::File: return "SatelLite file";
        case ValueFrom::Yours: return "Yours";
        case ValueFrom::Default: return "FruitSlicer's";
        case ValueFrom::Printer: return "Printer";
        case ValueFrom::Checked: return "Checked";
        case ValueFrom::NotChecked: return "Not checked";
        case ValueFrom::Missing: return "Missing";
        case ValueFrom::OutOfDate: return "Out of date";
    }
    return "";
}

std::string hoursMinutes(double seconds) {
    const long m = std::lround(std::max(0.0, seconds) / 60);
    if (m < 60) return strf("%ld min", m);
    return strf("%ld h %02ld min", m / 60, m % 60);
}

double printLayerHeight(const Document& doc) {
    const PrintSettings& s = doc.print.settings;
    if (s.layer_height > 0) return s.layer_height;
    if (s.sample) return fruit::gooLayerHeight(*s.sample);
    return doc.printer.layer_height;
}

std::string slicingLine(const Document& doc) {
    const fruit::GooHeader h = headerFor(doc, doc.print.settings);
    const std::string resin = resinName(doc);
    const std::string light = h.print.exposure > 0 ? secondsText(h.print.exposure) : std::string("no exposure yet");
    return strf("%s · %s layers · %s", resin.c_str(), trimmed(fruit::gooLayerHeight(h), "mm").c_str(), light.c_str());
}

// ---- Light ----

double LightCurve::at(double h) const { return ok ? t1 * std::exp((h - h1) / dp) : 0; }

std::vector<TestedLight> testedLight(const PrintSettings& s, const Resin* resin) {
    std::vector<TestedLight> out;
    auto has = [&](double h) {
        return std::any_of(out.begin(), out.end(), [&](const TestedLight& t) { return std::abs(t.layer_height - h) < 1e-6; });
    };
    if (s.sample && s.sample->print.exposure > 0)
        out.push_back({fruit::gooLayerHeight(*s.sample), s.sample->print.exposure, "your SatelLite file"});
    if (resin)
        for (const TestedLight& t : resin->tested_light)
            if (t.layer_height > 0 && t.exposure > 0 && !has(t.layer_height)) out.push_back(t);
    std::sort(out.begin(), out.end(), [](const TestedLight& a, const TestedLight& b) { return a.layer_height < b.layer_height; });
    return out;
}

LightCurve lightCurve(const std::vector<TestedLight>& tested) {
    LightCurve c;
    if (tested.size() < 2) return c;
    const TestedLight& a = tested.front();
    const TestedLight& b = tested.back();
    if (!(b.layer_height > a.layer_height + 1e-9) || !(b.exposure > a.exposure)) return c;
    c.ok = true;
    c.h1 = a.layer_height;
    c.t1 = a.exposure;
    c.dp = (b.layer_height - a.layer_height) / std::log(b.exposure / a.exposure);
    return c;
}

LightAt lightAt(const std::vector<TestedLight>& tested, double h) {
    LightAt l;
    for (const TestedLight& t : tested)
        if (std::abs(t.layer_height - h) < 1e-6) {
            l.kind = LightAt::Kind::Tested;
            l.exposure = t.exposure;
            l.note = t.note;
            return l;
        }
    const LightCurve c = lightCurve(tested);
    if (c.ok) {
        l.kind = LightAt::Kind::Modeled;
        l.exposure = std::round(c.at(h) * 100) / 100;
    }
    return l;
}

// ---- Shape ----

PrintShape printShape(const fruit::TriangleMesh& m, double top) {
    PrintShape s;
    s.top = top;
    s.sloped_lo = 1e9;
    s.sloped_hi = -1e9;
    std::map<long, double> levels;  // µm → mm²
    for (const fruit::Face& f : m.faces) {
        const fruit::Vec3& a = m.vertices[static_cast<std::size_t>(f[0])];
        const fruit::Vec3& b = m.vertices[static_cast<std::size_t>(f[1])];
        const fruit::Vec3& c = m.vertices[static_cast<std::size_t>(f[2])];
        const fruit::Vec3 n = fruit::cross(b - a, c - a);
        const double len = fruit::norm(n);
        if (!(len > 0)) continue;
        const double area = len / 2, nz = std::abs(n.z) / len;
        s.area += area;
        if (nz > 0.9995) {
            s.flat += area;
            levels[std::lround((a.z + b.z + c.z) / 3 * 1000)] += area;
        } else if (nz < 0.05) {
            s.walls += area;
        } else {
            s.sloped += area;
            s.steepest = std::max(s.steepest, nz);
            const double z = (a.z + b.z + c.z) / 3;
            s.sloped_lo = std::min(s.sloped_lo, z);
            s.sloped_hi = std::max(s.sloped_hi, z);
        }
    }
    if (s.sloped <= 0) s.sloped_lo = s.sloped_hi = 0;
    for (const auto& [um, a] : levels)
        if (a >= 0.5) s.flat_levels.push_back(um / 1000.0);
    return s;
}

const std::vector<double>& layerHeightChoices() {
    static const std::vector<double> h = {0.01, 0.02, 0.03, 0.05, 0.08, 0.10};
    return h;
}

PrintSettings withLayerHeight(const PrintSettings& s, double h, const LightAt& light) {
    PrintSettings out = s;
    const bool file_height = s.sample && std::abs(fruit::gooLayerHeight(*s.sample) - h) < 1e-6;
    out.layer_height = file_height ? 0 : h;
    if (light.kind != LightAt::Kind::Unknown && light.exposure > 0) {
        const bool file_light = file_height && std::abs(s.sample->print.exposure - light.exposure) < 1e-6;
        out.exposure = file_light ? 0 : light.exposure;
    }
    return out;
}

HeightChoice heightChoice(const Document& doc, const PrintShape& shape, const std::vector<TestedLight>& tested,
                          double h) {
    HeightChoice c;
    c.h = h;
    c.light = lightAt(tested, h);
    const PrintSettings s = withLayerHeight(doc.print.settings, h, c.light);
    const fruit::GooHeader header = headerFor(doc, s);
    const bool on_plate = !doc.parts().empty() && doc.placement().on_plate;
    const int base = on_plate ? fruit::heightCorrectionLayers(heightCorrection(s), h) : 0;
    c.layers = static_cast<std::size_t>(std::max(0.0, std::ceil(shape.top / h - 1e-9))) + static_cast<std::size_t>(base);
    c.seconds = fruit::estimatePrintSeconds(header, static_cast<std::uint32_t>(c.layers), fruit::GooExportOptions{});
    c.step_um = h * shape.steepest * 1000;
    for (double z : shape.flat_levels) {
        const double off = std::abs(z - std::round(z / h) * h);
        if (off > 0.005) {
            ++c.flat_off;
            c.flat_off_um = std::max(c.flat_off_um, off * 1000);
        }
    }
    c.long_layers = static_cast<int>(header.print.bottom_layers + header.print.transition_layers);
    c.long_mm = c.long_layers * h;
    if (!on_plate && doc.supports.result) {
        c.raft = doc.supports.result->tree.raft_top;
        c.raft_thin = c.raft > 0 && c.long_mm > c.raft + 1e-9;
    }
    if (on_plate) {
        const int bottom = static_cast<int>(header.print.bottom_layers);
        c.held_mm = std::max(0, bottom - base) * h;
        if (doc.supports.result) c.supports_change = !doc.supports.result->heldLayersFit(plateHeldLayers(c.held_mm));
    }
    return c;
}

// ---- The settings view ----

SliceSettingsView sliceSettingsView(const Document& doc, const PrinterSetup& setup, const Resin* resin,
                                    const PrintSettings* cut_with) {
    SliceSettingsView v;
    const PrintSettings& s = doc.print.settings;
    const fruit::PrinterProfile& pr = doc.printer;
    const fruit::GooHeader h = headerFor(doc, s);
    const std::map<std::string, PrintRow> rows = rowsByKey(doc);
    const bool on_plate = !doc.parts().empty() && doc.placement().on_plate;
    const double lh = fruit::gooLayerHeight(h);
    const std::vector<TestedLight> tested = testedLight(s, resin);
    using F = ValueFrom;

    // Printer.
    {
        SettingGroup g{"PRINTER", "Settings", {}};
        g.rows.push_back({"Printer", pr.name, "", F::Printer, ""});
        g.rows.push_back({"Screen",
                          strf("%d × %d pixels, %.0f × %.0f µm each", pr.res_x, pr.res_y, pr.pixelX() * 1000, pr.pixelY() * 1000),
                          "Pure black and white, no anti-aliasing, as SatelLite writes.", F::Printer, ""});
        g.rows.push_back({"Build volume", strf("%.1f × %.1f mm plate, %.0f mm tall", pr.build_x, pr.build_y, pr.build_z), "",
                          F::Printer, ""});
        if (setup.orientation_confirmed)
            g.rows.push_back({"Image orientation", orientationText(setup),
                              "Checked against SatelLite's slice of the F, under Calibrate.", F::Checked, ""});
        else
            g.rows.push_back({"Image orientation", "A guess, as SatelLite writes",
                              "Not checked yet: lettering and left- or right-handed parts could print mirrored. The "
                              "check under Calibrate takes 5 minutes and no resin.",
                              F::NotChecked, ""});
        if (setup.hinge)
            g.rows.push_back({"Vat hinge", strf("On the %s side", fruit::hingeName(*setup.hinge)),
                              setup.orientation_confirmed
                                  ? "Supports hold more on the far side, where the vat pulls hardest."
                                  : "Used once the image orientation is checked.",
                              F::Yours, ""});
        else
            g.rows.push_back({"Vat hinge", "Not known", "Supports hold the same on every side.", F::NotChecked, ""});
        v.groups.push_back(std::move(g));
    }

    // Resin.
    {
        SettingGroup g{"RESIN", "Resins", {}};
        g.rows.push_back({"Resin", resinName(doc), "", F::Yours, ""});
        if (s.sample)
            g.rows.push_back({"Print settings from", fileName(s.sample_path),
                              "Saved by SatelLite for this printer. Your own values go over it.", F::File, ""});
        else if (h.print.exposure > 0)
            g.rows.push_back({"Print settings from", "Values you typed", "No SatelLite file: FruitSlicer's own values under them.",
                              F::Yours, ""});
        else
            g.rows.push_back({"Print settings from", "No SatelLite file yet",
                              "FruitSlicer never guesses exposures. Choose a .goo SatelLite saved for this printer, or "
                              "type them.",
                              F::Missing, ""});
        const auto d = rows.find("density");
        g.rows.push_back({"Density, for the weight", d != rows.end() ? strf("%g g/ml", resinDensity(s)) : "",
                          "", d != rows.end() ? fromRow(d->second) : F::Default, ""});
        v.groups.push_back(std::move(g));
    }

    // Layers and light.
    {
        SettingGroup g{"LAYERS AND LIGHT", "Change for this job", {}};
        const LightRows now = lightRows(h);
        LightRows was;
        if (cut_with) was = lightRows(headerFor(doc, *cut_with));
        auto from = [&](const char* key) {
            const auto r = rows.find(key);
            return r != rows.end() ? fromRow(r->second) : F::Default;
        };
        auto base = [&](const char* key) {
            const auto r = rows.find(key);
            return r != rows.end() ? r->second.base : std::string();
        };
        auto add = [&](const char* label, const std::string& value, const std::string& old, std::string help, F f) {
            SettingRow r{label, value, std::move(help), f, ""};
            if (cut_with && old != value) r.was = old;
            g.rows.push_back(std::move(r));
        };
        // The height: where the light for it comes from.
        const LightAt light = lightAt(tested, lh);
        std::string height_help;
        if (std::abs(lh - kLayerHeight) > 1e-6)
            height_help = strf("Layers shows them cut at %g mm, which is quicker. Saving cuts them again at %s.", kLayerHeight,
                               trimmed(lh, "mm").c_str());
        g.rows.push_back({"Layer height", now.height, height_help, from("layer_height"), ""});
        if (cut_with && was.height != now.height) g.rows.back().was = was.height;
        add("Bottom layers", now.bottom, was.bottom, "", h.print.bottom_exposure > 0 ? from("bottom") : F::Missing);
        add("Transition layers", now.transition, was.transition, "", from("transition"));
        std::string light_help;
        if (h.print.exposure <= 0)
            light_help = "Needed to save the print file. The layers can be cut and checked without it.";
        else if (light.kind == LightAt::Kind::Tested && std::abs(light.exposure - h.print.exposure) < 1e-6)
            light_help = strf("Tested with your resin at %s: %s.", trimmed(lh, "mm").c_str(), light.note.c_str());
        else if (light.kind == LightAt::Kind::Modeled && std::abs(light.exposure - h.print.exposure) < 1e-6)
            light_help = strf("Modeled from your tested times: not printed at %s yet.", trimmed(lh, "mm").c_str());
        else if (const auto r = rows.find("layer_height"); r != rows.end() && !r->second.note.empty())
            light_help = r->second.note;
        add("Normal exposure", now.exposure, was.exposure, light_help, h.print.exposure > 0 ? from("exposure") : F::Missing);
        add("Light power", now.power, was.power, "", from("light_power"));
        {
            const F f = from("waits");
            const std::string b = base("waits");
            add("Waits", now.waits, was.waits,
                f == F::Yours && !b.empty() && s.sample ? strf("The SatelLite file has %s.", b.c_str()) : std::string(), f);
        }
        {
            const F f = from("bottom_waits");
            std::string help = s.bottom_wait_before_lift < 0 ? "Before lift as the other layers." : "";
            if (f == F::Yours && s.sample && s.bottom_wait_after_retract >= 0)
                help += strf("%sThe SatelLite file has %g s after retract.", help.empty() ? "" : " ",
                             s.sample->print.bottom_wait_after_retract);
            add("Bottom-layer waits", now.bottom_waits, was.bottom_waits, help, f);
        }
        add("Lift and retract", now.lift + (s.sample ? ", as the file has" : ", a placeholder"), was.lift + (s.sample ? ", as the file has" : ", a placeholder"),
            "Not offered: the vat tilts by itself.", s.sample ? F::File : F::Default);
        v.groups.push_back(std::move(g));
    }

    // The model, its supports and raft.
    const bool fresh = doc.supports.fresh(doc.revision(), doc.supportSettings());
    const auto& sup = doc.supports.result;
    {
        SettingGroup g{"MODEL, SUPPORTS AND RAFT", "Supports", {}};
        const std::size_t parts = doc.partCount();
        g.rows.push_back({"Parts",
                          parts == 1 ? std::string(doc.fits() ? "1 part, on the plate" : "1 part, not on the plate yet")
                                     : strf("%zu parts%s", parts, doc.fits() ? ", all on the plate" : ", not all on the plate yet"),
                          "", doc.fits() ? F::Checked : F::Missing, ""});
        const SupportChoices& c = doc.supports.choices;
        if (!sup) {
            g.rows.push_back({"Supports", "None yet", "Slicing needs them.", F::Missing, ""});
        } else {
            const double tip = sup->cfg.tip_classes.empty()
                                   ? 0
                                   : sup->cfg.tip_classes[static_cast<std::size_t>(std::clamp(
                                                              sup->cfg.default_tip, 0, int(sup->cfg.tip_classes.size()) - 1))]
                                         .diameter;
            std::string value = strf("%s tips · %g mm", thousands(sup->tips).c_str(), tip);
            if (c.how_many != kHowManyCentre) value += strf(" · %s", howManyWord(c.how_many, c));
            std::string help;
            if (c.look_further) help += "Looks further for a way down. ";
            if (!c.advanced.empty())
                help += strf("%zu value%s of your own on the Advanced page. ", c.advanced.size(), c.advanced.size() == 1 ? "" : "s");
            help += strf("%.1f ml of resin.", sup->support_ml);
            if (!fresh) help = "Made before the last change to the parts or the settings. Remaking keeps these settings and your hand edits.";
            const bool own = c.how_many != kHowManyCentre || c.look_further || !c.advanced.empty();
            g.rows.push_back({"Supports", value, help, fresh ? (own ? F::Yours : F::Default) : F::OutOfDate, ""});
        }
        if (!on_plate) {
            const double thick = sup ? sup->tree.raft_top : c.raft.thickness;
            const std::string name = c.raft_preset.name.empty() ? std::string("This job's own raft") : c.raft_preset.name;
            g.rows.push_back({"Raft", strf("%s · %.1f mm thick", name.c_str(), thick), raftSummary(c.raft, thick),
                              name == fruit::kDefaultRaftPreset ? F::Default : F::Yours, ""});
            g.rows.push_back({"Height correction", "None: the parts stand on a raft",
                              strf("%g mm is added only under parts that stand on the plate.", heightCorrection(s)), F::Default,
                              ""});
        } else {
            const int n = fruit::heightCorrectionLayers(heightCorrection(s), lh);
            const auto r = rows.find("height_correction");
            g.rows.push_back({"Raft", "None: the parts stand on the plate", "", F::Yours, ""});
            g.rows.push_back({"Height correction", strf("%g mm, %d extra layer%s", n * lh, n, n == 1 ? "" : "s"),
                              "Added under the parts, because the printer squashes the first layers.",
                              r != rows.end() ? fromRow(r->second) : F::Default, ""});
        }
        {
            const HollowChoices& hc = doc.hollow.choices;
            std::string value, help;
            if (hc.hollow) {
                value = strf("Hollowed, %g mm walls", hc.wall);
                help = hc.auto_holes ? strf("%g mm drain and air holes in each hollow.", hc.hole_diameter)
                                     : "With the holes you placed.";
            } else if (hc.fix == HollowChoices::Fix::Holes) {
                value = "Solid";
                help = strf("A hollow that would hold resin gets %g mm drain holes.", hc.hole_diameter);
            } else if (hc.fix == HollowChoices::Fix::Fill) {
                value = "Solid, its hollows filled";
            } else {
                value = "Solid, its hollows left as they are";
                help = "A hollow that holds resin is listed before saving.";
            }
            g.rows.push_back({"Hollowing", value, help, hc == HollowChoices{} ? F::Default : F::Yours, ""});
        }
        g.rows.push_back({"Print file", ".goo, for the " + pr.name, "Layer count, resin and print time come from the slice.",
                          F::Printer, ""});
        v.groups.push_back(std::move(g));
    }

    // What has been checked, and what hasn't.
    if (!sup) v.checked.push_back({-1, "There are no supports yet"});
    else if (fresh) v.checked.push_back({1, "The supports were made for these parts where they stand, with these settings"});
    else v.checked.push_back({-1, "Failed: the supports match the parts where they stand now"});
    if (sup && !on_plate) {
        const int n = static_cast<int>(h.print.bottom_layers + h.print.transition_layers);
        const double need = n * lh, raft = sup->tree.raft_top;
        if (raft + 1e-9 >= need)
            v.checked.push_back({1, strf("The %.1f mm raft is thicker than the %d bottom and transition layers, %.2f mm", raft, n, need)});
        else
            v.checked.push_back({-1, strf("The %.1f mm raft is thinner than the %d bottom and transition layers, %.2f mm", raft, n, need)});
    }
    v.checked.push_back(doc.fits() ? SliceCheck{1, "Every part fits the plate"} : SliceCheck{-1, "A part reaches past the plate"});
    if (s.sample && h.print.exposure > 0) v.checked.push_back({1, "The SatelLite file still reads, and has exposure times"});
    if (setup.orientation_confirmed) v.checked.push_back({1, "Image orientation, against SatelLite's slice of the F"});

    if (!setup.orientation_confirmed) v.not_checked.push_back({0, "Which way the printer flips the layer images"});
    v.not_checked.push_back({0, strf("Spots that start in mid-air, and the pull on each layer: slicing checks these, on %g mm layers",
                                     kLayerHeight)});
    if (h.print.exposure <= 0) {
        v.not_checked.push_back({0, "Exposure times: there are none to check yet"});
    } else {
        const LightAt light = lightAt(tested, lh);
        const std::string resin_name = resinName(doc);
        if (!(light.kind == LightAt::Kind::Tested && std::abs(light.exposure - h.print.exposure) < 1e-6))
            v.not_checked.push_back({0, strf("Whether %s suits %s at %s: only a test print shows that",
                                             secondsText(h.print.exposure).c_str(), resin_name.c_str(), trimmed(lh, "mm").c_str())});
    }

    // What slicing waits for, and what only saving needs.
    if (!doc.fits()) {
        v.waits_for = "A part doesn't fit the plate yet. Bring it onto the plate with Move, Rotate or Scale first.";
    } else if (!sup) {
        v.waits_for = "Slicing needs supports. Nothing else has to come first.";
        v.fix = "Make supports";
    } else if (!fresh) {
        v.waits_for = "The supports were made before the last change, so layers cut now wouldn't match.";
        v.fix = "Remake supports";
    }
    if (h.print.exposure <= 0 || h.print.bottom_exposure <= 0)
        v.missing = "No exposure times yet: they don't stop slicing, only saving.";
    return v;
}

}  // namespace gui
