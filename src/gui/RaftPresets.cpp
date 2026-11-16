// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "RaftPresets.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "Format.h"

namespace gui {

using nlohmann::json;

const OwnRaftPreset* RaftPresets::find(const std::string& name) const {
    for (const OwnRaftPreset& p : own)
        if (p.name == name) return &p;
    return nullptr;
}

OwnRaftPreset* RaftPresets::find(const std::string& name) {
    for (OwnRaftPreset& p : own)
        if (p.name == name) return &p;
    return nullptr;
}

RaftPresets loadRaftPresets(const std::string& file) {
    RaftPresets p;
    std::ifstream in(file);
    if (!in) return p;
    try {
        const json j = json::parse(in);
        if (const auto it = j.find("presets"); it != j.end() && it->is_array())
            for (const json& e : *it) {
                if (!e.is_object() || !e.contains("name") || !e["name"].is_string()) continue;
                OwnRaftPreset o;
                o.name = e["name"].get<std::string>();
                o.from = e.value("from", std::string());
                o.spec = fruit::raftSpecFromJson(e.value("raft", json::object()));
                if (nameProblem(o.name, p).empty()) p.own.push_back(std::move(o));
            }
        const std::string nj = j.value("new_jobs", std::string(fruit::kDefaultRaftPreset));
        if (presetSpec(nj, p)) p.new_jobs = nj;
    } catch (const std::exception&) {
        return RaftPresets{};
    }
    return p;
}

void saveRaftPresets(const std::string& file, const RaftPresets& p) {
    json list = json::array();
    for (const OwnRaftPreset& o : p.own) list.push_back({{"name", o.name}, {"from", o.from}, {"raft", fruit::raftSpecToJson(o.spec)}});
    const json j = {{"presets", list}, {"new_jobs", p.new_jobs}};
    const std::filesystem::path path(file);
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    const std::string tmp = file + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << j.dump(2) << "\n";
        if (!out) throw std::runtime_error("cannot write " + tmp);
    }
    std::filesystem::rename(tmp, file);
}

bool isBuiltInPreset(const std::string& name) { return fruit::builtInRaftPreset(name) != nullptr; }

std::string copyName(const std::string& base, const RaftPresets& p) {
    const std::string first = "Copy of " + base;
    if (nameProblem(first, p).empty()) return first;
    for (int n = 2;; ++n) {
        const std::string s = first + " " + std::to_string(n);
        if (nameProblem(s, p).empty()) return s;
    }
}

std::string nameProblem(const std::string& name, const RaftPresets& p, const std::string& except) {
    const auto first = name.find_first_not_of(" \t");
    if (first == std::string::npos) return "Give it a name.";
    if (isBuiltInPreset(name)) return "A built-in preset has that name.";
    if (name != except && p.find(name)) return "One of your presets has that name.";
    return "";
}

const fruit::RaftSpec* presetSpec(const std::string& name, const RaftPresets& p) {
    if (const fruit::RaftPreset* b = fruit::builtInRaftPreset(name)) return &b->spec;
    if (const OwnRaftPreset* o = p.find(name)) return &o->spec;
    return nullptr;
}

std::string presetOrigin(const std::string& name, const RaftPresets& p) {
    if (isBuiltInPreset(name)) return name;
    if (const OwnRaftPreset* o = p.find(name)) return o->from;
    return "";
}

std::string raftSummary(const fruit::RaftSpec& s, double thickness) {
    const double t = std::round(thickness * 100) / 100;
    if (s.kind == fruit::RaftKind::Pads) {
        std::string out = strf("A %g mm pad under each support, joined by %s bars", t, s.bar_width < 1.6 ? "thin" : "stiff");
        if (s.frame)
            out += s.windows != fruit::RaftWindows::None ? ", inside a frame with windows for a screwdriver"
                                                          : ", inside a frame";
        return out + ".";
    }
    std::vector<std::string> has;
    if (s.side_angle < 88) has.push_back("a tapered side");
    else if (s.side_angle > 92) has.push_back("a sloped edge for the scraper");
    if (s.lip_height > 0) has.push_back(s.rounded ? "a rounded lip" : "a lip");
    if (s.windows != fruit::RaftWindows::None) has.push_back("windows for a screwdriver");
    std::string out = strf("A %g mm raft, one base under all the supports", t);
    for (size_t i = 0; i < has.size(); ++i)
        out += (i == 0 ? ", with " : i + 1 == has.size() ? " and " : ", ") + has[i];
    return out + ".";
}

namespace {

std::string num(double v, const char* unit) {
    const std::string u(unit);
    if (u == "°") return strf("%.0f", v) + u;
    std::string s = strf("%.1f", v);
    if (u == "%" && s.size() > 2 && s.substr(s.size() - 2) == ".0") s.resize(s.size() - 2);
    return s + u;
}

const char* windowsWord(fruit::RaftWindows w) {
    switch (w) {
        case fruit::RaftWindows::Few: return "Few";
        case fruit::RaftWindows::Some: return "Some";
        case fruit::RaftWindows::EveryTip: return "Every tip";
        default: return "None";
    }
}

}  // namespace

std::string raftChange(const fruit::RaftSpec& was, const fruit::RaftSpec& now) {
    std::vector<std::string> d;
    auto value = [&](const char* label, double a, double b, const char* unit) {
        if (std::abs(a - b) > 1e-9) {
            // "1.8 → 2.5 mm"; degrees and percentages keep their sign on both ("112° → 90°").
            std::string from = num(a, unit);
            if (std::string(unit) == " mm") from.resize(from.size() - 3);
            d.push_back(std::string(label) + " " + from + " → " + num(b, unit));
        }
    };
    auto word = [&](const char* label, const char* a, const char* b) {
        if (std::string(a) != b) d.push_back(std::string(label) + " " + a + " → " + b);
    };
    word("Kind", was.kind == fruit::RaftKind::Full ? "Full" : "Pads", now.kind == fruit::RaftKind::Full ? "Full" : "Pads");
    value("Reach past the trunks", was.reach, now.reach, " mm");
    value("Thickness", was.thickness, now.thickness, " mm");
    value("Side angle", was.side_angle, now.side_angle, "°");
    word("Rounded edges", was.rounded ? "on" : "off", now.rounded ? "on" : "off");
    value("Bridge gaps up to", was.gap_fill, now.gap_fill, " mm");
    value("Lip height", was.lip_height, now.lip_height, " mm");
    word("Windows", windowsWord(was.windows), windowsWord(now.windows));
    value("Window height", was.window_pct, now.window_pct, "%");
    value("Bar width", was.bar_width, now.bar_width, " mm");
    value("Bar height", was.bar_pct, now.bar_pct, "%");
    word("Join pads", was.join == fruit::PadJoin::Grid ? "Grid" : "Fewest bars", now.join == fruit::PadJoin::Grid ? "Grid" : "Fewest bars");
    word("Frame", was.frame ? "on" : "off", now.frame ? "on" : "off");
    value("Frame width", was.frame_width, now.frame_width, " mm");
    value("Gap to the pads", was.frame_gap, now.frame_gap, " mm");
    value("Frame height", was.frame_height, now.frame_height, " mm");
    value("Frame side angle", was.frame_angle, now.frame_angle, "°");
    return d.size() == 1 ? d[0] : "";
}

}  // namespace gui
