#include "support/SupportIO.h"

#include <cstdio>
#include <stdexcept>
#include <unordered_map>

namespace fruit {

using json = nlohmann::json;

static json vec(const Vec3& v) { return json::array({v.x, v.y, v.z}); }
static Vec3 vec(const json& j) { return {j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>()}; }

json pointsToJson(const std::vector<SupportPoint>& pts) {
    json arr = json::array();
    for (const auto& p : pts)
        arr.push_back({{"id", p.id},
                       {"pos", vec(p.pos)},
                       {"normal", vec(p.normal)},
                       {"tip_diameter", p.tip_diameter},
                       {"category", categoryName(p.category)},
                       {"pinned", p.pinned},
                       {"layer", p.layer}});
    return {{"version", 1}, {"frame", "printer"}, {"units", "mm"}, {"points", arr}};
}

std::vector<SupportPoint> pointsFromJson(const json& j) {
    std::vector<SupportPoint> out;
    for (const auto& e : j.at("points")) {
        SupportPoint p;
        p.id = e.at("id").get<uint64_t>();
        p.pos = vec(e.at("pos"));
        if (e.contains("normal")) p.normal = vec(e.at("normal"));
        p.tip_diameter = e.value("tip_diameter", 0.4);
        p.category = categoryFromName(e.value("category", std::string("overhang")));
        p.pinned = e.value("pinned", false);
        p.layer = e.value("layer", 0);
        out.push_back(p);
    }
    return out;
}

std::vector<SupportPoint> applyEdits(const json& edits, const std::vector<SupportPoint>& previous,
                                     CosmeticSpec& cosmetic) {
    std::unordered_map<uint64_t, const SupportPoint*> by_id;
    for (const auto& p : previous) by_id[p.id] = &p;
    auto lookup = [&](uint64_t id) {
        auto it = by_id.find(id);
        if (it == by_id.end()) throw std::runtime_error("edit refers to unknown point id " + std::to_string(id));
        return it->second;
    };
    auto forbid = [&](const SupportPoint& p) { cosmetic.exclusions.push_back({p.pos, p.tip_diameter / 2 + 0.3}); };
    std::vector<SupportPoint> manual;
    auto addManual = [&](const Vec3& pos, double d) {
        SupportPoint m;
        m.pos = pos;
        m.tip_diameter = d;
        m.category = PointCategory::Manual;
        m.pinned = true;
        m.id = stablePointId(pos, PointCategory::Manual);
        manual.push_back(m);
    };
    // Previously manual points are kept: they are the user's.
    for (const auto& p : previous)
        if (p.category == PointCategory::Manual) addManual(p.pos, p.tip_diameter);
    if (edits.contains("delete"))
        for (const auto& id : edits.at("delete")) {
            const SupportPoint* p = lookup(id.get<uint64_t>());
            forbid(*p);
            std::erase_if(manual, [&](const SupportPoint& m) { return distance(m.pos, p->pos) < 1e-6; });
        }
    if (edits.contains("move"))
        for (const auto& e : edits.at("move")) {
            const SupportPoint* p = lookup(e.at("id").get<uint64_t>());
            forbid(*p);
            std::erase_if(manual, [&](const SupportPoint& m) { return distance(m.pos, p->pos) < 1e-6; });
            addManual(vec(e.at("pos")), p->tip_diameter);
        }
    if (edits.contains("resize"))
        for (const auto& e : edits.at("resize")) {
            const SupportPoint* p = lookup(e.at("id").get<uint64_t>());
            forbid(*p);
            std::erase_if(manual, [&](const SupportPoint& m) { return distance(m.pos, p->pos) < 1e-6; });
            addManual(p->pos, e.at("tip_diameter").get<double>());
        }
    if (edits.contains("add"))
        for (const auto& e : edits.at("add")) addManual(vec(e.at("pos")), e.value("tip_diameter", 0.4));
    // A manual point inside an exclusion zone wins: drop zones that contain one.
    std::erase_if(cosmetic.exclusions, [&](const CosmeticSpec::Exclusion& z) {
        for (const auto& m : manual)
            if (distance(m.pos, z.center) <= z.radius) return true;
        return false;
    });
    return manual;
}

void writePointsPly(const std::string& path, const std::vector<SupportPoint>& pts) {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) throw std::runtime_error("cannot write " + path);
    const int nv = 6 * static_cast<int>(pts.size()), nf = 8 * static_cast<int>(pts.size());
    std::fprintf(f, "ply\nformat ascii 1.0\nelement vertex %d\nproperty float x\nproperty float y\nproperty float z\n"
                    "property uchar red\nproperty uchar green\nproperty uchar blue\nelement face %d\n"
                    "property list uchar int vertex_indices\nend_header\n", nv, nf);
    for (const auto& p : pts) {
        int r = 255, g = 255, b = 255;
        switch (p.category) {
            case PointCategory::Island: r = 255; g = 140; b = 0; break;        // orange
            case PointCategory::Overhang: r = 240; g = 220; b = 40; break;     // yellow
            case PointCategory::Capacity: r = 200; g = 60; b = 220; break;     // purple
            case PointCategory::Stabilization: r = 60; g = 200; b = 90; break; // green
            case PointCategory::Manual: r = 60; g = 140; b = 255; break;       // blue
        }
        const double s = std::max(0.15, p.tip_diameter / 2);
        const Vec3 o[6] = {{s, 0, 0}, {-s, 0, 0}, {0, s, 0}, {0, -s, 0}, {0, 0, s}, {0, 0, -s}};
        for (const auto& d : o) std::fprintf(f, "%.4f %.4f %.4f %d %d %d\n", p.pos.x + d.x, p.pos.y + d.y, p.pos.z + d.z, r, g, b);
    }
    const int tri[8][3] = {{0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4}, {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5}};
    for (size_t i = 0; i < pts.size(); ++i)
        for (const auto& t : tri)
            std::fprintf(f, "3 %zu %zu %zu\n", 6 * i + t[0], 6 * i + t[1], 6 * i + t[2]);
    std::fclose(f);
}

}  // namespace fruit
