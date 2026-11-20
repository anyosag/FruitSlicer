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

// Which sharp edges a query looks at: all, the outside ones (a ridge, the
// rim of a face; and edges that aren't between two faces), or the inside ones
// (a valley: an inside corner, where a face meets a wall that rises from it
// on its outer side, as a ceiling meets the wall going down beside it).
enum class EdgeSide { Both, Outside, Inside };

// Sharp mesh edges (dihedral angle above a threshold) in a hash grid, for
// "distance to nearest sharp edge" queries.
class SharpEdgeIndex {
public:
    SharpEdgeIndex() = default;
    SharpEdgeIndex(const TriangleMesh& mesh, double angle_deg, double cell = 1.0);
    // Distance to the nearest sharp edge on `side`, or `limit` if none is
    // closer. With `face`: only edges one of whose two faces faces within 37°
    // of it (an edge of a face like the one at `p`).
    double distance(const Vec3& p, double limit, EdgeSide side = EdgeSide::Both, const Vec3* face = nullptr) const;
    // The same, and where on that edge the nearest point is (`at`, left as it
    // was when no edge is nearer than `limit`).
    double nearest(const Vec3& p, double limit, Vec3* at, EdgeSide side = EdgeSide::Both, const Vec3* face = nullptr) const;
    size_t edgeCount() const { return segs_.size(); }

private:
    struct Seg { Vec3 a, b; bool inside = false; Vec3 n0, n1; };  // n0, n1: its faces' normals
    std::vector<Seg> segs_;
    double cell_ = 1.0;
    std::vector<std::pair<uint64_t, int32_t>> cells_;  // sorted (cell key, segment)
    uint64_t key(int64_t x, int64_t y, int64_t z) const;
};

struct CostField {
    const CosmeticSpec* cosmetic = nullptr;
    const SharpEdgeIndex* edges = nullptr;
    double cosmetic_weight = 4.0, edge_weight = 1.0, edge_margin = 0.2;
    double corner_factor = 1.0;  // SupportConfig::corner_factor

    bool isCosmetic(const Vec3& p, const Vec3& n) const;
    // Within the tip's radius plus edge_margin of an outside edge, where its
    // mark would show (an inside corner is not one).
    bool nearEdge(const Vec3& p, double tip_radius) const;
    // The same of an inside corner of the face at `p` (facing `n`): its mark
    // sits in the corner.
    bool inCorner(const Vec3& p, const Vec3& n, double tip_radius) const;
    bool excluded(const Vec3& p) const;
    // 1 (corner_factor in an inside corner), plus cosmetic_weight on a face
    // people see (but in a corner), plus edge_weight beside an outside edge.
    // A corner beside an outside edge too (a narrow ledge) is no corner here.
    double cost(const Vec3& p, const Vec3& n, double tip_radius) const;
};

}  // namespace fruit
