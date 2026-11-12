// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The Print step's work: the print file's settings, what saving would write,
// and saving it with a read-back check. No UI here.

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "core/JobControl.h"
#include "formats/GooExport.h"
#include "formats/GooFile.h"
#include "formats/GooSetup.h"
#include "geometry/BBox.h"
#include "mesh/TriangleMesh.h"
#include "printer/PrinterProfile.h"

namespace gui {

class Document;
struct LayerResult;

// Print settings of your own, over a sample's (or the printer's own and
// FruitSlicer's defaults): each applies when set. Waits, lift and light
// power apply to bottom and normal layers alike; "lift" sets the retract
// distance too. A resin keeps them; a job copies them with the rest.
struct PrintEdits {
    double exposure = 0, bottom_exposure = 0;        // s, > 0
    int bottom_layers = -1, transition_layers = -1;  // >= 0
    double layer_height = 0;                         // mm, > 0: over the sample's
    // After the light (before the vat tilts away) and before it (after the
    // vat is back). The file's "wait after lift" is not offered: the vat
    // tilts, there is no lift to wait after.
    double wait_before_lift = -1, wait_after_retract = -1;  // s, >= 0
    // The bottom layers' own waits, s, >= 0; < 0: as the other layers.
    double bottom_wait_before_lift = -1, bottom_wait_after_retract = -1;
    double lift = -1;                                // mm, > 0
    int light_power = -1;                            // 1-255
    double density = 0;                              // g/ml, > 0: for the resin's weight
    // mm, >= 0: added under a model standing on the plate (no raft), because
    // the printer squashes the first layers; < 0: FruitSlicer's typical 0.10.
    double height_correction = -1;
    // The thickest layer the print file may merge up to, as a multiple of the
    // layer height (1: off; 2, 4, 8), so it survives a change of layer height.
    int adaptive = 1;
    // mm, >= 0: how deep light hardens the resin, for the exposure of thicker
    // layers; < 0: FruitSlicer's typical 0.10.
    double light_depth = -1;
    bool operator==(const PrintEdits&) const = default;
    bool any() const { return !(*this == PrintEdits{}); }
};

// Where exposure and motion settings come from: a SatelLite .goo saved for
// this printer (its settings are copied), or values typed in (the edits,
// which override the sample's). The engine never guesses exposures.
struct PrintSettings : PrintEdits {
    std::string sample_path;
    std::shared_ptr<const fruit::GooHeader> sample;
    // The image orientation fix the check confirmed (the app fills these in
    // from the printer's settings; they are not the job's).
    bool flip_x = false, flip_y = false;
};

// The engine's request for these settings; `layer_height` is the job's
// (used without a sample or an edited one).
fruit::GooPrintRequest printRequest(const PrintSettings& s, double layer_height);
// The resin density for the weight: the edited one, else FruitSlicer's.
double resinDensity(const PrintEdits& e);
// The height correction, mm: the edited one, else FruitSlicer's typical value.
double heightCorrection(const PrintEdits& e);
// The light depth, mm: the edited one, else FruitSlicer's typical value.
double lightDepth(const PrintEdits& e);
// The thickest layer, in thin layers of `layer_height`, as the print file
// would use it (1: no merging): the setting held to the printer's 0.2 mm.
int adaptiveCount(const PrintEdits& e, double layer_height);
// Whole layers of `layer_height` added under `doc`'s models for the height
// correction of its print settings: none with a raft.
int baseLayers(const Document& doc, double layer_height);

// Reads a sample's header; throws std::runtime_error with a readable message.
std::shared_ptr<const fruit::GooHeader> loadSample(const std::string& path);

// Inside the printer's build volume (the plate is centred on the origin).
bool fitsPlate(const fruit::PrinterProfile& printer, const fruit::BBox3& b);

struct PrintPlan {
    bool ready = false;    // a print file can be written
    std::string why_not;   // otherwise, what is missing
    fruit::GooPrintSetup setup;
    double layer_height = 0;   // of the print file (the sample's, when there is one)
    bool reslice = false;      // differs from the job's layers: saving cuts them again
    std::size_t layers = 0;
    std::size_t base_layers = 0;  // of those, copies of the first layer (height correction)
    double height = 0;         // mm
    double resin_ml = 0, resin_g = 0;
    double density = 1.15;     // g/ml, for the weight (written in the file too)
    double print_time_s = 0;   // estimate
    int adaptive = 1;          // thickest layer in thin layers (1: no merging)
    double light_depth = 0.10; // mm, for the exposure of thicker layers
    double raft_needed = 0;    // bottom + transition layers' thickness
    bool raft_thick_enough = true;
    bool fits = true;          // model and supports inside the build volume
};

PrintPlan planPrint(const Document& doc, const LayerResult& layers, double raft_top, const PrintSettings& s);

struct SaveResult {
    std::string path;
    fruit::GooExportResult written;
    fruit::GooVerifyResult check;
    bool volume_matches = false;  // read back within 0.5 % of what was written
    bool good = false;            // every check passed: checksums, footer, layer count, volume, no grey
    double seconds = 0;
};

// Writes the print file (atomically; forced onto the drive when
// `removable`), then reads it back. Progress: writing to 0.85, then
// "verify". On failure or cancel no file is left at `path`.
SaveResult savePrint(const Document& doc, const LayerResult& layers, const fruit::TriangleMesh& supports,
                     const PrintPlan& plan, const std::string& path, bool removable, const fruit::JobControl& ctl);
// The same for a mesh already where it prints (a test plate).
SaveResult saveMeshPrint(const fruit::TriangleMesh& mesh, const fruit::GooPrintSetup& setup, const std::string& path,
                         bool removable, const fruit::JobControl& ctl);
// Reads back the print file at `r.path` and fills in the checks; a file
// that can't be read, or a cancel, leaves no file.
void readBack(SaveResult& r, const fruit::JobControl& ctl);

}  // namespace gui
