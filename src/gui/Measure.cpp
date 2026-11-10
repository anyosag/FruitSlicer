// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Measure.h"

#include <algorithm>
#include <cmath>

#include "Document.h"

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

void MeasureState::click(const fruit::Vec3& p, int part, int geometry_id) {
    if (points.size() >= 2) points.clear();
    points.push_back({p, part, geometry_id});
}

bool measureStillOn(const MeasureState& s, const Document& doc) {
    for (const MeasureState::Point& p : s.points) {
        const Part* part = doc.findPart(p.part);
        if (!part || part->geometryId() != p.geometry_id) return false;
    }
    return true;
}

std::vector<fruit::Vec3> pointsOnPlate(const MeasureState& s, const PartFrames& frames) {
    std::vector<fruit::Vec3> out;
    for (const MeasureState::Point& p : s.points)
        if (const fruit::Transform* t = frames.find(p.part)) out.push_back(t->apply(p.at));
    return out;
}

std::optional<Measurement> measurementOf(const MeasureState& s, const PartFrames& frames) {
    if (!s.complete()) return std::nullopt;
    const std::vector<fruit::Vec3> on = pointsOnPlate(s, frames);
    if (on.size() != 2) return std::nullopt;
    return measureBetween(on[0], on[1]);
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
