// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// 2D polygon helpers on top of Clipper2.
//
// Polygons are stored in fixed point: 1 unit = 0.1 µm (kScale units per mm).
// That is ~1/140 of a Saturn 4 Ultra 16K pixel, far below anything printable,
// and keeps every cross product of build-plate coordinates exact in double.

#include <clipper2/clipper.h>

#include <cstdint>
#include <vector>

#include "geometry/Vec.h"

namespace fruit {

using Path = Clipper2Lib::Path64;
using Paths = Clipper2Lib::Paths64;
using Point64 = Clipper2Lib::Point64;

constexpr double kScale = 1e4;  // units per mm

inline int64_t toUnits(double mm) { return static_cast<int64_t>(std::llround(mm * kScale)); }
inline double toMm(int64_t u) { return static_cast<double>(u) / kScale; }
inline Point64 toPoint(const Vec2& p) { return {toUnits(p.x), toUnits(p.y)}; }
inline Vec2 toVec(const Point64& p) { return {toMm(p.x), toMm(p.y)}; }

struct BBox2 {
    Vec2 min{std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
    Vec2 max{std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest()};
    bool empty() const { return min.x > max.x || min.y > max.y; }
    void extend(const Vec2& p) {
        min = {std::min(min.x, p.x), std::min(min.y, p.y)};
        max = {std::max(max.x, p.x), std::max(max.y, p.y)};
    }
    void extend(const BBox2& b) {
        if (!b.empty()) { extend(b.min); extend(b.max); }
    }
    bool overlaps(const BBox2& o, double margin = 0) const {
        return !(o.min.x > max.x + margin || o.max.x < min.x - margin ||
                 o.min.y > max.y + margin || o.max.y < min.y - margin);
    }
    Vec2 size() const { return empty() ? Vec2{} : max - min; }
    Vec2 center() const { return (min + max) * 0.5; }
};

// A polygon with holes. contour is counter-clockwise, holes clockwise.
struct ExPolygon {
    Path contour;
    Paths holes;

    double area() const;  // mm²
    BBox2 bbox() const;
    Paths toPaths() const;
    bool contains(const Vec2& p) const;  // inside or on boundary
};
using ExPolygons = std::vector<ExPolygon>;

double area(const Path& p);   // signed, mm²
double area(const Paths& p);  // signed sum, mm²
double perimeter(const Path& p);  // mm
BBox2 bbox(const Path& p);
BBox2 bbox(const Paths& p);

Paths unionNonZero(const Paths& subjects);
Paths unionOf(const Paths& a, const Paths& b);
Paths intersection(const Paths& a, const Paths& b);
Paths difference(const Paths& a, const Paths& b);
// Offset by delta mm (negative shrinks). Round joins with 0.5 µm arc tolerance.
Paths offset(const Paths& p, double delta_mm);

// Group (already unioned, NonZero) paths into polygons-with-holes. Islands
// inside holes become their own ExPolygons.
ExPolygons toExPolygons(const Paths& p);
Paths toPaths(const ExPolygons& ex);
double area(const ExPolygons& ex);

}  // namespace fruit
