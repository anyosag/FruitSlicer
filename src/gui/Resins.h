// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The resin library (canvas board "Resins") without the UI: each resin's
// print settings (from a SatelLite file, or typed) and the support values
// measured for it (a `fruitslicer fit` profile). A job keeps a copy of its
// resin's values, so changing the resin later never changes a saved job.

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Plates.h"
#include "Print.h"
#include "formats/GooFile.h"
#include "printer/PrinterProfile.h"

namespace gui {

struct Resin {
    std::string id;    // its file's name in the resins folder, without ".json"
    std::string name;  // as shown
    // Print settings: a SatelLite file's header kept in FruitSlicer's own
    // folder, and values of your own over it (or, without one, over the
    // printer's own and FruitSlicer's defaults). A new file clears them.
    std::string sample_path, sample_name;
    PrintEdits print;
    // Support values measured for this resin (a fitted profile), and under
    // kResinOwnKey the ones set on the Resins page; null: FruitSlicer's own
    // starting values.
    nlohmann::json profile;
    // The test plates saved for it, by kind ("tips", "spacing", "islands").
    std::map<std::string, PlateFiles> plates;
    bool operator==(const Resin&) const = default;
};

// The library's order: by name, ignoring case.
bool resinOrder(const Resin& a, const Resin& b);
// Every resin in `dir`, by name. Files that don't read are left out.
std::vector<Resin> loadResins(const std::string& dir);
void saveResin(const std::string& dir, const Resin& r);  // <dir>/<id>.json
void removeResin(const std::string& dir, const std::string& id);
// An id for a new resin called `name` ("my-resin", then "my-resin-2", …).
std::string newResinId(const std::vector<Resin>& resins, const std::string& name);
// A name not taken yet: `name`, else "name (copy)", "name (copy 2)", …
std::string freeResinName(const std::vector<Resin>& resins, const std::string& name);

// The resins on the first launch that has them: "My resin", with the print
// settings first-run setup kept in `settings_file` (an earlier version's
// "print_defaults"), if any. Saved to `dir`; nothing happens when `dir`
// already has resins.
std::vector<Resin> firstResins(const std::string& dir, const std::string& settings_file);

// A profile written by `fruitslicer fit` (or exported from here), as a new
// resin. Throws std::runtime_error with a readable reason.
Resin resinFromProfileFile(const std::string& json_path, const std::vector<Resin>& existing);

// The print settings a job on this resin starts with. A kept header that has
// gone or no longer reads is left out; the Print step then asks for one.
PrintSettings printSettingsFor(const Resin& r);
// Where its print settings come from, in a few words.
std::string printSummary(const Resin& r);
// For the list: "0.03 mm · 1.5 s · defaults" (`sample` is its header, if read).
std::string resinSummary(const Resin& r, const fruit::GooHeader* sample);
// "Defaults", or which plates were measured: "Measured: tips, spacing".
std::string resinState(const Resin& r);
// Whether a fit of that plate ("tips", "spacing", "islands") is in `profile`.
bool plateMeasured(const nlohmann::json& profile, const std::string& kind);
// What to do next with a resin's test plates: for the first, in order, not
// done yet (fully scored, or fitted), print it or score it; once all are,
// fit any scores not in its values yet.
struct PlateStep {
    enum class Action { Print, Score, Fit, None };
    Action action = Action::None;
    std::string kind;  // the plate to print or score
    std::string text;  // for the Calibrate page's Next card
};
PlateStep nextPlateStep(const Resin& r);
std::string profileState(const nlohmann::json& profile);
// A job's resin as its chip shows it: "My resin · Defaults".
std::string resinLabel(const std::string& name, const nlohmann::json& profile);

// One print setting as the Resins page lists and edits it.
struct PrintField {
    std::string key;      // as setPrintValue takes it
    const char* suffix;   // after the field ("s", "mm")
    double lo, hi;        // allowed range
    double value;         // in use; 0 for an exposure not set yet
};
struct PrintRow {
    enum class Source { File, Default, Yours };
    std::string key;      // the row, as resetPrintRow takes it
    std::string label;
    std::string value;    // in use, as text
    std::string base;     // the file's value (or FruitSlicer's own), as text; "" when there is none
    Source source = Source::Default;
    std::string note;     // what hasn't been checked about an edited value
    std::vector<PrintField> fields;
    std::string after;    // words after the fields (the transition layers' exposures)
};
// Every print setting, in page order: layer height, bottom layers, height
// correction (with no raft), transition layers, normal exposure, waits,
// bottom-layer waits, light power, density. `sample` is
// the resin's kept header, if read.
std::vector<PrintRow> printRows(const Resin& r, const fruit::GooHeader* sample, const fruit::PrinterProfile& printer);
// Sets one of the resin's own print values (clamped to its field's range).
// A value equal to the file's (or FruitSlicer's own) is not kept: the row
// reads as it did before.
void setPrintValue(Resin& r, const fruit::GooHeader* sample, const fruit::PrinterProfile& printer,
                   const std::string& key, double value);
// A row's values back to the file's (or FruitSlicer's own).
void resetPrintRow(PrintEdits& e, const std::string& row);

// The support values the Resins page lists, and where each comes from.
struct ResinValue {
    enum class Source { Default, Borrowed, Measured, Yours };
    std::string label, value;
    Source source = Source::Default;
};
// The advanced rows the page lists, by label, in order.
inline constexpr const char* kResinSupportRows[] = {"Usual tip size",  "How much a tip holds", "Tip reach",
                                                    "Faces that need tips", "Spots in mid-air", "Contact depth",
                                                    "Snap-off zone"};
std::vector<ResinValue> resinSupportValues(const Resin& r);

}  // namespace gui
