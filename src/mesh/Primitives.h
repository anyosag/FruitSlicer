// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Closed, outward-oriented primitive meshes. Used for procedural test models,
// calibration coupons, and support geometry (tips, shafts, bases, rafts).

#include "mesh/TriangleMesh.h"

namespace fruit {

// Segment count that keeps the chord error of a circle of radius r below
// `chord_error` mm (clamped to [6, 512]).
int segmentsFor(double radius, double chord_error);

void appendBox(TriangleMesh& m, const Vec3& min, const Vec3& max);
// Frustum (cylinder/cone) from point a (radius ra) to point b (radius rb) along
// an arbitrary axis. Either radius may be 0, giving a cone apex.
void appendFrustum(TriangleMesh& m, const Vec3& a, double ra, const Vec3& b, double rb, int segments);
void appendSphere(TriangleMesh& m, const Vec3& c, double r, int segments);

TriangleMesh makeBox(const Vec3& min, const Vec3& max);
TriangleMesh makeCylinder(const Vec3& base_center, double r, double height, int segments);
TriangleMesh makeCone(const Vec3& base_center, double r_base, const Vec3& apex, int segments);
TriangleMesh makeSphere(const Vec3& c, double r, int segments);

}  // namespace fruit
