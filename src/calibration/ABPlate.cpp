// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "calibration/ABPlate.h"

#include <cmath>
#include <random>
#include <sstream>
#include <stdexcept>

#include "mesh/Primitives.h"

namespace fruit {

ABPlate makeABPlate(const TriangleMesh& model, const TriangleMesh& supports, const PrinterProfile& printer,
                    const ABPlateOptions& o) {
    if (o.copies < 2) throw std::runtime_error("an A/B plate needs at least 2 copies");
    BBox3 fp = model.bbox();
    fp.extend(supports.bbox());
    const double cx = fp.size().x + o.margin, cy = fp.size().y + o.margin;
    const double edge = 4.0;
    const int cols = static_cast<int>(std::floor((printer.build_x - 2 * edge) / cx));
    const int rows = static_cast<int>(std::floor((printer.build_y - 2 * edge) / cy));
    const int capacity = rows * cols - 1;  // cell 0 holds the marker
    if (capacity < o.copies) {
        std::ostringstream m;
        m << o.copies << " copies requested but only " << std::max(0, capacity) << " fit (each needs "
          << cx << " × " << cy << " mm)";
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

}  // namespace fruit
