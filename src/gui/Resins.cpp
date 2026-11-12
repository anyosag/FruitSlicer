// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Resins.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "Format.h"
#include "Supports.h"
#include "formats/GooExport.h"
#include "layers/AdaptiveLayers.h"
#include "support/SupportSettings.h"

namespace gui {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string seconds(double s) { return strf("%g s", std::round(s * 100) / 100); }

json printJson(const Resin& r) {
    const PrintEdits& e = r.print;
    return {{"sample_path", r.sample_path},
            {"sample_name", r.sample_name},
            {"exposure", e.exposure},
            {"bottom_exposure", e.bottom_exposure},
            {"bottom_layers", e.bottom_layers},
            {"transition_layers", e.transition_layers},
            {"layer_height", e.layer_height},
            {"wait_before_lift", e.wait_before_lift},
            {"wait_after_retract", e.wait_after_retract},
            {"bottom_wait_before_lift", e.bottom_wait_before_lift},
            {"bottom_wait_after_retract", e.bottom_wait_after_retract},
            {"lift", e.lift},
            {"light_power", e.light_power},
            {"density", e.density},
            {"height_correction", e.height_correction},
            {"adaptive", e.adaptive},
            {"light_depth", e.light_depth}};
}

void readPrint(const json& p, Resin& r) {
    r.sample_path = p.value("sample_path", std::string());
    r.sample_name = p.value("sample_name", std::string());
    PrintEdits& e = r.print;
    e.exposure = p.value("exposure", 0.0);
    e.bottom_exposure = p.value("bottom_exposure", 0.0);
    e.bottom_layers = p.value("bottom_layers", -1);
    e.transition_layers = p.value("transition_layers", -1);
    e.layer_height = p.value("layer_height", 0.0);
    e.wait_before_lift = p.value("wait_before_lift", -1.0);
    e.wait_after_retract = p.value("wait_after_retract", -1.0);
    e.bottom_wait_before_lift = p.value("bottom_wait_before_lift", -1.0);
    e.bottom_wait_after_retract = p.value("bottom_wait_after_retract", -1.0);
    e.lift = p.value("lift", -1.0);
    e.light_power = p.value("light_power", -1);
    e.density = p.value("density", 0.0);
    e.height_correction = p.value("height_correction", -1.0);
    e.adaptive = std::clamp(p.value("adaptive", 1), 1, fruit::kMaxAdaptiveMultiplier);
    e.light_depth = p.value("light_depth", -1.0);
}

bool hasMeasured(const json& p, const char* key) { return p.is_object() && p.contains(key) && !p[key].is_null(); }

json platesJson(const Resin& r) {
    json j = json::object();
    for (const auto& [kind, f] : r.plates)
        j[kind] = {{"file", f.file}, {"scored", f.scored}, {"coupons", f.coupons}};  // the rest is kept by id
    return j;
}

void readPlates(const json& j, Resin& r) {
    if (!j.is_object()) return;
    for (auto it = j.begin(); it != j.end(); ++it) {
        const json& f = it.value();
        if (!f.is_object()) continue;
        PlateFiles p;
        p.file = f.value("file", std::string());
        p.scored = f.value("scored", 0);
        p.coupons = f.value("coupons", 0);
        r.plates[it.key()] = p;
    }
}

}  // namespace

bool resinOrder(const Resin& a, const Resin& b) {
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string la = lower(a.name), lb = lower(b.name);
    return la != lb ? la < lb : a.name < b.name;
}

std::vector<Resin> loadResins(const std::string& dir) {
    std::vector<Resin> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".json") continue;
        try {
            std::ifstream in(e.path());
            const json j = json::parse(in);
            Resin r;
            r.id = e.path().stem().string();
            r.name = j.value("name", r.id);
            if (j.contains("print")) readPrint(j["print"], r);
            if (j.contains("profile") && j["profile"].is_object()) r.profile = j["profile"];
            if (j.contains("plates")) readPlates(j["plates"], r);
            out.push_back(std::move(r));
        } catch (const std::exception&) {
            // Not a resin (or damaged): left out.
        }
    }
    std::sort(out.begin(), out.end(), resinOrder);
    return out;
}

