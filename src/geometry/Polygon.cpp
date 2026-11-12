// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "geometry/Polygon.h"

#include <algorithm>

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

AreaMoments areaMoments(const Paths& p) {
    // Green's theorem, edge by edge, about the first point: sums of numbers
    // of the outline's own size, not of its distance from the plate's centre.
    const Path* first = nullptr;
    for (const Path& path : p)
        if (!path.empty()) { first = &path; break; }
    AreaMoments m;
    if (!first) return m;
    const Vec2 o = toVec(first->front());
    double a2 = 0, sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    for (const Path& path : p) {
        const size_t n = path.size();
        if (n < 3) continue;
        for (size_t i = 0; i < n; ++i) {
            const Vec2 a = toVec(path[i]) - o, b = toVec(path[(i + 1) % n]) - o;
            const double c = cross(a, b);
            a2 += c;
            sx += (a.x + b.x) * c;
            sy += (a.y + b.y) * c;
            sxx += (a.x * a.x + a.x * b.x + b.x * b.x) * c;
            syy += (a.y * a.y + a.y * b.y + b.y * b.y) * c;
            sxy += (a.x * b.y + 2 * a.x * a.y + 2 * b.x * b.y + b.x * a.y) * c;
        }
    }
    const double area = a2 / 2;
    if (!(std::abs(area) > 1e-14)) return m;
    const double cx = sx / (6 * area), cy = sy / (6 * area);
    // A clockwise outline has all its sums negated: the centre is the same, the rest turns round.
    const double sign = area < 0 ? -1.0 : 1.0;
    m.area = area * sign;
    m.centroid = o + Vec2{cx, cy};
    m.sxx = std::max(0.0, (sxx / 12 - area * cx * cx) * sign);
    m.syy = std::max(0.0, (syy / 12 - area * cy * cy) * sign);
    m.sxy = (sxy / 24 - area * cx * cy) * sign;
    return m;
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

std::vector<std::pair<double, double>> lineSpans(const Paths& p, const Vec2& at, const Vec2& dir) {
    // Each edge that crosses the line, where along it and which way; then
    // the winding number swept along the line.
    std::vector<std::pair<double, int>> hits;
    for (const Path& path : p) {
        const size_t n = path.size();
        for (size_t i = 0; i < n; ++i) {
            const Vec2 a = toVec(path[i]), b = toVec(path[(i + 1) % n]);
            const double sa = cross(dir, a - at), sb = cross(dir, b - at);
            if ((sa > 0) == (sb > 0)) continue;
            const Vec2 q = a + (b - a) * (sa / (sa - sb));
            hits.emplace_back(dot(q - at, dir), sb > 0 ? 1 : -1);
        }
    }
    std::sort(hits.begin(), hits.end());
    std::vector<std::pair<double, double>> out;
    int winding = 0;
    double start = 0;
    for (const auto& [t, w] : hits) {
        const int before = winding;
        winding += w;
        if (before == 0 && winding != 0) start = t;
        else if (before != 0 && winding == 0 && t > start) out.emplace_back(start, t);
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
