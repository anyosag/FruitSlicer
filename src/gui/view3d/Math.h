// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Small float vector/matrix helpers for the 3D view (column-major, as GL).

#include <cmath>

namespace gui {

struct V3 {
    float x = 0, y = 0, z = 0;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 normalize(V3 a) {
    const float l = length(a);
    return l > 0 ? a * (1.0f / l) : a;
}

struct M4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // column-major
    float& at(int row, int col) { return m[col * 4 + row]; }
    float at(int row, int col) const { return m[col * 4 + row]; }
};

inline M4 operator*(const M4& a, const M4& b) {
    M4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.at(i, k) * b.at(k, j);
            r.at(i, j) = s;
        }
    return r;
}

inline M4 lookAt(V3 eye, V3 target, V3 up) {
    const V3 f = normalize(target - eye);
    const V3 s = normalize(cross(f, up));
    const V3 u = cross(s, f);
    M4 r;
    r.at(0, 0) = s.x, r.at(0, 1) = s.y, r.at(0, 2) = s.z, r.at(0, 3) = -dot(s, eye);
    r.at(1, 0) = u.x, r.at(1, 1) = u.y, r.at(1, 2) = u.z, r.at(1, 3) = -dot(u, eye);
    r.at(2, 0) = -f.x, r.at(2, 1) = -f.y, r.at(2, 2) = -f.z, r.at(2, 3) = dot(f, eye);
    return r;
}

inline M4 perspective(float fov_y_rad, float aspect, float near_z, float far_z) {
    const float t = 1.0f / std::tan(fov_y_rad / 2);
    M4 r;
    r.at(0, 0) = t / aspect;
    r.at(1, 1) = t;
    r.at(2, 2) = (far_z + near_z) / (near_z - far_z);
    r.at(2, 3) = 2 * far_z * near_z / (near_z - far_z);
    r.at(3, 2) = -1;
    r.at(3, 3) = 0;
    return r;
}

}  // namespace gui
