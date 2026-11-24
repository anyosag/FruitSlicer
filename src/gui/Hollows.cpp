// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Hollows.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>

#include "Document.h"
#include "Format.h"
#include "GooTools.h"
#include "Orient.h"
#include "Placing.h"
#include "core/Parallel.h"
#include "ops/MeshOps.h"

namespace gui {

namespace {

double volumeOf(const fruit::LayerStack& ls) {
    double v = 0;
    for (const fruit::Paths& s : ls.slices) v += std::max(0.0, fruit::area(s)) * ls.layer_height;
    return v;
}

}  // namespace

std::size_t HollowPlan::undrained() const {
    return static_cast<std::size_t>(std::count(drains.begin(), drains.end(), 0));
}

std::size_t HollowPlan::toFix() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < closed.cavities.size(); ++i)
        n += !isAccepted(i) && grade(i) == fruit::CupGrade::Problem;
    return n;
}

std::size_t HollowPlan::missed() const {
    return static_cast<std::size_t>(
        std::count_if(placed.begin(), placed.end(), [](const fruit::PlacedHole& h) { return h.cavity < 0; }));
}

std::vector<fruit::PlacedHole> placeHoles(const std::vector<fruit::DrainHole>& holes, const PartFrames& frames,
                                          const fruit::LayerStack& solid, const fruit::HollowResult& hollow) {
    // Each part's holes with its own transform, back in the holes' order.
    std::vector<fruit::PlacedHole> out(holes.size());
    std::vector<int> parts;
    for (const fruit::DrainHole& h : holes)
        if (std::find(parts.begin(), parts.end(), h.part) == parts.end()) parts.push_back(h.part);
    for (const int part : parts) {
        const fruit::Transform* t = frames.find(part);
        std::vector<fruit::DrainHole> mine;
        std::vector<std::size_t> at;
        for (std::size_t i = 0; i < holes.size(); ++i)
            if (holes[i].part == part) {
                mine.push_back(holes[i]);
                at.push_back(i);
            }
        if (!t) continue;  // not on the plate: not drilled
        const std::vector<fruit::PlacedHole> placed = fruit::placeHoles(mine, *t, solid, hollow);
        for (std::size_t k = 0; k < placed.size(); ++k) out[at[k]] = placed[k];
    }
    return out;
}

namespace {

bool sameSize(double was, double now) { return std::abs(now - was) <= was / 3 + 1e-9; }

}  // namespace

bool acceptedIs(const AcceptedHollow& a, const fruit::HollowResult& closed, std::size_t i, const PartFrames& frames,
                double layer_height) {
    if (i >= closed.cavities.size() || layer_height <= 0) return false;
    const fruit::Transform* t = frames.find(a.part);
    if (!t) return false;
    const fruit::Cavity& c = closed.cavities[i];
    if (c.opens_up != a.cup || !sameSize(a.volume, c.volume)) return false;
    const fruit::Vec3 p = t->apply(a.at);
    if (p.z < 0) return false;
    const auto layer = static_cast<std::size_t>(p.z / layer_height);
    return closed.cavityAt(layer, {p.x, p.y}) == static_cast<int>(i);
}

bool acceptedIs(const AcceptedHollow& a, const fruit::Warning& w, const PartFrames& frames) {
    const bool cup = w.code.rfind("suction_cup", 0) == 0;
    if (!cup && w.code != "resin_trap") return false;
    if (cup != a.cup) return false;
    auto v = [&](const char* k) {
        const auto it = w.values.find(k);
        return it == w.values.end() ? std::nan("") : it->second;
    };
    const double x0 = v("x0"), y0 = v("y0"), x1 = v("x1"), y1 = v("y1"), z0 = v("z0"), z1 = v("z1");
    if (std::isnan(x0 + y0 + x1 + y1 + z0 + z1 + v("ix") + v("iy") + v("wz") + v("volume"))) return false;  // an older job
    if (!sameSize(a.volume, v("volume"))) return false;
    const fruit::Transform* t = frames.find(a.part);
    if (!t) return false;
    // The part may have moved since: the warning's point moved with it.
    const fruit::Vec3 shift = w.where - fruit::Vec3{v("ix"), v("iy"), v("wz")};
    const fruit::Vec3 p = t->apply(a.at) - shift;
    const double m = 0.01;
    return p.x >= x0 - m && p.x <= x1 + m && p.y >= y0 - m && p.y <= y1 + m && p.z >= z0 - m && p.z <= z1 + m;
}

bool acceptedIs(const AcceptedHollow& a, const fruit::HollowComponent& c, double layer_height, const PartFrames& frames) {
    if ((c.kind == fruit::HollowKind::SuctionCup) != a.cup || !sameSize(a.volume, c.volume)) return false;
    const fruit::Transform* t = frames.find(a.part);
    if (!t) return false;
    const fruit::Vec3 p = t->apply(a.at);
    const double m = 0.01;
    return p.x >= c.box.min.x - m && p.x <= c.box.max.x + m && p.y >= c.box.min.y - m && p.y <= c.box.max.y + m &&
           p.z >= c.first_layer * layer_height - m && p.z <= (c.last_layer + 1) * layer_height + m;
}

AcceptedHollow acceptedFor(const Document& doc, const HollowPlan& p, std::size_t i) {
    const fruit::Cavity& c = p.closed.cavities.at(i);
    fruit::Vec3 at = c.inside_layer >= 0
                         ? fruit::Vec3{c.inside.x, c.inside.y, (c.inside_layer + 0.5) * p.layer_height}
                         : cavityLabelAt(p.closed, i, p.layer_height);
    AcceptedHollow a;
    a.part = doc.partNear(at);
    const PartFrames frames = doc.frames();
    if (const fruit::Transform* t = frames.find(a.part)) at = t->inverse().apply(at);
    a.at = at;
    a.volume = c.volume;
    a.cup = c.opens_up;
    return a;
}

fruit::CupLimits cupLimits(const Document& doc) { return cupLimitsOf(doc.supports.choices); }

