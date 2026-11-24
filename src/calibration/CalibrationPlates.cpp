// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "calibration/CalibrationPlates.h"


#include <algorithm>
#include <cmath>
#include <functional>
#include <random>
#include <sstream>
#include <stdexcept>

#include "core/Parallel.h"
#include "layers/LayerStack.h"
#include "mesh/Primitives.h"
#include "ops/MeshOps.h"
#include "stability/Stability.h"
#include "support/PointGenerator.h"
#include "support/SupportGeometry.h"
#include "support/SupportTree.h"

namespace fruit {

using json = nlohmann::json;

namespace {

constexpr double kPlateMargin = 4.0;  // keep clear of the plate edge (mm)

// Grid of cells over the printable area; cell 0 is the orientation marker.
struct Layout {
    int rows = 0, cols = 0;
    double cell = 0;
    double x0 = 0, y0 = 0;

    Layout(double cell_size, const PrinterProfile& p) : cell(cell_size) {
        const double ux = p.build_x - 2 * kPlateMargin, uy = p.build_y - 2 * kPlateMargin;
        cols = static_cast<int>(std::floor(ux / cell));
        rows = static_cast<int>(std::floor(uy / cell));
        x0 = -cols * cell / 2;
        y0 = -rows * cell / 2;
    }
    int capacity() const { return rows * cols - 1; }
    Vec2 center(int row, int col) const { return {x0 + (col + 0.5) * cell, y0 + (row + 0.5) * cell}; }
};

// Deterministic Fisher–Yates: mt19937's output sequence is fixed by the
// standard, unlike std::shuffle / distributions.
template <typename T>
void shuffleDeterministic(std::vector<T>& v, uint32_t seed) {
    std::mt19937 rng(seed);
    for (size_t i = v.size(); i > 1; --i) std::swap(v[i - 1], v[rng() % i]);
}

void addMarker(TriangleMesh& m, const Layout& L, const PlateCommon& c) {
    const Vec2 ctr = L.center(0, 0);
    const double h = L.cell / 2 - 1.0;
    if (!c.one_raft) appendRaft(m, ctr - Vec2{h, h}, ctr + Vec2{h, h}, 0.0, c.raft_thickness, 0.4);
    // "L": long arm along +X, short arm along +Y, from the cell's min corner.
    const double z = c.raft_thickness, t = 1.5, arm = 2 * h - 2;
    const Vec2 o = ctr - Vec2{h - 1, h - 1};
    appendBox(m, {o.x, o.y, z - 0.1}, {o.x + arm, o.y + t, z + 3});
    appendBox(m, {o.x, o.y, z - 0.1}, {o.x + t, o.y + arm * 0.6, z + 3});
}

// One raft under the cells at `centers` (each `cell` wide), all one piece but
// no solid slab: a pad under each cell, as the small rafts were, the pads in
// each row joined by a bar along the row, and the rows by one bar down the
// first column. The channels between the rows stay open at the far end, so
// resin flows in under each layer as it lifts and no closed pocket forms
// (a solid 190 × 106 mm slab would pull on the film as one suction cup).
void oneRaft(TriangleMesh& m, const std::vector<Vec2>& centers, double cell, const PlateCommon& c) {
    if (centers.empty()) return;
    const double h = cell / 2 - 1.0, bar = 2.0;
    double x0 = centers.front().x, x1 = x0, y0 = centers.front().y, y1 = y0;
    std::vector<double> rows;
    for (const Vec2& q : centers) {
        appendRaft(m, q - Vec2{h, h}, q + Vec2{h, h}, 0.0, c.raft_thickness, 0.4);
        x0 = std::min(x0, q.x), x1 = std::max(x1, q.x), y0 = std::min(y0, q.y), y1 = std::max(y1, q.y);
        if (std::none_of(rows.begin(), rows.end(), [&](double y) { return std::abs(y - q.y) < 1e-6; })) rows.push_back(q.y);
    }
    for (double y : rows) appendRaft(m, {x0, y - bar / 2}, {x1, y + bar / 2}, 0.0, c.raft_thickness, 0.4);
    appendRaft(m, {x0 - bar / 2, y0}, {x0 + bar / 2, y1}, 0.0, c.raft_thickness, 0.4);
}

// Pinhead at `contact` (outward normal `n`), vertical shaft, base cone on the raft.
void addSupport(TriangleMesh& m, const Vec3& contact, const Vec3& n, double tip_d, double pen,
                const PlateCommon& c, double raft_top, double neck_back = 0, double neck_ratio = 0.6) {
    PinheadSpec s;
    s.tip_diameter = tip_d;
    s.penetration = pen;
    s.neck_back = neck_back;
    s.neck_ratio = neck_ratio;
    s.head_length = c.head_length;
    s.shaft_diameter = c.shafts_as_jobs ? shaftDiameterFor(tip_d) : std::max(c.shaft_diameter, tip_d * 1.5);
    const Vec3 dir = headDirection(n);
    const Vec3 j = appendPinhead(m, contact, dir, s);
    const double cone_h = 1.0;
    const double shaft_bottom_z = raft_top + cone_h;
    if (j.z > shaft_bottom_z) {
        appendShaft(m, j, {j.x, j.y, shaft_bottom_z}, s.shaft_diameter, {}, false);
        appendBaseCone(m, {j.x, j.y, shaft_bottom_z}, s.shaft_diameter, 2.5, raft_top - 0.2);
    } else {
        appendBaseCone(m, j, s.shaft_diameter, 2.5, raft_top - 0.2);
    }
}

void checkCommon(const PlateCommon& c, const PrinterProfile& p) {
    if (c.raft_thickness < p.minRaftThickness())
        throw std::runtime_error("raft thinner than bottom + transition layers");
    if (c.elevation < c.raft_thickness + c.head_length + 1.0)
        throw std::runtime_error("elevation too low for raft + head");
}

CalibrationPlate assemble(const std::string& kind, const std::vector<json>& combos, double cell,
                          const PlateCommon& c, const PrinterProfile& p,
                          const std::function<void(TriangleMesh&, const Vec2&, const json&, double)>& build) {
    checkCommon(c, p);
    Layout L(cell, p);
    std::vector<json> trials;
    for (int r = 0; r < std::max(1, c.replicates); ++r)
        for (const auto& cb : combos) trials.push_back(cb);
    if (static_cast<int>(trials.size()) > L.capacity()) {
        std::ostringstream msg;
        msg << kind << " plate: " << trials.size() << " coupons but only " << L.capacity()
            << " cells fit; reduce parameters or replicates";
        throw std::runtime_error(msg.str());
    }
    shuffleDeterministic(trials, c.seed);

    CalibrationPlate plate;
    plate.kind = kind;
    plate.rows = L.rows;
    plate.cols = L.cols;
    plate.cell = cell;
    plate.settings = {{"elevation", c.elevation}, {"raft_thickness", c.raft_thickness},
                      {"shaft_diameter", c.shaft_diameter}, {"head_length", c.head_length},
                      {"seed", c.seed}, {"replicates", c.replicates}, {"printer", p.name}};
    addMarker(plate.mesh, L, c);
    const double half = cell / 2 - 1.0;
    for (size_t i = 0; i < trials.size(); ++i) {
        const int slot = static_cast<int>(i) + 1;  // skip the marker cell
        PlateCoupon pc;
        pc.id = static_cast<int>(i) + 1;
        pc.row = slot / L.cols;
        pc.col = slot % L.cols;
        pc.center = L.center(pc.row, pc.col);
        pc.params = trials[i];
        if (!c.one_raft)
            appendRaft(plate.mesh, pc.center - Vec2{half, half}, pc.center + Vec2{half, half}, 0.0, c.raft_thickness, 0.4);
        build(plate.mesh, pc.center, pc.params, c.raft_thickness);
        plate.coupons.push_back(std::move(pc));
    }
    if (c.one_raft) {
        std::vector<Vec2> at{L.center(0, 0)};
        for (const auto& pc : plate.coupons) at.push_back(pc.center);
        oneRaft(plate.mesh, at, cell, c);
        plate.settings["one_raft"] = true;
    }
    return plate;
}

}  // namespace

json CalibrationPlate::manifest() const {
    json j;
    j["kind"] = kind;
    j["settings"] = settings;
    j["grid"] = {{"rows", rows}, {"cols", cols}, {"cell_mm", cell},
                 {"marker", "cell row 0 col 0: L-shaped marker; long arm points to +col, short arm to +row"}};
    json list = json::array();
    for (const auto& c : coupons)
        list.push_back({{"id", c.id}, {"row", c.row}, {"col", c.col}, {"center", {c.center.x, c.center.y}},
                        {"params", c.params}});
    j["coupons"] = list;
    j["record"] =
        "Fill in the CSV: result = ok / failed / partial (failed = detached or missing; check the film "
        "for cured residue), scar = 1 (invisible) .. 5 (pit or torn skin) judged under raking light at "
        "fixed magnification, ideally blind to the parameters. Note resin, layer height, exposure, vat "
        "and room temperature.";
    return j;
}

std::string CalibrationPlate::csvTemplate() const {
    std::ostringstream out;
    std::vector<std::string> keys;
    // One column a setting; a list (a tile's tip loads) stays in the manifest: its commas would split it.
    if (!coupons.empty())
        for (auto it = coupons.front().params.begin(); it != coupons.front().params.end(); ++it)
            if (it.value().is_primitive()) keys.push_back(it.key());
    out << "id,row,col";
    for (const auto& k : keys) out << "," << k;
    out << ",result,scar,notes\n";
    for (const auto& c : coupons) {
        out << c.id << "," << c.row << "," << c.col;
        for (const auto& k : keys) {
            const json& v = c.params.contains(k) ? c.params.at(k) : json();
            out << "," << (v.is_string() ? v.get<std::string>() : v.dump());
        }
        out << ",,,\n";
    }
    return out.str();
}

// Where a point lies along the peel axis, from -1 at the hinge's edge to +1 at
// the far edge (as the planner's peelFactor measures it).
double peelT(const Vec2& xy, const Vec2& axis, const PrinterProfile& p) {
    const Vec2 a = normalized(axis);
    const double half = std::max(std::abs(a.x) * p.build_x / 2 + std::abs(a.y) * p.build_y / 2, 1.0);
    return std::clamp(dot(xy, a) / half, -1.0, 1.0);
}

// Two sets of `combos`, each shuffled on its own: one in the cells nearest the
// hinge's edge, one in those nearest the far edge. Each coupon's parameters
// say which ("side": "hinge" or "far") and where along the peel axis it is
// ("peel_t").
CalibrationPlate assembleSides(const std::string& kind, const std::vector<json>& combos, double cell,
                               const PlateCommon& c, const PrinterProfile& p, const Vec2& axis,
                               const std::function<void(TriangleMesh&, const Vec2&, const json&, double)>& build,
                               const std::vector<json>& middle = {}) {
    checkCommon(c, p);
    Layout L(cell, p);
    std::vector<json> one;
    for (int r = 0; r < std::max(1, c.replicates); ++r)
        for (const auto& cb : combos) one.push_back(cb);
    if (2 * static_cast<int>(one.size() + middle.size()) > L.capacity() + static_cast<int>(middle.size())) {
        std::ostringstream msg;
        msg << kind << " plate: two sets of " << one.size() << " coupons but only " << L.capacity() << " cells fit";
        throw std::runtime_error(msg.str());
    }
    // The cells, the marker's left out, from the hinge's edge to the far one.
    struct Slot { int row, col; double t; };
    std::vector<Slot> slots;
    for (int r = 0; r < L.rows; ++r)
        for (int k = 0; k < L.cols; ++k)
            if (r != 0 || k != 0) slots.push_back({r, k, peelT(L.center(r, k), axis, p)});
    std::stable_sort(slots.begin(), slots.end(), [](const Slot& a, const Slot& b) { return a.t < b.t; });
    CalibrationPlate plate;
    plate.kind = kind;
    plate.rows = L.rows;
    plate.cols = L.cols;
    plate.cell = cell;
    plate.settings = {{"elevation", c.elevation}, {"raft_thickness", c.raft_thickness},
                      {"shaft_diameter", c.shaft_diameter}, {"head_length", c.head_length},
                      {"seed", c.seed}, {"replicates", c.replicates}, {"printer", p.name},
                      {"sides", true}, {"peel_axis", {axis.x, axis.y}}};
    addMarker(plate.mesh, L, c);
    const double half = cell / 2 - 1.0;
    int id = 0;
    for (const int side : {0, 1}) {  // 0: by the hinge, 1: far from it
        std::vector<json> trials = one;
        shuffleDeterministic(trials, c.seed + static_cast<uint32_t>(side));
        // This side's cells: the nearest to its edge, in plate order (rows, then columns).
        std::vector<Slot> mine(side == 0 ? slots.begin() : slots.end() - static_cast<long>(trials.size()),
                               side == 0 ? slots.begin() + static_cast<long>(trials.size()) : slots.end());
        std::sort(mine.begin(), mine.end(), [](const Slot& a, const Slot& b) { return a.row != b.row ? a.row < b.row : a.col < b.col; });
        for (size_t i = 0; i < trials.size(); ++i) {
            PlateCoupon pc;
            pc.id = ++id;
            pc.row = mine[i].row;
            pc.col = mine[i].col;
            pc.center = L.center(pc.row, pc.col);
            pc.params = trials[i];
            pc.params["side"] = side == 0 ? "hinge" : "far";
            pc.params["peel_t"] = std::round(mine[i].t * 1000) / 1000;
            if (!c.one_raft)
                appendRaft(plate.mesh, pc.center - Vec2{half, half}, pc.center + Vec2{half, half}, 0.0, c.raft_thickness, 0.4);
            build(plate.mesh, pc.center, pc.params, c.raft_thickness);
            plate.coupons.push_back(std::move(pc));
        }
    }
    // Then `middle`, shuffled, in the cells left nearest the middle of the plate along the peel.
    if (!middle.empty()) {
        std::vector<Slot> rest(slots.begin() + static_cast<long>(one.size()), slots.end() - static_cast<long>(one.size()));
        std::stable_sort(rest.begin(), rest.end(), [](const Slot& a, const Slot& b) { return std::abs(a.t) < std::abs(b.t); });
        rest.resize(std::min(rest.size(), middle.size()));
        std::sort(rest.begin(), rest.end(), [](const Slot& a, const Slot& b) { return a.row != b.row ? a.row < b.row : a.col < b.col; });
        std::vector<json> trials = middle;
        shuffleDeterministic(trials, c.seed + 2);
        for (size_t i = 0; i < trials.size() && i < rest.size(); ++i) {
            PlateCoupon pc;
            pc.id = ++id;
            pc.row = rest[i].row;
            pc.col = rest[i].col;
            pc.center = L.center(pc.row, pc.col);
            pc.params = trials[i];
            pc.params["side"] = "middle";
            pc.params["peel_t"] = std::round(rest[i].t * 1000) / 1000;
            if (!c.one_raft)
                appendRaft(plate.mesh, pc.center - Vec2{half, half}, pc.center + Vec2{half, half}, 0.0, c.raft_thickness, 0.4);
            build(plate.mesh, pc.center, pc.params, c.raft_thickness);
            plate.coupons.push_back(std::move(pc));
        }
    }
    std::sort(plate.coupons.begin(), plate.coupons.end(), [](const PlateCoupon& a, const PlateCoupon& b) { return a.id < b.id; });
    if (c.one_raft) {
        std::vector<Vec2> at{L.center(0, 0)};
        for (const auto& pc : plate.coupons) at.push_back(pc.center);
        oneRaft(plate.mesh, at, cell, c);
        plate.settings["one_raft"] = true;
    }
    return plate;
}

CalibrationPlate discTipsPlate(const DiscTipsPlateOptions& o, const PrinterProfile& p) {
    const bool sides = norm(o.peel_axis) > 0;
    if (o.ladders.empty()) throw std::runtime_error("tips plate: no loads");
    // A size's ladders; a size with none, the nearest size's, scaled by tip area (as the starting values
    // scale, CapacityModel), to 0.05 mm².
    auto laddersFor = [&](double d) {
        double near = o.ladders.front().tip;
        for (const auto& l : o.ladders)
            if (std::abs(l.tip - d) < std::abs(near - d)) near = l.tip;
        const double k = (d / near) * (d / near);
        auto scale = [&](std::vector<double> v) {
            if (std::abs(near - d) > 1e-9)
                for (double& x : v) x = std::max(0.05, std::round(x * k * 20) / 20);
            return v;
        };
        std::vector<DiscTipsPlateOptions::Ladder> out;
        for (const auto& l : o.ladders)
            if (std::abs(l.tip - near) < 1e-9) out.push_back({d, l.depth, scale(l.loads), scale(l.extra)});
        return out;
    };
    double max_r = 0;
    auto coupon = [&](double d, double pen, double load) {
        const double disc_area = load * o.tips_per_coupon;
        max_r = std::max(max_r, std::sqrt(disc_area / kPi));
        return json{{"tip_diameter", d}, {"penetration", pen}, {"load_per_tip_mm2", load},
                    {"tips", o.tips_per_coupon}, {"disc_area_mm2", disc_area}};
    };
    // Per side (two sets) or per half (one set): every load once, the in-between ones twice.
    std::vector<json> combos;
    for (double d : o.tip_diameters)
        for (const auto& l : laddersFor(d)) {
            for (double load : l.loads) combos.push_back(coupon(d, l.depth, load));
            for (int k = 0; k < 2; ++k)
                for (double load : l.extra) combos.push_back(coupon(d, l.depth, load));
        }
    std::vector<json> necks;
    for (double d : o.tip_diameters)
        for (const auto& ns : o.neck_sets) {
            if (std::abs(ns.tip - d) > 1e-9) continue;
            double pen = 1e9;
            for (const auto& l : laddersFor(d)) pen = std::min(pen, l.depth);
            for (int k = 0; k < o.neck_copies; ++k)
                for (double back : o.neck_backs)
                    for (double load : ns.loads) {
                        json c = coupon(d, pen, load);
                        c["neck_back"] = back;
                        c["neck_ratio"] = o.neck_ratio;
                        necks.push_back(c);
                    }
        }
    const double cell = std::ceil(2 * max_r + 3.0);
    const double z0 = std::max(o.common.elevation, o.tip_height);
    PlateCommon heads = o.common;
    heads.shafts_as_jobs = true;  // each shaft as wide as a job's with that tip
    const double gap = TreeConfig{}.clearance;  // between shafts, as between a support and the model
    auto build = [&](TriangleMesh& m, const Vec2& c, const json& prm, double raft_top) {
        const double area_mm2 = prm["disc_area_mm2"].get<double>();
        const double r = std::sqrt(area_mm2 / kPi);
        const double tip = prm["tip_diameter"].get<double>();
        const int k = prm["tips"].get<int>();
        // Tips half way out (the spread that leaves the least of the disc far from a tip), and no closer
        // to each other than a shaft and the gap supports keep: at the lightest loads, half way out put
        // shafts 0-0.15 mm apart, and they would print as one trunk, unlike a job's supports.
        std::vector<Vec2> at;
        const double apart = shaftDiameterFor(tip) + gap;
        const double rr = k == 1 ? 0.0 : std::max(r / 2, apart / (2 * std::sin(kPi / k)));
        if (rr + tip / 2 > r + 1e-9) throw std::runtime_error("tips plate: a disc too small for its tips' shafts");
        for (int i = 0; i < k; ++i) {
            const double a = deg2rad(90.0 + 360.0 * i / k);
            at.push_back({c.x + rr * std::cos(a), c.y + rr * std::sin(a)});
        }
        // The disc's underside widens at 45° from a pad on each tip, so every layer stands on the one
        // below and no fresh skin hangs past the tips: the tips carry the disc's whole pull, and only that
        // is measured. (A flat disc's first layer reached 1.9-3.8 mm past its tips, beyond the 1.6 mm
        // a part is trusted with: it could fail as an overhang before the tips let go.)
        const double pad = tip / 2 + 0.1;
        double reach = 0;  // farthest point of the disc from its nearest tip: on its rim, or its centre
        for (int j = 0; j <= 360; ++j) {
            const Vec2 q = j == 360 ? c : Vec2{c.x + r * std::cos(deg2rad(j)), c.y + r * std::sin(deg2rad(j))};
            double nearest = 1e9;
            for (const Vec2& t : at) nearest = std::min(nearest, norm(q - t));
            reach = std::max(reach, nearest);
        }
        const double rise = std::max(0.0, std::ceil((reach - pad) * 20) / 20);
        if (rise > 0) {
            TriangleMesh cones, cyl;
            ops::Solid fan;
            for (const Vec2& t : at) {
                TriangleMesh one;
                appendFrustum(one, {t.x, t.y, z0}, pad, {t.x, t.y, z0 + rise}, pad + rise, segmentsFor(pad + rise, 0.007));
                const ops::Solid s(one);
                fan = fan.valid() ? ops::unite(fan, s) : s;
            }
            appendFrustum(cyl, {c.x, c.y, z0}, r, {c.x, c.y, z0 + rise}, r, segmentsFor(r, 0.007));
            const ops::Solid under = ops::intersect(fan, ops::Solid(cyl));
            if (!under.valid() || under.empty()) throw std::runtime_error("tips plate: a disc's underside could not be made");
            m.append(under.toMesh());
        }
        appendFrustum(m, {c.x, c.y, z0 + rise}, r, {c.x, c.y, z0 + rise + o.disc_thickness}, r, segmentsFor(r, 0.007));
        for (const Vec2& t : at)
            addSupport(m, {t.x, t.y, z0}, {0, 0, -1}, tip, prm["penetration"].get<double>(), heads, raft_top,
                       prm.value("neck_back", 0.0), prm.value("neck_ratio", 0.6));
    };
    PlateCommon common = o.common;
    common.replicates = 1;  // the copies are in the lists already
    common.one_raft = o.one_raft;
    common.elevation = z0;
    common.shafts_as_jobs = true;
    std::vector<json> all;
    if (!sides) {
        for (int k = 0; k < 2; ++k) all.insert(all.end(), combos.begin(), combos.end());
        all.insert(all.end(), necks.begin(), necks.end());
    }
    CalibrationPlate plate = sides ? assembleSides("tips", combos, cell, common, p, o.peel_axis, build, necks)
                                   : assemble("tips", all, cell, common, p, build);
    plate.settings["disc_thickness"] = o.disc_thickness;
    plate.settings["disc_underside"] = "45deg";
    plate.settings["shafts_as_jobs"] = true;  // 0.8 / 1.0 / 1.2 mm, by tip (shaftDiameterFor)
    plate.settings["tip_height"] = z0;
    return plate;
}

SupportConfig tileSupportConfig(double tip, double strength, const Vec2& peel_axis, double layer_height) {
    SupportConfig cfg;  // FruitSlicer's own, as a job with no resin values of its own starts
    cfg.layer_height = layer_height;
    // The hinge's extra pull far from it, as resolveSettings sets it for a job with the hinge known.
    if (norm(peel_axis) > 0) {
        cfg.peel_axis = normalized(peel_axis);
        cfg.peel_bias = kAssumedPeelBias;
    }
    // One size: the usual tip, the tip for spots in mid-air and the tip near edges alike, so a tip that would
    // carry too much gets a neighbour (as at the biggest size) and is not made bigger.
    cfg.tip_classes = {{"plate", tip}};
    cfg.default_tip = cfg.minima_tip = cfg.edge_tip = 0;
    // Trusted with `strength` times FruitSlicer's own value. The limit on each tip's own load stays the one
    // FruitSlicer's own table has (1): the strength is what changes.
    cfg.capacity.table = {{tip, strength * CapacityModel{}.capacity(tip)}};
    return cfg;
}

namespace {

// A tile and its supports, as a job of that tile gets them, and what the replay says each tip carries.
struct TileMade {
    TriangleMesh mesh;  // the tile and its supports, without a raft (the plate's raft is under them)
    json params;        // what the manifest keeps of it
};

TileMade makeTile(const PlateCoupon& pc, const TipsPlateOptions& o, const PrinterProfile& p, bool sided) {
    const double d = pc.params.at("tip_diameter").get<double>(), k = pc.params.at("strength").get<double>();
    const double s = pc.params.at("tile_mm").get<double>(), z0 = o.common.elevation;
    TriangleMesh tile = makeBox({pc.center.x - s / 2, pc.center.y - s / 2, z0},
                                {pc.center.x + s / 2, pc.center.y + s / 2, z0 + o.tile_thickness});
    const SupportConfig cfg = tileSupportConfig(d, k, sided ? o.peel_axis : Vec2{0, 0}, o.layer_height);
    TreeConfig tc;  // FruitSlicer's own heads, depths, shafts, trunks and feet
    tc.raft_thickness = o.common.raft_thickness;
    const double min_raft = p.minRaftThickness();
    PointGenOptions po;
    po.relieve = true;  // each tip's own load, as the app plans a job
    po.slice_threads = 1;
    po.support_lengths = [&](const std::vector<SupportPoint>& pts) {
        return supportLengthsOnModel(tile, pts, tc, min_raft);
    };
    const PointGenResult planned = generateSupportPoints(tile, cfg, po);
    const SupportTree tree = buildSupportTree(tile, planned.points, tc, min_raft);
    if (tree.heads.empty()) throw std::runtime_error("tips plate: a tile got no supports");

    // The replay of the print, as for a job: the tile, its supports and their own raft (what holds them up
    // while the tile prints). Only the loads are wanted, so one replay; the extra pull far from the hinge is
    // left out (the fit measures it).
    const TriangleMesh sup = tree.mesh(tc);
    const LayerStack sup_layers = sliceModel(sup, o.layer_height, nullptr, 1);
    const LayerStack& model_layers = *planned.analysis.layers;
    const LayerStack print = unionLayers(model_layers, sup_layers, 1);
    StabilityInput in;
    in.print = &print;
    in.model = &model_layers;
    in.supports = &sup_layers;
    in.model_graph = planned.analysis.islands;
    for (const Head& h : tree.heads) in.tips.push_back({h.contact, h.spec.tip_diameter});
    StabilityConfig sc = stabilityConfig(cfg);
    sc.peel_bias = 0;
    sc.replays = 1;
    sc.threads = 1;
    const StabilityResult res = analyzeStability(in, sc);
    std::vector<double> loads;
    for (const TipRisk& t : res.tips)
        if (t.contact >= 0 && t.contact < static_cast<int>(tree.heads.size()) && !t.foot) loads.push_back(t.load);
    if (loads.size() != tree.heads.size()) throw std::runtime_error("tips plate: the replay lost a tile's tips");
    std::sort(loads.rbegin(), loads.rend());
    double sum = 0;
    for (double l : loads) sum += std::pow(l, sc.steepness);
    auto round2 = [](double v) { return std::round(v * 100) / 100; };
    json each = json::array();
    for (double l : loads) each.push_back(round2(l));

    // A head the tree had to build smaller (no room at its own size) would make the tile two sizes.
    int smaller = 0;
    for (const Head& h : tree.heads) smaller += std::abs(h.spec.tip_diameter - d) > 1e-9 ? 1 : 0;

    TileMade out;
    out.mesh = std::move(tile);
    out.mesh.append(tree.mesh(tc, false));
    out.params = {{"penetration", tipPenetration(d, tc)},
                  {"tips", tree.heads.size()},
                  {"tips_planned", planned.points.size()},
                  {"tips_smaller", smaller},
                  {"assumed_mm2", round2(cfg.capacity.capacity(d))},
                  {"load_mm2", round2(std::pow(sum, 1 / sc.steepness))},
                  {"load_max_mm2", round2(loads.front())},
                  {"tip_loads_mm2", each}};
    return out;
}

}  // namespace

CalibrationPlate tipsPlate(const TipsPlateOptions& o, const PrinterProfile& p) {
    const bool sides = norm(o.peel_axis) > 0;
    if (o.sizes.empty()) throw std::runtime_error("tips plate: no tip sizes");
    double biggest = 0;
    std::vector<json> combos;
    for (const auto& sz : o.sizes) {
        if (sz.strengths.empty()) throw std::runtime_error("tips plate: a tip size with no strengths");
        biggest = std::max(biggest, sz.tile);
        for (double k : sz.strengths) combos.push_back({{"tip_diameter", sz.tip}, {"strength", k}, {"tile_mm", sz.tile}});
    }
    // A tile's supports reach about 2 mm past it where they stand (the feet of the trunks at its edges): the
    // cells leave room for them on the plate's raft, whose pad under each cell is 2 mm smaller than the cell.
    const double cell = std::ceil(biggest + 4.0);
    PlateCommon common = o.common;
    common.one_raft = true;
    std::vector<json> all = combos;
    if (!sides) all.insert(all.end(), combos.begin(), combos.end());  // one set: as many tiles as two would have
    // The cells and each tile's parameters first; then every tile with its supports, side by side.
    auto none = [](TriangleMesh&, const Vec2&, const json&, double) {};
    CalibrationPlate plate = sides ? assembleSides("tips", combos, cell, common, p, o.peel_axis, none)
                                   : assemble("tips", all, cell, common, p, none);
    std::vector<TileMade> made(plate.coupons.size());
    parallelFor(plate.coupons.size(), o.threads,
                [&](size_t i) { made[i] = makeTile(plate.coupons[i], o, p, sides); });
    for (size_t i = 0; i < made.size(); ++i) {
        plate.mesh.append(made[i].mesh);
        plate.coupons[i].params.update(made[i].params);
    }
    plate.settings["tiles"] = "flat tiles held by FruitSlicer's own tips and supports, one tip size a tile";
    plate.settings["tile_thickness"] = o.tile_thickness;
    plate.settings["lift"] = o.common.elevation;
    plate.settings["head_length"] = TreeConfig{}.head_length;
    plate.settings["shafts_as_jobs"] = true;  // the tree's own: 0.8 / 1.0 / 1.2 mm by tip (shaftDiameterFor)
    plate.settings["loads"] = "replayed";     // load_mm2 and the rest, from the replay of the print
    plate.settings["steepness"] = StabilityConfig{}.steepness;
    return plate;
}

CalibrationPlate spacingPlate(const SpacingPlateOptions& o, const PrinterProfile& p) {
    std::vector<json> combos;
    for (double a : o.angles)
        for (double s : o.spacings) combos.push_back({{"kind", "plate"}, {"angle_deg", a}, {"spacing_mm", s}});
    for (double w : o.wedge_angles) combos.push_back({{"kind", "wedge"}, {"angle_deg", w}, {"spacing_mm", 2.0}});
    const double wedge_h = 4.0, wedge_base = 6.0;
    double wedge_len = wedge_base;
    for (double w : o.wedge_angles) wedge_len = std::max(wedge_len, wedge_base + wedge_h * std::tan(deg2rad(w)));
    const double cell = std::ceil(std::max(o.plate_size, wedge_len) + 3.0);
    const double z0 = o.common.elevation;

    auto build = [&](TriangleMesh& m, const Vec2& c, const json& prm, double raft_top) {
        const double s = prm["spacing_mm"].get<double>();
        if (prm["kind"] == "plate") {
            const double a = prm["angle_deg"].get<double>();
            const Transform rot = Transform::rotationXYZ(a, 0, 0);
            const double L = o.plate_size / 2, t = o.plate_thickness;
            // Plate corners in local coordinates, rotated, then lifted so the
            // lowest corner sits at the elevation and centred on the cell.
            Vec3 v[8] = {{-L, -L, 0}, {L, -L, 0}, {L, L, 0}, {-L, L, 0},
                         {-L, -L, t}, {L, -L, t}, {L, L, t}, {-L, L, t}};
            double zmin = 1e9;
            Vec3 ctr{};
            for (auto& q : v) {
                q = rot.apply(q);
                zmin = std::min(zmin, q.z);
                ctr += q;
            }
            ctr = ctr / 8.0;
            const Vec3 shift{c.x - ctr.x, c.y - ctr.y, z0 - zmin};
            for (auto& q : v) q += shift;
            appendHexahedron(m, v);
            const Vec3 normal = rot.applyLinear({0, 0, -1});
            // Grid anchored at the low edge (local -Y), which is the first
            // part of a tilted plate to print and must always be held; the
            // high edge is left to cantilever up to one spacing.
            // For a tilted plate the low edge itself is the surface minimum,
            // so the first row sits on it (the sphere penetrates the edge).
            const double edge = 0.5;
            const int n = static_cast<int>(std::floor((o.plate_size - 2 * edge) / s)) + 1;
            const int nv = static_cast<int>(std::floor((o.plate_size - edge) / s)) + 1;
            const double start_u = -(n - 1) * s / 2;
            const double first_v = a > 0 ? -L : -L + edge;
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < nv; ++j) {
                    const Vec3 local{start_u + i * s, first_v + j * s, 0};
                    addSupport(m, rot.apply(local) + shift, normal, o.tip_diameter, o.penetration, o.common, raft_top);
                }
        } else {
            // Wedge: flat supported base, +X face leaning out at the angle
            // from vertical, unsupported.
            const double w = prm["angle_deg"].get<double>();
            const double top_x = wedge_base + wedge_h * std::tan(deg2rad(w));
            const double ox = c.x - top_x / 2, oy = c.y - wedge_base / 2;
            const Vec3 v[8] = {{ox, oy, z0},
                               {ox + wedge_base, oy, z0},
                               {ox + wedge_base, oy + wedge_base, z0},
                               {ox, oy + wedge_base, z0},
                               {ox, oy, z0 + wedge_h},
                               {ox + top_x, oy, z0 + wedge_h},
                               {ox + top_x, oy + wedge_base, z0 + wedge_h},
                               {ox, oy + wedge_base, z0 + wedge_h}};
            appendHexahedron(m, v);
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    addSupport(m, {ox + 1 + i * s, oy + 1 + j * s, z0}, {0, 0, -1}, o.tip_diameter, o.penetration,
                               o.common, raft_top);
        }
    };
    CalibrationPlate plate = assemble("spacing", combos, cell, o.common, p, build);
    plate.settings["plate_size"] = o.plate_size;
    plate.settings["plate_thickness"] = o.plate_thickness;
    plate.settings["tip_diameter"] = o.tip_diameter;
    plate.settings["penetration"] = o.penetration;
    return plate;
}