void saveResin(const std::string& dir, const Resin& r) {
    if (r.id.empty()) throw std::runtime_error("a resin needs an id");
    fs::create_directories(dir);
    const json j = {{"name", r.name}, {"print", printJson(r)}, {"profile", r.profile}, {"plates", platesJson(r)}};
    const fs::path file = fs::path(dir) / (r.id + ".json");
    const std::string tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << j.dump(2) << "\n";
        if (!out) throw std::runtime_error("cannot write " + tmp);
    }
    fs::rename(tmp, file);
}

void removeResin(const std::string& dir, const std::string& id) {
    std::error_code ec;
    fs::remove(fs::path(dir) / (id + ".json"), ec);
}

std::string newResinId(const std::vector<Resin>& resins, const std::string& name) {
    std::string base;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) base += static_cast<char>(std::tolower(u));
        else if (!base.empty() && base.back() != '-') base += '-';
    }
    while (!base.empty() && base.back() == '-') base.pop_back();
    if (base.empty()) base = "resin";
    auto taken = [&](const std::string& id) {
        return std::any_of(resins.begin(), resins.end(), [&](const Resin& r) { return r.id == id; });
    };
    std::string id = base;
    for (int n = 2; taken(id); ++n) id = base + "-" + std::to_string(n);
    return id;
}

std::string freeResinName(const std::vector<Resin>& resins, const std::string& name) {
    auto taken = [&](const std::string& n) {
        return std::any_of(resins.begin(), resins.end(), [&](const Resin& r) { return r.name == n; });
    };
    if (!taken(name)) return name;
    std::string n = name + " (copy)";
    for (int k = 2; taken(n); ++k) n = name + " (copy " + std::to_string(k) + ")";
    return n;
}

std::vector<Resin> firstResins(const std::string& dir, const std::string& settings_file) {
    std::vector<Resin> have = loadResins(dir);
    if (!have.empty()) return have;
    Resin r;
    r.id = "my-resin";
    r.name = "My resin";
    // Print settings an earlier version's setup kept with the printer's settings.
    try {
        std::ifstream in(settings_file);
        if (in) {
            const json j = json::parse(in);
            if (j.contains("print_defaults")) readPrint(j["print_defaults"], r);
        }
    } catch (const std::exception&) {
    }
    saveResin(dir, r);
    return {r};
}

Resin resinFromProfileFile(const std::string& json_path, const std::vector<Resin>& existing) {
    json j;
    try {
        std::ifstream in(json_path);
        if (!in) throw std::runtime_error("cannot open it");
        j = json::parse(in);
    } catch (const std::exception&) {
        throw std::runtime_error("That file isn't a resin profile: it couldn't be read as JSON.");
    }
    Resin r;
    std::string name = fs::path(json_path).stem().string();
    if (j.is_object() && j.contains("profile") && j.contains("name")) {
        // A resin from this page (its print settings too).
        name = j.value("name", name);
        if (j.contains("print")) readPrint(j["print"], r);
        if (j["profile"].is_object()) r.profile = j["profile"];
    } else {
        // A profile from `fruitslicer fit`.
        const char* keys[] = {"capacity_table", "coverage_curve", "critical_angle_deg", "island_one_tip_length",
                              "island_two_tip_length", "penetration_table", "default_tip_diameter"};
        const bool any = j.is_object() && std::any_of(std::begin(keys), std::end(keys),
                                                      [&](const char* k) { return hasMeasured(j, k); });
        if (!any) throw std::runtime_error("That file isn't a resin profile: it has none of the measured values.");
        r.profile = j;
        if (!j.value("resin", std::string()).empty()) name = j["resin"].get<std::string>();
    }
    r.name = freeResinName(existing, name);
    r.id = newResinId(existing, r.name);
    return r;
}

PrintSettings printSettingsFor(const Resin& r) {
    PrintSettings p;
    if (!r.sample_path.empty()) {
        try {
            p.sample = loadSample(r.sample_path);
            p.sample_path = r.sample_path;
        } catch (const std::exception&) {
            // Gone or unreadable: the job starts without it.
        }
    }
    static_cast<PrintEdits&>(p) = r.print;
    return p;
}

