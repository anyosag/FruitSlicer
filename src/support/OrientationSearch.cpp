#include "support/OrientationSearch.h"

#include <algorithm>
#include <cmath>

#include "layers/LayerStack.h"
#include "mesh/MeshAnalysis.h"
#include "mesh/Placement.h"

namespace fruit {

Vec3 eulerXYZ(const Transform& r) {
    const double sy = std::clamp(-r.m[2][0], -1.0, 1.0);
    const double ry = std::asin(sy);
    double rx, rz;
    if (std::abs(sy) < 0.999999) {
        rx = std::atan2(r.m[2][1], r.m[2][2]);
        rz = std::atan2(r.m[1][0], r.m[0][0]);
    } else {  // gimbal lock: fold Z into X
        rz = 0;
        rx = std::atan2(-r.m[1][2], r.m[1][1]);
    }
    return {rad2deg(rx), rad2deg(ry), rad2deg(rz)};
}

namespace {

// Rotation taking unit vector `from` onto unit vector `to` (Rodrigues).
Transform rotationBetween(const Vec3& from, const Vec3& to) {
    const Vec3 a = normalized(from), b = normalized(to);
    const Vec3 v = cross(a, b);
    const double c = dot(a, b);
    Transform t;
    if (c < -0.999999) {
        // 180°: rotate about any axis perpendicular to a.
        const Vec3 axis = normalized(std::abs(a.x) < 0.9 ? cross(a, Vec3{1, 0, 0}) : cross(a, Vec3{0, 1, 0}));
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) t.m[i][j] = 2 * axis[i] * axis[j] - (i == j ? 1.0 : 0.0);
        return t;
    }
    const double k = 1.0 / (1.0 + c);
    const double vx[3][3] = {{0, -v.z, v.y}, {v.z, 0, -v.x}, {-v.y, v.x, 0}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double vv = 0;
            for (int q = 0; q < 3; ++q) vv += vx[i][q] * vx[q][j];
            t.m[i][j] = (i == j ? 1.0 : 0.0) + vx[i][j] + vv * k;
        }
    return t;
}

std::vector<Vec3> fibonacciSphere(int n) {
    std::vector<Vec3> out;
    const double golden = kPi * (3.0 - std::sqrt(5.0));
    for (int i = 0; i < n; ++i) {
        const double z = 1.0 - 2.0 * (i + 0.5) / n;
        const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
        out.push_back({r * std::cos(golden * i), r * std::sin(golden * i), z});
    }
    return out;
}

void stageOne(const TriangleMesh& placed, const OrientOptions& o, const Transform& rot, OrientationResult& r) {
    const double s_crit = std::sin(deg2rad(o.cfg.critical_angle_deg));
    for (size_t f = 0; f < placed.faces.size(); ++f) {
        const Vec3 n = placed.faceNormal(f);
        if (n.z >= -s_crit) continue;  // leans less than the critical angle
        const double a = placed.faceArea(f);
        r.support_area += a;
        for (const Vec3& v : o.visible_from)
            if (dot(n, normalized(rot.applyLinear(v))) > o.visible_cos) { r.cosmetic_area += a; break; }
    }
    r.minima = findMinima(placed).size();
    const BBox3 b = placed.bbox();
    r.height = b.max.z;
    const LayerStack coarse = sliceModel(placed, o.proxy_layer);
    for (const auto& s : coarse.slices) r.peak_area = std::max(r.peak_area, area(s));
}

}  // namespace

