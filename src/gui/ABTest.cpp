// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "ABTest.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

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

}  // namespace gui