std::string printSummary(const Resin& r) {
    if (!r.sample_path.empty())
        return "From " + (r.sample_name.empty() ? fs::path(r.sample_path).filename().string() : r.sample_name);
    if (r.print.exposure > 0 && r.print.bottom_exposure > 0)
        return "Typed in: " + seconds(r.print.exposure) + ", bottom " + seconds(r.print.bottom_exposure);
    return "None yet: each job asks";
}

std::string resinState(const Resin& r) { return profileState(r.profile); }

bool plateMeasured(const json& profile, const std::string& kind) {
    if (kind == "tips") return hasMeasured(profile, "capacity_table");
    if (kind == "spacing") return hasMeasured(profile, "coverage_curve") || hasMeasured(profile, "critical_angle_deg");
    if (kind == "islands")
        return hasMeasured(profile, "island_one_tip_length") || hasMeasured(profile, "island_two_tip_length");
    return false;
}

PlateStep nextPlateStep(const Resin& r) {
    using Action = PlateStep::Action;
    bool to_fit = false;
    for (const char* kind : kPlateKinds) {
        const std::string k = kind;
        const auto it = r.plates.find(kind);
        const bool measured = plateMeasured(r.profile, kind);
        if (it == r.plates.end()) {
            if (measured) continue;
            return {Action::Print, k,
                    k == "tips" ? "Print the resin test plates, starting with the tips plate."
                                : "Print the " + k + " plate next."};
        }
        const PlateFiles& f = it->second;
        if (f.coupons > 0 && f.scored >= f.coupons) {
            to_fit |= !measured;
            continue;
        }
        if (measured) continue;  // fitted from an earlier sheet: scoring this one is up to you
        return {Action::Score, k,
                f.scored == 0 ? "Score the " + k + " plate once it's printed."
                              : "Score the rest of the " + k + " plate: " + std::to_string(f.scored) + " of " +
                                    std::to_string(f.coupons) + " so far."};
    }
    if (to_fit) return {Action::Fit, "", "Turn the scores into this resin's support values."};
    return {Action::None, "", "This resin is calibrated: its support values come from its own test plates."};
}

std::string profileState(const json& profile) {
    std::vector<const char*> plates;
    for (const char* kind : kPlateKinds)
        if (plateMeasured(profile, kind)) plates.push_back(kind);
    if (plates.empty()) return "Defaults";
    std::string s = "Measured: ";
    for (std::size_t i = 0; i < plates.size(); ++i) s += (i ? ", " : "") + std::string(plates[i]);
    return s;
}

std::string resinLabel(const std::string& name, const json& profile) {
    return (name.empty() ? std::string("My resin") : name) + " · " + profileState(profile);
}

std::string resinSummary(const Resin& r, const fruit::GooHeader* sample) {
    std::string out;
    const double lh = r.print.layer_height > 0 ? r.print.layer_height : sample ? sample->layer_height : 0;
    if (lh > 0) out = strf("%g mm · ", std::round(lh * 1000) / 1000);
    const double e = r.print.exposure > 0 ? r.print.exposure : sample ? sample->print.exposure : 0;
    out += e > 0 ? seconds(e) : std::string("no print settings");
    const std::string state = resinState(r);
    if (state == "Defaults") return out + " · defaults";
    std::string measured = state.substr(std::string("Measured: ").size());
    return out + " · " + measured + " measured";
}

