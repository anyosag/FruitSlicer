// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "support/SupportCheck.h"

#include <algorithm>
#include <cstdio>

#include "mesh/AabbTree.h"
#include "support/CosmeticField.h"
#include "support/DetailMap.h"
#include "support/PointGenerator.h"

namespace fruit {

namespace {

std::string fmt2(const char* f, double a, double b = 0, double c = 0) {
    char buf[256];
    std::snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}

// A straight piece of a support: from `a` to `b`, round, radius `r`.
struct Piece {
    Vec3 a, b;
    double r;
};

}  // namespace

GapCheck checkSupportGaps(const TriangleMesh& model, const SupportTree& tree, const TreeConfig& tc,
                          const SupportConfig& cfg, const DetailMap* details) {
    if (model.faces.empty()) return {};
    AabbTree at;
    at.build(model);
    const SharpEdgeIndex edges(model, cfg.sharp_edge_angle_deg);
    return checkSupportGaps(model, at, edges, tree, tc, cfg, details);
}

GapCheck checkSupportGaps(const TriangleMesh& model, const AabbTree& at, const SharpEdgeIndex& edges,
                          const SupportTree& tree, const TreeConfig& tc, const SupportConfig& cfg,
                          const DetailMap* details) {
    GapCheck g;
    if (model.faces.empty()) return g;
    std::vector<Piece> pieces;
    for (const Pillar& P : tree.pillars)
        if (P.top > P.bottom) pieces.push_back({{P.xy.x, P.xy.y, P.bottom}, {P.xy.x, P.xy.y, P.top}, P.diameter / 2});
    for (const auto* rods : {&tree.bridges, &tree.branches, &tree.braces})
        for (const Rod& r : *rods) pieces.push_back({r.a, r.b, r.diameter / 2});
    // Shafts, bridges, branches and braces: the gap all along.
    const double tol = 0.02;  // sampling and the model's facets
    for (const Piece& p : pieces) {
        const double len = distance(p.a, p.b);
        const int n = std::max(1, static_cast<int>(std::ceil(len / 0.2)));
        double least = 1e9, close = 0;
        Vec3 where;
        bool in_slot = false;
        Vec3 slot_at;
        for (int i = 0; i <= n; ++i) {
            const Vec3 q = p.a + (p.b - p.a) * (static_cast<double>(i) / n);
            const double d = at.distanceBelow(q, p.r + tc.clearance) - p.r;
            if (d < tc.clearance - tol) {
                close += len / n;
                if (d < least) least = d, where = q;
            }
            if (details && !in_slot && details->reaches(q, p.r)) in_slot = true, slot_at = q;
        }
        if (least < 1e9) {
            ++g.near_model;
            g.nearest = std::min(g.nearest, least);
            g.warnings.push_back({"shaft_near_model",
                                  fmt2("a support runs %.2f mm from the model for %.1f mm; the gap it should keep is %.2f mm",
                                       std::max(0.0, least), close, tc.clearance),
                                  -1, where});
        }
        if (in_slot) {
            ++g.in_slot;
            g.warnings.push_back({"support_in_slot", "a support goes into a narrow slot or hole, or under where one opens",
                                  -1, slot_at});
        }
    }
    // Heads: the contact keeps inside a slot's edge or off a wall going down;
    // the cone, below the contact, out of the slots.
    for (const Head& h : tree.heads) {
        if (h.stub || h.on_plate) continue;
        const double need = h.spec.tip_diameter / 2 + std::max(0.0, cfg.tip_edge_margin);
        Vec3 e;
        const double d = edges.nearest(h.contact, need, &e);
        if (d < need - 0.01 && edgeWantsRoom(at, model, h.contact, e, cfg.detail_width)) {
            ++g.over_edge;
            g.warnings.push_back({"tip_over_edge",
                                  fmt2("a %.1f mm tip sits %.2f mm from the edge beside it; its contact needs %.2f mm",
                                       h.spec.tip_diameter, d, need),
                                  -1, h.contact});
        }
        if (details) {
            const Vec3 start = h.contact + h.dir * (h.spec.tip_diameter / 2 + 0.05);
            for (const double f : {0.4, 0.7, 1.0}) {
                const Vec3 q = start + (h.junction - start) * f;
                const double r = h.spec.tip_diameter / 2 + (h.spec.shaft_diameter - h.spec.tip_diameter) / 2 * f;
                if (details->reaches(q, r)) {
                    ++g.in_slot;
                    g.warnings.push_back({"support_in_slot", "a support head reaches into a narrow slot or hole, or under where one opens",
                                          -1, h.contact});
                    break;
                }
            }
        }
    }
    return g;
}

}  // namespace fruit
