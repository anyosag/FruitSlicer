// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The A/B test (canvas boards "ABSetup" and "ABResults") without the UI:
// copies of one model on one plate, half with FruitSlicer's supports (A) and
// half bare for SatelLite's auto-supports (B), and what FruitSlicer keeps of
// the test it set up last.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "calibration/ABPlate.h"

namespace gui {

// The test set up last: what was tested, and where its files went. Its
// manifest and score sheet are kept in FruitSlicer's own folder.
struct ABRecord {
    std::string model, resin;     // what was tested, in which resin
    std::string file_a, file_b;   // the two STLs, where they were saved
    int copies = 0, scored = 0;
    bool operator==(const ABRecord&) const = default;
};
ABRecord loadABRecord(const std::string& file);  // empty if there is none
void saveABRecord(const std::string& file, const ABRecord& r);

// "x_A.stl" gives "x_B.stl"; any other name gets "_B" before ".stl".
std::string abFileB(const std::string& file_a);

// Keeps an A/B plate's manifest and a blank blind score sheet. A sheet with
// scores already there is set aside first (as "<name>.earlier.results.csv",
// with its manifest), never overwritten. Throws std::runtime_error.
void keepABTest(const nlohmann::json& manifest, const std::string& manifest_path, const std::string& results_path);

// The copies to offer by default: an even number (so the arms match), at
// most 8 and at most `capacity`; 0 when fewer than 2 fit.
int defaultABCopies(int capacity);

// What the results say, in words. A side "did better" only where chance is
// an unlikely explanation (p < 0.05), and never against the other side
// doing better at something else.
struct ABVerdict {
    enum class Winner { NotYet, None, A, B, Mixed };
    Winner winner = Winner::NotYet;
    std::string headline, detail, caveat;
};
ABVerdict abVerdict(const fruit::ABSummary& s);

// The two sides side by side: stayed on, average mark, clean-up, tips.
struct ABTile {
    std::string label, a, b;
};
std::vector<ABTile> abTiles(const fruit::ABSummary& s, int tips_per_copy);

}  // namespace gui
