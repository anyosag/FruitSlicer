#include "mesh/Primitives.h"

#include <cmath>

namespace fruit {

int segmentsFor(double radius, double chord_error) {
    if (radius <= 0) return 6;
    if (chord_error >= radius) return 6;
    const double half_angle = std::acos(1.0 - chord_error / radius);
    const int n = static_cast<int>(std::ceil(kPi / half_angle));
    return std::clamp(n, 6, 512);
}

void appendBox(TriangleMesh& m, const Vec3& lo, const Vec3& hi) {
    const auto b = static_cast<int32_t>(m.vertices.size());
    for (int i = 0; i < 8; ++i)
        m.vertices.emplace_back((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
    // Each quad listed counter-clockwise seen from outside.
    const int quads[6][4] = {
        {0, 2, 3, 1},  // z-
        {4, 5, 7, 6},  // z+
        {0, 1, 5, 4},  // y-
        {2, 6, 7, 3},  // y+
        {0, 4, 6, 2},  // x-
        {1, 3, 7, 5},  // x+
    };
    for (const auto& q : quads) {
        m.faces.push_back({b + q[0], b + q[1], b + q[2]});
        m.faces.push_back({b + q[0], b + q[2], b + q[3]});
    }
}

static void basisFor(const Vec3& w, Vec3& u, Vec3& v) {
    const Vec3 ref = std::abs(w.z) < 0.9 ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
    u = normalized(cross(ref, w));
    v = cross(w, u);  // (u, v, w) right-handed
}

void appendFrustum(TriangleMesh& m, const Vec3& a, double ra, const Vec3& b, double rb, int n) {
    const Vec3 axis = b - a;
    const double len = norm(axis);
    if (len <= 0 || (ra <= 0 && rb <= 0)) return;
    n = std::max(n, 3);
    const Vec3 w = axis / len;
    Vec3 u, v;
    basisFor(w, u, v);
    const auto base = static_cast<int32_t>(m.vertices.size());

    // Ring (or apex) vertex indices for each end.
    auto makeEnd = [&](const Vec3& c, double r, std::vector<int32_t>& idx) {
        idx.resize(n);
        if (r <= 0) {
            const auto apex = static_cast<int32_t>(m.vertices.size());
            m.vertices.push_back(c);
            std::fill(idx.begin(), idx.end(), apex);
            return;
        }
        for (int i = 0; i < n; ++i) {
            const double t = 2.0 * kPi * i / n;
            idx[i] = static_cast<int32_t>(m.vertices.size());
            m.vertices.push_back(c + (u * std::cos(t) + v * std::sin(t)) * r);
        }
    };
    std::vector<int32_t> ia, ib;
    makeEnd(a, ra, ia);
    makeEnd(b, rb, ib);
    (void)base;

    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        if (ia[i] != ia[j]) m.faces.push_back({ia[i], ia[j], ib[j]});
        if (ib[i] != ib[j]) m.faces.push_back({ia[i], ib[j], ib[i]});
    }
    if (ra > 0) {
        const auto ca = static_cast<int32_t>(m.vertices.size());
        m.vertices.push_back(a);
        for (int i = 0; i < n; ++i) m.faces.push_back({ca, ia[(i + 1) % n], ia[i]});
    }
    if (rb > 0) {
        const auto cb = static_cast<int32_t>(m.vertices.size());
        m.vertices.push_back(b);
        for (int i = 0; i < n; ++i) m.faces.push_back({cb, ib[i], ib[(i + 1) % n]});
    }
}

void appendSphere(TriangleMesh& m, const Vec3& c, double r, int n) {
    n = std::max(n, 6);
    const int stacks = std::max(3, n / 2);
    const auto south = static_cast<int32_t>(m.vertices.size());
    m.vertices.push_back(c + Vec3{0, 0, -r});
    // Rings from south to north (excluding poles).
    std::vector<int32_t> ring_start;
    for (int s = 1; s < stacks; ++s) {
        const double phi = -kPi / 2 + kPi * s / stacks;
        ring_start.push_back(static_cast<int32_t>(m.vertices.size()));
        for (int i = 0; i < n; ++i) {
            const double t = 2.0 * kPi * i / n;
            m.vertices.push_back(c + Vec3{std::cos(phi) * std::cos(t), std::cos(phi) * std::sin(t),
                                          std::sin(phi)} * r);
        }
    }
    const auto north = static_cast<int32_t>(m.vertices.size());
    m.vertices.push_back(c + Vec3{0, 0, r});
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        m.faces.push_back({south, ring_start.front() + j, ring_start.front() + i});
        m.faces.push_back({north, ring_start.back() + i, ring_start.back() + j});
    }
    for (size_t s = 0; s + 1 < ring_start.size(); ++s)
        for (int i = 0; i < n; ++i) {
            const int j = (i + 1) % n;
            const int32_t a0 = ring_start[s] + i, a1 = ring_start[s] + j;
            const int32_t b0 = ring_start[s + 1] + i, b1 = ring_start[s + 1] + j;
            m.faces.push_back({a0, a1, b1});
            m.faces.push_back({a0, b1, b0});
        }
}

TriangleMesh makeBox(const Vec3& lo, const Vec3& hi) {
    TriangleMesh m;
    appendBox(m, lo, hi);
    return m;
}

TriangleMesh makeCylinder(const Vec3& base_center, double r, double height, int segments) {
    TriangleMesh m;
    appendFrustum(m, base_center, r, base_center + Vec3{0, 0, height}, r, segments);
    return m;
}

TriangleMesh makeCone(const Vec3& base_center, double r_base, const Vec3& apex, int segments) {
    TriangleMesh m;
    appendFrustum(m, base_center, r_base, apex, 0.0, segments);
    return m;
}

TriangleMesh makeSphere(const Vec3& c, double r, int segments) {
    TriangleMesh m;
    appendSphere(m, c, r, segments);
    return m;
}

}  // namespace fruit
