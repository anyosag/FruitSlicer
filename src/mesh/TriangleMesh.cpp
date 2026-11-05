// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "mesh/TriangleMesh.h"

#include <cmath>
#include <map>
#include <numeric>
#include <unordered_map>

namespace fruit {

BBox3 TriangleMesh::bbox() const {
    BBox3 b;
    for (const auto& v : vertices) b.extend(v);
    return b;
}

static double signedTetVolume(const Vec3& a, const Vec3& b, const Vec3& c) {
    return dot(a, cross(b, c)) / 6.0;
}

double TriangleMesh::volume() const {
    // Reference point at the bbox centre improves accuracy far from the origin.
    const Vec3 o = bbox().center();
    double v = 0;
    for (const auto& f : faces)
        v += signedTetVolume(vertices[f[0]] - o, vertices[f[1]] - o, vertices[f[2]] - o);
    return v;
}

double TriangleMesh::faceArea(size_t i) const {
    const Face& f = faces[i];
    return 0.5 * norm(cross(vertices[f[1]] - vertices[f[0]], vertices[f[2]] - vertices[f[0]]));
}

double TriangleMesh::surfaceArea() const {
    double a = 0;
    for (size_t i = 0; i < faces.size(); ++i) a += faceArea(i);
    return a;
}

Vec3 TriangleMesh::faceNormal(size_t i) const {
    const Face& f = faces[i];
    return normalized(cross(vertices[f[1]] - vertices[f[0]], vertices[f[2]] - vertices[f[0]]));
}

void TriangleMesh::transform(const Transform& t) {
    for (auto& v : vertices) v = t.apply(v);
    if (t.determinant() < 0)
        for (auto& f : faces) std::swap(f[1], f[2]);
}

void TriangleMesh::translate(const Vec3& d) {
    for (auto& v : vertices) v += d;
}

void TriangleMesh::append(const TriangleMesh& other) {
    const auto base = static_cast<int32_t>(vertices.size());
    vertices.insert(vertices.end(), other.vertices.begin(), other.vertices.end());
    faces.reserve(faces.size() + other.faces.size());
    for (const auto& f : other.faces) faces.push_back({f[0] + base, f[1] + base, f[2] + base});
}

size_t TriangleMesh::weld(double tolerance) {
    // Hash vertices into cells of size `tolerance`; compare against the 27
    // neighbouring cells so points straddling a cell border still merge.
    struct Key {
        int64_t x, y, z;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            uint64_t h = static_cast<uint64_t>(k.x) * 73856093ULL;
            h ^= static_cast<uint64_t>(k.y) * 19349663ULL;
            h ^= static_cast<uint64_t>(k.z) * 83492791ULL;
            return static_cast<size_t>(h);
        }
    };
    const double cell = std::max(tolerance, 1e-12);
    auto keyOf = [cell](const Vec3& p) {
        return Key{static_cast<int64_t>(std::floor(p.x / cell)),
                   static_cast<int64_t>(std::floor(p.y / cell)),
                   static_cast<int64_t>(std::floor(p.z / cell))};
    };
    std::unordered_map<Key, std::vector<int32_t>, KeyHash> grid;
    std::vector<Vec3> out;
    std::vector<int32_t> remap(vertices.size());
    const double tol2 = tolerance * tolerance;
    for (size_t i = 0; i < vertices.size(); ++i) {
        const Vec3& p = vertices[i];
        const Key k = keyOf(p);
        int32_t found = -1;
        for (int dx = -1; dx <= 1 && found < 0; ++dx)
            for (int dy = -1; dy <= 1 && found < 0; ++dy)
                for (int dz = -1; dz <= 1 && found < 0; ++dz) {
                    auto it = grid.find({k.x + dx, k.y + dy, k.z + dz});
                    if (it == grid.end()) continue;
                    for (int32_t idx : it->second)
                        if (norm2(out[idx] - p) <= tol2) { found = idx; break; }
                }
        if (found < 0) {
            found = static_cast<int32_t>(out.size());
            out.push_back(p);
            grid[k].push_back(found);
        }
        remap[i] = found;
    }
    const size_t removed = vertices.size() - out.size();
    vertices = std::move(out);
    std::vector<Face> kept;
    kept.reserve(faces.size());
    for (const auto& f : faces) {
        Face g{remap[f[0]], remap[f[1]], remap[f[2]]};
        if (g[0] == g[1] || g[1] == g[2] || g[0] == g[2]) continue;
        kept.push_back(g);
    }
    faces = std::move(kept);
    return removed;
}