CalibrationPlate islandsPlate(const IslandsPlateOptions& o, const PrinterProfile& p) {
    std::vector<json> combos;
    json skipped = json::array();
    for (double D : o.island_diameters)
        for (int k : o.tip_counts) {
            // Largest tip that fits k tips inside the island footprint.
            const double bound = k <= 1 ? D : (k == 2 ? D / 2 : D / 3);
            // Small tolerance so e.g. 0.3 / 3 is not rejected against a 0.1 minimum.
            const double tip = std::min(o.tip_diameter, bound);
            if (k > 0 && tip < o.min_tip_diameter - 1e-9) {
                skipped.push_back({{"island_diameter", D}, {"tips", k}});
                continue;
            }
            combos.push_back({{"island_diameter", D}, {"tips", k}, {"tip_diameter", tip}});
        }
    const double cell = 8.0;
    const double z0 = o.common.elevation;
    auto build = [&](TriangleMesh& m, const Vec2& c, const json& prm, double raft_top) {
        const double D = prm["island_diameter"].get<double>();
        const int k = prm["tips"].get<int>();
        const double tip = prm["tip_diameter"].get<double>();
        appendFrustum(m, {c.x, c.y, z0}, D / 2, {c.x, c.y, z0 + o.column_height}, D / 2, segmentsFor(D / 2, 0.007));
        const double ring = k == 1 ? 0.0 : (k == 2 ? D / 4 : D / 3);
        for (int i = 0; i < k; ++i) {
            const double a = deg2rad(90.0 + 360.0 * i / k);
            addSupport(m, {c.x + ring * std::cos(a), c.y + ring * std::sin(a), z0}, {0, 0, -1}, tip, o.penetration,
                       o.common, raft_top);
        }
    };
    CalibrationPlate plate = assemble("islands", combos, cell, o.common, p, build);
    plate.settings["column_height"] = o.column_height;
    plate.settings["penetration"] = o.penetration;
    plate.settings["skipped_combinations"] = skipped;
    return plate;
}