std::vector<OrientationResult> searchOrientations(const TriangleMesh& model, const OrientOptions& o) {
    std::vector<OrientationResult> cands;
    // The input orientation first, then directions over the sphere.
    {
        OrientationResult r;
        r.is_input = true;
        r.down_model = {0, 0, -1};
        cands.push_back(r);
    }
    for (const Vec3& d : fibonacciSphere(o.directions)) {
        OrientationResult r;
        r.down_model = d;
        r.rotation = rotationBetween(d, {0, 0, -1});
        cands.push_back(r);
    }
    for (auto& c : cands) {
        c.euler_deg = eulerXYZ(c.rotation);
        TriangleMesh m = model;
        m.transform(c.rotation);
        placeOnPlate(m, {0, 0, 0}, o.elevation);
        stageOne(m, o, c.rotation, c);
    }
    // Normalise each proxy by its median over all candidates so the weights
    // compare like with like.
    auto median = [&](auto get) {
        std::vector<double> v;
        for (const auto& c : cands) v.push_back(get(c));
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return std::max(v[v.size() / 2], 1e-9);
    };
    const double m_area = median([](const OrientationResult& c) { return c.support_area; });
    const double m_min = median([](const OrientationResult& c) { return static_cast<double>(c.minima); });
    const double m_peak = median([](const OrientationResult& c) { return c.peak_area; });
    const double m_h = median([](const OrientationResult& c) { return c.height; });
    double total_area = 0;
    for (size_t f = 0; f < model.faces.size(); ++f) total_area += model.faceArea(f);
    const OrientWeights& w = o.weights;
    for (auto& c : cands)
        c.proxy = w.support_area * c.support_area / m_area + w.minima * c.minima / m_min +
                  w.peak_area * c.peak_area / m_peak + w.height * c.height / m_h +
                  w.cosmetic * c.cosmetic_area / std::max(total_area * 0.05, 1e-9);
    std::stable_sort(cands.begin(), cands.end(),
                     [](const OrientationResult& a, const OrientationResult& b) { return a.proxy < b.proxy; });

    // Stage 2: the real generator on the finalists (always including the input orientation).
    const int k = std::min<int>(o.finalists, static_cast<int>(cands.size()));
    std::vector<size_t> finalists;
    for (int i = 0; i < k; ++i) finalists.push_back(i);
    for (size_t i = 0; i < cands.size(); ++i)
        if (cands[i].is_input && i >= static_cast<size_t>(k)) finalists.push_back(i);
    for (size_t i : finalists) {
        OrientationResult& c = cands[i];
        TriangleMesh m = model;
        m.transform(c.rotation);
        placeOnPlate(m, {0, 0, 0}, o.elevation);
        CosmeticSpec cos;
        for (const Vec3& v : o.visible_from) cos.visible_from.push_back(normalized(c.rotation.applyLinear(v)));
        cos.visible_cos = o.visible_cos;
        PointGenOptions po;
        po.cosmetic = &cos;
        const PointGenResult res = generateSupportPoints(m, o.cfg, po);
        CostField cf;
        cf.cosmetic = &cos;
        c.evaluated = true;
        c.points = res.points.size();
        for (const auto& p : res.points) {
            c.cosmetic_points += cf.isCosmetic(p.pos, p.normal);
            c.islands += p.category == PointCategory::Island;
        }
        c.uncovered = res.stats.uncovered;
        c.capacity_deficits = res.stats.capacity_deficits;
    }
    // Rank finalists against the best finalist on each metric.
    double best_pts = 1e300, best_peak = 1e300, best_h = 1e300;
    for (size_t i : finalists) {
        best_pts = std::min(best_pts, static_cast<double>(std::max<size_t>(cands[i].points, 1)));
        best_peak = std::min(best_peak, std::max(cands[i].peak_area, 1e-9));
        best_h = std::min(best_h, std::max(cands[i].height, 1e-9));
    }
    for (size_t i : finalists) {
        OrientationResult& c = cands[i];
        c.score = static_cast<double>(c.points) / best_pts + w.cosmetic * static_cast<double>(c.cosmetic_points) / best_pts +
                  w.peak_area * c.peak_area / best_peak + w.height * c.height / best_h +
                  10.0 * static_cast<double>(c.uncovered + c.capacity_deficits) / best_pts;
    }
    std::vector<OrientationResult> out, rest;
    for (size_t i = 0; i < cands.size(); ++i) (cands[i].evaluated ? out : rest).push_back(cands[i]);
    std::stable_sort(out.begin(), out.end(),
                     [](const OrientationResult& a, const OrientationResult& b) { return a.score < b.score; });
    out.insert(out.end(), rest.begin(), rest.end());
    return out;
}

}  // namespace fruit