std::vector<int> TriangleMesh::shellIds(int* shell_count) const {
    // Union-find over vertices, then label faces.
    std::vector<int32_t> parent(vertices.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto find = [&](int32_t a) {
        while (parent[a] != a) a = parent[a] = parent[parent[a]];
        return a;
    };
    for (const auto& f : faces) {
        const int32_t a = find(f[0]), b = find(f[1]), c = find(f[2]);
        parent[b] = a;
        parent[find(c)] = a;
    }
    std::unordered_map<int32_t, int> label;
    std::vector<int> ids(faces.size());
    for (size_t i = 0; i < faces.size(); ++i) {
        const int32_t r = find(faces[i][0]);
        auto [it, inserted] = label.try_emplace(r, static_cast<int>(label.size()));
        ids[i] = it->second;
    }
    if (shell_count) *shell_count = static_cast<int>(label.size());
    return ids;
}

int TriangleMesh::orientShellsOutward(int* cavities) {
    int count = 0;
    const std::vector<int> ids = shellIds(&count);
    std::vector<double> vol(count, 0.0);
    const Vec3 o = bbox().center();
    for (size_t i = 0; i < faces.size(); ++i) {
        const Face& f = faces[i];
        vol[ids[i]] += signedTetVolume(vertices[f[0]] - o, vertices[f[1]] - o, vertices[f[2]] - o);
    }
    // Parity test: cast a ray from a vertex of the shell along a slightly
    // skewed direction and count crossings with faces of *other* shells.
    auto enclosed = [&](int shell) {
        Vec3 p{};
        for (size_t i = 0; i < faces.size(); ++i)
            if (ids[i] == shell) { p = vertices[faces[i][0]]; break; }
        const Vec3 d = normalized(Vec3{1.0, 0.000123, 0.000457});
        int crossings = 0;
        for (size_t i = 0; i < faces.size(); ++i) {
            if (ids[i] == shell) continue;
            const Vec3 &a = vertices[faces[i][0]], &b = vertices[faces[i][1]], &c = vertices[faces[i][2]];
            const Vec3 e1 = b - a, e2 = c - a, pv = cross(d, e2);
            const double det = dot(e1, pv);
            if (std::abs(det) < 1e-18) continue;
            const Vec3 tv = p - a;
            const double u = dot(tv, pv) / det;
            if (u < 0 || u > 1) continue;
            const Vec3 qv = cross(tv, e1);
            const double v = dot(d, qv) / det;
            if (v < 0 || u + v > 1) continue;
            if (dot(e2, qv) / det > 1e-12) ++crossings;
        }
        return crossings % 2 == 1;
    };
    int flipped = 0, kept = 0;
    std::vector<bool> flip(count, false);
    for (int s = 0; s < count; ++s) {
        if (vol[s] >= 0) continue;
        if (count > 1 && enclosed(s)) { ++kept; continue; }
        flip[s] = true;
        ++flipped;
    }
    if (flipped)
        for (size_t i = 0; i < faces.size(); ++i)
            if (flip[ids[i]]) std::swap(faces[i][1], faces[i][2]);
    if (cavities) *cavities = kept;
    return flipped;
}

bool TriangleMesh::isClosedManifold() const {
    std::map<std::pair<int32_t, int32_t>, int> directed;
    for (const auto& f : faces)
        for (int e = 0; e < 3; ++e) ++directed[{f[e], f[(e + 1) % 3]}];
    for (const auto& [edge, n] : directed) {
        if (n != 1) return false;
        auto it = directed.find({edge.second, edge.first});
        if (it == directed.end() || it->second != 1) return false;
    }
    return true;
}

}  // namespace fruit
