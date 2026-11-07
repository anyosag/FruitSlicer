// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "geometry/Transform.h"

#include <stdexcept>

#include <cmath>

namespace fruit {

Transform Transform::translation(const Vec3& d) {
    Transform tr;
    tr.t = d;
    return tr;
}

static Transform fromMatrix(const double a[3][3]) {
    Transform tr;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) tr.m[i][j] = a[i][j];
    return tr;
}

Transform Transform::rotationXYZ(double rx_deg, double ry_deg, double rz_deg) {
    const double cx = std::cos(deg2rad(rx_deg)), sx = std::sin(deg2rad(rx_deg));
    const double cy = std::cos(deg2rad(ry_deg)), sy = std::sin(deg2rad(ry_deg));
    const double cz = std::cos(deg2rad(rz_deg)), sz = std::sin(deg2rad(rz_deg));
    const double rx[3][3] = {{1, 0, 0}, {0, cx, -sx}, {0, sx, cx}};
    const double ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
    const double rz[3][3] = {{cz, -sz, 0}, {sz, cz, 0}, {0, 0, 1}};
    return fromMatrix(rx).then(fromMatrix(ry)).then(fromMatrix(rz));
}

Vec3 Transform::applyLinear(const Vec3& v) const {
    return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
            m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
            m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
}

Vec3 Transform::apply(const Vec3& p) const { return applyLinear(p) + t; }

Transform Transform::then(const Transform& next) const {
    Transform out;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double s = 0;
            for (int k = 0; k < 3; ++k) s += next.m[i][k] * m[k][j];
            out.m[i][j] = s;
        }
    out.t = next.applyLinear(t) + next.t;
    return out;
}

double Transform::determinant() const {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
           m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

Transform Transform::inverse() const {
    const double d = determinant();
    if (std::abs(d) < 1e-15) throw std::domain_error("transform cannot be inverted");
    Transform r;
    // Adjugate over determinant.
    r.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / d;
    r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / d;
    r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / d;
    r.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / d;
    r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / d;
    r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / d;
    r.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / d;
    r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / d;
    r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / d;
    const Vec3 nt = r.applyLinear(t);
    r.t = {-nt.x, -nt.y, -nt.z};
    return r;
}

}  // namespace fruit
