// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Slicing settings (canvas "Slicing settings"): every setting a slice and its
// print file use, where each value comes from, and what has been checked and
// what hasn't. And the Layer height page: what each layer height takes in
// light and time, and what it changes in the print.
//
// Without the UI, so the tests can check the words and numbers.

#include <cstddef>
#include <string>
#include <vector>

#include "Print.h"

namespace fruit {
class TriangleMesh;
}

namespace gui {

class Document;
struct PrinterSetup;
struct Resin;
struct TestedLight;

// Where a value comes from, or what is known of it.
enum class ValueFrom {
    File,        // the SatelLite file's
    Yours,       // set by you, for the resin or this job
    Default,     // FruitSlicer's own
    Printer,     // fixed by the printer
    Checked,     // checked, and fine
    NotChecked,  // not checked yet
    Missing,     // needed before saving, not there
    OutOfDate,   // made for something that has changed since
};
const char* valueFromWord(ValueFrom f);

struct SettingRow {
    std::string label, value, help;
    ValueFrom from = ValueFrom::Default;
    std::string was;  // its value when the layers were cut, when it differs ("" otherwise)
};

struct SettingGroup {
    std::string title;
    std::string link;  // where its values are changed ("Settings", "Resins", "Change for this job", "Supports")
    std::vector<SettingRow> rows;
};

struct SliceCheck {
    int state = 0;  // 1 checked and fine, -1 checked and failed, 0 not checked
    std::string text;
};

struct SliceSettingsView {
    std::vector<SettingGroup> groups;  // Printer, Resin, Layers and light, Model supports and raft
    std::vector<SliceCheck> checked;   // checked: fine (1) or failed (-1)
    std::vector<SliceCheck> not_checked;
    // Slicing can't start yet: why, and the button that fixes it ("" for none).
    std::string waits_for, fix;
    // Something missing that stops saving, not slicing.
    std::string missing;
};

// The view of `doc`'s settings. `resin`: the job's resin in the library (null
// when it is not there any more). `cut_with`: the print settings the layers
// were cut with, to mark what changed since (null: nothing is marked).
SliceSettingsView sliceSettingsView(const Document& doc, const PrinterSetup& setup, const Resin* resin,
                                    const PrintSettings* cut_with = nullptr);

// One line for the Job panel: "My resin · 0.03 mm layers · 1.5 s".
std::string slicingLine(const Document& doc);

// "1 h 02 min", "37 min".
std::string hoursMinutes(double seconds);

// ---- Light by layer height ----

// What is known of the light a layer height needs.
struct LightAt {
    enum class Kind { Tested, Modeled, Unknown };
    Kind kind = Kind::Unknown;
    double exposure = 0;  // s; 0 when unknown
    std::string note;     // where a tested one comes from
};

// Every tested point for this job: its SatelLite file's own (its layer height
// and normal exposure, "your SatelLite file"), then the resin's, by height.
// One point per height: the file's wins.
std::vector<TestedLight> testedLight(const PrintSettings& s, const Resin* resin);

// The exposure each height needs, modeled as t = t₁ · e^((h − h₁)/Dp): the
// usual rule that the light needed grows exponentially with the layer's
// thickness. It goes through the thinnest and the thickest tested points
// (when the thicker one needs more light); with fewer than two such points
// there is no curve.
struct LightCurve {
    bool ok = false;
    double h1 = 0, t1 = 0, dp = 0;  // mm, s, mm
    double at(double h) const;
};
LightCurve lightCurve(const std::vector<TestedLight>& tested);

// Tested when a tested point is at `h` (to 1 µm), else modeled on the curve,
// else unknown.
LightAt lightAt(const std::vector<TestedLight>& tested, double h);

// ---- What the layer height changes ----

// The placed model's surfaces, by what a layer height does to them.
struct PrintShape {
    double top = 0;                  // the print's top, supports and raft included, mm
    double area = 0;                 // the model's surface, mm²
    double sloped = 0, walls = 0, flat = 0;  // of it: sloped, vertical, flat (mm²)
    double steepest = 0;             // the largest |n·z| of the sloped faces: the deepest steps are h × this
    double sloped_lo = 0, sloped_hi = 0;  // where the sloped faces are, mm up
    std::vector<double> flat_levels;      // the heights of the flat faces, mm (each at least 0.5 mm²)
};
PrintShape printShape(const fruit::TriangleMesh& placed, double top);

// The layer heights the page compares, thinnest first.
const std::vector<double>& layerHeightChoices();

struct HeightChoice {
    double h = 0;               // mm
    std::size_t layers = 0;     // in the print file, the height correction's included
    LightAt light;
    double seconds = 0;         // the print's time with that light (the current one when unknown)
    double step_um = 0;         // the deepest stair step on a sloped face, µm
    int flat_off = 0;           // flat faces that fall between two layers (by more than 5 µm)
    double flat_off_um = 0;     // the most any of them is off, µm
    int long_layers = 0;        // bottom + transition layers
    double long_mm = 0;         // how high those reach, mm
    double raft = 0;            // the raft's thickness, mm (0: the parts stand on the plate)
    bool raft_thin = false;     // the long-exposure layers reach past the raft
    double held_mm = 0;         // on the plate: how much of the part the long exposures print, mm
    bool supports_change = false;  // on the plate: the supports would have to be made again
};

// The print's layer height now: the job's own, else its SatelLite file's, else the printer's.
double printLayerHeight(const Document& doc);
HeightChoice heightChoice(const Document& doc, const PrintShape& shape, const std::vector<TestedLight>& tested,
                          double h);

// The print settings with `h` mm layers and the light it needs: the job's own
// values for both, unless they are the SatelLite file's (then cleared, so
// the row reads "SatelLite file" again). An unknown light keeps the exposure.
PrintSettings withLayerHeight(const PrintSettings& s, double h, const LightAt& light);

}  // namespace gui
