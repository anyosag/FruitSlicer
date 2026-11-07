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
#include "formats/GooExport.h"
#include "support/SupportSettings.h"

namespace gui {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string seconds(double s) { return strf("%g s", std::round(s * 100) / 100); }

json printJson(const Resin& r) {
    return {{"sample_path", r.sample_path},     {"sample_name", r.sample_name},
            {"exposure", r.exposure},           {"bottom_exposure", r.bottom_exposure},
            {"bottom_layers", r.bottom_layers}, {"transition_layers", r.transition_layers}};
}

void readPrint(const json& p, Resin& r) {
    r.sample_path = p.value("sample_path", std::string());
    r.sample_name = p.value("sample_name", std::string());
    r.exposure = p.value("exposure", 0.0);
    r.bottom_exposure = p.value("bottom_exposure", 0.0);
    r.bottom_layers = p.value("bottom_layers", -1);
    r.transition_layers = p.value("transition_layers", -1);
}

bool hasMeasured(const json& p, const char* key) { return p.is_object() && p.contains(key) && !p[key].is_null(); }

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
    const json j = {{"name", r.name}, {"print", printJson(r)}, {"profile", r.profile}};
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
    p.exposure = r.exposure;
    p.bottom_exposure = r.bottom_exposure;
    p.bottom_layers = r.bottom_layers;
    p.transition_layers = r.transition_layers;
    return p;
}

std::string printSummary(const Resin& r) {
    if (!r.sample_path.empty())
        return "From " + (r.sample_name.empty() ? fs::path(r.sample_path).filename().string() : r.sample_name);
    if (r.exposure > 0 && r.bottom_exposure > 0)
        return "Typed in: " + seconds(r.exposure) + ", bottom " + seconds(r.bottom_exposure);
    return "None yet: each job asks";
}

std::string resinState(const Resin& r) { return profileState(r.profile); }

std::string profileState(const json& profile) {
    std::vector<const char*> plates;
    if (hasMeasured(profile, "capacity_table")) plates.push_back("tips");
    if (hasMeasured(profile, "coverage_curve") || hasMeasured(profile, "critical_angle_deg"))
        plates.push_back("spacing");
    if (hasMeasured(profile, "island_one_tip_length") || hasMeasured(profile, "island_two_tip_length"))
        plates.push_back("islands");
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
    if (sample) out = strf("%g mm · ", std::round(sample->layer_height * 1000) / 1000);
    const double e = r.exposure > 0 ? r.exposure : sample ? sample->print.exposure : 0;
    out += e > 0 ? seconds(e) : std::string("no print settings");
    const std::string state = resinState(r);
    if (state == "Defaults") return out + " · defaults";
    std::string measured = state.substr(std::string("Measured: ").size());
    return out + " · " + measured + " measured";
}

std::vector<std::pair<std::string, std::string>> printRows(const Resin& r, const fruit::GooHeader* h,
                                                           const fruit::PrinterProfile& printer) {
    std::vector<std::pair<std::string, std::string>> rows;
    const double density = fruit::GooExportOptions{}.resin_density;
    if (h) {
        const fruit::GooPrintSettings& p = h->print;
        const double normal = r.exposure > 0 ? r.exposure : p.exposure;
        const double bottom = r.bottom_exposure > 0 ? r.bottom_exposure : p.bottom_exposure;
        rows.push_back({"Layer height", strf("%g mm", fruit::gooLayerHeight(*h))});
        rows.push_back({"Bottom layers", strf("%u × %s", p.bottom_layers, seconds(bottom).c_str())});
        if (p.transition_layers == 0)
            rows.push_back({"Transition layers", "None"});
        else
            rows.push_back({"Transition layers",
                            strf("%u, from %.2f to %.2f s", p.transition_layers,
                                 fruit::layerDefFor(*h, p.bottom_layers).exposure,
                                 fruit::layerDefFor(*h, p.bottom_layers + p.transition_layers - 1u).exposure)});
        rows.push_back({"Normal exposure", seconds(normal)});
        rows.push_back({"Waits (before lift, after lift, after retract)",
                        strf("%g · %g · %g s", p.wait_before_lift, p.wait_after_lift, p.wait_after_retract)});
        rows.push_back({"Lift and retract", strf("%g mm", std::round(p.lift_distance * 1000) / 1000)});
        rows.push_back({"Light power", strf("%u of 255", p.pwm)});
        rows.push_back({"Density, for weight", strf("%g g/ml", density)});
        return rows;
    }
    if (r.exposure <= 0 && r.bottom_exposure <= 0) return rows;
    rows.push_back({"Layer height", strf("%g mm, FruitSlicer's own", printer.layer_height)});
    rows.push_back({"Bottom layers",
                    r.bottom_layers >= 0 ? strf("%d × %s", r.bottom_layers, seconds(r.bottom_exposure).c_str())
                                         : strf("%d × %s (the printer's own count)", printer.bottom_layers,
                                                seconds(r.bottom_exposure).c_str())});
    rows.push_back({"Transition layers", r.transition_layers >= 0
                                             ? std::to_string(r.transition_layers)
                                             : strf("%d (the printer's own)", printer.transition_layers)});
    rows.push_back({"Normal exposure", seconds(r.exposure)});
    rows.push_back({"Density, for weight", strf("%g g/ml", density)});
    return rows;
}

std::vector<ResinValue> resinSupportValues(const Resin& r) {
    fruit::SupportConfig cfg;
    fruit::TreeConfig tc;
    fruit::SupportChoices ch;
    ch.layer_height = 0.05;
    const json* prof = r.profile.is_object() ? &r.profile : nullptr;
    fruit::resolveSettings(prof, ch, cfg, &tc);
    using S = ResinValue::Source;
    auto from = [&](const char* key, S otherwise) { return hasMeasured(r.profile, key) ? S::Measured : otherwise; };
    std::vector<ResinValue> v;
    const int dt = std::clamp(cfg.default_tip, 0, static_cast<int>(cfg.tip_classes.size()) - 1);
    v.push_back({"Usual tip size", strf("%g mm", cfg.tip_classes[static_cast<std::size_t>(dt)].diameter),
                 from("default_tip_diameter", S::Default)});
    v.push_back({"Hold per 0.4 mm tip", strf("%.1f mm²", cfg.capacity.capacity(0.4)), from("capacity_table", S::Borrowed)});
    v.push_back({"Tip reach",
                 strf("%.1f → %.1f mm", cfg.coverage.curve.front().second, cfg.coverage.curve.back().second),
                 from("coverage_curve", S::Borrowed)});
    v.push_back({"Faces that need tips", strf("lean > %g°", cfg.critical_angle_deg),
                 from("critical_angle_deg", S::Borrowed)});
    v.push_back({"Spots in mid-air", strf("1 tip ≤ %g mm", cfg.island_one_tip_length),
                 from("island_one_tip_length", S::Borrowed)});
    std::string depth = "½ the tip size, 0.08–0.2 mm";
    if (!tc.penetration_table.empty()) {
        double lo = 1e9, hi = 0;
        for (const auto& [d, pen] : tc.penetration_table) {
            lo = std::min(lo, pen);
            hi = std::max(hi, pen);
        }
        depth = std::abs(hi - lo) < 1e-9 ? strf("%g mm", lo) : strf("%g–%g mm, by tip size", lo, hi);
    }
    v.push_back({"Contact depth", depth, from("penetration_table", S::Default)});
    v.push_back({"Snap-off zone", strf("%.1f mm", tc.flex_zone), S::Default});
    return v;
}

}  // namespace gui