namespace {

// A convex polygon: a triangle, or what clipping it by two planes leaves.
struct Piece {
    static constexpr int kMax = 8;
    fruit::Vec3 v[kMax];
    int n = 0;
};

double along(const fruit::Vec3& p, int axis) { return axis == 0 ? p.x : axis == 1 ? p.y : p.z; }

// What of `in` lies at or above `at` along `axis` (keep_above), or at or below it.
Piece clipAxis(const Piece& in, int axis, double at, bool keep_above) {
    Piece out;
    for (int i = 0; i < in.n; ++i) {
        const fruit::Vec3& a = in.v[i];
        const fruit::Vec3& b = in.v[(i + 1) % in.n];
        const double va = along(a, axis), vb = along(b, axis);
        const bool ia = keep_above ? va >= at : va <= at, ib = keep_above ? vb >= at : vb <= at;
        if (ia && out.n < Piece::kMax) out.v[out.n++] = a;
        if (ia != ib && out.n < Piece::kMax) {
            const double t = (at - va) / (vb - va);
            fruit::Vec3 q{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
            if (axis == 0) q.x = at;
            else if (axis == 1) q.y = at;
            else q.z = at;
            out.v[out.n++] = q;
        }
    }
    return out;
}

Piece clipZ(const Piece& in, double at, bool keep_above) { return clipAxis(in, 2, at, keep_above); }

// The model's faces round each cavity, cut at its layers: a face (or the
// part of it in a layer) whose outside, just off it, is in the cavity.
std::vector<fruit::TriangleMesh> paintCavities(const fruit::TriangleMesh& mesh, const fruit::HollowResult& h,
                                               double layer_height) {
    std::vector<fruit::TriangleMesh> out(h.cavities.size());
    const int layers = static_cast<int>(h.pieces.size());
    if (h.cavities.empty() || layers == 0 || !(layer_height > 0)) return out;
    // Each cavity's room, a little wider, to pass over the faces far from all of them.
    std::vector<fruit::BBox2> boxes(h.cavities.size());
    for (int L = 0; L < layers; ++L)
        for (const fruit::InteriorPiece& k : h.pieces[static_cast<std::size_t>(L)])
            boxes[static_cast<std::size_t>(k.cavity)].extend(fruit::bbox(h.piece(static_cast<std::size_t>(L), k)));
    for (fruit::BBox2& b : boxes) {
        b.extend({b.min.x - 0.5, b.min.y - 0.5});
        b.extend({b.max.x + 0.5, b.max.y + 0.5});
    }
    auto layerOf = [&](double z) { return std::clamp(static_cast<int>(std::floor(z / layer_height)), 0, layers - 1); };
    auto centre = [](const Piece& p) {
        fruit::Vec3 c{};
        for (int i = 0; i < p.n; ++i) c = c + p.v[i];
        return c / static_cast<double>(p.n);
    };
    // The cavity just outside the face at `c` in layer L, or -1: a little
    // way off it first, then further (a sloping face's slice may sit a
    // little away from it, up to half a layer's height along the slope).
    auto cavityOff = [&](int L, const fruit::Vec3& c, const fruit::Vec3& n) {
        if (h.pieces[static_cast<std::size_t>(L)].empty()) return -1;
        for (const double e : {0.02, 0.08, 0.2}) {
            const int k = h.cavityAt(static_cast<std::size_t>(L), {c.x + n.x * e, c.y + n.y * e});
            if (k >= 0) return k;
        }
        return -1;
    };
    // A piece wider than this is looked at in squares of about this size:
    // a big face may be round a cup in one place and not in another (or run
    // inside another shell, in a file of several overlapping ones).
    constexpr double kCell = 1.0;
    auto wide = [](const Piece& p) {
        fruit::BBox2 b;
        for (int i = 0; i < p.n; ++i) b.extend({p.v[i].x, p.v[i].y});
        return b.size().x > 1.5 * kCell || b.size().y > 1.5 * kCell;
    };
    constexpr std::size_t kBlock = 4096;
    const std::size_t blocks = (mesh.faces.size() + kBlock - 1) / kBlock;
    std::vector<std::vector<fruit::TriangleMesh>> parts(blocks, std::vector<fruit::TriangleMesh>(h.cavities.size()));
    fruit::parallelFor(blocks, 0, [&](std::size_t blk) {
        auto emit = [&](int cav, const Piece& p) {
            if (cav < 0 || p.n < 3) return;
            fruit::TriangleMesh& m = parts[blk][static_cast<std::size_t>(cav)];
            const auto base = static_cast<std::int32_t>(m.vertices.size());
            for (int i = 0; i < p.n; ++i) m.vertices.push_back(p.v[i]);
            for (int i = 1; i + 1 < p.n; ++i) m.faces.push_back({base, base + i, base + i + 1});
        };
        // A wide piece in layer L (flat: -1, the layer on its outside), square by square.
        auto emitSquares = [&](int L, const Piece& piece, const fruit::Vec3& n) {
            fruit::BBox2 box;
            for (int i = 0; i < piece.n; ++i) box.extend({piece.v[i].x, piece.v[i].y});
            const int nx = std::clamp(static_cast<int>(std::ceil(box.size().x / kCell)), 1, 200);
            const int ny = std::clamp(static_cast<int>(std::ceil(box.size().y / kCell)), 1, 200);
            for (int i = 0; i < nx; ++i) {
                const Piece col = clipAxis(clipAxis(piece, 0, box.min.x + i * box.size().x / nx, true), 0,
                                           box.min.x + (i + 1) * box.size().x / nx, false);
                if (col.n < 3) continue;
                for (int j = 0; j < ny; ++j) {
                    const Piece sq = clipAxis(clipAxis(col, 1, box.min.y + j * box.size().y / ny, true), 1,
                                              box.min.y + (j + 1) * box.size().y / ny, false);
                    if (sq.n < 3) continue;
                    const fruit::Vec3 c = centre(sq);
                    emit(cavityOff(L >= 0 ? L : layerOf(c.z + n.z * 0.02), c, n), sq);
                }
            }
        };
        const std::size_t end = std::min(mesh.faces.size(), (blk + 1) * kBlock);
        for (std::size_t f = blk * kBlock; f < end; ++f) {
            Piece tri;
            tri.n = 3;
            fruit::BBox2 fb;
            for (int i = 0; i < 3; ++i) {
                tri.v[i] = mesh.vertices[static_cast<std::size_t>(mesh.faces[f][static_cast<std::size_t>(i)])];
                fb.extend({tri.v[i].x, tri.v[i].y});
            }
            bool near = false;
            for (const fruit::BBox2& b : boxes) near = near || fb.overlaps(b);
            if (!near) continue;
            const fruit::Vec3 n = mesh.faceNormal(f);
            if (n.x == 0 && n.y == 0 && n.z == 0) continue;
            const double z0 = std::min({tri.v[0].z, tri.v[1].z, tri.v[2].z});
            const double z1 = std::max({tri.v[0].z, tri.v[1].z, tri.v[2].z});
            const int l0 = layerOf(z0), l1 = layerOf(z1);
            if (l0 == l1) {
                // Flat, or within a layer: the layer on its outside.
                if (wide(tri)) {
                    emitSquares(-1, tri, n);
                    continue;
                }
                const fruit::Vec3 c = centre(tri);
                emit(cavityOff(layerOf(c.z + n.z * 0.02), c, n), tri);
                continue;
            }
            auto within = [&](int a, int b) { return clipZ(clipZ(tri, a * layer_height, true), (b + 1) * layer_height, false); };
            int run_from = l0, run_cav = -2;
            for (int L = l0; L <= l1; ++L) {
                const Piece piece = within(L, L);
                if (piece.n >= 3 && wide(piece)) {
                    if (run_cav >= 0) emit(run_cav, within(run_from, L - 1));
                    emitSquares(L, piece, n);
                    run_from = L + 1;
                    run_cav = -2;
                    continue;
                }
                const int cav = piece.n >= 3 ? cavityOff(L, centre(piece), n) : -1;
                if (cav != run_cav) {
                    if (run_cav >= 0) emit(run_cav, within(run_from, L - 1));
                    run_from = L;
                    run_cav = cav;
                }
            }
            if (run_cav >= 0) emit(run_cav, within(run_from, l1));
        }
    });
    for (auto& part : parts)
        for (std::size_t c = 0; c < part.size(); ++c) out[c].append(part[c]);
    return out;
}

// Where the film seals each suction cup: on each of its layers, what of it
// the next layer leaves open, as a thin sheet over it.
std::vector<fruit::TriangleMesh> cupOpenings(const fruit::LayerStack& solid, const fruit::HollowResult& h) {
    std::vector<fruit::TriangleMesh> out(h.cavities.size());
    const std::size_t n = std::min(solid.size(), h.pieces.size());
    std::vector<fruit::Paths> open(h.cavities.size() * n);
    fruit::parallelFor(n, 0, [&](std::size_t L) {
        for (const fruit::InteriorPiece& k : h.pieces[L]) {
            if (!h.cavities[static_cast<std::size_t>(k.cavity)].opens_up) continue;
            fruit::Paths over;
            if (L + 1 < n) {
                over = solid.slices[L + 1];
                over.insert(over.end(), h.interior[L + 1].begin(), h.interior[L + 1].end());
            }
            fruit::Paths o = over.empty() ? h.piece(L, k) : fruit::difference(h.piece(L, k), over);
            if (std::abs(fruit::area(o)) < 0.05) continue;
            fruit::Paths& to = open[static_cast<std::size_t>(k.cavity) * n + L];
            to.insert(to.end(), o.begin(), o.end());
        }
    });
    for (std::size_t c = 0; c < h.cavities.size(); ++c)
        for (std::size_t L = 0; L < n; ++L) {
            const fruit::Paths& o = open[c * n + L];
            if (o.empty()) continue;
            for (const fruit::ExPolygon& ex : fruit::toExPolygons(o)) {
                std::vector<std::vector<fruit::Vec2>> polys;
                auto add = [&](const fruit::Path& p) {
                    std::vector<fruit::Vec2> v;
                    for (const fruit::Point64& q : p) v.push_back(fruit::toVec(q));
                    polys.push_back(std::move(v));
                };
                add(ex.contour);
                for (const fruit::Path& hole : ex.holes) add(hole);
                const fruit::ops::Solid sheet = fruit::ops::extrude(polys, 0.02);
                if (!sheet.valid() || sheet.empty()) continue;
                fruit::TriangleMesh m = sheet.toMesh();
                const double z = solid.layerTop(L);
                for (fruit::Vec3& v : m.vertices) v.z += z;
                out[c].append(m);
            }
        }
    return out;
}

}  // namespace

std::size_t HollowPlan::vents() const {
    return static_cast<std::size_t>(std::count_if(holes.begin(), holes.end(), [](const fruit::DrainHole& h) { return h.vent; }));
}

std::size_t HollowPlan::trapHoles() const { return holes.size() - vents(); }

HollowPlan planHollow(const Document& doc, const HollowChoices& c, const std::vector<fruit::DrainHole>& manual,
                      double layer_height, const fruit::JobControl& ctl) {
    const auto t0 = std::chrono::steady_clock::now();
    HollowPlan p;
    p.choices = c;
    p.manual = manual;
    p.vents_with = doc.hollow.vents;
    p.layer_height = layer_height;
    auto solid = std::make_shared<const fruit::LayerStack>(
        fruit::sliceModel(doc.placed(), layer_height, nullptr, 0, ctl.sub(0.0, 0.15)));
    p.solid = solid;
    p.closed = fruit::closedHollows(*solid);
    ctl.check();
    // How each grades, and those accepted: holes and filling leave them.
    const PartFrames frames = doc.frames();
    const fruit::CupLimits limits = cupLimits(doc);
    p.accepted_with = doc.hollow.accepted;
    const std::size_t own = p.closed.cavities.size();
    p.accepted.assign(own, 0);
    p.grades.assign(own, fruit::CupGrade::Problem);
    std::vector<char> keep(own, 1), traps(own, 0), traps_all(own, 0), cups(own, 0);
    for (std::size_t i = 0; i < own; ++i) {
        const fruit::Cavity& k = p.closed.cavities[i];
        if (k.opens_up) p.grades[i] = fruit::gradeCup(k.cup, k.last_layer - k.first_layer + 1, limits);
        for (const AcceptedHollow& a : p.accepted_with)
            if (acceptedIs(a, p.closed, i, frames, layer_height)) p.accepted[i] = 1;
        keep[i] = !p.accepted[i];
        traps_all[i] = !k.opens_up;
        traps[i] = keep[i] && !k.opens_up;
        cups[i] = keep[i] && k.opens_up;
    }
    p.fixing = std::count(keep.begin(), keep.end(), 0) ? fruit::keepCavities(p.closed, keep) : p.closed;
    const fruit::HollowResult trap_set = fruit::keepCavities(p.closed, traps);
    const fruit::HollowResult cup_set = fruit::keepCavities(p.closed, cups);
    // Their cavities in closed's numbering.
    auto numbering = [&](const std::vector<char>& flags) {
        std::vector<int> of;
        for (std::size_t i = 0; i < own; ++i)
            if (flags[i]) of.push_back(static_cast<int>(i));
        return of;
    };
    const std::vector<int> trap_of = numbering(traps), cup_of = numbering(cups), fixing_of = numbering(keep);
    // What holes are drilled into: the model hollowed (its own resin traps
    // filled first, to go with the rest; its suction cups stay, they are
    // open to the outside), or the model as it is.
    std::shared_ptr<const fruit::LayerStack> base = solid;
    if (c.hollow) {
        base = std::make_shared<const fruit::LayerStack>(fruit::filledLayers(*solid, fruit::keepCavities(p.closed, traps_all)));
        fruit::HollowOptions o;
        o.wall = c.wall;
        o.roof_lean = c.ceiling_lean;
        p.hollowed = fruit::hollowInterior(*base, o, ctl.sub(0.15, 0.8));
    }
    ctl.step("holes", 0.8);
    // The traps' holes (or the hollowed cavities'): placed automatically
    // into those, or by hand into any hollow.
    const fruit::HollowResult& traps_to = c.hollow ? p.hollowed : trap_set;
    const fruit::HollowResult& by_hand_to = c.hollow ? p.hollowed : p.fixing;
    std::vector<fruit::DrainHole> holes;
    auto suggest = [&](const fruit::HollowResult& into) {
        std::vector<fruit::DrainHole> out;
        if (doc.partCount() == 1) {
            out = fruit::suggestDrainHoles(doc.placed(), doc.part().transform(), *base, into, c.hole_diameter);
            for (fruit::DrainHole& h : out) h.part = doc.part().id();
        } else {
            // Found on the parts together, then kept on the part each goes into.
            out = fruit::suggestDrainHoles(doc.placed(), fruit::Transform{}, *base, into, c.hole_diameter);
            for (fruit::DrainHole& h : out) {
                h.part = doc.partNear(h.at);
                const fruit::Transform to_part = frames.find(h.part)->inverse();
                h.at = to_part.apply(h.at);
                h.axis = fruit::normalized(to_part.applyLinear(h.axis));
            }
        }
        return out;
    };
    if (p.drilling()) {
        if (!c.auto_holes) {
            for (const fruit::DrainHole& h : manual)
                if (!h.vent && frames.find(h.part)) holes.push_back(h);
        } else if (!traps_to.cavities.empty()) {
            holes = suggest(traps_to);
        }
    }
    const fruit::HollowResult& holes_to = c.auto_holes ? traps_to : by_hand_to;
    std::vector<fruit::PlacedHole> placed = holes.empty() ? std::vector<fruit::PlacedHole>{}
                                                          : placeHoles(holes, frames, *base, holes_to);
    // The cups' vents, as asked for.
    std::vector<fruit::DrainHole> vents;
    for (fruit::DrainHole h : doc.hollow.vents)
        if (frames.find(h.part)) {
            h.vent = true;
            h.diameter = c.hole_diameter;
            vents.push_back(h);
        }
    const std::vector<fruit::PlacedHole> vents_placed =
        vents.empty() ? std::vector<fruit::PlacedHole>{} : placeHoles(vents, frames, *base, cup_set);
    // What prints.
    fruit::LayerStack print;
    if (c.hollow) print = fruit::drilledLayers(*base, p.hollowed, placed);
    else if (p.filling() && !trap_set.cavities.empty()) print = fruit::filledLayers(*solid, trap_set);
    else if (!placed.empty()) print = fruit::drilledLayers(*solid, holes_to, placed);
    if (!vents_placed.empty()) print = fruit::drilledLayers(print.size() ? print : *solid, cup_set, vents_placed);
    p.layers = print.size() ? std::make_shared<const fruit::LayerStack>(std::move(print)) : solid;
    p.holes = holes;
    p.holes.insert(p.holes.end(), vents.begin(), vents.end());
    p.placed = placed;
    p.placed.insert(p.placed.end(), vents_placed.begin(), vents_placed.end());
    auto own_of = [](const std::vector<int>& of, int cav) {
        return cav >= 0 && static_cast<std::size_t>(cav) < of.size() ? of[static_cast<std::size_t>(cav)] : -1;
    };
    for (const fruit::PlacedHole& h : placed)
        p.into_own.push_back(c.hollow ? -1 : own_of(c.auto_holes ? trap_of : fixing_of, h.cavity));
    for (const fruit::PlacedHole& h : vents_placed) p.into_own.push_back(own_of(cup_of, h.cavity));
    ctl.check();
    p.drains = fruit::cavitiesDrain(*p.layers, p.cavities());
    p.own_drains = c.hollow ? fruit::cavitiesDrain(*p.layers, p.closed) : p.drains;
    p.filled.assign(own, 0);
    for (std::size_t i = 0; i < own; ++i) {
        p.filled[i] = (c.hollow && traps_all[i]) || (p.filling() && traps[i]);
        if (p.filled[i]) p.own_drains[i] = 0;  // printed solid: nothing to drain
    }
    // A vent that would fit each cup still to fix, as it would be drilled.
    p.vent_for.assign(own, std::nullopt);
    p.vent_preview.assign(own, std::nullopt);
    {
        std::vector<char> want(cup_set.cavities.size(), 0);
        std::vector<std::size_t> of(cup_of.begin(), cup_of.end());
        for (std::size_t j = 0; j < of.size(); ++j) want[j] = !p.own_drains[of[j]];
        if (std::count(want.begin(), want.end(), 1)) {
            const auto found = fruit::suggestVents(doc.placed(), fruit::Transform{}, *base, cup_set, want, c.hole_diameter);
            for (std::size_t j = 0; j < of.size() && j < found.size(); ++j) {
                if (!found[j]) continue;
                fruit::DrainHole h = *found[j];  // printer coordinates
                h.part = doc.partNear(h.at);
                const fruit::Transform* t = frames.find(h.part);
                if (!t) continue;
                const fruit::Transform to_part = t->inverse();
                h.at = to_part.apply(h.at);
                h.axis = fruit::normalized(to_part.applyLinear(h.axis));
                const std::vector<fruit::PlacedHole> pl = placeHoles({h}, frames, *base, cup_set);
                if (pl.empty() || pl[0].cavity < 0) continue;
                p.vent_for[of[j]] = h;
                p.vent_preview[of[j]] = pl[0];
            }
        }
    }
    // Resin used: what prints, and what stays sealed inside a hollow that
    // doesn't drain. Against the model as it is with its resin traps full
    // (their resin is used either way, cured or trapped). A suction cup is
    // open: as the print rises out of the vat it hangs upside down and the
    // cup empties, so its resin is never counted.
    p.print_volume = volumeOf(*p.layers);
    double traps_volume = 0, held = 0;
    for (std::size_t i = 0; i < own; ++i) {
        if (!traps_all[i]) continue;
        traps_volume += p.closed.cavities[i].volume;
        if (!c.hollow && !p.filled[i] && !(i < p.drains.size() && p.drains[i])) held += p.closed.cavities[i].volume;
    }
    if (c.hollow)
        for (std::size_t i = 0; i < p.hollowed.cavities.size(); ++i)
            if (i >= p.drains.size() || !p.drains[i]) held += p.hollowed.cavities[i].volume;
    p.resin_saved = volumeOf(*solid) + traps_volume - p.print_volume - held;
    ctl.check();
    p.paint = paintCavities(doc.placed(), p.closed, layer_height);
    p.opening = cupOpenings(*solid, p.closed);
    p.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ctl.report("holes", 1.0);
    return p;
}

std::string cavityKind(const fruit::Cavity& c, bool hollowed) {
    if (hollowed) return "Hollowed";
    return c.opens_up ? "Suction cup" : "Resin trap";
}

std::string cavityFacts(const fruit::Cavity& c, double layer_height) {
    return volumeText(c.volume) + strf(" · from %.1f to %.1f mm", c.first_layer * layer_height,
                                       (c.last_layer + 1) * layer_height);
}

std::string cavityHeights(const fruit::Cavity& c, double layer_height) {
    return strf("%.1f to %.1f mm", c.first_layer * layer_height, (c.last_layer + 1) * layer_height);
}

std::string cavityMeaning(const fruit::Cavity& c, bool hollowed) {
    if (hollowed)
        return "Taken out to save resin, with a ceiling steep enough to hold itself up. Resin stays inside until "
               "a hole lets it out.";
    if (c.opens_up)
        return "Open towards the film and closed below, so while its layers print each peel pulls on the resin "
               "sealed in it as well as on the layer that just cured.";
    return "Sealed inside: uncured resin stays in it, and pressure builds as the top closes. The inside ceiling "
           "can't have tips: they could never be removed.";
}

namespace {

std::string mm2(double a) { return thousands(static_cast<std::size_t>(std::lround(a))) + " mm²"; }

std::string pct(double share) {
    const double p = 100 * share;
    return p < 9.95 && p >= 0.05 ? strf("%.1f %%", p) : strf("%.0f %%", p);
}

std::string layersText(int n) { return n == 1 ? std::string("1 layer") : strf("%d layers", n); }

}  // namespace

std::string cavityGradeText(const HollowPlan& p, std::size_t i, bool applied) {
    const fruit::Cavity& c = p.closed.cavities.at(i);
    const std::string t = cavityKind(c, false);
    if (p.isAccepted(i)) return t + " · accepted";
    if (applied && i < p.filled.size() && p.filled[i]) return t + " · printed solid";
    if (applied && i < p.own_drains.size() && p.own_drains[i]) return t + (c.opens_up ? " · vented" : " · drains");
    return c.opens_up ? t + " · " + fruit::gradeName(p.grade(i)) : t;
}

std::string cavityPull(const fruit::Cavity& c, double layer_height, const fruit::CupLimits& l) {
    if (!c.opens_up || c.cup.share_layer < 0) return "";
    const fruit::CupMeasures& m = c.cup;
    const double at = (m.share_layer + 1) * layer_height;
    std::string s;
    if (m.cups_there > 1) {
        const int others = m.cups_there - 1;
        s = strf("It pulls hardest at %.1f mm, where it and %s open beside each other: together %s of what is "
                 "cured round them (%s open against %s cured).",
                 at, others == 1 ? "1 other cup no bigger are" : strf("%d other cups no bigger are", others).c_str(),
                 pct(m.share).c_str(), mm2(m.open_area).c_str(), mm2(m.cured_around).c_str());
    } else {
        s = strf("It pulls hardest at %.1f mm: what is open there is %s of what is cured round it (%s open against "
                 "%s cured), so that layer pulls about %s harder as it peels.",
                 at, pct(m.share).c_str(), mm2(m.open_area).c_str(), mm2(m.cured_around).c_str(), pct(m.share).c_str());
    }
    if (m.all_cups > m.cups_there)
        s += strf(" A bigger cup is open on that layer too, and with it the layer pulls %s: that cup is graded on "
                  "all of it, and this one as if that were fixed.",
                  pct(m.all_open / std::max(m.cured_around, 1e-9)).c_str());
    const int layers = c.last_layer - c.first_layer + 1;
    const int warn = m.by_layer.empty() ? layers : m.layersFrom(l.note_below);
    const int bad = m.by_layer.empty() ? (m.share >= l.problem_from ? layers : 0) : m.layersFrom(l.problem_from);
    if (warn == 0)
        s += strf(" Under %s on all of its %s.", pct(l.note_below).c_str(), layersText(layers).c_str());
    else if (bad == 0)
        s += strf(" %s or more on %d of its %s.", pct(l.note_below).c_str(), warn, layersText(layers).c_str());
    else
        s += strf(" %s or more on %d of its %s, %s or more on %d.", pct(l.note_below).c_str(), warn,
                  layersText(layers).c_str(), pct(l.problem_from).c_str(), bad);
    return s;
}

std::string cavityGradeWhy(const fruit::Cavity& c, fruit::CupGrade g, const fruit::CupLimits& l) {
    if (!c.opens_up || c.cup.share_layer < 0) return "";
    const std::string lo = pct(l.note_below), hi = pct(l.problem_from);
    std::string s;
    if (g == fruit::CupGrade::Note) s = "Under " + lo + " everywhere, so it is a note.";
    else if (g == fruit::CupGrade::Warning) s = "At most between " + lo + " and " + hi + ", so it is a warning.";
    else if (c.cup.share >= l.problem_from) s = hi + " or more, so it is a problem.";
    else s = strf("Between %s and %s, but on %d layers or more, so it is a problem.", lo.c_str(), hi.c_str(), l.deep_layers);
    return s + strf(" The %s and %s lines and the %d layers are guesses, not measured on prints: change them under "
                    "Advanced, Suction cups.",
                    lo.c_str(), hi.c_str(), l.deep_layers);
}

const std::vector<double>& cupTiltAngles() {
    static const std::vector<double> a = {5, 10, 15, 20, 30, 45};
    return a;
}

namespace {

// The cups of `mesh` as placed: the largest share, the volume they hold, how many, and the worst grade.
CupTiltTry cupsOf(const fruit::TriangleMesh& mesh, const fruit::CupLimits& limits, const fruit::JobControl& ctl) {
    CupTiltTry t;
    const fruit::LayerStack ls = fruit::sliceModel(mesh, kLayerHeight, nullptr, 0, ctl);
    for (const fruit::HollowComponent& h : fruit::analyzeHollows(ls).components) {
        if (h.kind != fruit::HollowKind::SuctionCup) continue;
        ++t.cups;
        t.cup_volume += h.volume;
        t.worst = std::max(t.worst, h.cup.share);
        t.grade = std::max(t.grade, fruit::gradeCup(h.cup, h.layers(), limits));
    }
    return t;
}

// Better: a lower grade, then less pull, then less resin held.
bool betterTilt(const CupTiltTry& a, const CupTiltTry& b) {
    if (a.grade != b.grade) return a.grade < b.grade;
    if (std::abs(a.worst - b.worst) > 1e-6) return a.worst < b.worst;
    return a.cup_volume < b.cup_volume;
}

}  // namespace

CupTilt findCupTilt(const Document& doc, int part_id, const fruit::JobControl& ctl) {
    const auto t0 = std::chrono::steady_clock::now();
    CupTilt out;
    const Part* part = doc.findPart(part_id);
    if (!part) return out;
    out.part = part_id;
    const fruit::CupLimits limits = cupLimits(doc);
    const fruit::Placement now = part->placement();
    const std::vector<double>& angles = cupTiltAngles();
    const double steps = 1.0 + 4.0 * static_cast<double>(angles.size());
    ctl.step("tilt", 0);
    out.now = cupsOf(part->placed(), limits, ctl.sub(0, 1 / steps));
    out.now.rotation_deg = now.rotation_deg;
    int done = 1;
    for (const double deg : angles) {
        CupTiltTry best;
        bool any = false;
        for (int axis = 0; axis < 2; ++axis)
            for (const double sign : {1.0, -1.0}) {
                ctl.check();
                fruit::Placement p = now;
                p.rotation_deg = turnAbout(now.rotation_deg, axis, sign * deg);
                p.on_plate = false;  // tilted, it stands on supports
                p.elevation = now.on_plate ? fruit::Placement{}.elevation : now.elevation;
                CupTiltTry t = cupsOf(fruit::placeModel(part->mesh(), p), limits,
                                      ctl.sub(done / steps, (done + 1) / steps));
                ++done;
                t.axis = axis;
                t.deg = sign * deg;
                t.rotation_deg = p.rotation_deg;
                if (!any || betterTilt(t, best)) best = t;
                any = true;
            }
        out.tries.push_back(best);
        if (best.cups == 0 || best.grade != fruit::CupGrade::Problem) {
            out.pick = static_cast<int>(out.tries.size()) - 1;
            break;
        }
    }
    out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ctl.report("tilt", 1.0);
    return out;
}

std::string tiltWords(const CupTiltTry& t) {
    const std::string turn = std::abs(t.deg) < 1e-9 ? std::string("As it sits") : strf("At %.0f°", std::abs(t.deg));
    if (t.cups == 0) return turn + ", no suction cup is left.";
    const std::string held = volumeText(t.cup_volume);
    if (t.grade == fruit::CupGrade::Problem)
        return turn + strf(", still a problem: %s held, %s at most.", held.c_str(), pct(t.worst).c_str());
    return turn + strf(", no cup is a problem: %s held, %s at most (a %s).", held.c_str(), pct(t.worst).c_str(),
                       fruit::gradeName(t.grade));
}

std::string applyLabel(const HollowPlan& p) {
    if (p.hollowing()) return "Hollow it";
    std::vector<std::string> what;
    std::size_t traps = 0;
    for (std::size_t i = 0; i < p.filled.size(); ++i) traps += p.filled[i] ? 1 : 0;
    if (p.filling() && traps) what.push_back(traps == 1 ? "print the trap solid" : "print the traps solid");
    const std::size_t holes = p.drilling() ? p.trapHoles() : 0;
    if (holes) what.push_back(holes == 1 ? std::string("add a drain hole") : strf("add %zu drain holes", holes));
    if (const std::size_t v = p.vents()) what.push_back(v == 1 ? std::string("add a vent") : strf("add %zu vents", v));
    if (what.empty()) return "Leave it";
    std::string s = what[0];
    for (std::size_t i = 1; i < what.size(); ++i) s += (i + 1 == what.size() ? " and " : ", ") + what[i];
    s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

HollowOutcome cavityLabel(const HollowPlan& p, std::size_t i, bool applied) {
    const fruit::Cavity& c = p.cavities().cavities.at(i);
    const std::string v = volumeText(c.volume);
    const bool drains = applied && i < p.drains.size() && p.drains[i];
    if (p.hollowing())
        return {!applied || drains,
                v + (applied ? " taken out" : " to take out") + (applied && !drains ? " · holds resin" : ""), ""};
    const bool accepted = p.isAccepted(i);
    const std::string what = c.opens_up ? " suction cup" : " of trapped resin";
    if (accepted) return {true, v + what + " · accepted", ""};
    if (applied && i < p.filled.size() && p.filled[i]) return {true, v + " · printed solid", ""};
    if (drains) return {true, v + (c.opens_up ? " suction cup · vented" : " · drains"), ""};
    if (c.opens_up)
        return {p.grade(i) != fruit::CupGrade::Problem, v + " suction cup · " + fruit::gradeName(p.grade(i)), ""};
    return {false, v + " of trapped resin", ""};
}

unsigned hollowColour(const HollowPlan& p, std::size_t i, bool applied) {
    if (p.isAccepted(i)) return 0x8E97A6;
    if (applied && p.ownFixed(i)) return 0x6FBF8A;
    if (!p.isCup(i)) return 0xF2B84B;
    switch (p.grade(i)) {
        case fruit::CupGrade::Problem: return 0xF2B84B;
        case fruit::CupGrade::Warning: return 0xE3D27C;
        default: return 0x7FA7D6;
    }
}

fruit::Vec3 cavityLabelAt(const fruit::HollowResult& h, std::size_t i, double layer_height) {
    const fruit::Cavity& c = h.cavities.at(i);
    const std::size_t mid = static_cast<std::size_t>((c.first_layer + c.last_layer) / 2);
    fruit::Paths own;
    if (mid < h.pieces.size())
        for (const fruit::InteriorPiece& k : h.pieces[mid])
            if (k.cavity == static_cast<int>(i)) {
                const fruit::Paths piece = h.piece(mid, k);
                own.insert(own.end(), piece.begin(), piece.end());
            }
    const fruit::BBox2 b = fruit::bbox(own);
    const double z = (c.first_layer + c.last_layer + 1) * layer_height / 2;
    return own.empty() ? fruit::Vec3{0, 0, z} : fruit::Vec3{(b.min.x + b.max.x) / 2, (b.min.y + b.max.y) / 2, z};
}

HollowSection hollowSection(const HollowPlan& p, fruit::Vec2 at, fruit::Vec2 along) {
    HollowSection s;
    s.at = at;
    s.along = along;
    s.layer_height = p.layer_height;
    if (!p.solid || !p.layers) return s;
    // Layer by layer, in parallel: a big model has millions of outline points.
    const std::size_t n = p.solid->size();
    struct Layer {
        std::vector<SectionBand> outline, hollow, prints;
    };
    std::vector<Layer> per(n);
    const fruit::HollowResult& cav = p.cavities();
    fruit::parallelFor(n, 0, [&](std::size_t k) {
        auto add = [&](std::vector<SectionBand>& to, const fruit::Paths& paths) {
            for (const auto& [t0, t1] : fruit::lineSpans(paths, at, along)) to.push_back({k, t0, t1});
        };
        // The model as it is, with its own hollows: one piece.
        fruit::Paths whole = p.solid->slices[k];
        if (k < p.closed.interior.size()) whole.insert(whole.end(), p.closed.interior[k].begin(), p.closed.interior[k].end());
        add(per[k].outline, whole);
        if (!p.filling() && k < cav.interior.size()) add(per[k].hollow, cav.interior[k]);
        if (k < p.layers->size()) add(per[k].prints, p.layers->slices[k]);
    });
    for (Layer& l : per) {
        s.outline.insert(s.outline.end(), l.outline.begin(), l.outline.end());
        s.hollow.insert(s.hollow.end(), l.hollow.begin(), l.hollow.end());
        s.prints.insert(s.prints.end(), l.prints.begin(), l.prints.end());
    }
    return s;
}

std::size_t hollowsToFix(const Document& doc) {
    const HollowState& hs = doc.hollow;
    if (hs.planFresh(doc.revision())) {
        const HollowPlan& p = *hs.plan;
        // Notes, warnings and the accepted don't count; nor, once applied,
        // what drains or prints solid.
        std::size_t n = 0;
        for (std::size_t i = 0; i < p.closed.cavities.size(); ++i)
            n += !p.isAccepted(i) && p.grade(i) == fruit::CupGrade::Problem && !(hs.applied && p.ownFixed(i));
        if (hs.applied && p.hollowing()) n += p.undrained();
        return n;
    }
    // Until it is planned again: what was chosen is taken to work on the
    // resin traps; a suction cup is only fixed by its own vent.
    const bool traps_fixed = hs.applied && (hs.choices.hollow || hs.choices.fix != HollowChoices::Fix::Leave);
    if (hs.plan && hs.plan_revision == doc.revision()) {
        const HollowPlan& p = *hs.plan;
        std::size_t n = 0;
        for (std::size_t i = 0; i < p.closed.cavities.size(); ++i) {
            if (p.isAccepted(i) || p.grade(i) != fruit::CupGrade::Problem) continue;
            if (p.isCup(i) ? (hs.applied && i < p.own_drains.size() && p.own_drains[i]) : traps_fixed) continue;
            ++n;
        }
        return n;
    }
    if (traps_fixed) return 0;
    std::size_t n = 0;
    for (const Part& part : doc.parts()) n += static_cast<std::size_t>(std::max(0, part.check().cavities));
    return n;
}

HollowOutcome hollowSummary(const Document& doc) {
    const HollowState& hs = doc.hollow;
    const HollowChoices& c = hs.choices;
    const std::size_t left = hollowsToFix(doc);
    const HollowPlan* p = hs.plan && hs.plan_revision == doc.revision() ? hs.plan.get() : nullptr;
    const std::size_t found = p ? p->closed.cavities.size() : 0;
    const std::string to_fix = left == 1 ? std::string("1 hollow to fix") : strf("%zu hollows to fix", left);
    if (hs.applied && c.hollow)
        return {left == 0, strf("Hollowed · %.1f mm wall", c.wall),
                left ? to_fix : std::string("Nothing left that holds resin.")};
    if (left) {
        if (hs.applied && !c.hollow && c.fix == HollowChoices::Fix::Leave)
            return {false, to_fix, "Left as it is: shown as a problem before saving."};
        if (hs.applied) return {false, to_fix, "Shown as a problem before saving. The Hollow screen shows where."};
        return {false, to_fix, "The Hollow screen shows where each one is, and the fixes that fit it."};
    }
    if (found == 0) {
        if (hs.applied && c.fix != HollowChoices::Fix::Leave) return {true, "Nothing holds resin", "As applied."};
        return {true, "No hollows found", "Hollow and drain options"};
    }
    if (hs.applied) {
        std::size_t drained = 0, filled = 0, vented = 0;
        for (std::size_t i = 0; i < found; ++i) {
            if (!p->ownFixed(i)) continue;
            if (i < p->filled.size() && p->filled[i]) ++filled;
            else if (p->isCup(i)) ++vented;
            else ++drained;
        }
        const std::size_t fixed = drained + filled + vented;
        if (fixed) {
            const std::string title = fixed == drained ? std::string("Drain holes added")
                                      : fixed == filled ? std::string("Printed solid")
                                      : fixed == vented ? std::string(vented == 1 ? "Vent added" : "Vents added")
                                                        : strf("%zu hollows fixed", fixed);
            const std::string rest = fixed < found ? "The rest are notes, warnings or accepted."
                                     : fixed == drained ? (c.auto_holes ? "Placed automatically" : "Placed by hand")
                                                        : "Nothing left that holds resin or pulls.";
            return {true, title, rest};
        }
    }
    return {true, found == 1 ? std::string("1 hollow, not a problem") : strf("%zu hollows, none a problem", found),
            "Notes, warnings or accepted: on the Hollow screen"};
}

std::vector<HollowOutcome> hollowOutcomes(const HollowPlan& p) {
    std::vector<HollowOutcome> out;
    const bool several = p.cavities().cavities.size() > 1;
    if (p.hollowing()) {
        const auto& cavities = p.hollowed.cavities;
        for (std::size_t i = 0; i < cavities.size(); ++i) {
            const std::string which = several ? strf("Hollow %zu", i + 1) : std::string("The hollow");
            if (i < p.drains.size() && p.drains[i])
                out.push_back({true, which + " drains",
                               "A hole lets resin in and out of it, so it no longer traps resin or pulls a vacuum."});
            else
                out.push_back({false, which + " still holds resin",
                               "No hole reaches its lowest point. Place one there, or let FruitSlicer place them."});
        }
    }
    // The model's own hollows (with hollowing, its traps go with the rest; its cups stay).
    const auto& own = p.closed.cavities;
    const bool several_own = own.size() > 1;
    for (std::size_t i = 0; i < own.size(); ++i) {
        const bool cup = own[i].opens_up;
        if (p.hollowing() && !cup) continue;
        const std::string which = several_own ? strf("%s %zu", cup ? "Suction cup" : "Resin trap", i + 1)
                                              : std::string(cup ? "The suction cup" : "The resin trap");
        const bool counts = p.grade(i) == fruit::CupGrade::Problem;
        const std::string graded = std::string("Graded a ") + fruit::gradeName(p.grade(i)) + ": not counted as a problem.";
        if (p.isAccepted(i)) {
            out.push_back({true, which + " prints as it is", "Accepted for this job: not counted as a problem."});
        } else if (i < p.filled.size() && p.filled[i]) {
            out.push_back({true, which + " is printed solid", "More resin, and nothing trapped."});
        } else if (i < p.own_drains.size() && p.own_drains[i]) {
            out.push_back(cup ? HollowOutcome{true, which + " is vented",
                                              "Resin gets in under it through the vent, so its layers don't peel "
                                              "against a sealed pocket."}
                              : HollowOutcome{true, which + " drains",
                                              "A hole lets resin in and out of it, so it no longer traps resin or "
                                              "pulls a vacuum."});
        } else if (cup) {
            out.push_back({!counts, which + " stays as it is",
                           counts ? "Shown as a problem before saving. Tilt the model, add a vent, or accept it."
                                  : graded});
        } else if (p.drilling()) {
            out.push_back({false, which + " still holds resin",
                           "No hole reaches its lowest point. Place one there, or let FruitSlicer place them."});
        } else {
            out.push_back({false, which + " stays as it is", "Shown as a problem before saving."});
        }
    }
    if (p.missed())
        out.push_back({false, p.missed() == 1 ? "A hole reaches no hollow" : strf("%zu holes reach no hollow", p.missed()),
                       "It runs into solid or out of the model again, so it isn't drilled."});
    bool any_drains = false;
    for (std::size_t i = 0; i < p.drains.size(); ++i) any_drains = any_drains || (p.drains[i] && !p.isCup(i));
    if (p.hollowing()) any_drains = p.undrained() < p.hollowed.cavities.size();
    if (p.trapHoles() && any_drains)
        out.push_back({true, "Wash it out after printing", "Flush through the lower hole, then cure."});
    if (p.resin_saved > 0.5)
        out.push_back({true, "Saves " + volumeText(p.resin_saved) + " of resin",
                       p.hollowing() ? strf("Walls stay at least %.1f mm thick everywhere.", p.choices.wall)
                                     : std::string("What was sealed inside drains out instead of staying in the part.")});
    return out;
}

}  // namespace gui
