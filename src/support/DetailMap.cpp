// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "support/DetailMap.h"

#include <algorithm>
#include <atomic>
#include <cmath>

#include "core/Parallel.h"

#include "ops/MeshOps.h"

namespace fruit {

namespace {

constexpr double kBand = 1.0;  // mm of height per index band
// A slot's outline may move this far (mm) from where it started and still be
// one region: a slanted slot moves a little each layer, and its region is all
// the layers' outlines together, at most this much bigger than any of them.
constexpr double kDrift = 0.1;

// How much of a gap's outline is the model's: near 1 for a slot or a hole,
// about half for the fillet a closing leaves in an inside corner (two walls
// and an arc), which is no slot.
double modelShare(const ExPolygon& gap, const Paths& model) {
    double length = perimeter(gap.contour);
    for (const Path& h : gap.holes) length += perimeter(h);
    if (length <= 0) return 0;
    const double t = 0.01;
    const double touching = std::abs(area(intersection(offset(gap.toPaths(), t), model))) / t;
    return touching / length;
}

double edgeDistance(const Path& p, const Vec2& q) {
    double best = 1e18;
    for (std::size_t i = 0; i < p.size(); ++i) {
        const Vec2 a = toVec(p[i]), b = toVec(p[(i + 1) % p.size()]);
        const Vec2 ab = b - a;
        const double l2 = dot(ab, ab);
        const double t = l2 > 0 ? std::clamp(dot(q - a, ab) / l2, 0.0, 1.0) : 0.0;
        best = std::min(best, distance(q, a + ab * t));
    }
    return best;
}

// `b` (box `bb`) is the same slot as the one that started as `first` (box
// `fb`), moved by no more than kDrift.
bool sameSlot(const ExPolygon& first, const BBox2& fb, const ExPolygon& b, const BBox2& bb) {
    if (std::abs(fb.min.x - bb.min.x) > kDrift || std::abs(fb.min.y - bb.min.y) > kDrift ||
        std::abs(fb.max.x - bb.max.x) > kDrift || std::abs(fb.max.y - bb.max.y) > kDrift)
        return false;
    const double aa = first.area(), ba = b.area();
    return std::abs(aa - ba) <= 0.1 * std::max(aa, ba) + 1e-6;
}

}  // namespace

DetailMap DetailMap::build(const LayerStack& layers, double width, double depth, const JobControl& ctl) {
    DetailMap m;
    if (width <= 0 || layers.size() == 0) return m;
    const double h = width / 2;
    const std::size_t n = layers.size();
    // Each layer on its own, side by side: the model closed over its slots,
    // and the slots (simplified).
    std::vector<Paths> closed(n);
    std::vector<ExPolygons> gaps(n);
    std::atomic<std::size_t> done{0};
    parallelFor(n, 0, [&](std::size_t L) {
        ctl.check();
        const Paths& S = layers.slices[L];
        if (S.empty()) return;
        closed[L] = offset(offset(S, h, 0.005), -h, 0.005);
        for (const ExPolygon& g0 : toExPolygons(difference(closed[L], S))) {
            if (g0.area() < 0.01) continue;
            BBox2 b = g0.bbox();
            b.min.x -= 0.1, b.min.y -= 0.1, b.max.x += 0.1, b.max.y += 0.1;
            const Paths near = intersection(S, Paths{Path{toPoint(b.min), toPoint({b.max.x, b.min.y}), toPoint(b.max),
                                                          toPoint({b.min.x, b.max.y})}});
            if (modelShare(g0, near) < 0.62) continue;
            for (const ExPolygon& g : toExPolygons(simplify(g0.toPaths(), 0.005))) gaps[L].push_back(g);
        }
        if ((++done & 63) == 0) ctl.report("details", 0.9 * static_cast<double>(done) / static_cast<double>(n));
    });
    // Then joined up the layers: a slot is one region for as long as its
    // outline stays within kDrift of where it started; where it opens
    // downwards (over neither the model nor a slot of the layer below), the
    // space under it is a region too.
    struct Open {
        int region;
        bool seen;
        ExPolygon first;  // its outline where it started
        BBox2 first_box;
    };
    std::vector<Open> open;
    for (std::size_t L = 0; L < n; ++L) {
        for (Open& o : open) o.seen = false;
        for (const ExPolygon& g : gaps[L]) {
            const BBox2 gb = g.bbox();
            bool extended = false;
            for (Open& o : open) {
                if (o.seen || !sameSlot(o.first, o.first_box, g, gb)) continue;
                DetailRegion& r = m.regions_[static_cast<std::size_t>(o.region)];
                const ExPolygons both = toExPolygons(unionOf(r.shape.toPaths(), g.toPaths()));
                if (both.size() != 1) continue;
                r.shape = both.front();
                r.box = r.shape.bbox();
                r.z1 = layers.layerTop(L);
                o.seen = extended = true;
                break;
            }
            if (extended) continue;
            m.regions_.push_back({g, gb, layers.layerBottom(L), layers.layerTop(L), false});
            open.push_back({static_cast<int>(m.regions_.size()) - 1, true, g, gb});
            if (depth <= 0) continue;
            const Paths mouth = L == 0 || closed[L - 1].empty() ? g.toPaths() : difference(g.toPaths(), closed[L - 1]);
            for (const ExPolygon& mo : toExPolygons(mouth))
                if (mo.area() >= 0.005)
                    m.regions_.push_back({mo, mo.bbox(), layers.layerBottom(L) - depth, layers.layerBottom(L), true});
        }
        open.erase(std::remove_if(open.begin(), open.end(), [](const Open& o) { return !o.seen; }), open.end());
    }
    ctl.report("details", 1);
    m.index();
    return m;
}

void DetailMap::index() {
    by_z_.clear();
    if (regions_.empty()) return;
    double lo = 1e18, hi = -1e18;
    for (const DetailRegion& r : regions_) lo = std::min(lo, r.z0), hi = std::max(hi, r.z1);
    z_base_ = std::floor(lo / kBand) * kBand;
    by_z_.assign(static_cast<std::size_t>(std::floor((hi - z_base_) / kBand)) + 1, {});
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        const auto a = static_cast<std::size_t>(std::floor((regions_[i].z0 - z_base_) / kBand));
        const auto b = std::min(by_z_.size() - 1, static_cast<std::size_t>(std::floor((regions_[i].z1 - z_base_) / kBand)));
        for (std::size_t k = a; k <= b; ++k) by_z_[k].push_back(static_cast<int>(i));
    }
}

