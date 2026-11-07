// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "calibration/ABPlate.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>

#include "mesh/Primitives.h"

namespace fruit {

namespace {

std::string trimmed(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    const auto e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

// Fields of one CSV line; double quotes group, and are dropped.
std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (char c : line) {
        if (c == '"') quoted = !quoted;
        else if (c == ',' && !quoted) {
            out.push_back(trimmed(cur));
            cur.clear();
        } else cur += c;
    }
    out.push_back(trimmed(cur));
    return out;
}

std::string lowered(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

ABPlateFit abPlateFit(const TriangleMesh& model, const TriangleMesh& supports, const PrinterProfile& printer,
                      double margin) {
    BBox3 fp = model.bbox();
    if (!supports.faces.empty()) fp.extend(supports.bbox());
    ABPlateFit f;
    f.cell_x = fp.size().x + margin;
    f.cell_y = fp.size().y + margin;
    const double edge = 4.0;
    f.cols = static_cast<int>(std::floor((printer.build_x - 2 * edge) / f.cell_x));
    f.rows = static_cast<int>(std::floor((printer.build_y - 2 * edge) / f.cell_y));
    f.capacity = std::max(0, f.rows * f.cols - 1);  // cell 0 holds the marker
    return f;
}

ABPlate makeABPlate(const TriangleMesh& model, const TriangleMesh& supports, const PrinterProfile& printer,
                    const ABPlateOptions& o) {
    if (o.copies < 2) throw std::runtime_error("an A/B plate needs at least 2 copies");
    BBox3 fp = model.bbox();
    if (!supports.faces.empty()) fp.extend(supports.bbox());
    const ABPlateFit fit = abPlateFit(model, supports, printer, o.margin);
    const double cx = fit.cell_x, cy = fit.cell_y;
    const int cols = fit.cols, rows = fit.rows;
    if (fit.capacity < o.copies) {
        std::ostringstream m;
        m << o.copies << " copies requested but only " << fit.capacity << " fit (each needs " << cx << " × " << cy
          << " mm)";
        throw std::runtime_error(m.str());
    }
    // Balanced arms, shuffled deterministically (mt19937's sequence is fixed
    // by the standard; std::shuffle's is not).
    std::vector<char> arm(o.copies);
    for (int i = 0; i < o.copies; ++i) arm[i] = i < (o.copies + 1) / 2 ? 'A' : 'B';
    std::mt19937 rng(o.seed);
    for (size_t i = arm.size(); i > 1; --i) std::swap(arm[i - 1], arm[rng() % i]);

    ABPlate p;
    p.rows = rows;
    p.cols = cols;
    const double x0 = -cols * cx / 2, y0 = -rows * cy / 2;
    auto cellCenter = [&](int r, int c) { return Vec2{x0 + (c + 0.5) * cx, y0 + (r + 0.5) * cy}; };
    const Vec2 fc = fp.center().xy();
    const double model_bottom = model.bbox().min.z;

    // L marker in cell 0: long arm along +col, short arm along +row.
    {
        const Vec2 c = cellCenter(0, 0);
        const double h = std::min(cx, cy) / 2 - 1;
        appendBox(p.supported, {c.x - h, c.y - h, 0}, {c.x + h, c.y - h + 1.5, 2});
        appendBox(p.supported, {c.x - h, c.y - h, 0}, {c.x - h + 1.5, c.y + h * 0.2, 2});
    }
    nlohmann::json list = nlohmann::json::array();
    std::ostringstream csv;
    csv << "id,row,col,arm,result,scar,removal_s,notes\n";
    for (int i = 0; i < o.copies; ++i) {
        const int slot = i + 1;
        const int r = slot / cols, c = slot % cols;
        const Vec2 cc = cellCenter(r, c);
        const Vec3 shift{cc.x - fc.x, cc.y - fc.y, 0};
        if (arm[i] == 'A') {
            TriangleMesh m = model, s = supports;
            m.translate(shift);
            s.translate(shift);
            p.supported.append(m);
            p.supported.append(s);
        } else {
            TriangleMesh m = model;
            m.translate({shift.x, shift.y, -model_bottom});  // stands on the plate; the slicer lifts it
            p.bare.append(m);
        }
        list.push_back({{"id", i + 1}, {"row", r}, {"col", c}, {"arm", std::string(1, arm[i])},
                        {"center", {cc.x, cc.y}}});
        csv << i + 1 << "," << r << "," << c << "," << arm[i] << ",,,,\n";
    }
    p.manifest = {{"kind", "ab"},
                  {"printer", printer.name},
                  {"model", o.model},
                  {"tips_per_copy", o.tips_per_copy >= 0 ? nlohmann::json(o.tips_per_copy) : nlohmann::json(nullptr)},
                  {"seed", o.seed},
                  {"grid", {{"rows", rows}, {"cols", cols}, {"cell_mm", {cx, cy}}}},
                  {"arms", {{"A", "FruitSlicer supports (file _A.stl)"}, {"B", "vendor auto-supports (file _B.stl)"}}},
                  {"copies", list},
                  {"record",
                   "Per copy: result ok/failed/partial, scar 1 (invisible) .. 5 (pit) judged blind under raking "
                   "light, removal_s = seconds to clear supports. Count support contacts per arm from the slicer."}};
    p.csv = csv.str();
    return p;
}

std::string blindABCsv(const nlohmann::json& manifest, const std::vector<ABCopyResult>& scores) {
    std::map<int, const ABCopyResult*> by_id;
    for (const auto& s : scores) by_id[s.id] = &s;
    std::ostringstream out;
    out << "id,row,col,result,scar,removal_s,notes\n";
    for (const auto& c : manifest.at("copies")) {
        const int id = c.at("id").get<int>();
        out << id << "," << c.value("row", 0) << "," << c.value("col", 0) << ",";
        const auto it = by_id.find(id);
        const ABCopyResult* s = it == by_id.end() ? nullptr : it->second;
        if (s && s->recorded) out << (s->ok ? "ok" : s->partial ? "partial" : "failed");
        out << ",";
        if (s && s->scar > 0) out << s->scar;
        out << ",";
        if (s && s->removal_s >= 0) out << s->removal_s;
        out << ",";
        if (s && !s->notes.empty()) {
            std::string n = s->notes;  // quoted: commas stay; line breaks and quotes don't
            for (char& ch : n)
                if (ch == '\n' || ch == '\r') ch = ' ';
                else if (ch == '"') ch = '\'';
            out << '"' << n << '"';
        }
        out << "\n";
    }
    return out.str();
}

std::vector<ABCopyResult> loadABResults(const std::string& manifest_path, const std::string& csv_path) {
    nlohmann::json m;
    {
        std::ifstream in(manifest_path);
        if (!in) throw std::runtime_error("cannot open " + manifest_path);
        m = nlohmann::json::parse(in);
    }
    if (m.value("kind", std::string()) != "ab") throw std::runtime_error(manifest_path + " is not an A/B test's manifest");
    std::vector<ABCopyResult> out;
    std::map<int, std::size_t> index;
    std::map<int, std::string> arms;
    for (const auto& c : m.at("copies")) {
        ABCopyResult r;
        r.id = c.at("id").get<int>();
        index[r.id] = out.size();
        arms[r.id] = c.at("arm").get<std::string>();
        out.push_back(r);
    }
    std::ifstream in(csv_path);
    if (!in) throw std::runtime_error("cannot open " + csv_path);
    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error(csv_path + " is empty");
    const std::vector<std::string> head = fields(line);
    auto col = [&](const char* name) {
        for (std::size_t i = 0; i < head.size(); ++i)
            if (lowered(head[i]) == name) return static_cast<int>(i);
        return -1;
    };
    const int c_id = col("id"), c_arm = col("arm"), c_result = col("result"), c_scar = col("scar"),
              c_removal = col("removal_s"), c_notes = col("notes");
    if (c_id < 0 || c_result < 0) throw std::runtime_error(csv_path + ": the header needs id and result columns");
    int line_no = 1;
    while (std::getline(in, line)) {
        ++line_no;
        if (trimmed(line).empty()) continue;
        const std::vector<std::string> f = fields(line);
        auto at = [&](int c) { return c >= 0 && c < static_cast<int>(f.size()) ? f[static_cast<std::size_t>(c)] : std::string(); };
        const std::string where = csv_path + " line " + std::to_string(line_no);
        int id = 0;
        try {
            id = std::stoi(at(c_id));
        } catch (const std::exception&) {
            throw std::runtime_error(where + ": no copy number");
        }
        const auto it = index.find(id);
        if (it == index.end()) throw std::runtime_error(where + ": copy " + std::to_string(id) + " isn't on the plate");
        if (const std::string arm = at(c_arm); !arm.empty() && arm != arms[id])
            throw std::runtime_error(where + ": copy " + std::to_string(id) + " is " + arm + " here but " + arms[id] +
                                     " in its manifest");
        ABCopyResult& r = out[it->second];
        const std::string res = lowered(at(c_result));
        if (!res.empty()) {
            if (res != "ok" && res != "partial" && res != "failed")
                throw std::runtime_error(where + ": result must be ok, partial or failed, got '" + res + "'");
            r.recorded = true;
            r.ok = res == "ok";
            r.partial = res == "partial";
        }
        if (const std::string sc = at(c_scar); !sc.empty()) {
            try {
                r.scar = std::stoi(sc);
            } catch (const std::exception&) {
                r.scar = -1;
            }
            if (r.scar < 1 || r.scar > 5) throw std::runtime_error(where + ": scar must be 1 to 5, got '" + sc + "'");
        }
        if (const std::string t = at(c_removal); !t.empty()) {
            try {
                r.removal_s = std::stod(t);
            } catch (const std::exception&) {
                r.removal_s = -1;
            }
            if (!(r.removal_s >= 0)) throw std::runtime_error(where + ": removal_s must be seconds, got '" + t + "'");
        }
        r.notes = at(c_notes);
    }
    return out;
}

}  // namespace fruit
