// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Small fixed-size vector types used throughout FruitSlicer.
// All 3D coordinates are millimetres in the printer frame (Z up, build plate at Z = 0).

#include <algorithm>
#include <cmath>
#include <limits>

namespace fruit {

struct Vec2 {
    double x = 0, y = 0;

    constexpr Vec2() = default;
    constexpr Vec2(double x_, double y_) : x(x_), y(y_) {}

    constexpr Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator*(double s) const { return {x * s, y * s}; }
    constexpr Vec2 operator/(double s) const { return {x / s, y / s}; }
    constexpr Vec2 operator-() const { return {-x, -y}; }
    Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(const Vec2& o) { x -= o.x; y -= o.y; return *this; }
    Vec2& operator*=(double s) { x *= s; y *= s; return *this; }
    constexpr bool operator==(const Vec2&) const = default;
};

constexpr double dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
constexpr double cross(const Vec2& a, const Vec2& b) { return a.x * b.y - a.y * b.x; }
inline double norm(const Vec2& a) { return std::sqrt(dot(a, a)); }
constexpr double norm2(const Vec2& a) { return dot(a, a); }
inline Vec2 normalized(const Vec2& a) {
    const double n = norm(a);
    return n > 0 ? a / n : Vec2{};
}
inline double distance(const Vec2& a, const Vec2& b) { return norm(a - b); }

struct Vec3 {
    double x = 0, y = 0, z = 0;

    constexpr Vec3() = default;
    constexpr Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

    constexpr Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator/(double s) const { return {x / s, y / s, z / s}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(double s) { x *= s; y *= s; z *= s; return *this; }
    constexpr bool operator==(const Vec3&) const = default;

    double& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
    double operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }

    constexpr Vec2 xy() const { return {x, y}; }
};

constexpr Vec3 operator*(double s, const Vec3& v) { return v * s; }
constexpr double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double norm(const Vec3& a) { return std::sqrt(dot(a, a)); }
constexpr double norm2(const Vec3& a) { return dot(a, a); }
inline Vec3 normalized(const Vec3& a) {
    const double n = norm(a);
    return n > 0 ? a / n : Vec3{};
}
inline double distance(const Vec3& a, const Vec3& b) { return norm(a - b); }
inline Vec3 min(const Vec3& a, const Vec3& b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline Vec3 max(const Vec3& a, const Vec3& b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }

constexpr double kPi = 3.14159265358979323846;
constexpr double deg2rad(double d) { return d * kPi / 180.0; }
constexpr double rad2deg(double r) { return r * 180.0 / kPi; }

}  // namespace fruit
