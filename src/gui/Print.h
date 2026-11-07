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

// Where exposure and motion settings come from: a SatelLite .goo saved for
// this printer (its settings are copied), or exposure times typed in. The
// engine never guesses exposures.
struct PrintSettings {
    std::string sample_path;
    std::shared_ptr<const fruit::GooHeader> sample;
    double exposure = 0, bottom_exposure = 0;  // typed, s; > 0 overrides the sample
    int bottom_layers = -1, transition_layers = -1;
    // The image orientation fix the check confirmed (the app fills these in
    // from the printer's settings; they are not the job's).
    bool flip_x = false, flip_y = false;
};

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
    double height = 0;         // mm
    double resin_ml = 0, resin_g = 0;
    double print_time_s = 0;   // estimate
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