namespace {

// The print settings a job on `r` would start from: the file's (or the
// printer's own and FruitSlicer's defaults), with `e` over them.
fruit::GooHeader headerWith(const Resin& r, const PrintEdits& e, const fruit::GooHeader* sample,
                            const fruit::PrinterProfile& printer) {
    PrintSettings s;
    static_cast<PrintEdits&>(s) = e;
    std::shared_ptr<const fruit::GooHeader> keep(sample, [](const fruit::GooHeader*) {});
    if (!r.sample_path.empty() && sample) s.sample = keep;
    return fruit::printHeader(printer, printRequest(s, printer.layer_height));
}

struct FieldRange {
    const char* key;
    double lo, hi;
    bool whole;  // a count
};
// Each value's range, as the page's fields allow it.
const FieldRange kFields[] = {
    {"layer_height", 0.01, 0.2, false},     {"bottom_layers", 0, 50, true},    {"bottom_exposure", 0.1, 600, false},
    {"transition_layers", 0, 50, true},     {"exposure", 0.1, 600, false},     {"wait_before_lift", 0, 120, false},
    {"wait_after_retract", 0, 120, false},  {"lift", 0.01, 20, false},
    {"bottom_wait_before_lift", 0, 120, false}, {"bottom_wait_after_retract", 0, 120, false},
    {"light_power", 1, 255, true},          {"density", 0.5, 3, false},
    {"height_correction", 0, fruit::kMaxHeightCorrection, false},
    {"adaptive", 0.01, fruit::kMaxLayerHeight, false},
    {"light_depth", fruit::kMinLightDepth, fruit::kMaxLightDepth, false},
};

const FieldRange* rangeOf(const std::string& key) {
    for (const FieldRange& f : kFields)
        if (key == f.key) return &f;
    return nullptr;
}

PrintField field(const char* key, const char* suffix, double value) {
    const FieldRange* f = rangeOf(key);
    return {key, suffix, f->lo, f->hi, value};
}

// One value of a header (or of FruitSlicer's own, not in a print file), by field key.
double valueOf(const fruit::GooHeader& h, const PrintEdits& e, const std::string& key) {
    const fruit::GooPrintSettings& p = h.print;
    if (key == "layer_height") return fruit::gooLayerHeight(h);
    if (key == "bottom_layers") return p.bottom_layers;
    if (key == "bottom_exposure") return std::round(p.bottom_exposure * 1000) / 1000;
    if (key == "transition_layers") return p.transition_layers;
    if (key == "exposure") return std::round(p.exposure * 1000) / 1000;
    if (key == "wait_before_lift") return std::round(p.wait_before_lift * 1000) / 1000;
    if (key == "wait_after_retract") return std::round(p.wait_after_retract * 1000) / 1000;
    if (key == "bottom_wait_before_lift") return std::round(p.bottom_wait_before_lift * 1000) / 1000;
    if (key == "bottom_wait_after_retract") return std::round(p.bottom_wait_after_retract * 1000) / 1000;
    if (key == "lift") return std::round(p.lift_distance * 1000) / 1000;
    if (key == "light_power") return p.pwm;
    if (key == "density") return resinDensity(e);
    if (key == "height_correction") return heightCorrection(e);
    if (key == "adaptive") {
        const double h0 = fruit::gooLayerHeight(h);
        return h0 * adaptiveCount(e, h0);
    }
    if (key == "light_depth") return lightDepth(e);
    return 0;
}

void setEdit(PrintEdits& e, const std::string& key, double v) {
    const int n = static_cast<int>(std::lround(v));
    if (key == "layer_height") e.layer_height = v;
    else if (key == "bottom_layers") e.bottom_layers = n;
    else if (key == "bottom_exposure") e.bottom_exposure = v;
    else if (key == "transition_layers") e.transition_layers = n;
    else if (key == "exposure") e.exposure = v;
    else if (key == "wait_before_lift") e.wait_before_lift = v;
    else if (key == "wait_after_retract") e.wait_after_retract = v;
    else if (key == "bottom_wait_before_lift") e.bottom_wait_before_lift = v;
    else if (key == "bottom_wait_after_retract") e.bottom_wait_after_retract = v;
    else if (key == "lift") e.lift = v;
    else if (key == "light_power") e.light_power = n;
    else if (key == "density") e.density = v;
    else if (key == "height_correction") e.height_correction = v;
    else if (key == "adaptive") e.adaptive = n;  // the multiplier (the page's mm are turned into it first)
    else if (key == "light_depth") e.light_depth = v;
}

void clearEdit(PrintEdits& e, const std::string& key) {
    const PrintEdits none;
    if (key == "layer_height") e.layer_height = none.layer_height;
    else if (key == "bottom_layers") e.bottom_layers = none.bottom_layers;
    else if (key == "bottom_exposure") e.bottom_exposure = none.bottom_exposure;
    else if (key == "transition_layers") e.transition_layers = none.transition_layers;
    else if (key == "exposure") e.exposure = none.exposure;
    else if (key == "wait_before_lift") e.wait_before_lift = none.wait_before_lift;
    else if (key == "wait_after_retract") e.wait_after_retract = none.wait_after_retract;
    else if (key == "bottom_wait_before_lift") e.bottom_wait_before_lift = none.bottom_wait_before_lift;
    else if (key == "bottom_wait_after_retract") e.bottom_wait_after_retract = none.bottom_wait_after_retract;
    else if (key == "lift") e.lift = none.lift;
    else if (key == "light_power") e.light_power = none.light_power;
    else if (key == "density") e.density = none.density;
    else if (key == "height_correction") e.height_correction = none.height_correction;
    else if (key == "adaptive") e.adaptive = none.adaptive;
    else if (key == "light_depth") e.light_depth = none.light_depth;
}

// The rows and the values each one holds, in page order.
const std::vector<std::pair<const char*, std::vector<const char*>>>& rowKeys() {
    static const std::vector<std::pair<const char*, std::vector<const char*>>> rows = {
        {"layer_height", {"layer_height"}},
        {"bottom", {"bottom_layers", "bottom_exposure"}},
        {"height_correction", {"height_correction"}},
        {"transition", {"transition_layers"}},
        {"exposure", {"exposure"}},
        {"adaptive", {"adaptive"}},
        {"light_depth", {"light_depth"}},
        {"waits", {"wait_before_lift", "wait_after_retract"}},
        {"bottom_waits", {"bottom_wait_before_lift", "bottom_wait_after_retract"}},
        {"lift", {"lift"}},
        {"light_power", {"light_power"}},
        {"density", {"density"}}};
    return rows;
}

bool edited(const PrintEdits& e, const std::vector<const char*>& keys) {
    for (const char* k : keys) {
        PrintEdits without = e;
        clearEdit(without, k);
        if (!(without == e)) return true;
    }
    return false;
}

// What a row shows for header `h` and edits `e` (the values no print file has).
std::string rowText(const std::string& row, const fruit::GooHeader& h, const PrintEdits& e) {
    const double density = resinDensity(e);
    const fruit::GooPrintSettings& p = h.print;
    auto secondsOr = [](double s) { return s > 0 ? seconds(s) : std::string("not set"); };
    if (row == "layer_height") return strf("%g mm", fruit::gooLayerHeight(h));
    if (row == "bottom")
        return p.bottom_exposure > 0 ? strf("%u × %s", p.bottom_layers, seconds(p.bottom_exposure).c_str())
                                     : strf("%u, time not set", p.bottom_layers);
    if (row == "transition") {
        if (p.transition_layers == 0) return "None";
        if (p.bottom_exposure <= 0 || p.exposure <= 0) return std::to_string(p.transition_layers);
        return strf("%u, from %.2f to %.2f s", p.transition_layers, fruit::layerDefFor(h, p.bottom_layers).exposure,
                    fruit::layerDefFor(h, p.bottom_layers + p.transition_layers - 1u).exposure);
    }
    if (row == "exposure") return secondsOr(p.exposure);
    if (row == "waits") return strf("%g · %g s", p.wait_before_lift, p.wait_after_retract);
    if (row == "bottom_waits") return strf("%g · %g s", p.bottom_wait_before_lift, p.bottom_wait_after_retract);
    if (row == "lift") return strf("%g mm", std::round(p.lift_distance * 1000) / 1000);
    if (row == "light_power") return strf("%u of 255", p.pwm);
    if (row == "density") return strf("%g g/ml", density);
    if (row == "height_correction") return strf("%g mm", heightCorrection(e));
    if (row == "adaptive") {
        const double h0 = fruit::gooLayerHeight(h);
        const int n = adaptiveCount(e, h0);
        return n > 1 ? strf("up to %g mm", h0 * n) : "Off";
    }
    if (row == "light_depth") return strf("%g mm", lightDepth(e));
    return "";
}

}  // namespace

