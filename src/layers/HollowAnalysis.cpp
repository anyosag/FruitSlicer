// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "layers/HollowAnalysis.h"

#include <algorithm>
#include <numeric>

#include "core/Parallel.h"

namespace fruit {

namespace {

namespace C = Clipper2Lib;

struct Hollow {
    int layer;
    Paths shape;  // the region (non-zero): the hole counter-clockwise, the islands in it clockwise
    BBox2 box;
    double area;
    double cured;  // mm² of resin cured inside the outline of the polygon it is a hole of
};

Path oriented(const Path& p, bool ccw) {
    Path out = p;
    if ((C::Area(out) > 0) != ccw) std::reverse(out.begin(), out.end());
    return out;
}

// What cures inside a polygon's outline: the polygon, and the islands in its
// holes with theirs.
double material(const C::PolyPath64& polygon) {
    double a = std::abs(area(polygon.Polygon()));
    for (size_t j = 0; j < polygon.Count(); ++j) {
        const C::PolyPath64& hole = *polygon.Child(j);
        a -= std::abs(area(hole.Polygon()));
        for (size_t k = 0; k < hole.Count(); ++k) a += material(*hole.Child(k));
    }
    return a;
}

// The hollows under `node` (the layer's root, or a hole), the deepest first:
// each hole of each polygon as the open region it leaves, which is the hole
// less the islands standing in it. A groove round a solid block is the
// groove, not all of its outline.
void collectHollows(const C::PolyPath64& node, int layer, std::vector<Hollow>& out) {
    for (size_t i = 0; i < node.Count(); ++i) {
        const C::PolyPath64& polygon = *node.Child(i);
        for (size_t j = 0; j < polygon.Count(); ++j) collectHollows(*polygon.Child(j), layer, out);
        const double cured = polygon.Count() ? material(polygon) : 0;
        for (size_t j = 0; j < polygon.Count(); ++j) {
            const C::PolyPath64& hole = *polygon.Child(j);
            Paths shape{oriented(hole.Polygon(), true)};
            for (size_t k = 0; k < hole.Count(); ++k) shape.push_back(oriented(hole.Child(k)->Polygon(), false));
            const double a = area(shape);
            if (a < 1e-6) continue;
            const BBox2 box = bbox(shape.front());
            out.push_back({layer, std::move(shape), box, a, cured});
        }
    }
}

// The widest circle inside `region`: its radius, and a point well inside
// (to a hundredth of a millimetre).
std::pair<double, Vec2> widestCircle(const Paths& region) {
    const BBox2 b = bbox(region);
    double lo = 0, hi = 0.5 * std::min(b.size().x, b.size().y);
    Paths last = region;
    while (hi - lo > 0.005) {
        const double mid = 0.5 * (lo + hi);
        Paths e = offset(region, -mid);
        if (std::abs(area(e)) < 1e-8) {
            hi = mid;
        } else {
            lo = mid;
            last = std::move(e);
        }
    }
    // A point of what is left: its biggest piece's middle when that is in it
    // (it isn't in a ring), else a corner of it.
    const ExPolygons parts = toExPolygons(last);
    const ExPolygon* biggest = nullptr;
    for (const ExPolygon& e : parts)
        if (!biggest || e.area() > biggest->area()) biggest = &e;
    if (!biggest || biggest->contour.empty()) return {lo, b.center()};
    const Vec2 mid = biggest->bbox().center();
    return {lo, biggest->contains(mid) ? mid : toVec(biggest->contour.front())};
}

// How far `pocket` is from `outside` (to a hundredth of a millimetre, up to
// `cap`), and the point of the outside nearest it. -1 when further than cap.
std::pair<double, Vec2> nearestOutside(const Paths& pocket, const Paths& outside, double cap) {
    if (outside.empty()) return {-1, {}};
    auto meets = [&](double d) { return intersection(offset(pocket, d), outside); };
    if (std::abs(area(meets(cap))) < 1e-8) return {-1, {}};
    double lo = 0, hi = cap;
    while (hi - lo > 0.005) {
        const double mid = 0.5 * (lo + hi);
        (std::abs(area(meets(mid))) < 1e-8 ? lo : hi) = mid;
    }
    return {hi, bbox(meets(hi + 0.02)).center()};
}

}  // namespace

CupGrade gradeCup(const CupMeasures& m, int layers, const CupLimits& l) {
    if (m.share >= l.problem_from) return CupGrade::Problem;
    if (m.share < l.note_below) return CupGrade::Note;
    return layers >= l.deep_layers ? CupGrade::Problem : CupGrade::Warning;
}

const char* gradeName(CupGrade g) {
    return g == CupGrade::Problem ? "problem" : g == CupGrade::Warning ? "warning" : "note";
}

HollowReport analyzeHollows(const LayerStack& layers, double min_area, double min_volume, unsigned threads) {
    const size_t n = layers.size();
    HollowReport report;
    report.trap_regions.assign(n, {});
    report.cup_regions.assign(n, {});
    std::vector<Hollow> hollows;
    std::vector<std::vector<int>> by_layer(n);
    std::vector<Paths> filled(n);  // material + enclosed holes (outer contours only)
    // Each layer on its own (in parallel), then numbered in layer order.
    std::vector<std::vector<Hollow>> holes_in(n);
    parallelFor(n, threads, [&](size_t L) {
        if (layers.slices[L].empty()) return;
        C::Clipper64 clipper;
        clipper.AddSubject(layers.slices[L]);
        C::PolyTree64 tree;
        clipper.Execute(C::ClipType::Union, C::FillRule::NonZero, tree);
        collectHollows(tree, static_cast<int>(L), holes_in[L]);
        // Every hole filled: the outlines of the outermost polygons.
        for (size_t i = 0; i < tree.Count(); ++i) filled[L].push_back(oriented(tree.Child(i)->Polygon(), true));
    });
    for (size_t L = 0; L < n; ++L)
        for (Hollow& h : holes_in[L]) {
            by_layer[L].push_back(static_cast<int>(hollows.size()));
            hollows.push_back(std::move(h));
        }
    holes_in.clear();
    // Union-find over hollows overlapping between adjacent layers.
    std::vector<int> uf(hollows.size());
    std::iota(uf.begin(), uf.end(), 0);
    auto find = [&](int a) {
        while (uf[a] != a) a = uf[a] = uf[uf[a]];
        return a;
    };
    std::vector<char> open_below(hollows.size(), 0), open_above(hollows.size(), 0);
    auto outsideAt = [&](const Hollow& h, long L) {
        // Part of the hollow's footprint that is outside the model at layer L.
        // Under the first layer is the plate, which seals it.
        if (L < 0) return false;
        if (L >= static_cast<long>(n)) return true;
        return std::abs(area(difference(h.shape, filled[L]))) > 1e-4;
    };
    // The geometry for each hollow in parallel; the joins after, in order.
    std::vector<std::vector<int>> joins(hollows.size());
    parallelFor(hollows.size(), threads, [&](size_t i) {
        const Hollow& h = hollows[i];
        open_below[i] = outsideAt(h, h.layer - 1);
        open_above[i] = outsideAt(h, h.layer + 1);
        if (h.layer + 1 < static_cast<int>(n))
            for (int j : by_layer[h.layer + 1]) {
                const Hollow& g = hollows[j];
                if (!h.box.overlaps(g.box)) continue;
                if (std::abs(area(intersection(h.shape, g.shape))) > 1e-4) joins[i].push_back(j);
            }
    });
    for (size_t i = 0; i < hollows.size(); ++i)
        for (int j : joins[i]) uf[find(static_cast<int>(i))] = find(j);
    struct Acc {
        bool below = false, above = false;
        int first = 1 << 30, last = -1;
        double volume = 0, max_area = 0;
        Vec2 moment{};
        std::vector<int> members;
    };
    std::vector<Acc> acc(hollows.size());
    for (size_t i = 0; i < hollows.size(); ++i) {
        Acc& a = acc[find(static_cast<int>(i))];
        const Hollow& h = hollows[i];
        a.below = a.below || open_below[i];
        a.above = a.above || open_above[i];
        a.first = std::min(a.first, h.layer);
        a.last = std::max(a.last, h.layer);
        a.volume += h.area * layers.layer_height;
        a.max_area = std::max(a.max_area, h.area);
        a.moment += h.box.center() * h.area;
        a.members.push_back(static_cast<int>(i));
    }
    for (size_t r = 0; r < acc.size(); ++r) {
        const Acc& a = acc[r];
        if (a.members.empty() || a.below) continue;  // drains towards the plate
        if (a.max_area < min_area || a.volume < min_volume) continue;
        HollowComponent c;
        c.kind = a.above ? HollowKind::SuctionCup : HollowKind::ResinTrap;
        c.first_layer = a.first;
        c.last_layer = a.last;
        c.volume = a.volume;
        c.max_area = a.max_area;
        double total = 0;
        for (int m : a.members) total += hollows[m].area;
        c.center = a.moment / std::max(total, 1e-12);
        // Its biggest layer (the lowest of equals) and a point inside it.
        int big = a.members.front();
        for (int m : a.members) {
            c.box.extend(hollows[m].box);
            const Hollow &h = hollows[m], &b = hollows[big];
            if (h.area > b.area * (1 + 1e-9) || (h.area >= b.area * (1 - 1e-9) && h.layer < b.layer)) big = m;
        }
        const Hollow& biggest = hollows[big];
        const auto [radius, inside] = widestCircle(biggest.shape);
        c.inside = inside;
        c.inside_layer = biggest.layer;
        if (c.kind == HollowKind::SuctionCup) {
            CupMeasures& cm = c.cup;
            for (int m : a.members) {
                const Hollow& h = hollows[m];
                const double share = h.area / std::max(h.cured, 1e-9);
                if (share > cm.share * (1 + 1e-9) || (share >= cm.share * (1 - 1e-9) && cm.share_layer >= 0 && h.layer < cm.share_layer)) {
                    cm.share = share;
                    cm.share_layer = h.layer;
                    cm.open_area = h.area;
                    cm.cured_around = h.cured;
                }
            }
            double edges = 0;
            for (const Path& q : biggest.shape) edges += perimeter(q);
            cm.width = 2 * biggest.area / std::max(edges, 1e-9);
            cm.length = biggest.area / std::max(cm.width, 1e-9);
            cm.across = 2 * radius;
            // The thinnest wall out: from the hole's edge to outside every outline.
            const Paths edge{biggest.shape.front()};
            BBox2 around = bbox(filled[biggest.layer]);
            around.extend(Vec2{around.min.x - 1, around.min.y - 1});
            around.extend(Vec2{around.max.x + 1, around.max.y + 1});
            const Path frame{toPoint(around.min), toPoint({around.max.x, around.min.y}), toPoint(around.max),
                             toPoint({around.min.x, around.max.y})};
            const auto [wall, at] = nearestOutside(edge, difference({frame}, filled[biggest.layer]), 50.0);
            cm.wall = wall;
            cm.wall_at = at;
            // Standing on the plate under its lowest layer (solid all the way
            // down), or the plate is its floor.
            if (a.first == 0) {
                cm.floor_on_plate = true;
            } else if (!layers.slices.empty() && !layers.slices[0].empty()) {
                const Hollow* low = nullptr;
                for (int m : a.members)
                    if (hollows[m].layer == a.first && (!low || hollows[m].area > low->area)) low = &hollows[m];
                if (low) {
                    const Paths foot{low->shape.front()};
                    cm.floor_on_plate = std::abs(area(intersection(foot, layers.slices[0]))) >= 0.5 * std::abs(area(foot));
                }
            }
        }
        report.components.push_back(c);
        auto& regions = c.kind == HollowKind::ResinTrap ? report.trap_regions : report.cup_regions;
        for (int m : a.members) {
            Paths& dst = regions[hollows[m].layer];
            dst.insert(dst.end(), hollows[m].shape.begin(), hollows[m].shape.end());
        }
    }
    return report;
}

}  // namespace fruit
