// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Plates.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

#include "Format.h"

namespace gui {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

double num(const json& p, const char* k) { return p.contains(k) && p[k].is_number() ? p[k].get<double>() : 0.0; }
std::string str(const json& p, const char* k) {
    return p.contains(k) && p[k].is_string() ? p[k].get<std::string>() : std::string();
}

// "0.2, 0.3, 0.4 and 0.6"
std::string listOf(const std::set<double>& v) {
    std::string out;
    std::size_t i = 0;
    for (double x : v) {
        if (i) out += i + 1 == v.size() ? " and " : ", ";
        out += strf("%g", x);
        ++i;
    }
    return out;
}

// The values one setting takes across the plate's coupons (of one kind, if given).
std::set<double> valuesOf(const fruit::CalibrationPlate& p, const char* key, const char* only_kind = nullptr) {
    std::set<double> out;
    for (const auto& c : p.coupons)
        if (!only_kind || str(c.params, "kind") == only_kind) out.insert(num(c.params, key));
    return out;
}

std::string times(int n) { return n == 1 ? "once" : n == 2 ? "twice" : strf("%d times", n); }

bool exists(const std::string& path) {
    std::error_code ec;
    return !path.empty() && fs::exists(path, ec);
}

std::string fileName(const std::string& path) { return fs::path(path).filename().string(); }

}  // namespace

// Written whole or not at all.
void writeWhole(const std::string& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        out.flush();
        if (!out) throw std::runtime_error("Couldn't write " + fileName(path) + ".");
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        throw std::runtime_error("Couldn't write " + fileName(path) + ": " + ec.message());
    }
}