std::vector<PrintRow> printRows(const Resin& r, const fruit::GooHeader* sample, const fruit::PrinterProfile& printer) {
    const bool file = sample && !r.sample_path.empty();
    const fruit::GooHeader h = headerWith(r, r.print, sample, printer);
    const fruit::GooHeader base = headerWith(r, PrintEdits{}, sample, printer);
    const fruit::GooPrintSettings& p = h.print;
    using S = PrintRow::Source;
    std::vector<PrintRow> rows;
    for (const auto& [key, keys] : rowKeys()) {
        PrintRow row;
        row.key = key;
        row.value = rowText(key, h, r.print);
        row.base = rowText(key, base, PrintEdits{});
        // Density and the height correction are FruitSlicer's: no print file has them.
        const bool ours = row.key == "density" || row.key == "height_correction" || row.key == "adaptive" ||
                          row.key == "light_depth";
        row.source = edited(r.print, keys) ? S::Yours : file && !ours ? S::File : S::Default;
        rows.push_back(std::move(row));
    }
    rows[0].label = "Layer height";
    rows[0].fields = {field("layer_height", "mm", fruit::gooLayerHeight(h))};
    rows[1].label = "Bottom layers";
    rows[1].fields = {field("bottom_layers", "×", p.bottom_layers), field("bottom_exposure", "s", p.bottom_exposure)};
    rows[2].label = "Height correction, with no raft";
    rows[2].fields = {field("height_correction", "mm", heightCorrection(r.print))};
    if (rows[2].source != S::Yours)
        rows[2].note = "Added under a model standing on the plate, which prints short because the printer squashes "
                       "the first layers. 0.10 mm is a typical value, not measured on your printer: print a block on "
                       "the plate, measure how short it is, and type that.";
    rows[3].label = "Transition layers";
    rows[3].fields = {field("transition_layers", "", p.transition_layers)};
    if (p.transition_layers > 0 && p.bottom_exposure > 0 && p.exposure > 0)
        rows[3].after = strf("from %.2f to %.2f s", fruit::layerDefFor(h, p.bottom_layers).exposure,
                             fruit::layerDefFor(h, p.bottom_layers + p.transition_layers - 1u).exposure);
    rows[4].label = "Normal exposure";
    rows[4].fields = {field("exposure", "s", p.exposure)};
    rows[5].label = "Adaptive layers, up to";
    rows[5].fields = {field("adaptive", "mm", valueOf(h, r.print, "adaptive"))};
    {
        const double h0 = fruit::gooLayerHeight(h);
        const int n = adaptiveCount(r.print, h0);
        if (n <= 1) {
            rows[5].after = "Off";
        } else {
            // Each thicker height and the exposure the formula gives it.
            std::string words;
            for (const double height : fruit::adaptiveHeights(h0, n)) {
                if (height <= h0 + 1e-9) continue;
                if (!words.empty()) words += " · ";
                words += p.exposure > 0
                             ? strf("%g mm at %.2f s", height, fruit::adaptiveExposure(p.exposure, h0, height, lightDepth(r.print)))
                             : strf("%g mm", height);
            }
            rows[5].after = words;
            rows[5].note = "Thick layers are used where the model's walls are straight. This printer has not been "
                           "checked to obey a height per layer, so make a small part first and measure it.";
        }
    }
    rows[6].label = "Light depth";
    rows[6].fields = {field("light_depth", "mm", lightDepth(r.print))};
    if (rows[6].source != S::Yours)
        rows[6].note = "How deep light hardens this resin; it sets the exposure of thicker layers. 0.10 mm is a "
                       "typical value, not measured for this resin. Too little exposure makes thick layers come "
                       "apart, so try a small part first.";
    rows[7].label = "Waits (before lift, after retract)";
    rows[7].fields = {field("wait_before_lift", "·", p.wait_before_lift),
                      field("wait_after_retract", "s", p.wait_after_retract)};
    rows[8].label = "Bottom-layer waits (before lift, after retract)";
    rows[8].fields = {field("bottom_wait_before_lift", "·", p.bottom_wait_before_lift),
                      field("bottom_wait_after_retract", "s", p.bottom_wait_after_retract)};
    if (rows[8].source == S::Yours) {
        rows[8].note = "Not checked on the printer: whether it waits differently on the bottom layers hasn't been "
                       "tested.";
        // The way back is to wait as the other layers do (their waits may be yours too).
        PrintEdits without = r.print;
        resetPrintRow(without, "bottom_waits");
        rows[8].base = rowText("bottom_waits", headerWith(r, without, sample, printer), without);
    }
    rows[9].label = "Lift and retract";
    rows[9].fields = {field("lift", "mm", valueOf(h, r.print, "lift"))};
    rows[10].label = "Light power";
    rows[10].fields = {field("light_power", "of 255", p.pwm)};
    rows[11].label = "Density, for weight";
    rows[11].fields = {field("density", "g/ml", resinDensity(r.print))};
    // Exposures with no file and none typed: nothing to go back to.
    if (!file) {
        if (base.print.bottom_exposure <= 0) rows[1].base.clear();
        if (base.print.exposure <= 0) rows[4].base.clear();
    }
    // Plain words about what an edited value hasn't been checked with.
    const double lh = fruit::gooLayerHeight(h), base_lh = fruit::gooLayerHeight(base);
    if (rows[0].source == S::Yours && std::abs(lh - base_lh) > 1e-6 && (file || base.print.exposure > 0))
        rows[0].note = strf("The exposures were set for %g mm layers and haven't been checked at %g mm.", base_lh, lh);
    if (rows[9].source == S::Yours && std::abs(p.lift_distance - base.print.lift_distance) > 1e-6)
        rows[9].note = "Not checked on the printer: the 16K tilts its vat, and how it uses this distance hasn't been "
                       "tested.";
    if (rows[10].source == S::Yours && p.pwm != base.print.pwm)
        rows[10].note = strf("The exposures were set at %u and haven't been checked at %u.", base.print.pwm, p.pwm);
    return rows;
}

