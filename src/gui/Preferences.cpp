// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Preferences.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace gui {

using nlohmann::json;

Preferences loadPreferences(const std::string& file) {
    Preferences p;
    std::ifstream in(file);
    if (!in) return p;
    try {
        const json j = json::parse(in);
        p.smooth_view = j.value("smooth_view", p.smooth_view);
        p.cores = j.value("cores", p.cores);
        p.goo_to_drive = j.value("goo_to_drive", p.goo_to_drive);
        p.goo_folder = j.value("goo_folder", p.goo_folder);
    } catch (const std::exception&) {
        return Preferences{};
    }
    return p;
}

void savePreferences(const std::string& file, const Preferences& p) {
    const json j = {{"smooth_view", p.smooth_view},
                    {"cores", p.cores},
                    {"goo_to_drive", p.goo_to_drive},
                    {"goo_folder", p.goo_folder}};
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

std::vector<unsigned> coreChoices(unsigned all) {
    std::vector<unsigned> out;
    for (unsigned n : {all, all - 1, all / 2, 1u})
        if (n >= 1 && n <= all && (out.empty() || n < out.back())) out.push_back(n);
    return out;
}

}  // namespace gui
