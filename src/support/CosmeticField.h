// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Where tips should preferably not touch: user cosmetic regions and sharp
// edges. Feeds the placement cost; it never forbids support outright except
// for explicit exclusion zones (deleted points from a manual edit).

#include <vector>

#include <nlohmann/json.hpp>

#include "geometry/BBox.h"
#include "mesh/TriangleMesh.h"

namespace fruit {

struct CosmeticSpec {
    std::vector<BBox3> keep_out_boxes;   // printer coordinates
    std::vector<Vec3> visible_from;      // faces whose normal · v > visible_cos are cosmetic
    double visible_cos = 0.3;
    struct Exclusion { Vec3 center; double radius; };
    std::vector<Exclusion> exclusions;   // hard: no auto tip inside

    // {"keep_out": [{"min":[..], "max":[..]}], "visible_from": [[x,y,z], ...]}
    static CosmeticSpec fromJson(const nlohmann::json& j);
};

// Sharp mesh edges (dihedral angle above a threshold) in a hash grid, for
// "distance to nearest sharp edge" queries.
class SharpEdgeIndex {
public:
    SharpEdgeIndex() = default;
    SharpEdgeIndex(const TriangleMesh& mesh, double angle_deg, double cell = 1.0);
    // Distance to the nearest sharp edge, or `limit` if none is closer.
    double distance(const Vec3& p, double limit) const;
    size_t edgeCount() const { return segs_.size(); }

private:
    struct Seg { Vec3 a, b; };
    std::vector<Seg> segs_;
    double cell_ = 1.0;
    std::vector<std::pair<uint64_t, int32_t>> cells_;  // sorted (cell key, segment)
    uint64_t key(int64_t x, int64_t y, int64_t z) const;
};

struct CostField {
    const CosmeticSpec* cosmetic = nullptr;
    const SharpEdgeIndex* edges = nullptr;
    double cosmetic_weight = 4.0, edge_weight = 1.0, edge_margin = 0.2;

    bool isCosmetic(const Vec3& p, const Vec3& n) const;
    bool nearEdge(const Vec3& p, double tip_radius) const;
    bool excluded(const Vec3& p) const;
    double cost(const Vec3& p, const Vec3& n, double tip_radius) const;
};

}  // namespace fruit