CalibrationPlate gapsPlate(const GapsPlateOptions& o, const PrinterProfile& p) {
    std::vector<json> combos;
    for (const std::string& place : o.places)
        for (double h : o.heights)
            for (double g : o.gaps) combos.push_back({{"gap", g}, {"height", h}, {"place", place}});
    const double cell = 10.0;
    const double r = o.shaft_diameter / 2, t = o.wall;
    auto build = [&](TriangleMesh& m, const Vec2& c, const json& prm, double raft_top) {
        const double g = prm["gap"].get<double>(), h = prm["height"].get<double>();
        const double z0 = raft_top - 0.05;  // into the raft
        const int seg = segmentsFor(r, 0.007);
        if (prm["place"].get<std::string>() == "cup") {
            // A cup 4 × 4 mm inside, its walls `t` thick, capped 1 mm above the
            // shaft's top; a door 2 mm wide and 1.5 mm tall in its front wall,
            // by the raft, is its only opening. The shaft stands `g` from the
            // back wall.
            const double in = 2.0, top = raft_top + h + 1.0;
            appendBox(m, {c.x - in - t, c.y - in, z0}, {c.x - in, c.y + in, top});      // left
            appendBox(m, {c.x + in, c.y - in, z0}, {c.x + in + t, c.y + in, top});      // right
            appendBox(m, {c.x - in - t, c.y + in, z0}, {c.x + in + t, c.y + in + t, top});  // back
            appendBox(m, {c.x - in - t, c.y - in - t, z0}, {c.x - 1.0, c.y - in, top});      // front, left of the door
            appendBox(m, {c.x + 1.0, c.y - in - t, z0}, {c.x + in + t, c.y - in, top});      // front, right of it
            appendBox(m, {c.x - 1.0, c.y - in - t, raft_top + 1.5}, {c.x + 1.0, c.y - in, top});  // over the door
            appendBox(m, {c.x - in - t, c.y - in - t, top}, {c.x + in + t, c.y + in + t, top + 1.0});  // the cap
            const double y = c.y + in - g - r;
            appendFrustum(m, {c.x, y, z0}, r, {c.x, y, raft_top + h}, r, seg);
        } else {
            // A wall 6 mm long, `t` thick, 1 mm taller than the shaft; the shaft `g` in front of it.
            appendBox(m, {c.x - 3.0, c.y - t, z0}, {c.x + 3.0, c.y, raft_top + h + 1.0});
            const double y = c.y + g + r;
            appendFrustum(m, {c.x, y, z0}, r, {c.x, y, raft_top + h}, r, seg);
        }
    };
    PlateCommon common = o.common;
    common.elevation = std::max(common.elevation, common.raft_thickness + common.head_length + 1.0);
    CalibrationPlate plate = assemble("gaps", combos, cell, common, p, build);
    plate.settings["shaft_diameter"] = o.shaft_diameter;
    plate.settings["wall"] = o.wall;
    plate.settings["pixel"] = std::max(p.pixelX(), p.pixelY());
    return plate;
}

}  // namespace fruit
