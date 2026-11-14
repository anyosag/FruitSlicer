// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "mesh/AabbTree.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace fruit {

namespace {

constexpr int kLeafSize = 4;

bool rayBox(const BBox3& b, const Vec3& o, const Vec3& inv, double t_min, double t_max) {
    for (int a = 0; a < 3; ++a) {
        double t0 = (b.min[a] - o[a]) * inv[a];
        double t1 = (b.max[a] - o[a]) * inv[a];
        if (t0 > t1) std::swap(t0, t1);
        t_min = std::max(t_min, t0);
        t_max = std::min(t_max, t1);
        if (t_max < t_min) return false;
    }
    return true;
}

// Möller–Trumbore. Returns t along dir (not normalised) or -1.
double rayTriangle(const Vec3& o, const Vec3& d, const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 e1 = b - a, e2 = c - a;
    const Vec3 p = cross(d, e2);
    const double det = dot(e1, p);
    if (std::abs(det) < 1e-18) return -1;
    const double inv = 1.0 / det;
    const Vec3 s = o - a;
    const double u = dot(s, p) * inv;
    if (u < 0 || u > 1) return -1;
    const Vec3 q = cross(s, e1);
    const double v = dot(d, q) * inv;
    if (v < 0 || u + v > 1) return -1;
    return dot(e2, q) * inv;
}

// Closest point on triangle (Ericson, Real-Time Collision Detection 5.1.5).
Vec3 closestOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 ab = b - a, ac = c - a, ap = p - a;
    const double d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    const Vec3 bp = p - b;
    const double d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
    const Vec3 cp = p - c;
    const double d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const double denom = 1.0 / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

double boxDistance2(const BBox3& b, const Vec3& p) {
    double d2 = 0;
    for (int a = 0; a < 3; ++a) {
        const double v = p[a] < b.min[a] ? b.min[a] - p[a] : (p[a] > b.max[a] ? p[a] - b.max[a] : 0);
        d2 += v * v;
    }
    return d2;
}

}  // namespace

AabbTree::AabbTree(const TriangleMesh& mesh) { build(mesh); }

void AabbTree::build(const TriangleMesh& mesh) {
    mesh_ = &mesh;
    nodes_.clear();
    const auto n = static_cast<int32_t>(mesh.faces.size());
    order_.resize(n);
    std::iota(order_.begin(), order_.end(), 0);
    std::vector<Vec3> centroids(n);
    std::vector<BBox3> boxes(n);
    for (int32_t i = 0; i < n; ++i) {
        const Face& f = mesh.faces[i];
        BBox3 b;
        for (int k = 0; k < 3; ++k) b.extend(mesh.vertices[f[k]]);
        boxes[i] = b;
        centroids[i] = b.center();
    }
    nodes_.reserve(2 * static_cast<size_t>(n) / kLeafSize + 1);
    if (n > 0) buildNode(0, n, centroids, boxes);
}

int32_t AabbTree::buildNode(int32_t begin, int32_t end, std::vector<Vec3>& centroids,
                            std::vector<BBox3>& boxes) {
    const auto idx = static_cast<int32_t>(nodes_.size());
    nodes_.push_back({});
    BBox3 box, cbox;
    for (int32_t i = begin; i < end; ++i) {
        box.extend(boxes[order_[i]]);
        cbox.extend(centroids[order_[i]]);
    }
    nodes_[idx].box = box;
    if (end - begin <= kLeafSize) {
        nodes_[idx].begin = begin;
        nodes_[idx].end = end;
        return idx;
    }
    const int axis = cbox.longestAxis();
    const int32_t mid = begin + (end - begin) / 2;
    std::nth_element(order_.begin() + begin, order_.begin() + mid, order_.begin() + end,
                     [&](int32_t a, int32_t b) {
                         // Tie-break on index keeps the tree deterministic.
                         const double ca = centroids[a][axis], cb = centroids[b][axis];
                         return ca < cb || (ca == cb && a < b);
                     });
    const int32_t l = buildNode(begin, mid, centroids, boxes);
    const int32_t r = buildNode(mid, end, centroids, boxes);
    nodes_[idx].left = l;
    nodes_[idx].right = r;
    return idx;
}

std::optional<RayHit> AabbTree::raycast(const Vec3& o, const Vec3& dir, double t_max, double t_min) const {
    if (nodes_.empty()) return std::nullopt;
    const double len = norm(dir);
    if (len <= 0) return std::nullopt;
    const Vec3 d = dir / len;
    const Vec3 inv{1.0 / d.x, 1.0 / d.y, 1.0 / d.z};
    std::optional<RayHit> best;
    double best_t = t_max;
    std::vector<int32_t> stack{0};
    while (!stack.empty()) {
        const Node& node = nodes_[stack.back()];
        stack.pop_back();
        if (!rayBox(node.box, o, inv, t_min, best_t)) continue;
        if (node.left < 0) {
            for (int32_t i = node.begin; i < node.end; ++i) {
                const int32_t fi = order_[i];
                const Face& f = mesh_->faces[fi];
                const double t = rayTriangle(o, d, mesh_->vertices[f[0]], mesh_->vertices[f[1]],
                                             mesh_->vertices[f[2]]);
                if (t > t_min && t <= best_t) {
                    best_t = t;
                    best = RayHit{t, fi, o + d * t};
                }
            }
        } else {
            stack.push_back(node.left);
            stack.push_back(node.right);
        }
    }
    return best;
}

