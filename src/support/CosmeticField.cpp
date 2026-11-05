#include "support/CosmeticField.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace fruit {

CosmeticSpec CosmeticSpec::fromJson(const nlohmann::json& j) {
    CosmeticSpec s;
    if (j.contains("keep_out"))
        for (const auto& b : j.at("keep_out")) {
            BBox3 box;
            box.extend({b.at("min")[0].get<double>(), b.at("min")[1].get<double>(), b.at("min")[2].get<double>()});
            box.extend({b.at("max")[0].get<double>(), b.at("max")[1].get<double>(), b.at("max")[2].get<double>()});
            s.keep_out_boxes.push_back(box);
        }
    if (j.contains("visible_from"))
        for (const auto& v : j.at("visible_from"))
            s.visible_from.push_back(normalized(Vec3{v[0].get<double>(), v[1].get<double>(), v[2].get<double>()}));
    if (j.contains("visible_cos")) s.visible_cos = j.at("visible_cos").get<double>();
    return s;
}

uint64_t SharpEdgeIndex::key(int64_t x, int64_t y, int64_t z) const {
    const uint64_t ux = static_cast<uint64_t>(x + (1 << 20)) & 0x1FFFFF;
    const uint64_t uy = static_cast<uint64_t>(y + (1 << 20)) & 0x1FFFFF;
    const uint64_t uz = static_cast<uint64_t>(z + (1 << 20)) & 0x1FFFFF;
    return (ux << 42) | (uy << 21) | uz;
}

SharpEdgeIndex::SharpEdgeIndex(const TriangleMesh& mesh, double angle_deg, double cell) : cell_(cell) {
    // Edge -> adjacent faces.
    std::map<std::pair<int32_t, int32_t>, std::vector<int32_t>> edge_faces;
    for (size_t f = 0; f < mesh.faces.size(); ++f)
        for (int e = 0; e < 3; ++e) {
            int32_t a = mesh.faces[f][e], b = mesh.faces[f][(e + 1) % 3];
            if (a > b) std::swap(a, b);
            edge_faces[{a, b}].push_back(static_cast<int32_t>(f));
        }
    const double cos_t = std::cos(deg2rad(angle_deg));
    for (const auto& [e, fs] : edge_faces) {
        bool sharp = fs.size() != 2;  // boundary / non-manifold edges count as sharp
        if (!sharp) sharp = dot(mesh.faceNormal(fs[0]), mesh.faceNormal(fs[1])) < cos_t;
        if (!sharp) continue;
        // Split long edges so each piece touches only a few grid cells.
        const Vec3 a = mesh.vertices[e.first], b = mesh.vertices[e.second];
        const int pieces = std::max(1, static_cast<int>(std::ceil(fruit::distance(a, b) / cell_)));
        for (int k = 0; k < pieces; ++k)
            segs_.push_back({a + (b - a) * (double(k) / pieces), a + (b - a) * (double(k + 1) / pieces)});
    }
    for (size_t i = 0; i < segs_.size(); ++i) {
        const Vec3 lo = min(segs_[i].a, segs_[i].b), hi = max(segs_[i].a, segs_[i].b);
        for (int64_t x = static_cast<int64_t>(std::floor(lo.x / cell_)); x <= static_cast<int64_t>(std::floor(hi.x / cell_)); ++x)
            for (int64_t y = static_cast<int64_t>(std::floor(lo.y / cell_)); y <= static_cast<int64_t>(std::floor(hi.y / cell_)); ++y)
                for (int64_t z = static_cast<int64_t>(std::floor(lo.z / cell_)); z <= static_cast<int64_t>(std::floor(hi.z / cell_)); ++z)
                    cells_.emplace_back(key(x, y, z), static_cast<int32_t>(i));
    }
    std::sort(cells_.begin(), cells_.end());
}

static double segDistance(const Vec3& p, const Vec3& a, const Vec3& b) {
    const Vec3 ab = b - a;
    const double l2 = norm2(ab);
    const double t = l2 > 0 ? std::clamp(dot(p - a, ab) / l2, 0.0, 1.0) : 0.0;
    return norm(p - (a + ab * t));
}

double SharpEdgeIndex::distance(const Vec3& p, double limit) const {
    if (segs_.empty()) return limit;
    double best = limit;
    const int64_t r = static_cast<int64_t>(std::ceil(limit / cell_));
    const int64_t cx = static_cast<int64_t>(std::floor(p.x / cell_));
    const int64_t cy = static_cast<int64_t>(std::floor(p.y / cell_));
    const int64_t cz = static_cast<int64_t>(std::floor(p.z / cell_));
    for (int64_t x = cx - r; x <= cx + r; ++x)
        for (int64_t y = cy - r; y <= cy + r; ++y)
            for (int64_t z = cz - r; z <= cz + r; ++z) {
                const uint64_t k = key(x, y, z);
                auto it = std::lower_bound(cells_.begin(), cells_.end(), std::make_pair(k, int32_t{-1}));
                for (; it != cells_.end() && it->first == k; ++it)
                    best = std::min(best, segDistance(p, segs_[it->second].a, segs_[it->second].b));
            }
    return best;
}

bool CostField::isCosmetic(const Vec3& p, const Vec3& n) const {
    if (!cosmetic) return false;
    for (const auto& b : cosmetic->keep_out_boxes)
        if (p.x >= b.min.x && p.x <= b.max.x && p.y >= b.min.y && p.y <= b.max.y && p.z >= b.min.z && p.z <= b.max.z)
            return true;
    for (const auto& v : cosmetic->visible_from)
        if (dot(n, v) > cosmetic->visible_cos) return true;
    return false;
}

bool CostField::nearEdge(const Vec3& p, double tip_radius) const {
    if (!edges) return false;
    const double lim = tip_radius + edge_margin;
    return edges->distance(p, lim + 1e-9) <= lim;
}

bool CostField::excluded(const Vec3& p) const {
    if (!cosmetic) return false;
    for (const auto& e : cosmetic->exclusions)
        if (distance(p, e.center) <= e.radius) return true;
    return false;
}

double CostField::cost(const Vec3& p, const Vec3& n, double tip_radius) const {
    double c = 1.0;
    if (isCosmetic(p, n)) c += cosmetic_weight;
    if (nearEdge(p, tip_radius)) c += edge_weight;
    return c;
}

}  // namespace fruit
