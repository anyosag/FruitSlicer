// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "mesh/Placement.h"

namespace fruit {

Transform placeOnPlate(TriangleMesh& mesh, const Vec3& rotation_deg, double elevation) {
    Transform applied;
    if (rotation_deg.x != 0 || rotation_deg.y != 0 || rotation_deg.z != 0) {
        applied = Transform::rotationXYZ(rotation_deg.x, rotation_deg.y, rotation_deg.z);
        mesh.transform(applied);
    }
    const BBox3 b = mesh.bbox();
    const Vec3 c = b.center();
    const Vec3 d{-c.x, -c.y, elevation - b.min.z};
    mesh.translate(d);
    return applied.then(Transform::translation(d));
}

Transform mirrorTransform(const Placement& p) {
    Transform m;
    if (p.mirror_x) m.m[0][0] = -1;
    if (p.mirror_y) m.m[1][1] = -1;
    if (p.mirror_z) m.m[2][2] = -1;
    return m;
}

Transform placementRotation(const Placement& p) {
    return mirrorTransform(p).then(Transform::rotationXYZ(p.rotation_deg.x, p.rotation_deg.y, p.rotation_deg.z));
}

namespace {

Transform scaleRotation(const Placement& p) {
    Transform s;
    for (int i = 0; i < 3; ++i) s.m[i][i] = p.scale;
    return s.then(placementRotation(p));
}

}  // namespace

Transform placementTransform(const TriangleMesh& model, const Placement& p) {
    const Transform sr = scaleRotation(p);
    BBox3 b;
    for (const Vec3& v : model.vertices) b.extend(sr.apply(v));
    if (model.vertices.empty()) return sr;
    const Vec3 c = b.center();
    return sr.then(Transform::translation({p.offset.x - c.x, p.offset.y - c.y, p.lift() - b.min.z}));
}

TriangleMesh placeModel(const TriangleMesh& model, const Placement& p) {
    TriangleMesh m = model;
    if (p.mirrored()) m.transform(mirrorTransform(p));
    if (p.scale != 1.0) {
        Transform s;
        for (int i = 0; i < 3; ++i) s.m[i][i] = p.scale;
        m.transform(s);
    }
    placeOnPlate(m, p.rotation_deg, p.lift());
    if (!p.centred()) m.translate({p.offset.x, p.offset.y, 0});
    return m;
}

}  // namespace fruit
