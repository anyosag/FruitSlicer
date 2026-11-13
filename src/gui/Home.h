// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Home's words (canvas boards "Home · redesign" and "Home · redesign, first
// run"): what has been checked before a print and what hasn't, what a saved
// job's card says, and where Continue takes the open job. No UI here.

#include <ctime>
#include <string>
#include <vector>

#include "Calibration.h"
#include "JobFile.h"
#include "Navigation.h"
#include "Resins.h"
#include "formats/GooFile.h"
#include "printer/PrinterProfile.h"

namespace gui {

class Document;

// One row of "Before you print".
struct SetupCheck {
    enum class State { Ok, Review, Note };
    enum class Go { None, Resins, OrientationCheck, Calibrate };
    State state = State::Note;
    std::string title, detail;
    std::string action;  // the link's words ("" none)
    Go go = Go::None;
};
struct SetupChecks {
    std::vector<SetupCheck> rows;  // the printer, print settings, image orientation, support values
    int checked = 0;               // rows that are Ok
    struct Plate {
        std::string label;  // "Tips"
        bool measured = false;
    };
    std::vector<Plate> plates;  // tips, spacing, islands: measured on the resin or not
};
// For the resin new jobs use; `sample` is its kept SatelLite header, if it read.
SetupChecks setupChecks(const fruit::PrinterProfile& printer, const PrinterSetup& setup, const Resin& resin,
                        const fruit::GooHeader* sample);

// Where the open job's Continue goes: the next thing its places need.
struct ContinueTo {
    std::string label;           // "Continue: cut the layers"
    Step step = Step::Model;
    bool make_supports = false;  // make them (or make them again) on the way
};
ContinueTo continueTo(const Document& d);

// The open job's places, as its card lists them: done, the next one, or later.
struct StepChip {
    enum class State { Done, Next, Later };
    std::string label, detail;  // "Supports", " · 75 tips"
    State state = State::Later;
};
std::vector<StepChip> stepChips(const Document& d);

// The open job in a line: "Not saved yet · 1 part · 30.0 × 20.0 × 24.0 mm · My resin · Defaults".
std::string openJobLine(const Document& d);
// Its supports, when it has some: "Supports are made: 75 tips, 2.1 ml." ("" none).
std::string openJobSupports(const Document& d);

// A saved job's card: its state with a capital ("Layers cut") and what it
// held when saved ("2 parts on the plate · 25 tips · 0.2 ml supports"; ""
// for a job remembered before these were kept).
std::string jobState(const RecentJob& j);
std::string jobFacts(const RecentJob& j);
// When it was saved, as `when` keeps it ("2026-11-12 15:32"), against `now`:
// "Today, 15:32", "Yesterday, 15:32", "11 Nov, 18:40", "3 Feb 2025, 09:10".
std::string whenWords(const std::string& when, std::time_t now);

}  // namespace gui
