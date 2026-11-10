// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "mesh/ModelCheck.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "mesh/MeshAnalysis.h"

namespace fruit {

bool MeshCheck::maybeInches() const {
    if (bbox.empty()) return false;
    const Vec3 s = bbox.size();
    return std::max({s.x, s.y, s.z}) < 12.0;
}

MeshCheck checkMesh(const LoadedModel& m) {
    MeshCheck c;
    c.triangles = m.mesh.faceCount();
    c.points = m.mesh.vertexCount();
    c.shells = m.shells;
    c.flipped = m.shells_flipped;
    c.cavities = m.cavities;
    c.volume_ml = m.mesh.volume() / 1000.0;
    c.bbox = m.mesh.bbox();
    c.open_edges = countOpenEdges(m.mesh);
    c.repair = m.repair;
    return c;
}

double plateMargin(const Placement& p) { return p.on_plate ? 0.5 : 3.0; }

namespace {

// Largest scale at which a (w, d, h) box at 100 % fits in the room left.
double largestScale(double w, double d, double h, double room_x, double room_y, double room_z) {
    double s = std::numeric_limits<double>::infinity();
    if (w > 0) s = std::min(s, room_x / w);
    if (d > 0) s = std::min(s, room_y / d);
    if (h > 0) s = std::min(s, room_z / h);
    if (!std::isfinite(s) || s <= 0) return 0;
    return std::floor(s * 100 + 1e-9) / 100;  // whole percent, rounded down
}

}  // namespace

PlateFit checkFit(const TriangleMesh& model, const Placement& p, const PrinterProfile& printer) {
    PlateFit f;
    if (model.empty()) return f;
    // Size at 100 % as rotated now; uniform scale only multiplies it.
    const Transform r = placementRotation(p);
    BBox3 b;
    for (const Vec3& v : model.vertices) b.extend(r.applyLinear(v));
    const Vec3 unit = b.max - b.min;
    f.size = unit * p.scale;
    f.top = f.size.z + p.lift();
    const double m = plateMargin(p);
    const double rx = printer.build_x - 2 * m, ry = printer.build_y - 2 * m, rz = printer.build_z - p.lift();
    f.fits_centred = f.size.x <= rx && f.size.y <= ry && f.size.z <= rz;
    f.fits_turned = f.size.y <= rx && f.size.x <= ry && f.size.z <= rz;  // a quarter turn swaps the outline's sides
    f.room = {(rx - f.size.x) / 2, (ry - f.size.y) / 2};
    // A hair over (from typing or dragging) still counts as inside.
    constexpr double kSlack = 1e-6;
    f.fits = f.fits_centred && std::abs(p.offset.x) <= f.room.x + kSlack && std::abs(p.offset.y) <= f.room.y + kSlack;
    f.largest = largestScale(unit.x, unit.y, unit.z, rx, ry, rz);
    f.largest_turned = largestScale(unit.y, unit.x, unit.z, rx, ry, rz);
    return f;
}

std::size_t countLowPoints(const TriangleMesh& placed, bool on_plate) {
    std::size_t n = 0;
    for (const MinimumRegion& r : findMinima(placed))
        if (!on_plate || r.z > 1e-3) ++n;
    return n;
}

}  // namespace fruit