void setPrintValue(Resin& r, const fruit::GooHeader* sample, const fruit::PrinterProfile& printer,
                   const std::string& key, double value) {
    const FieldRange* f = rangeOf(key);
    if (!f) throw std::invalid_argument("no print setting called " + key);
    if (key == "adaptive") {
        // Typed in mm: the nearest height the layer height allows (the layer
        // height itself is off), kept as a multiple of it.
        const double h0 = fruit::gooLayerHeight(headerWith(r, r.print, sample, printer));
        r.print.adaptive = fruit::snapAdaptiveMultiplier(h0, value);
        return;
    }
    double v = std::clamp(value, f->lo, f->hi);
    if (f->whole) v = std::round(v);
    PrintEdits without = r.print;
    clearEdit(without, key);
    const double base = valueOf(headerWith(r, without, sample, printer), without, key);
    if (std::abs(v - base) < 1e-9) r.print = without;
    else setEdit(r.print, key, v);
}

void resetPrintRow(PrintEdits& e, const std::string& row) {
    for (const auto& [key, keys] : rowKeys())
        if (row == key)
            for (const char* k : keys) clearEdit(e, k);
}

std::vector<ResinValue> resinSupportValues(const Resin& r) {
    // As a job on this resin starts: its measured values and its own.
    const SupportChoices c = resinChoices(r.profile);
    const fruit::SupportConfig cfg = supportConfig(fruit::Placement{}, c);
    const fruit::TreeConfig tc = treeConfig(c);
    using S = ResinValue::Source;
    std::vector<ResinValue> v;
    for (const char* label : kResinSupportRows) {
        const AdvancedSetting* s = advancedRow(label);
        if (!s) continue;
        S source = s->source == AdvancedSetting::Source::Borrowed ? S::Borrowed : S::Default;
        if (resinChanged(*s, r.profile)) source = S::Yours;
        else if (measuredByResin(*s, c)) source = S::Measured;
        v.push_back({label, advancedValueText(*s, cfg, tc), source});
    }
    return v;
}

}  // namespace gui
