// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Printer-wide settings and the image orientation check (canvas boards
// "Printer" and "OrientCheck"). No UI here.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/JobControl.h"
#include "formats/GooCompare.h"
#include "printer/PrinterProfile.h"
#include "support/SupportSettings.h"

namespace gui {

// What FruitSlicer keeps about the printer between launches.
struct PrinterSetup {
    // The image orientation check: whether it confirmed FruitSlicer's
    // orientation, and the flips every print file gets from then on
    // (relative to FruitSlicer's default, as `goo --flip-x/--flip-y`).
    bool orientation_confirmed = false;
    bool flip_x = false, flip_y = false;
    std::string checked_with;  // the SatelLite file the check used (its name)
    // Side the tilting vat hinges on, in plate coordinates. Extra hold on the
    // far side applies only once the orientation is confirmed.
    std::optional<fruit::HingeSide> hinge;
    // First-run setup has been through (Settings can run it again).
    bool setup_done = false;
    // The print settings each new job starts with (setup's second step):
    // a SatelLite file's header, kept in FruitSlicer's own folder so moving
    // the file loses nothing, or exposures typed in (> 0 to apply; layer
    // counts -1 for the printer's own).
    std::string sample_path;  // the kept copy
    std::string sample_name;  // the SatelLite file it came from
    double exposure = 0, bottom_exposure = 0;
    int bottom_layers = -1, transition_layers = -1;
    bool operator==(const PrinterSetup&) const = default;
};

// Missing or unreadable files read as the defaults; writing creates the folder.
PrinterSetup loadPrinterSetup(const std::string& file);
void savePrinterSetup(const std::string& file, const PrinterSetup& s);

// The hinge as people pick it, looking at the printer from the front.
enum class HingeChoice { Back, Left, Right, Front };
const char* hingeLabel(HingeChoice c);
// Plate side for each choice: left −X, right +X, back +Y, front −Y. This
// takes the plate's axes as SatelLite shows them, seen from the front: the
// reason the extra hold waits for a confirmed orientation. A wrong guess
// adds hold on the wrong side; it never removes any.
fruit::HingeSide plateSide(HingeChoice c);
std::optional<HingeChoice> hingeChoice(const std::optional<fruit::HingeSide>& s);

// One layer of a print file, around what is lit, at square cells.
struct LayerPicture {
    int w = 0, h = 0;
    std::vector<std::uint8_t> lit;  // coverage 0–255, top row first
    double cell_mm = 0;
};

struct OrientationCheck {
    std::string satellite_path;
    std::string software;  // e.g. "SatelLite 1.0.2.43", from its header
    fruit::GooCompareResult compare;
    fruit::OrientationVerdict verdict = fruit::OrientationVerdict::Undetermined;
    bool flip_x = false, flip_y = false;  // the fix the verdict means (none when undetermined)
    LayerPicture ours, theirs;            // the compared layers; ours with the fix applied
};

// The F test piece as an STL, standing where SatelLite should load it as is.
void saveOrientationPiece(const std::string& path);

// Slices the F with the SatelLite file's own settings and lines up the two
// images. Progress: "slice" then "compare". Throws with a readable message
// when the file can't be read or has no exposure times.
OrientationCheck checkOrientation(const std::string& satellite_goo, const fruit::PrinterProfile& printer,
                                  const fruit::JobControl& ctl);

// Scripts and tests: a stand-in for SatelLite's slice of the F, whose
// image is flipped as asked under a header with the printer's usual mirror
// flags, so the check finds exactly that flip.
void writeStandInSatelliteF(const std::string& path, bool flip_x, bool flip_y);

// The verdict in plain words: a short title and one or two sentences.
std::string verdictTitle(const OrientationCheck& c);
std::string verdictBody(const OrientationCheck& c);
// "As is", "Flipped left to right", ... for a GooOrientationScore transform.
std::string transformLabel(const std::string& transform);

}  // namespace gui
