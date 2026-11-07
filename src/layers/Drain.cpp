// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "layers/Drain.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

#include "core/Parallel.h"
#include "layers/HollowAnalysis.h"
#include "mesh/AabbTree.h"

namespace fruit {

namespace {

constexpr int kSides = 32;

// Counter-clockwise convex hull (Andrew's monotone chain); empty below three points.
Path convexHull(std::vector<Point64> pts) {
    std::sort(pts.begin(), pts.end(), [](const Point64& a, const Point64& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    if (pts.size() < 3) return {};
    auto turn = [](const Point64& o, const Point64& a, const Point64& b) {
        return static_cast<double>(a.x - o.x) * static_cast<double>(b.y - o.y) -
               static_cast<double>(a.y - o.y) * static_cast<double>(b.x - o.x);
    };
    Path hull(2 * pts.size());
    size_t k = 0;
    for (const Point64& p : pts) {
        while (k >= 2 && turn(hull[k - 2], hull[k - 1], p) <= 0) --k;
        hull[k++] = p;
    }
    const size_t lower = k + 1;
    for (size_t i = pts.size() - 1; i-- > 0;) {
        while (k >= lower && turn(hull[k - 2], hull[k - 1], pts[i]) <= 0) --k;
        hull[k++] = pts[i];
    }
    hull.resize(k - 1);
    return hull.size() >= 3 ? hull : Path{};
}

// Whether points lie inside a stack's slices, or with `outline` inside their
// outlines (holes counted in); each layer's polygons made on first use.
class Inside {
public:
    explicit Inside(const LayerStack& s, bool outline = false) : s_(s), outline_(outline) {}
    bool operator()(const Vec3& p) {
        if (p.z < 0) return false;
        const size_t j = s_.layerAt(p.z);
        if (j >= s_.size()) return false;
        auto it = cache_.find(j);
        if (it == cache_.end()) it = cache_.emplace(j, toExPolygons(s_.slices[j])).first;
        for (const ExPolygon& e : it->second)
            if (outline_ ? ExPolygon{e.contour, {}}.contains(p.xy()) : e.contains(p.xy())) return true;
        return false;
    }

private:
    const LayerStack& s_;
    bool outline_;
    std::map<size_t, ExPolygons> cache_;
};

// The point deepest inside `region` (to a hundredth of a millimetre): what
// is left when it is eroded as far as it goes.
Vec2 deepestPoint(const Paths& region) {
    const BBox2 b = bbox(region);
    double lo = 0, hi = 0.5 * std::min(b.size().x, b.size().y);
    Paths last = region;
    while (hi - lo > 0.01) {
        const double mid = 0.5 * (lo + hi);
        Paths e = offset(region, -mid);
        if (e.empty()) {
            hi = mid;
        } else {
            lo = mid;
            last = std::move(e);
        }
    }
    const ExPolygons parts = toExPolygons(last);
    const ExPolygon* biggest = nullptr;
    for (const ExPolygon& e : parts)
        if (!biggest || e.area() > biggest->area()) biggest = &e;
    return biggest ? biggest->bbox().center() : b.center();
}

}  // namespace

Path holeFootprint(const Vec3& a, const Vec3& b, double r, double z0, double z1) {
    if (r <= 0 || norm(b - a) <= 0) return {};
    const Vec3 axis = normalized(b - a);
    const Vec3 any = std::abs(axis.z) < 0.9 ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
    const Vec3 u = normalized(cross(axis, any)), v = cross(axis, u);
    const double corner = r / std::cos(kPi / kSides);  // the prism's sides touch the circle
    std::vector<Vec3> ra(kSides), rb(kSides);
    for (int k = 0; k < kSides; ++k) {
        const double th = 2 * kPi * k / kSides;
        const Vec3 off = (u * std::cos(th) + v * std::sin(th)) * corner;
        ra[k] = a + off;
        rb[k] = b + off;
    }
    // The prism within the layer is a convex solid: its corners are the
    // prism's own corners in the layer and its edges' crossings of the
    // layer's top and bottom.
    std::vector<Point64> pts;
    auto corner_in = [&](const Vec3& p) {
        if (p.z >= z0 && p.z <= z1) pts.push_back(toPoint(p.xy()));
    };
    auto edge = [&](const Vec3& p, const Vec3& q) {
        for (const double z : {z0, z1})
            if ((p.z - z) * (q.z - z) < 0) pts.push_back(toPoint((p + (q - p) * ((z - p.z) / (q.z - p.z))).xy()));
    };
    for (int k = 0; k < kSides; ++k) {
        const int next = (k + 1) % kSides;
        corner_in(ra[k]);
        corner_in(rb[k]);
        edge(ra[k], ra[next]);
        edge(rb[k], rb[next]);
        edge(ra[k], rb[k]);
    }
    return convexHull(std::move(pts));
}

std::vector<PlacedHole> placeHoles(const std::vector<DrainHole>& holes, const Transform& to_printer,
                                   const LayerStack& solid, const HollowResult& hollow) {
    Inside inside(solid);
    const double step = 0.5 * std::min(0.05, solid.layer_height);
    const double top = static_cast<double>(solid.size()) * solid.layer_height;
    std::vector<PlacedHole> out;
    out.reserve(holes.size());
    for (const DrainHole& h : holes) {
        PlacedHole p;
        p.radius = 0.5 * h.diameter;
        const Vec3 at = to_printer.apply(h.at);
        const Vec3 axis = normalized(to_printer.applyLinear(h.axis));
        // Start outside: where the surface slants, its near lip goes too.
        p.from = at - axis * (2 * p.radius);
        p.to = at;
        // Into the solid (the start may lie a little off the surface), then
        // the first cavity, then far enough to know how deep it goes.
        double t_in = -1, t_out = -1;
        bool entered = false;
        for (double t = 0; t < 1000; t += step) {  // a metre: no model is bigger
            const Vec3 q = at + axis * t;
            const bool in_stack = q.z >= 0 && q.z < top;
            const int c = in_stack ? hollow.cavityAt(solid.layerAt(q.z), q.xy()) : -1;
            if (t_in < 0) {
                if (c >= 0) {
                    t_in = t;
                    p.cavity = c;
                } else if (inside(q)) {
                    entered = true;
                } else if (entered || t > 2 * p.radius) {
                    break;  // out of the model again, or never in it
                }
            } else if (c != p.cavity || t - t_in >= 2 * h.diameter) {
                t_out = t;
                break;
            }
        }
        if (p.cavity >= 0) p.to = at + axis * (t_in + std::min(h.diameter, 0.5 * (t_out - t_in)));
        out.push_back(p);
    }
    return out;
}

LayerStack drilledLayers(const LayerStack& solid, const HollowResult& hollow, const std::vector<PlacedHole>& holes) {
    LayerStack out = hollowedLayers(solid, hollow);
    std::vector<Paths> cuts(out.size());
    for (const PlacedHole& p : holes) {
        if (p.cavity < 0) continue;
        const double reach = p.radius / std::cos(kPi / kSides);
        const double lo = std::min(p.from.z, p.to.z) - reach, hi = std::max(p.from.z, p.to.z) + reach;
        if (hi < 0) continue;
        for (size_t i = out.layerAt(std::max(0.0, lo)); i < out.size() && out.layerBottom(i) <= hi; ++i) {
            Path f = holeFootprint(p.from, p.to, p.radius, out.layerBottom(i), out.layerTop(i));
            if (!f.empty()) cuts[i].push_back(std::move(f));
        }
    }
    parallelFor(out.size(), 0, [&](size_t i) {
        if (!cuts[i].empty()) out.slices[i] = difference(out.slices[i], cuts[i]);
    });
    return out;
}

std::vector<char> cavitiesDrain(const LayerStack& drilled, const HollowResult& hollow) {
    std::vector<char> drains(hollow.cavities.size(), 1);
    const HollowReport report = analyzeHollows(drilled);
    for (size_t j = 0; j < hollow.pieces.size() && j < drilled.size(); ++j) {
        Paths closed = report.trap_regions[j];
        closed.insert(closed.end(), report.cup_regions[j].begin(), report.cup_regions[j].end());
        if (closed.empty()) continue;
        for (const InteriorPiece& k : hollow.pieces[j])
            if (drains[k.cavity] && std::abs(area(intersection(hollow.piece(j, k), closed))) > 1e-3)
                drains[k.cavity] = 0;
    }
    return drains;
}

std::vector<DrainHole> suggestDrainHoles(const TriangleMesh& placed, const Transform& to_printer,
                                         const LayerStack& solid, const HollowResult& hollow, double diameter) {
    const AabbTree tree(placed);
    const Transform to_model = to_printer.inverse();
    // Straight down (up) first, then at 60° and 30°, then level, all round.
    auto waysOut = [](double sign) {
        std::vector<Vec3> ways{{0, 0, sign}};
        for (const double elevation : {60.0, 30.0, 0.0})
            for (int k = 0; k < 12; ++k) {
                const double az = deg2rad(30.0 * k), el = deg2rad(sign * elevation);
                ways.push_back({std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el)});
            }
        return ways;
    };
    const std::vector<Vec3> down = waysOut(-1), up = waysOut(1);
    // Where a ray from inside leaves the model for good: the first surface
    // it crosses beyond which it is outside every outline. (A model's own
    // sealed hollow has walls in the mesh; the ray crosses those first.)
    Inside outline(solid, true);
    auto exit = [&](const Vec3& start, const Vec3& w) -> std::optional<RayHit> {
        double t0 = 1e-9;
        for (int k = 0; k < 64; ++k) {
            const auto hit = tree.raycast(start, w, 1e30, t0);
            if (!hit) return std::nullopt;
            if (!outline(hit->point + w * 0.01)) return hit;
            t0 = hit->t + 1e-6;
        }
        return std::nullopt;
    };
    // A hole into cavity c from its biggest piece in `layer`, along the
    // shortest of `ways` out of the model.
    auto holeFrom = [&](size_t c, size_t layer, const std::vector<Vec3>& ways, bool air) -> std::optional<DrainHole> {
        Paths biggest;
        double best = 0;
        for (const InteriorPiece& k : hollow.pieces[layer])
            if (k.cavity == static_cast<int>(c)) {
                Paths piece = hollow.piece(layer, k);
                const double ar = std::abs(area(piece));
                if (ar > best) {
                    best = ar;
                    biggest = std::move(piece);
                }
            }
        if (biggest.empty()) return std::nullopt;
        const Vec2 deep = deepestPoint(biggest);
        const Vec3 start{deep.x, deep.y, solid.sample_z[layer]};
        std::optional<RayHit> shortest;
        Vec3 way;
        for (const Vec3& w : ways) {
            const auto hit = exit(start, w);
            if (hit && (!shortest || hit->t < 0.99 * shortest->t)) {  // straight down (up) wins a near tie
                shortest = hit;
                way = w;
            }
        }
        if (!shortest) return std::nullopt;
        return DrainHole{to_model.apply(shortest->point), normalized(to_model.applyLinear(-way)), diameter, air};
    };
    std::vector<DrainHole> out;
    for (size_t c = 0; c < hollow.cavities.size(); ++c) {
        if (auto h = holeFrom(c, static_cast<size_t>(hollow.cavities[c].first_layer), down, false)) out.push_back(*h);
        if (auto h = holeFrom(c, static_cast<size_t>(hollow.cavities[c].last_layer), up, true)) out.push_back(*h);
    }
    return out;
}

}  // namespace fruit
