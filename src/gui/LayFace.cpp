// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "LayFace.h"

#include "Placing.h"

namespace gui {

std::vector<RestingFace> restingFaces(const Part& part) {
    const fruit::FlatFaces& flat = part.flatFaces();
    const fruit::Placement& p = part.placement();
    std::vector<RestingFace> out;
    std::vector<fruit::Vec3> dirs;
    for (std::size_t i = 0; i < flat.faces.size(); ++i) {
        RestingFace f;
        f.region = static_cast<int>(i);
        f.area = flat.faces[i].area * p.scale * p.scale;
        f.dir = printerDirection(p, flat.faces[i].normal);
        f.on_plate = facesDown(f.dir);
        dirs.push_back(f.dir);
        out.push_back(std::move(f));
    }
    const std::vector<std::string> names = faceNames(dirs);
    for (std::size_t i = 0; i < out.size(); ++i) out[i].name = names[i];
    return out;
}

SurfacePick surfaceAt(const Part& part, int triangle, const fruit::Vec3& point) {
    SurfacePick s;
    const fruit::TriangleMesh& m = part.mesh();
    if (triangle < 0 || static_cast<std::size_t>(triangle) >= m.faces.size()) return s;
    const std::size_t t = static_cast<std::size_t>(triangle);
    const fruit::FlatFaces& flat = part.flatFaces();
    const fruit::Placement& p = part.placement();
    s.triangle = triangle;
    s.point = point;
    if (const int r = flat.regionOf(t); r >= 0) {
        const RestingFace f = restingFaces(part)[static_cast<std::size_t>(r)];
        s.kind = SurfacePick::Kind::Resting;
        s.region = r;
        s.normal = flat.faces[static_cast<std::size_t>(r)].normal;
        s.name = f.name;
        s.area = f.area;
        s.facing_down = f.on_plate;
    } else if (const int o = flat.otherOf(t); o >= 0) {
        const fruit::FlatFace& f = flat.others[static_cast<std::size_t>(o)];
        s.kind = SurfacePick::Kind::Flat;
        s.region = o;
        s.normal = f.normal;
        const fruit::Vec3 dir = printerDirection(p, f.normal);
        s.name = faceNames({dir})[0];
        s.area = f.area * p.scale * p.scale;
        s.facing_down = facesDown(dir);
    } else {
        s.kind = SurfacePick::Kind::Curved;
        s.normal = m.faceNormal(t);
        s.facing_down = facesDown(printerDirection(p, s.normal));
    }
    return s;
}

}  // namespace gui