void DetailMap::addPlateHoles(const Paths& first, double top, double max_width) {
    for (const ExPolygon& e : toExPolygons(first))
        for (const Path& h : e.holes) {
            ExPolygon hole;
            hole.contour = h;
            if (area(hole.contour) < 0) std::reverse(hole.contour.begin(), hole.contour.end());
            if (hole.area() < 0.01) continue;
            // Wider than `max_width` (a circle that wide fits in it): left to the feet's gap.
            if (!offset(hole.toPaths(), -max_width / 2).empty()) continue;
            DetailRegion d{hole, hole.bbox(), 0, top, false, true};
            regions_.push_back(d);
        }
    index();
}

std::size_t DetailMap::slotCount() const {
    return static_cast<std::size_t>(std::count_if(regions_.begin(), regions_.end(), [](const DetailRegion& r) { return !r.mouth; }));
}

std::vector<int> DetailMap::under(const Vec3& contact, double r, double below) const {
    std::vector<int> out;
    const Vec2 q = contact.xy();
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        const DetailRegion& d = regions_[i];
        if (d.plate_hole) continue;  // never: what is over it is held from elsewhere
        if (d.z0 > contact.z + 0.05 || d.z1 < contact.z - below) continue;
        if (q.x < d.box.min.x - r || q.x > d.box.max.x + r || q.y < d.box.min.y - r || q.y > d.box.max.y + r) continue;
        bool near = d.shape.contains(q) || edgeDistance(d.shape.contour, q) < r;
        for (const Path& hole : d.shape.holes) near = near || edgeDistance(hole, q) < r;
        if (near) out.push_back(static_cast<int>(i));
    }
    return out;
}

bool DetailMap::reaches(const Vec3& p, double r, const std::vector<int>* except) const {
    if (by_z_.empty()) return false;
    const double k = std::floor((p.z - z_base_) / kBand);
    if (k < 0 || k >= static_cast<double>(by_z_.size())) return false;
    const Vec2 q = p.xy();
    for (const int i : by_z_[static_cast<std::size_t>(k)]) {
        if (except && std::binary_search(except->begin(), except->end(), i)) continue;
        const DetailRegion& d = regions_[static_cast<std::size_t>(i)];
        if (p.z < d.z0 || p.z > d.z1) continue;
        if (q.x < d.box.min.x - r || q.x > d.box.max.x + r || q.y < d.box.min.y - r || q.y > d.box.max.y + r) continue;
        if (d.shape.contains(q)) return true;
        if (r <= 0) continue;
        if (edgeDistance(d.shape.contour, q) < r) return true;
        for (const Path& hole : d.shape.holes)
            if (edgeDistance(hole, q) < r) return true;
    }
    return false;
}

TriangleMesh DetailMap::mesh(bool with_mouths) const {
    TriangleMesh out;
    for (const DetailRegion& d : regions_) {
        if (d.mouth && !with_mouths) continue;
        std::vector<std::vector<Vec2>> polys;
        auto add = [&](const Path& p) {
            std::vector<Vec2> v;
            for (const auto& pt : p) v.push_back(toVec(pt));
            polys.push_back(std::move(v));
        };
        add(d.shape.contour);
        for (const Path& hole : d.shape.holes) add(hole);
        const ops::Solid s = ops::extrude(polys, std::max(d.z1 - d.z0, 1e-3));
        if (!s.valid()) continue;
        TriangleMesh piece = s.toMesh();
        for (Vec3& v : piece.vertices) v.z += d.z0;
        out.append(piece);
    }
    return out;
}

}  // namespace fruit
