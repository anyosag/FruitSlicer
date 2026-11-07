// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "ABTest.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#include "Format.h"
#include "Plates.h"

namespace gui {

namespace fs = std::filesystem;
using nlohmann::json;

ABRecord loadABRecord(const std::string& file) {
    ABRecord r;
    std::ifstream in(file);
    if (!in) return r;
    try {
        const json j = json::parse(in);
        r.model = j.value("model", std::string());
        r.resin = j.value("resin", std::string());
        r.file_a = j.value("file_a", std::string());
        r.file_b = j.value("file_b", std::string());
        r.copies = j.value("copies", 0);
        r.scored = j.value("scored", 0);
    } catch (const std::exception&) {
        return ABRecord{};  // damaged: as if there were none
    }
    return r;
}

void saveABRecord(const std::string& file, const ABRecord& r) {
    const json j = {{"model", r.model},   {"resin", r.resin},   {"file_a", r.file_a},
                    {"file_b", r.file_b}, {"copies", r.copies}, {"scored", r.scored}};
    writeWhole(file, j.dump(2) + "\n");
}

std::string abFileB(const std::string& file_a) {
    const fs::path p(file_a);
    std::string stem = p.stem().string();
    if (stem.size() >= 2 && stem.compare(stem.size() - 2, 2, "_A") == 0) stem.resize(stem.size() - 2);
    return (p.parent_path() / (stem + "_B.stl")).string();
}

void keepABTest(const json& manifest, const std::string& manifest_path, const std::string& results_path) {
    std::error_code ec;
    if (fs::exists(results_path, ec)) {
        bool scored = true;  // an unreadable sheet may be someone's work: keep it
        try {
            const auto rs = fruit::loadABResults(manifest_path, results_path);
            scored = std::any_of(rs.begin(), rs.end(), [](const fruit::ABCopyResult& r) { return r.recorded; });
        } catch (const std::exception&) {
        }
        if (scored) setAsideEarlier(manifest_path, results_path);
    }
    writeWhole(manifest_path, manifest.dump(2) + "\n");
    writeWhole(results_path, fruit::blindABCsv(manifest, {}));
}

int defaultABCopies(int capacity) {
    if (capacity < 2) return 0;
    const int n = std::min(capacity, 8);
    return n - n % 2;
}

namespace {

int sign(double v) { return v > 1e-9 ? 1 : v < -1e-9 ? -1 : 0; }

std::string seconds(double s) { return strf("%.0f s", s); }

}  // namespace

ABVerdict abVerdict(const fruit::ABSummary& s) {
    ABVerdict v;
    const fruit::ABArmStats &a = s.a, &b = s.b;
    if (a.scored == 0 || b.scored == 0) {
        v.headline = "Score both sides to compare them.";
        return v;
    }
    struct Measure {
        int dir;  // +1: FruitSlicer's did better, -1: SatelLite's
        bool sure;
        double best;
    };
    std::vector<Measure> ms;
    const double rate_a = static_cast<double>(a.ok) / a.scored, rate_b = static_cast<double>(b.ok) / b.scored;
    ms.push_back({sign(rate_a - rate_b), s.p_ok < 0.05, s.best_p_ok});
    std::vector<std::string> facts;
    if (a.ok == a.scored && b.ok == b.scored) facts.push_back("Every copy stayed on either way.");
    else if (ms.back().dir == 0)
        facts.push_back(strf("Both stayed on as often: %d of %d and %d of %d.", a.ok, a.scored, b.ok, b.scored));
    else
        facts.push_back(strf("%s stayed on more often: %d of %d against %d of %d.",
                             ms.back().dir > 0 ? "FruitSlicer's" : "SatelLite's", ms.back().dir > 0 ? a.ok : b.ok,
                             ms.back().dir > 0 ? a.scored : b.scored, ms.back().dir > 0 ? b.ok : a.ok,
                             ms.back().dir > 0 ? b.scored : a.scored));
    if (a.marked > 0 && b.marked > 0) {
        ms.push_back({sign(b.mean_scar - a.mean_scar), s.p_scar < 0.05, s.best_p_scar});
        const int d = ms.back().dir;
        facts.push_back(d == 0 ? strf("Both left the same marks: %.1f.", a.mean_scar)
                               : strf("%s left smaller marks: %.1f against %.1f.", d > 0 ? "FruitSlicer's" : "SatelLite's",
                                      d > 0 ? a.mean_scar : b.mean_scar, d > 0 ? b.mean_scar : a.mean_scar));
    }
    if (a.timed > 0 && b.timed > 0) {
        ms.push_back({sign(b.mean_removal_s - a.mean_removal_s), s.p_removal < 0.05, s.best_p_removal});
        const int d = ms.back().dir;
        facts.push_back(d == 0 ? "Both took as long to clean up: " + seconds(a.mean_removal_s) + "."
                               : std::string(d > 0 ? "FruitSlicer's" : "SatelLite's") + " cleaned up faster: " +
                                     seconds(d > 0 ? a.mean_removal_s : b.mean_removal_s) + " against " +
                                     seconds(d > 0 ? b.mean_removal_s : a.mean_removal_s) + ".");
    }
    bool for_a = false, for_b = false, leaning = false, can_show = false;
    for (const Measure& m : ms) {
        for_a |= m.sure && m.dir > 0;
        for_b |= m.sure && m.dir < 0;
        leaning |= m.dir != 0;
        can_show |= m.best < 0.05;
    }
    using W = ABVerdict::Winner;
    if (for_a && !for_b) {
        v.winner = W::A;
        v.headline = "On this model, FruitSlicer's supports did better.";
    } else if (for_b && !for_a) {
        v.winner = W::B;
        v.headline = "On this model, SatelLite's supports did better.";
    } else if (for_a && for_b) {
        v.winner = W::Mixed;
        v.headline = "Mixed: each side did better at something.";
    } else {
        v.winner = W::None;
        v.headline = leaning ? "No clear winner yet." : "No difference between them on this model.";
    }
    for (std::size_t i = 0; i < facts.size(); ++i) v.detail += (i ? " " : "") + facts[i];
    if (v.winner == W::None && leaning) v.detail += " With this few copies, that could easily be chance.";
    const std::string sides = a.copies == b.copies ? strf("%d copies a side", a.copies)
                                                   : strf("%d and %d copies", a.copies, b.copies);
    if (!can_show)
        v.caveat = "With " + sides + ", even a clean sweep could be chance: print at least 4 a side for a difference "
                   "to be able to show.";
    else if (v.winner == W::None)
        v.caveat = "With " + sides + ", only a large difference can show; a smaller one may still be there.";
    else
        v.caveat = sides + " is a small test. Repeat it, or try another model, before trusting the difference.";
    v.caveat[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(v.caveat[0])));
    return v;
}

std::vector<ABTile> abTiles(const fruit::ABSummary& s, int tips_per_copy) {
    const fruit::ABArmStats &a = s.a, &b = s.b;
    auto stayed = [](const fruit::ABArmStats& x) { return x.scored ? strf("%d of %d", x.ok, x.scored) : std::string("—"); };
    auto mark = [](const fruit::ABArmStats& x) { return x.marked ? strf("%.1f", x.mean_scar) : std::string("—"); };
    auto clean = [](const fruit::ABArmStats& x) { return x.timed ? seconds(x.mean_removal_s) : std::string("—"); };
    return {{"Stayed on", stayed(a), stayed(b)},
            {"Average mark", mark(a), mark(b)},
            {"Clean-up per copy", clean(a), clean(b)},
            {"Tips per copy", tips_per_copy >= 0 ? std::to_string(tips_per_copy) : std::string("—"), "from SatelLite"}};
}

}  // namespace gui
