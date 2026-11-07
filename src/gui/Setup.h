// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// First-run setup (canvas board "Setup") without the UI: what a SatelLite
// file's settings say, and keeping its header for a resin's print settings.

#include <string>
#include <vector>

#include "Calibration.h"
#include "Print.h"
#include "formats/GooFile.h"

namespace gui {

// "Found in the file": one of a SatelLite header's settings in words.
struct SampleFact {
    std::string label, value, note;
};
std::vector<SampleFact> sampleFacts(const fruit::GooHeader& h);

// Reads a SatelLite file (refusing one made for another printer, as
// loadSample does) and keeps a copy of its header in `dir`, under the
// file's own name; returns the copy's path. Throws std::runtime_error with
// a readable reason.
std::string keepSampleHeader(const std::string& goo, const std::string& dir);

}  // namespace gui
