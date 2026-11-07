// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The resin library (canvas board "Resins") without the UI: each resin's
// print settings (from a SatelLite file, or typed) and the support values
// measured for it (a `fruitslicer fit` profile). A job keeps a copy of its
// resin's values, so changing the resin later never changes a saved job.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Print.h"
#include "formats/GooFile.h"
#include "printer/PrinterProfile.h"

namespace gui {

struct Resin {
    std::string id;    // its file's name in the resins folder, without ".json"
    std::string name;  // as shown
    // Print settings: a SatelLite file's header kept in FruitSlicer's own
    // folder, or values typed in (> 0 to apply; layer counts -1 for the
    // printer's own).
    std::string sample_path, sample_name;
    double exposure = 0, bottom_exposure = 0;
    int bottom_layers = -1, transition_layers = -1;
    // Support values measured for this resin (a fitted profile); null:
    // FruitSlicer's own starting values.
    nlohmann::json profile;
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
std::string profileState(const nlohmann::json& profile);
// A job's resin as its chip shows it: "My resin · Defaults".
std::string resinLabel(const std::string& name, const nlohmann::json& profile);

// The print settings the Resins page lists: label and value.
std::vector<std::pair<std::string, std::string>> printRows(const Resin& r, const fruit::GooHeader* sample,
                                                           const fruit::PrinterProfile& printer);

// The support values the Resins page lists, and where each comes from.
struct ResinValue {
    enum class Source { Default, Borrowed, Measured };
    std::string label, value;
    Source source = Source::Default;
};
std::vector<ResinValue> resinSupportValues(const Resin& r);

}  // namespace gui
