// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Measure.h"

#include <algorithm>
#include <cmath>

namespace gui {

Measurement measureBetween(const fruit::Vec3& from, const fruit::Vec3& to) {
    Measurement m;
    m.from = from;
    m.to = to;
    m.dx = std::abs(to.x - from.x);
    m.dy = std::abs(to.y - from.y);
    m.dz = std::abs(to.z - from.z);
    m.distance = std::sqrt(m.dx * m.dx + m.dy * m.dy + m.dz * m.dz);
    return m;
}

void MeasureState::click(const fruit::Vec3& p) {
    if (points.size() >= 2) points.clear();
    points.push_back(p);
}

std::vector<fruit::Vec3> pointsOnPlate(const MeasureState& s, const fruit::Transform& t) {
    std::vector<fruit::Vec3> out;
    for (const fruit::Vec3& p : s.points) out.push_back(t.apply(p));
    return out;
}

std::optional<Measurement> measurementOf(const MeasureState& s, const fruit::Transform& t) {
    if (!s.complete()) return std::nullopt;
    return measureBetween(t.apply(s.points[0]), t.apply(s.points[1]));
}

std::optional<SurfacePoint> pickSurface(const fruit::AabbTree& tree, const fruit::Vec3& origin, const fruit::Vec3& dir) {
    const fruit::TriangleMesh* mesh = tree.mesh();
    if (!mesh) return std::nullopt;
    const auto hit = tree.raycast(origin, dir);
    if (!hit) return std::nullopt;
    SurfacePoint s;
    s.point = hit->point;
    s.normal = mesh->faceNormal(static_cast<std::size_t>(hit->face));
    return s;
}

void rayThroughPixel(const Camera& camera, float x, float y, float left, float top, float width, float height,
                     fruit::Vec3* origin, fruit::Vec3* dir) {
    const float nx = 2 * (x - left) / width - 1, ny = 1 - 2 * (y - top) / height;
    V3 o, d;
    camera.ray(nx, ny, width / std::max(1.0f, height), &o, &d);
    *origin = {o.x, o.y, o.z};
    *dir = {d.x, d.y, d.z};
}

}  // namespace gui