namespace {

std::optional<json> readJson(const std::string& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;
    try {
        return json::parse(in);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// Anything there worth keeping (an unreadable sheet counts: it may be
// someone's work).
bool hasScores(const PlateFiles& f) {
    if (!exists(f.results)) return false;
    try {
        return loadScores(f).scored() > 0;
    } catch (const std::exception&) {
        return true;
    }
}

void setAside(const PlateFiles& f) { setAsideEarlier(f.manifest, f.results); }

}  // namespace

void setAsideEarlier(const std::string& manifest, const std::string& results) {
    const std::string suffix = ".json";
    if (manifest.size() <= suffix.size()) throw std::runtime_error("a manifest must end in .json");
    const std::string base = manifest.substr(0, manifest.size() - suffix.size());
    for (int n = 1;; ++n) {
        const std::string tag = n == 1 ? ".earlier" : ".earlier-" + std::to_string(n);
        const std::string m = base + tag + ".json", r = base + tag + ".results.csv";
        if (exists(m) || exists(r)) continue;
        std::error_code ec;
        fs::rename(results, r, ec);  // the scores first: they are what matters
        if (!ec && exists(manifest)) fs::rename(manifest, m, ec);
        if (ec) throw std::runtime_error("Couldn't set the earlier scores aside: " + ec.message());
        return;
    }
}

bool isPlateKind(const std::string& kind) {
    return std::any_of(std::begin(kPlateKinds), std::end(kPlateKinds), [&](const char* k) { return kind == k; });
}

fruit::CalibrationPlate makePlate(const std::string& kind, const fruit::PrinterProfile& printer) {
    if (kind == "tips") return fruit::tipsPlate(fruit::TipsPlateOptions{}, printer);
    if (kind == "spacing") return fruit::spacingPlate(fruit::SpacingPlateOptions{}, printer);
    if (kind == "islands") return fruit::islandsPlate(fruit::IslandsPlateOptions{}, printer);
    throw std::runtime_error("no plate called " + kind);
}

std::string plateTitle(const std::string& kind) {
    if (kind == "tips") return "Tips plate";
    if (kind == "spacing") return "Spacing plate";
    if (kind == "islands") return "Islands plate";
    return kind;
}

std::string plateSubtitle(const fruit::CalibrationPlate& p) {
    return strf("%zu coupons · %.0f mm tall", p.coupons.size(), p.mesh.bbox().max.z);
}

std::vector<std::string> plateTests(const fruit::CalibrationPlate& p) {
    const int reps = std::max(1, p.settings.value("replicates", 1));
    const std::size_t n = p.coupons.size(), per = static_cast<std::size_t>(reps);
    if (p.kind == "tips") {
        const std::size_t combos = n / per;
        return {"Tip sizes " + listOf(valuesOf(p, "tip_diameter")) + " mm",
                "Contact depths " + listOf(valuesOf(p, "penetration")) + " mm",
                "Loads of " + listOf(valuesOf(p, "load_per_tip_mm2")) + " mm² per tip",
                strf("Each combination %s: %zu × %d = %zu", times(reps).c_str(), combos, reps, n)};
    }
    if (p.kind == "spacing") {
        std::size_t plates = 0;
        for (const auto& c : p.coupons) plates += str(c.params, "kind") == "plate" ? 1 : 0;
        return {"Plates tilted " + listOf(valuesOf(p, "angle_deg", "plate")) + "° from flat",
                "On tips " + listOf(valuesOf(p, "spacing_mm", "plate")) + " mm apart",
                "Wedges with one side leaning " + listOf(valuesOf(p, "angle_deg", "wedge")) +
                    "° from upright, and no tips under that side",
                strf("Each %s: %zu plates and %zu wedges", times(reps).c_str(), plates / per, (n - plates) / per)};
    }
    if (p.kind == "islands") {
        const std::set<double> tips = valuesOf(p, "tip_diameter");
        const double biggest = tips.empty() ? 0 : *tips.rbegin();
        return {"Spots " + listOf(valuesOf(p, "island_diameter")) + " mm across, starting in mid-air",
                "On " + listOf(valuesOf(p, "tips")) + " tips",
                tips.size() > 1 ? strf("Tips %g mm, smaller where a spot is too small for them", biggest)
                                : strf("Tips %g mm", biggest),
                strf("Each combination %s: %zu × %d = %zu", times(reps).c_str(), n / per, reps, n)};
    }
    return {};
}

PlateLayout layoutOf(const fruit::CalibrationPlate& p) {
    PlateLayout l;
    l.rows = p.rows;
    l.cols = p.cols;
    l.cell_mm = p.cell;
    for (const auto& c : p.coupons) l.cells.push_back({c.id, c.row, c.col});
    return l;
}

PlateLayout layoutOf(const json& m) {
    PlateLayout l;
    l.rows = m.at("grid").at("rows").get<int>();
    l.cols = m.at("grid").at("cols").get<int>();
    l.cell_mm = m.at("grid").value("cell_mm", 0.0);
    for (const auto& c : m.at("coupons")) {
        PlateLayout::Cell cell{c.at("id").get<int>(), c.at("row").get<int>(), c.at("col").get<int>()};
        if (cell.row < 0 || cell.row >= l.rows || cell.col < 0 || cell.col >= l.cols)
            throw std::runtime_error("coupon " + std::to_string(cell.id) + " lies outside the plate's grid");
        l.cells.push_back(cell);
    }
    std::sort(l.cells.begin(), l.cells.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    return l;
}

std::vector<CouponFact> couponFacts(const std::string& kind, const json& p) {
    if (kind == "tips")
        return {{"Tip size", strf("%g mm", num(p, "tip_diameter"))},
                {"Contact depth", strf("%g mm", num(p, "penetration"))},
                {"Load per tip", strf("%g mm²", num(p, "load_per_tip_mm2"))},
                {"Disc held", strf("%g mm² on %g tips", num(p, "disc_area_mm2"), num(p, "tips"))}};
    if (kind == "spacing") {
        if (str(p, "kind") == "wedge")
            return {{"Wedge, one side leaning", strf("%g° from upright", num(p, "angle_deg"))},
                    {"Tips under that side", "None"}};
        return {{"Plate, tilted", strf("%g° from flat", num(p, "angle_deg"))},
                {"Tips apart", strf("%g mm", num(p, "spacing_mm"))}};
    }
    if (kind == "islands")
        return {{"Spot across", strf("%g mm", num(p, "island_diameter"))},
                {"Tips", strf("%g", num(p, "tips"))},
                {"Tip size", strf("%g mm", num(p, "tip_diameter"))}};
    return {};
}

std::string couponMeaning(const std::string& kind, const json& p) {
    if (kind == "tips")
        return strf("A flat disc hangs from %g tips, %g mm² of it for each. ", num(p, "tips"),
                    num(p, "load_per_tip_mm2")) +
               "If it stays on, tips like these can hold that much layer area in your resin.";
    if (kind == "spacing") {
        if (str(p, "kind") == "wedge")
            return strf("A wedge whose side leans %g° from upright, with nothing under that side. ",
                        num(p, "angle_deg")) +
                   "If that side prints cleanly, faces leaning this far or less need no tips.";
        return strf("A plate tilted %g° from flat, on tips %g mm apart. ", num(p, "angle_deg"), num(p, "spacing_mm")) +
               "If it prints without sagging between them, tips can be that far apart on faces tilted at least "
               "this much.";
    }
    if (kind == "islands") {
        const double k = num(p, "tips");
        return strf("A column %g mm across that starts in mid-air, on %g tip%s. ", num(p, "island_diameter"), k,
                    k == 1 ? "" : "s") +
               "If it prints, spots that size need no more tips than that.";
    }
    return "";
}

bool samePlate(const json& a, const json& b) {
    try {
        if (a.at("kind") != b.at("kind") || a.value("settings", json()) != b.value("settings", json())) return false;
        if (a.at("grid").at("rows") != b.at("grid").at("rows") || a.at("grid").at("cols") != b.at("grid").at("cols"))
            return false;
        const json &ca = a.at("coupons"), &cb = b.at("coupons");
        if (ca.size() != cb.size()) return false;
        for (std::size_t i = 0; i < ca.size(); ++i)
            for (const char* k : {"id", "row", "col", "params"})
                if (ca[i].at(k) != cb[i].at(k)) return false;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void keepPlate(const fruit::CalibrationPlate& p, PlateFiles& f) {
    const json m = p.manifest();
    const std::optional<json> old = readJson(f.manifest);
    if (!(old && samePlate(*old, m) && exists(f.results))) {
        if (hasScores(f)) setAside(f);
        writeWhole(f.manifest, m.dump(2) + "\n");
        writeWhole(f.results, fruit::blindResultsCsv(m, {}));
    }
    const PlateScores s = loadScores(f);
    f.scored = s.scored();
    f.coupons = static_cast<int>(s.coupons.size());
}

int PlateScores::scored() const {
    int n = 0;
    for (const auto& c : coupons) n += c.recorded ? 1 : 0;
    return n;
}

const fruit::CouponResult* PlateScores::find(int id) const {
    for (const auto& c : coupons)
        if (c.id == id) return &c;
    return nullptr;
}

PlateScores loadScores(const PlateFiles& f) {
    PlateScores s;
    {
        std::ifstream in(f.manifest);
        if (!in) throw std::runtime_error("The plate's manifest (" + fileName(f.manifest) + ") isn't there.");
        try {
            s.manifest = json::parse(in);
            s.kind = s.manifest.at("kind").get<std::string>();
            s.layout = layoutOf(s.manifest);
            for (const auto& c : s.manifest.at("coupons")) {
                fruit::CouponResult r;
                r.id = c.at("id").get<int>();
                r.params = c.at("params");
                s.coupons.push_back(r);
            }
        } catch (const std::exception& e) {
            throw std::runtime_error("The plate's manifest (" + fileName(f.manifest) + ") couldn't be read: " +
                                     e.what());
        }
    }
    if (!exists(f.results)) return s;  // nothing scored yet
    fruit::PlateResults done;
    try {
        done = fruit::loadPlateResults(f.manifest, f.results);
    } catch (const std::exception& e) {
        throw std::runtime_error("The score sheet (" + fileName(f.results) + ") couldn't be read: " + e.what());
    }
    std::map<int, const fruit::CouponResult*> by_id;
    for (const auto& c : done.coupons) by_id[c.id] = &c;
    for (auto& c : s.coupons)
        if (const auto it = by_id.find(c.id); it != by_id.end()) c = *it->second;
    return s;
}

void saveScores(PlateFiles& f, const PlateScores& s) {
    writeWhole(f.results, fruit::blindResultsCsv(s.manifest, s.coupons));
    f.scored = s.scored();
    f.coupons = static_cast<int>(s.coupons.size());
}

PlateScores readScoreSheet(const std::string& csv) {
    const std::string suffix = ".results.csv";
    if (csv.size() <= suffix.size() || csv.compare(csv.size() - suffix.size(), suffix.size(), suffix) != 0)
        throw std::runtime_error("A score sheet's name ends in .results.csv, with the plate's manifest (.json) "
                                 "beside it.");
    PlateFiles f;
    f.results = csv;
    f.manifest = fruit::manifestForResults(csv);
    if (!exists(f.manifest))
        throw std::runtime_error("Its manifest (" + fileName(f.manifest) + ") isn't beside it: that file says which "
                                 "coupon is where.");
    PlateScores s = loadScores(f);
    if (!isPlateKind(s.kind)) throw std::runtime_error("That sheet isn't for a tips, spacing or islands plate.");
    return s;
}

void keepScores(const PlateScores& s, PlateFiles& f) {
    if (hasScores(f)) setAside(f);
    writeWhole(f.manifest, s.manifest.dump(2) + "\n");
    saveScores(f, s);
}

int nextToScore(const PlateScores& s, int id) {
    const auto& cs = s.coupons;
    if (cs.empty()) return 0;
    std::size_t at = cs.size() - 1;  // not found: start from the first
    for (std::size_t i = 0; i < cs.size(); ++i)
        if (cs[i].id == id) at = i;
    for (std::size_t k = 1; k <= cs.size(); ++k) {
        const auto& c = cs[(at + k) % cs.size()];
        if (!c.recorded) return c.id;
    }
    return cs[(at + 1) % cs.size()].id;
}

}  // namespace gui
