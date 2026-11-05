#include "calibration/CalibrationPlates.h"

#include <cmath>
#include <functional>
#include <random>
#include <sstream>
#include <stdexcept>

#include "mesh/Primitives.h"
#include "support/SupportGeometry.h"

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
    appendRaft(m, ctr - Vec2{h, h}, ctr + Vec2{h, h}, 0.0, c.raft_thickness, 0.4);
    // "L": long arm along +X, short arm along +Y, from the cell's min corner.
    const double z = c.raft_thickness, t = 1.5, arm = 2 * h - 2;
    const Vec2 o = ctr - Vec2{h - 1, h - 1};
    appendBox(m, {o.x, o.y, z - 0.1}, {o.x + arm, o.y + t, z + 3});
    appendBox(m, {o.x, o.y, z - 0.1}, {o.x + t, o.y + arm * 0.6, z + 3});
}

// Pinhead at `contact` (outward normal `n`), vertical shaft, base cone on the raft.
void addSupport(TriangleMesh& m, const Vec3& contact, const Vec3& n, double tip_d, double pen,
                const PlateCommon& c, double raft_top) {
    PinheadSpec s;
    s.tip_diameter = tip_d;
    s.penetration = pen;
    s.head_length = c.head_length;
    s.shaft_diameter = std::max(c.shaft_diameter, tip_d * 1.5);
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
        appendRaft(plate.mesh, pc.center - Vec2{half, half}, pc.center + Vec2{half, half}, 0.0,
                   c.raft_thickness, 0.4);
        build(plate.mesh, pc.center, pc.params, c.raft_thickness);
        plate.coupons.push_back(std::move(pc));
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
    if (!coupons.empty())
        for (auto it = coupons.front().params.begin(); it != coupons.front().params.end(); ++it)
            keys.push_back(it.key());
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

CalibrationPlate tipsPlate(const TipsPlateOptions& o, const PrinterProfile& p) {
    std::vector<json> combos;
    double max_r = 0;
    for (double d : o.tip_diameters)
        for (double pen : o.penetrations)
            for (double load : o.loads_per_tip) {
                const double disc_area = load * o.tips_per_coupon;
                max_r = std::max(max_r, std::sqrt(disc_area / kPi));
                combos.push_back({{"tip_diameter", d}, {"penetration", pen}, {"load_per_tip_mm2", load},
                                  {"tips", o.tips_per_coupon}, {"disc_area_mm2", disc_area}});
            }
    const double cell = std::ceil(2 * max_r + 3.0);
    const double z0 = o.common.elevation;
    auto build = [&](TriangleMesh& m, const Vec2& c, const json& prm, double raft_top) {
        const double area_mm2 = prm["disc_area_mm2"].get<double>();
        const double r = std::sqrt(area_mm2 / kPi);
        appendFrustum(m, {c.x, c.y, z0}, r, {c.x, c.y, z0 + o.disc_thickness}, r, segmentsFor(r, 0.007));
        const int k = prm["tips"].get<int>();
        for (int i = 0; i < k; ++i) {
            const double a = deg2rad(90.0 + 360.0 * i / k);
            const double rr = k == 1 ? 0.0 : r / 2;
            addSupport(m, {c.x + rr * std::cos(a), c.y + rr * std::sin(a), z0}, {0, 0, -1},
                       prm["tip_diameter"].get<double>(), prm["penetration"].get<double>(), o.common, raft_top);
        }
    };
    CalibrationPlate plate = assemble("tips", combos, cell, o.common, p, build);
    plate.settings["disc_thickness"] = o.disc_thickness;
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

}  // namespace fruit