bool AabbTree::anyHit(const Vec3& o, const Vec3& dir, double t_max, double t_min) const {
    if (nodes_.empty()) return false;
    const double len = norm(dir);
    if (len <= 0) return false;
    const Vec3 d = dir / len;
    const Vec3 inv{1.0 / d.x, 1.0 / d.y, 1.0 / d.z};
    std::vector<int32_t> stack{0};
    while (!stack.empty()) {
        const Node& node = nodes_[stack.back()];
        stack.pop_back();
        if (!rayBox(node.box, o, inv, t_min, t_max)) continue;
        if (node.left < 0) {
            for (int32_t i = node.begin; i < node.end; ++i) {
                const Face& f = mesh_->faces[order_[i]];
                const double t = rayTriangle(o, d, mesh_->vertices[f[0]], mesh_->vertices[f[1]],
                                             mesh_->vertices[f[2]]);
                if (t > t_min && t <= t_max) return true;
            }
        } else {
            stack.push_back(node.left);
            stack.push_back(node.right);
        }
    }
    return false;
}

namespace {

// A node to visit and its box's squared distance from the query point.
struct Visit {
    int32_t node;
    double d2;
};

// Deeper than any tree build() makes (median splits: about log2 of the faces).
constexpr int kMaxStack = 128;

}  // namespace

// The nearest face, depth first, nearer child first. `stop(best2)` ends the
// search early; `cap2` leaves out what is further than its square root (or
// level with it). Faces are visited in the same order whatever `stop` and
// `cap2` are, so the nearest one, and the first of equals, are the same.
template <class Stop>
ClosestPoint AabbTree::search(const Vec3& p, double cap2, Stop stop) const {
    ClosestPoint best;
    best.distance = 1e300;
    if (nodes_.empty()) return best;
    double best2 = 1e300;
    Visit stack[kMaxStack];
    int top = 0;
    stack[top++] = {0, boxDistance2(nodes_[0].box, p)};
    while (top > 0) {
        const Visit v = stack[--top];
        if (v.d2 > best2 || v.d2 >= cap2) continue;
        const Node& node = nodes_[v.node];
        if (node.left < 0) {
            for (int32_t i = node.begin; i < node.end; ++i) {
                const int32_t fi = order_[i];
                const Face& f = mesh_->faces[fi];
                const Vec3 q = closestOnTriangle(p, mesh_->vertices[f[0]], mesh_->vertices[f[1]],
                                                 mesh_->vertices[f[2]]);
                const double d2 = norm2(q - p);
                if (d2 < best2) {
                    best2 = d2;
                    best.face = fi;
                    best.point = q;
                }
            }
            if (stop(best2)) break;
        } else {
            // Visit the nearer child first (pushed last). One further than the
            // nearest face so far is never visited, so it isn't pushed.
            const double dl = boxDistance2(nodes_[node.left].box, p);
            const double dr = boxDistance2(nodes_[node.right].box, p);
            const Visit l{node.left, dl}, r{node.right, dr};
            const Visit& first = dl < dr ? r : l;
            const Visit& second = dl < dr ? l : r;
            if (top + 2 > kMaxStack) throw std::logic_error("AabbTree: deeper than expected");
            if (!(first.d2 > best2)) stack[top++] = first;
            if (!(second.d2 > best2)) stack[top++] = second;
        }
    }
    best.distance = std::sqrt(best2);
    return best;
}

ClosestPoint AabbTree::closest(const Vec3& p) const {
    return search(p, std::numeric_limits<double>::infinity(), [](double) { return false; });
}

bool AabbTree::nearerThan(const Vec3& p, double limit) const {
    // Boxes further than the limit can't hold an answer (a little margin for
    // rounding between a box and the faces in it).
    const double cap2 = limit > 0 ? limit * limit * (1 + 1e-9) + 1e-18 : 0.0;
    return search(p, cap2, [&](double best2) { return std::sqrt(best2) < limit; }).distance < limit;
}

double AabbTree::distanceBelow(const Vec3& p, double cap) const {
    const double cap2 = cap > 0 ? cap * cap * (1 + 1e-9) + 1e-18 : 0.0;
    const ClosestPoint c = search(p, cap2, [](double) { return false; });
    return c.face < 0 ? std::max(cap, 0.0) : c.distance;
}

}  // namespace fruit
