// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "mesh/FlatFaces.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <unordered_map>

namespace fruit {

namespace {

struct Key {
    int64_t x, y, z;
    bool operator==(const Key&) const = default;
};
struct KeyHash {
    std::size_t operator()(const Key& k) const {
        return static_cast<std::size_t>(k.x * 73856093LL ^ k.y * 19349663LL ^ k.z * 83492791LL);
    }
};

// Each vertex's first twin at the same spot, so a mesh whose triangles don't
// share their corners still has neighbours.
std::vector<int32_t> canonicalVertices(const TriangleMesh& mesh) {
    std::vector<int32_t> canon(mesh.vertices.size());
    std::unordered_map<Key, int32_t, KeyHash> seen;
    seen.reserve(mesh.vertices.size());
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const Vec3& v = mesh.vertices[i];
        const Key k{std::llround(v.x * 1e4), std::llround(v.y * 1e4), std::llround(v.z * 1e4)};
        canon[i] = seen.emplace(k, static_cast<int32_t>(i)).first->second;
    }
    return canon;
}

}  // namespace

FlatFaces findFlatFaces(const TriangleMesh& mesh, const FlatFaceOptions& o) {
    FlatFaces out;
    const std::size_t nf = mesh.faces.size();
    out.face_region.assign(nf, -1);
    if (nf == 0) return out;

    std::vector<Vec3> normal(nf);
    std::vector<double> area(nf);
    for (std::size_t f = 0; f < nf; ++f) {
        normal[f] = mesh.faceNormal(f);
        area[f] = mesh.faceArea(f);
    }

    // Neighbours across edges (by where the corners are).
    const std::vector<int32_t> canon = canonicalVertices(mesh);
    struct Edge {
        int32_t a, b, face;
    };
    std::vector<Edge> edges;
    edges.reserve(nf * 3);
    for (std::size_t f = 0; f < nf; ++f)
        for (int k = 0; k < 3; ++k) {
            const int32_t a = canon[static_cast<std::size_t>(mesh.faces[f][static_cast<std::size_t>(k)])];
            const int32_t b = canon[static_cast<std::size_t>(mesh.faces[f][static_cast<std::size_t>((k + 1) % 3)])];
            edges.push_back({std::min(a, b), std::max(a, b), static_cast<int32_t>(f)});
        }
    std::sort(edges.begin(), edges.end(), [](const Edge& l, const Edge& r) {
        return l.a != r.a ? l.a < r.a : l.b != r.b ? l.b < r.b : l.face < r.face;
    });
    std::vector<std::vector<int32_t>> next(nf);
    for (std::size_t i = 0; i < edges.size();) {
        std::size_t j = i;
        while (j < edges.size() && edges[j].a == edges[i].a && edges[j].b == edges[i].b) ++j;
        for (std::size_t p = i; p < j; ++p)
            for (std::size_t q = i; q < j; ++q)
                if (p != q && edges[p].face != edges[q].face) next[static_cast<std::size_t>(edges[p].face)].push_back(edges[q].face);
        i = j;
    }

    // Largest triangles first: a patch grows from its largest triangle, whose normal is the steadiest.
    std::vector<int32_t> order(nf);
    for (std::size_t f = 0; f < nf; ++f) order[f] = static_cast<int32_t>(f);
    std::sort(order.begin(), order.end(), [&](int32_t l, int32_t r) { return area[static_cast<std::size_t>(l)] > area[static_cast<std::size_t>(r)]; });

    const double cos_max = std::cos(o.max_angle_deg * std::numbers::pi / 180.0);
    std::vector<char> taken(nf, 0);
    struct Patch {
        FlatFace face;
        double d = 0;
    };
    std::vector<Patch> patches;
    std::vector<int32_t> stack;
    for (const int32_t seed : order) {
        const std::size_t s = static_cast<std::size_t>(seed);
        if (taken[s] || area[s] <= 0) continue;
        const Vec3 n0 = normal[s];
        const double d0 = dot(n0, mesh.vertices[static_cast<std::size_t>(mesh.faces[s][0])]);
        Patch patch;
        Vec3 nsum{}, csum{};
        stack.assign(1, seed);
        taken[s] = 1;
        while (!stack.empty()) {
            const std::size_t f = static_cast<std::size_t>(stack.back());
            stack.pop_back();
            patch.face.faces.push_back(static_cast<int32_t>(f));
            patch.face.area += area[f];
            nsum += normal[f] * area[f];
            const Vec3& a = mesh.vertices[static_cast<std::size_t>(mesh.faces[f][0])];
            const Vec3& b = mesh.vertices[static_cast<std::size_t>(mesh.faces[f][1])];
            const Vec3& c = mesh.vertices[static_cast<std::size_t>(mesh.faces[f][2])];
            csum += (a + b + c) * (area[f] / 3.0);
            for (const int32_t g : next[f]) {
                const std::size_t gi = static_cast<std::size_t>(g);
                if (taken[gi] || area[gi] <= 0 || dot(normal[gi], n0) < cos_max) continue;
                bool flat = true;
                for (int k = 0; k < 3; ++k)
                    if (std::abs(dot(n0, mesh.vertices[static_cast<std::size_t>(mesh.faces[gi][static_cast<std::size_t>(k)])]) - d0) > o.plane_tolerance)
                        flat = false;
                if (!flat) continue;
                taken[gi] = 1;
                stack.push_back(g);
            }
        }
        if (patch.face.area < o.min_area) continue;
        patch.face.normal = normalized(nsum);
        patch.face.centroid = csum * (1.0 / patch.face.area);
        patch.d = dot(patch.face.normal, patch.face.centroid);
        patches.push_back(std::move(patch));
    }

    // Keep the ones the whole model lies behind. Most others fail at their first few vertices.
    std::sort(patches.begin(), patches.end(), [](const Patch& l, const Patch& r) { return l.face.area > r.face.area; });
    for (Patch& p : patches) {
        bool rests = true;
        for (const Vec3& v : mesh.vertices)
            if (dot(p.face.normal, v) - p.d > o.rest_tolerance) {
                rests = false;
                break;
            }
        if (!rests) continue;
        const int32_t index = static_cast<int32_t>(out.faces.size());
        for (const int32_t f : p.face.faces) out.face_region[static_cast<std::size_t>(f)] = index;
        out.faces.push_back(std::move(p.face));
    }
    return out;
}

}  // namespace fruit
