#include "mesh/MeshAnalysis.h"

#include <algorithm>
#include <numeric>

namespace fruit {

std::vector<std::vector<int32_t>> vertexNeighbours(const TriangleMesh& mesh) {
    std::vector<std::vector<int32_t>> adj(mesh.vertices.size());
    for (const auto& f : mesh.faces)
        for (int e = 0; e < 3; ++e) {
            const int32_t a = f[e], b = f[(e + 1) % 3];
            adj[a].push_back(b);
            adj[b].push_back(a);
        }
    for (auto& n : adj) {
        std::sort(n.begin(), n.end());
        n.erase(std::unique(n.begin(), n.end()), n.end());
    }
    return adj;
}

std::vector<Vec3> vertexNormals(const TriangleMesh& mesh) {
    std::vector<Vec3> n(mesh.vertices.size());
    for (const auto& f : mesh.faces) {
        // Unnormalised cross product = 2 * area * normal (area weighting).
        const Vec3 c = cross(mesh.vertices[f[1]] - mesh.vertices[f[0]], mesh.vertices[f[2]] - mesh.vertices[f[0]]);
        for (int k = 0; k < 3; ++k) n[f[k]] += c;
    }
    for (auto& v : n) v = normalized(v);
    return n;
}

std::vector<MinimumRegion> findMinima(const TriangleMesh& mesh, double z_eps) {
    const auto adj = vertexNeighbours(mesh);
    const auto normals = vertexNormals(mesh);
    const size_t nv = mesh.vertices.size();
    std::vector<int32_t> order(nv);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int32_t a, int32_t b) {
        const Vec3 &p = mesh.vertices[a], &q = mesh.vertices[b];
        if (p.z != q.z) return p.z < q.z;
        if (p.x != q.x) return p.x < q.x;
        if (p.y != q.y) return p.y < q.y;
        return a < b;
    });
    std::vector<char> visited(nv, 0);
    std::vector<MinimumRegion> out;
    std::vector<int32_t> stack, plateau;
    for (int32_t seed : order) {
        if (visited[seed] || adj[seed].empty()) continue;
        const double z0 = mesh.vertices[seed].z;
        plateau.clear();
        stack.assign(1, seed);
        visited[seed] = 1;
        bool is_min = true;
        while (!stack.empty()) {
            const int32_t v = stack.back();
            stack.pop_back();
            plateau.push_back(v);
            for (int32_t w : adj[v]) {
                const double dz = mesh.vertices[w].z - z0;
                if (dz < -z_eps) is_min = false;
                else if (dz <= z_eps && !visited[w]) {
                    visited[w] = 1;
                    stack.push_back(w);
                }
            }
        }
        if (!is_min) continue;
        Vec3 c{}, navg{};
        for (int32_t v : plateau) {
            c += mesh.vertices[v];
            navg += normals[v];
        }
        c = c / static_cast<double>(plateau.size());
        if (navg.z >= 0) continue;  // up-facing (floor of a bowl/cavity): solid below
        MinimumRegion r;
        r.point = c;
        r.z = z0;
        r.vertices = plateau;
        std::sort(r.vertices.begin(), r.vertices.end());
        for (int32_t v : plateau) r.radius = std::max(r.radius, norm((mesh.vertices[v] - c).xy()));
        out.push_back(std::move(r));
    }
    std::sort(out.begin(), out.end(), [](const MinimumRegion& a, const MinimumRegion& b) {
        if (a.z != b.z) return a.z < b.z;
        if (a.point.x != b.point.x) return a.point.x < b.point.x;
        return a.point.y < b.point.y;
    });
    return out;
}

double localThickness(const AabbTree& tree, const Vec3& p, const Vec3& n, double max_dist) {
    const Vec3 dir = -normalized(n);
    const double eps = 1e-5;
    auto hit = tree.raycast(p + dir * eps, dir, max_dist, 0.0);
    return hit ? hit->t + eps : max_dist;
}

}  // namespace fruit
