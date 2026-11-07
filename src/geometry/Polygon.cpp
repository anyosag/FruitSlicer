// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "geometry/Polygon.h"

namespace fruit {

namespace C = Clipper2Lib;

double area(const Path& p) { return C::Area(p) / (kScale * kScale); }

double area(const Paths& p) {
    double a = 0;
    for (const auto& path : p) a += area(path);
    return a;
}

double perimeter(const Path& p) {
    if (p.size() < 2) return 0;
    double len = 0;
    for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) {
        const double dx = toMm(p[i].x - p[j].x), dy = toMm(p[i].y - p[j].y);
        len += std::sqrt(dx * dx + dy * dy);
    }
    return len;
}

BBox2 bbox(const Path& p) {
    BBox2 b;
    for (const auto& pt : p) b.extend(toVec(pt));
    return b;
}

BBox2 bbox(const Paths& p) {
    BBox2 b;
    for (const auto& path : p) b.extend(bbox(path));
    return b;
}

double ExPolygon::area() const {
    double a = fruit::area(contour);
    for (const auto& h : holes) a += fruit::area(h);  // holes are negative
    return a;
}

BBox2 ExPolygon::bbox() const { return fruit::bbox(contour); }

Paths ExPolygon::toPaths() const {
    Paths out;
    out.reserve(holes.size() + 1);
    out.push_back(contour);
    out.insert(out.end(), holes.begin(), holes.end());
    return out;
}

bool ExPolygon::contains(const Vec2& p) const {
    const Point64 q = toPoint(p);
    if (C::PointInPolygon(q, contour) == C::PointInPolygonResult::IsOutside) return false;
    for (const auto& h : holes)
        if (C::PointInPolygon(q, h) == C::PointInPolygonResult::IsInside) return false;
    return true;
}

Paths unionNonZero(const Paths& subjects) { return C::Union(subjects, C::FillRule::NonZero); }

Paths unionOf(const Paths& a, const Paths& b) { return C::Union(a, b, C::FillRule::NonZero); }

Paths intersection(const Paths& a, const Paths& b) {
    if (a.empty() || b.empty()) return {};
    return C::Intersect(a, b, C::FillRule::NonZero);
}

Paths difference(const Paths& a, const Paths& b) {
    if (a.empty()) return {};
    if (b.empty()) return a;
    return C::Difference(a, b, C::FillRule::NonZero);
}

Paths offset(const Paths& p, double delta_mm, double arc_tolerance_mm) {
    if (p.empty()) return {};
    if (delta_mm == 0) return unionNonZero(p);
    return C::InflatePaths(p, delta_mm * kScale, C::JoinType::Round, C::EndType::Polygon, 2.0,
                           arc_tolerance_mm * kScale);
}

Paths simplify(const Paths& p, double tolerance_mm) {
    Paths out;
    out.reserve(p.size());
    for (const Path& path : p) {
        Path q = C::SimplifyPath(path, tolerance_mm * kScale, true);
        if (q.size() >= 3) out.push_back(std::move(q));
    }
    return out;
}

static void collect(const C::PolyPath64& node, ExPolygons& out) {
    for (size_t i = 0; i < node.Count(); ++i) {
        const C::PolyPath64* outer = node.Child(i);
        ExPolygon ex;
        ex.contour = outer->Polygon();
        if (C::Area(ex.contour) < 0) std::reverse(ex.contour.begin(), ex.contour.end());
        for (size_t j = 0; j < outer->Count(); ++j) {
            const C::PolyPath64* hole = outer->Child(j);
            Path h = hole->Polygon();
            if (C::Area(h) > 0) std::reverse(h.begin(), h.end());
            ex.holes.push_back(std::move(h));
            collect(*hole, out);  // islands inside this hole
        }
        out.push_back(std::move(ex));
    }
}

ExPolygons toExPolygons(const Paths& p) {
    ExPolygons out;
    if (p.empty()) return out;
    C::Clipper64 clipper;
    clipper.AddSubject(p);
    C::PolyTree64 tree;
    clipper.Execute(C::ClipType::Union, C::FillRule::NonZero, tree);
    collect(tree, out);
    return out;
}

Paths toPaths(const ExPolygons& ex) {
    Paths out;
    for (const auto& e : ex) {
        out.push_back(e.contour);
        out.insert(out.end(), e.holes.begin(), e.holes.end());
    }
    return out;
}

double area(const ExPolygons& ex) {
    double a = 0;
    for (const auto& e : ex) a += e.area();
    return a;
}

}  // namespace fruit
