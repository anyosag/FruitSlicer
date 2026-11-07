// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "layers/Hollow.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

#include "core/Parallel.h"
#include "layers/HollowAnalysis.h"

namespace fruit {

namespace {

// Scanned models slice into outlines of many tiny edges, and hollowing needs
// no detail finer than a pixel. Outlines are simplified by this much, and
// arcs drawn with chords this far inside them; each erosion goes that much
// further for every one of those it rests on, so the interior stays inside
// the exact one but across cracks narrower than twice this, too narrow to
// print.
constexpr double kOutline = 0.005;  // mm

// Each offset in a roof adds points along its outline, nearly in line with
// their neighbours. Simplifying by this little after every layer drops them
// while the roof moves a micrometre at most.
constexpr double kRoofOutline = 0.001;  // mm

// Under half a Saturn 16K pixel: an interior piece this small is dropped.
constexpr double kTinyArea = 1e-4;  // mm²
// Two roundings of the same arc disagree by up to the offset's 0.5 µm arc
// tolerance, leaving slivers that thin, however long. Thinner than twice
// this is no shape at all.
constexpr double kSliver = 0.002;  // mm
// Where two roof slopes meet, a ceiling may reach a little beyond d (about
// 1.12 d where a ridge joins a cavity going up); past this it is filled.
constexpr double kHangSlack = 1.25;
// Solid hanging over the cavity narrower than twice this prints as a small
// overhang (the generator lets a ledge reach 1 mm); wider, it is held up by
// solid straight down to the cavity's floor. Below this size the roof's
// seams on a scanned model's uneven ceilings would each grow a pillar.
constexpr double kMinFeature = 0.25;  // mm

bool wide(const Paths& p) { return !p.empty() && !offset(p, -kMinFeature).empty(); }

bool nothing(const Paths& p) { return p.empty() || offset(p, -kSliver).empty(); }

void append(Paths& to, const Paths& from) { to.insert(to.end(), from.begin(), from.end()); }

// Whether a piece of `region` lies against the region's edge (its walls).
bool againstWalls(const ExPolygon& piece, const Paths& region) {
    return !nothing(difference(offset(piece.toPaths(), 3 * kSliver), region));
}

// Cavity ceilings that hold themselves up (see the header). `d`: how far a
// ceiling layer may reach beyond the solid under it.
std::vector<Paths> addRoofs(const std::vector<Paths>& in, double d, unsigned threads, const JobControl& ctl,
                            double f0, double f1) {
    const size_t n = in.size();
    const Paths none;
    // Layers that keep all their interior while the layer above keeps all of its.
    std::vector<char> whole(n, 1);
    parallelFor(n, threads, [&](size_t j) {
        if (in[j].empty()) return;
        ctl.check();
        whole[j] = nothing(difference(offset(in[j], -d), j + 1 < n ? in[j + 1] : none));
    });
    ctl.step("hollow", f0 + 0.2 * (f1 - f0));

    // A cavity piece whose ceiling slopes, and how deep inside its walls the
    // cavity still was (shrinking by d a layer going down).
    struct Slope {
        BBox2 box;
        double depth;
    };
    std::vector<Paths> out(n);
    std::vector<Slope> slopes_above;
    Paths cut_off_above;      // solid in the layer above that no wall holds: carried straight down
    bool above_whole = true;  // out[j + 1] is in[j + 1]
    for (size_t j = n; j-- > 0;) {
        ctl.check();
        if ((n - j) % 32 == 0) ctl.report("hollow", f0 + (f1 - f0) * (0.2 + 0.8 * double(n - j) / double(n)));
        if (in[j].empty()) {
            slopes_above.clear();
            cut_off_above.clear();
            above_whole = true;
            continue;
        }
        if (above_whole && whole[j]) {
            out[j] = in[j];
            slopes_above.clear();
            cut_off_above.clear();
            continue;
        }
        const Paths& up = j + 1 < n ? out[j + 1] : none;
        const ExPolygons up_parts = toExPolygons(up);
        std::vector<BBox2> up_boxes;
        for (const ExPolygon& e : up_parts) up_boxes.push_back(e.bbox());
        const Paths band = offset(up, d);  // within d of the cavity above
        std::vector<Slope> slopes;
        Paths kept, cut_off;
        bool changed = false;
        for (const ExPolygon& x : toExPolygons(in[j])) {
            const Paths xp = x.toPaths();
            const BBox2 box = x.bbox();
            Paths near;  // the part of the cavity above that can lie over x
            for (size_t k = 0; k < up_parts.size(); ++k)
                if (up_boxes[k].overlaps(box)) append(near, up_parts[k].toPaths());
            // Eroded by `depth`, x holds itself up: what lies deeper still,
            // by d, is under the cavity above.
            auto holds = [&](double depth) { return nothing(difference(offset(xp, -(depth + d)), near)); };
            const Paths core = offset(xp, -d);  // more than d inside the walls
            const bool carries = !cut_off_above.empty() && !nothing(intersection(xp, cut_off_above));
            if (!carries && nothing(difference(core, near))) {
                append(kept, xp);
                continue;
            }
            changed = true;
            Paths keep = intersection(xp, band);
            // What would turn solid: a roof can rise only from its pieces
            // against the walls.
            Paths walled;
            for (const ExPolygon& u : toExPolygons(difference(xp, band)))
                if (againstWalls(u, xp)) append(walled, u.toPaths());
            // Within d of the walls the new solid holds itself up: an
            // erosion adds nothing there (over a dome, layer after layer).
            if (!walled.empty() && !nothing(intersection(core, walled))) {
                double hint = 0;
                for (const Slope& s : slopes_above)
                    if (s.box.overlaps(box)) hint = std::max(hint, s.depth);
                double depth = std::max(0.0, hint - d);
                if (!holds(depth)) {
                    // Deeper, doubling the step until it holds, then halving
                    // back. At `cap` nothing is left, which always holds.
                    const double cap = 0.5 * std::min(box.size().x, box.size().y);
                    double lo = depth, step = d, hi = depth + d;
                    while (hi < cap && !holds(hi)) {
                        lo = hi;
                        step *= 2;
                        hi = lo + step;
                    }
                    hi = std::min(hi, cap);
                    while (hi - lo > 0.25 * d) {
                        const double mid = 0.5 * (lo + hi);
                        (holds(mid) ? hi : lo) = mid;
                    }
                    depth = hi;
                }
                append(keep, intersection(offset(xp, -depth), walled));
                if (depth > 0) slopes.push_back({box, depth});
            }
            if (carries) keep = difference(keep, cut_off_above);
            // New solid that no wall holds hangs from the layer above. Left
            // alone it would taper down to a point hanging in the cavity,
            // all of it resting on that point, so it goes straight down; a
            // roof can't rise from it either. A small one is gone within a
            // few layers.
            for (const ExPolygon& s : toExPolygons(difference(xp, keep))) {
                const Paths sp = s.toPaths();
                if (wide(sp) && !againstWalls(s, xp)) append(cut_off, sp);
            }
            append(kept, keep);
        }
        // Still hanging where slopes meet: held up the same way.
        const Paths deep = difference(offset(kept, -kHangSlack * d), up);
        const Paths hanging = deep.empty() ? deep : offset(offset(deep, -kMinFeature), kMinFeature);
        const bool hangs = !hanging.empty();
        if (hangs) {
            kept = difference(kept, hanging);
            append(cut_off, hanging);
        }
        // Each offset rounds corners with more points; simplifying keeps the
        // outline no busier than the interior's, and inside it.
        if (changed || hangs) kept = intersection(simplify(kept, kRoofOutline), in[j]);
        out[j] = std::move(kept);
        above_whole = !changed && !hangs;
        slopes_above = std::move(slopes);
        cut_off_above = std::move(cut_off);
    }
    return out;
}

// Pieces of the interior that overlap from layer to layer form one cavity;
// those under `min_volume` are dropped.
void collectCavities(std::vector<Paths>& in, double layer_height, double min_volume, unsigned threads,
                     std::vector<Cavity>& cavities, std::vector<std::vector<InteriorPiece>>& layer_pieces,
                     const JobControl& ctl) {
    const size_t n = in.size();
    struct Piece {
        Paths shape;
        BBox2 box;
        double area;
    };
    std::vector<std::vector<Piece>> pieces(n);
    parallelFor(n, threads, [&](size_t j) {
        if (in[j].empty()) return;
        ctl.check();
        for (const ExPolygon& e : toExPolygons(in[j])) {
            const double a = e.area();
            if (a >= kTinyArea) pieces[j].push_back({e.toPaths(), e.bbox(), a});
        }
    });
    std::vector<size_t> first_id(n + 1, 0);
    for (size_t j = 0; j < n; ++j) first_id[j + 1] = first_id[j] + pieces[j].size();
    // Overlaps between each layer and the next.
    std::vector<std::vector<std::pair<size_t, size_t>>> links(n);
    parallelFor(n > 0 ? n - 1 : 0, threads, [&](size_t j) {
        for (size_t a = 0; a < pieces[j].size(); ++a)
            for (size_t b = 0; b < pieces[j + 1].size(); ++b) {
                const Piece& p = pieces[j][a];
                const Piece& q = pieces[j + 1][b];
                if (p.box.overlaps(q.box) && !nothing(intersection(p.shape, q.shape)))
                    links[j].emplace_back(first_id[j] + a, first_id[j + 1] + b);
            }
    });
    std::vector<size_t> uf(first_id[n]);
    std::iota(uf.begin(), uf.end(), size_t{0});
    auto find = [&](size_t a) {
        while (uf[a] != a) a = uf[a] = uf[uf[a]];
        return a;
    };
    for (const auto& l : links)
        for (const auto& [a, b] : l) {
            const size_t ra = find(a), rb = find(b);
            if (ra != rb) uf[std::max(ra, rb)] = std::min(ra, rb);  // the lowest piece is the root
        }
    std::vector<double> volume(uf.size(), 0);
    for (size_t j = 0; j < n; ++j)
        for (size_t a = 0; a < pieces[j].size(); ++a) volume[find(first_id[j] + a)] += pieces[j][a].area * layer_height;
    std::vector<int> index(uf.size(), -1);  // cavity of each root
    layer_pieces.assign(n, {});
    for (size_t j = 0; j < n; ++j) {
        in[j].clear();
        for (size_t a = 0; a < pieces[j].size(); ++a) {
            const size_t root = find(first_id[j] + a);
            if (volume[root] < min_volume) continue;
            if (index[root] < 0) {
                index[root] = static_cast<int>(cavities.size());
                cavities.push_back({static_cast<int>(j), static_cast<int>(j), volume[root]});
            }
            cavities[index[root]].last_layer = static_cast<int>(j);
            layer_pieces[j].push_back({static_cast<uint32_t>(in[j].size()),
                                       static_cast<uint32_t>(pieces[j][a].shape.size()), index[root]});
            append(in[j], pieces[j][a].shape);
        }
    }
}

}  // namespace

std::vector<Paths> windowIntersection(const std::vector<Paths>& in, int m, unsigned threads, const JobControl& ctl) {
    const size_t n = in.size();
    if (m <= 0) return in;
    std::vector<Paths> out(n);
    const size_t w = 2 * static_cast<size_t>(m) + 1;
    if (w > n) return out;
    // Windows [a, a + w − 1] for a = 0 .. n − w, grouped into blocks of w
    // starts. A window starting at s (a block's first) is that block; any
    // other is a suffix of its block's slices met with a prefix of the next.
    const size_t starts = n - w + 1;
    parallelFor((starts + w - 1) / w, threads, [&](size_t block) {
        ctl.check();
        const size_t s = block * w;
        const size_t last = std::min(s + w - 1, starts - 1);  // last start in this block
        std::vector<Paths> suf(w);  // suf[a − s] = in[a] ∩ … ∩ in[s + w − 1]
        suf[w - 1] = in[s + w - 1];
        for (size_t a = s + w - 1; a-- > s;)
            if (!suf[a + 1 - s].empty()) suf[a - s] = intersection(in[a], suf[a + 1 - s]);
        std::vector<Paths> pre(last - s);  // pre[z − s − w] = in[s + w] ∩ … ∩ in[z]
        for (size_t z = s + w; z < last + w; ++z) {
            const size_t i = z - s - w;
            pre[i] = i == 0 ? in[z] : pre[i - 1].empty() ? Paths{} : intersection(pre[i - 1], in[z]);
        }
        out[s + m] = suf[0];
        for (size_t a = s + 1; a <= last; ++a) out[a + m] = intersection(suf[a - s], pre[a - s - 1]);
    });
    return out;
}

HollowResult hollowInterior(const LayerStack& solid, const HollowOptions& o, const JobControl& ctl) {
    if (!(o.wall > 0)) throw std::invalid_argument("hollowing needs a wall thicker than 0");
    if (!(o.roof_lean > 0)) throw std::invalid_argument("hollowing needs ceilings that may lean above 0°");
    const size_t n = solid.size();
    const double h = solid.layer_height;
    const int cylinders = std::clamp(o.cylinders, 1, 64);
    ctl.report("hollow", 0);

    std::vector<Paths> slices(n);
    parallelFor(n, o.threads, [&](size_t i) { slices[i] = simplify(solid.slices[i], kOutline); });
    ctl.check();

    // The solid eroded by each cylinder in turn.
    std::vector<Paths> in(n);
    std::vector<char> live(n, 1);  // still has interior after the cylinders so far
    std::vector<Paths> window;     // intersections of the slices within ±w layers
    int w = 0;
    for (int k = 1; k <= cylinders; ++k) {
        const double radius = o.wall * std::sqrt(1.0 - static_cast<double>(k - 1) / cylinders);
        const double half = o.wall * std::sqrt(static_cast<double>(k) / cylinders);
        const int m = static_cast<int>(std::ceil(half / h - 1e-9));
        if (w > 0 && m > w && m <= 2 * w) {
            // Taller windows from two of the last: ±w around i ∓ (m − w)
            // cover i − m .. i + m when m ≤ 2w. One operation a layer.
            const size_t s = static_cast<size_t>(m - w), mm = static_cast<size_t>(m);
            std::vector<Paths> taller(n);
            parallelFor(n, o.threads, [&](size_t i) {
                if (i < mm || i + mm >= n) return;
                ctl.check();
                taller[i] = intersection(window[i - s], window[i + s]);
            });
            window = std::move(taller);
        } else if (m != w) {
            window = windowIntersection(slices, m, o.threads, ctl);
        }
        w = m;
        parallelFor(n, o.threads, [&](size_t i) {
            if (!live[i]) return;
            ctl.check();
            // Arcs drawn kOutline inside: that much further again.
            Paths eroded = simplify(offset(window[i], -(radius + 3 * kOutline), kOutline), kOutline);
            in[i] = k == 1 ? std::move(eroded) : intersection(in[i], eroded);
            live[i] = !in[i].empty();
        });
        ctl.step("hollow", 0.6 * k / cylinders);
    }

    if (o.opening > 0)
        parallelFor(n, o.threads, [&](size_t i) {
            if (in[i].empty()) return;
            ctl.check();
            const double r = 0.5 * o.opening;
            in[i] = simplify(offset(offset(in[i], -(r + 2 * kOutline), kOutline), r, kOutline), kOutline);
        });
    ctl.step("hollow", 0.65);

    if (o.roof_lean < 90) in = addRoofs(in, h * std::tan(deg2rad(o.roof_lean)), o.threads, ctl, 0.65, 0.9);
    ctl.step("hollow", 0.9);

    HollowResult r;
    collectCavities(in, h, o.min_volume, o.threads, r.cavities, r.pieces, ctl);
    for (const Cavity& c : r.cavities) r.volume += c.volume;
    r.interior = std::move(in);
    ctl.report("hollow", 1);
    return r;
}

Paths HollowResult::piece(size_t layer, const InteriorPiece& k) const {
    const Paths& all = interior[layer];
    return Paths(all.begin() + k.begin, all.begin() + k.begin + k.count);
}

int HollowResult::cavityAt(size_t layer, const Vec2& p) const {
    if (layer >= pieces.size()) return -1;
    const Point64 q = toPoint(p);
    for (const InteriorPiece& k : pieces[layer]) {
        const Paths& all = interior[layer];
        if (Clipper2Lib::PointInPolygon(q, all[k.begin]) == Clipper2Lib::PointInPolygonResult::IsOutside) continue;
        bool in_hole = false;
        for (uint32_t i = 1; i < k.count && !in_hole; ++i)
            in_hole = Clipper2Lib::PointInPolygon(q, all[k.begin + i]) == Clipper2Lib::PointInPolygonResult::IsInside;
        if (!in_hole) return k.cavity;
    }
    return -1;
}

HollowResult closedHollows(const LayerStack& layers, double min_volume) {
    const HollowReport report = analyzeHollows(layers);
    HollowResult r;
    r.interior.resize(layers.size());
    parallelFor(layers.size(), 0, [&](size_t j) {
        Paths p = report.trap_regions[j];
        append(p, report.cup_regions[j]);
        if (!p.empty()) r.interior[j] = unionNonZero(p);
    });
    collectCavities(r.interior, layers.layer_height, min_volume, 0, r.cavities, r.pieces, JobControl::none());
    for (const Cavity& c : r.cavities) r.volume += c.volume;
    for (size_t j = 0; j < r.pieces.size(); ++j) {
        if (report.cup_regions[j].empty()) continue;
        for (const InteriorPiece& k : r.pieces[j])
            if (!r.cavities[k.cavity].opens_up && !nothing(intersection(r.piece(j, k), report.cup_regions[j])))
                r.cavities[k.cavity].opens_up = true;
    }
    return r;
}

LayerStack filledLayers(const LayerStack& layers, const HollowResult& h) {
    if (h.interior.size() != layers.size())
        throw std::invalid_argument("the hollows were found for a different set of slices");
    LayerStack out = layers;
    parallelFor(layers.size(), 0, [&](size_t i) {
        if (!h.interior[i].empty()) out.slices[i] = unionOf(layers.slices[i], h.interior[i]);
    });
    return out;
}

LayerStack hollowedLayers(const LayerStack& solid, const HollowResult& h) {
    if (h.interior.size() != solid.size())
        throw std::invalid_argument("the interior was found for a different set of slices");
    LayerStack out = solid;
    parallelFor(solid.size(), 0, [&](size_t i) {
        if (!h.interior[i].empty()) out.slices[i] = difference(solid.slices[i], h.interior[i]);
    });
    return out;
}

}  // namespace fruit
