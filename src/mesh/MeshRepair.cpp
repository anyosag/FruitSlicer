// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "mesh/MeshRepair.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <vector>

namespace fruit {

namespace {

using Id = int32_t;

// Everything below is ordered by sorting, never by hashing, so the same file
// repairs to the same mesh on every platform.

// ---- Edges ----

uint64_t edgeKey(Id a, Id b) {
    return (static_cast<uint64_t>(std::min(a, b)) << 32) | static_cast<uint64_t>(std::max(a, b));
}

struct HalfEdge {
    uint64_t key;
    int32_t face;
    uint8_t corner;  // the edge from this corner of the face to the next
    bool forward;    // it runs from the lower vertex index to the higher
};

// Every face's edges, those of one edge together.
std::vector<HalfEdge> halfEdges(const std::vector<Face>& faces) {
    std::vector<HalfEdge> he;
    he.reserve(faces.size() * 3);
    for (size_t f = 0; f < faces.size(); ++f)
        for (int k = 0; k < 3; ++k) {
            const Id a = faces[f][k], b = faces[f][(k + 1) % 3];
            if (a == b) continue;
            he.push_back({edgeKey(a, b), static_cast<int32_t>(f), static_cast<uint8_t>(k), a < b});
        }
    std::sort(he.begin(), he.end(), [](const HalfEdge& x, const HalfEdge& y) {
        if (x.key != y.key) return x.key < y.key;
        if (x.forward != y.forward) return x.forward < y.forward;
        if (x.face != y.face) return x.face < y.face;
        return x.corner < y.corner;
    });
    return he;
}

size_t countOpen(const std::vector<Face>& faces) {
    std::vector<uint64_t> codes;
    codes.reserve(faces.size() * 3);
    for (const Face& f : faces)
        for (int k = 0; k < 3; ++k) {
            const Id a = f[k], b = f[(k + 1) % 3];
            if (a == b) continue;
            codes.push_back((edgeKey(a, b) << 1) | static_cast<uint64_t>(a < b));
        }
    std::sort(codes.begin(), codes.end());
    size_t open = 0;
    for (size_t i = 0; i < codes.size();) {
        const uint64_t key = codes[i] >> 1;
        long balance = 0;
        size_t j = i;
        for (; j < codes.size() && (codes[j] >> 1) == key; ++j) balance += (codes[j] & 1) ? 1 : -1;
        if (balance != 0) ++open;
        i = j;
    }
    return open;
}

// ---- The mesh being repaired ----

struct Fill {
    double perimeter = 0;  // of the loops it closes
    double across = 0;     // their size
    Vec3 middle;           // of their corners
};

struct Work {
    const std::vector<Vec3>& v;
    std::vector<Face> faces;
    std::vector<int32_t> origin;  // -1: a face of the file; else the fill that added it
    std::vector<Fill> fills;
    std::vector<char> turned;     // the face now runs the other way round than in the file
    size_t removed = 0;           // faces of the file dropped

    void turn(size_t f) {
        std::swap(faces[f][1], faces[f][2]);
        turned[f] ^= 1;
    }
};

void keepFaces(Work& w, const std::vector<char>& keep) {
    size_t n = 0;
    for (size_t i = 0; i < w.faces.size(); ++i) {
        if (!keep[i]) {
            if (w.origin[i] < 0) ++w.removed;
            continue;
        }
        w.faces[n] = w.faces[i];
        w.origin[n] = w.origin[i];
        w.turned[n] = w.turned[i];
        ++n;
    }
    w.faces.resize(n);
    w.origin.resize(n);
    w.turned.resize(n);
}

// Faces on the same three points.
enum class Doubles {
    Keep,     // leave them: only faces that touch a point twice go
    Thin,     // of those the same way round, one stays
    Patched,  // after patching: where a patch landed on faces, those that face
              // opposite ways cancel and go, and as many stay as don't
};

// A pair facing opposite ways is never touched before patching: the wall
// between two solids glued together is one, and does no harm. After it, a
// patch cancels with the face it landed on: that is how a fin, or a face
// that was there a second time in mirror image, leaves with the patch that
// closed it.
void clean(Work& w, Doubles doubles) {
    const size_t n = w.faces.size();
    std::vector<char> keep(n, 1);
    struct Rec {
        Id a, b, c;
        int32_t face;
        bool even;  // runs a -> b -> c
    };
    std::vector<Rec> recs;
    recs.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const Face& f = w.faces[i];
        if (f[0] == f[1] || f[1] == f[2] || f[0] == f[2]) {
            keep[i] = 0;
            continue;
        }
        if (doubles == Doubles::Keep) continue;
        int s = 0;
        if (f[1] < f[s]) s = 1;
        if (f[2] < f[s]) s = 2;
        const Id x = f[(s + 1) % 3], y = f[(s + 2) % 3];
        recs.push_back({f[s], std::min(x, y), std::max(x, y), static_cast<int32_t>(i), x < y});
    }
    std::sort(recs.begin(), recs.end(), [](const Rec& p, const Rec& q) {
        if (p.a != q.a) return p.a < q.a;
        if (p.b != q.b) return p.b < q.b;
        if (p.c != q.c) return p.c < q.c;
        return p.face < q.face;  // the file's own faces before patches
    });
    const bool patched = doubles == Doubles::Patched;
    for (size_t i = 0; i < recs.size();) {
        size_t j = i;
        long even = 0, odd = 0;
        bool patch = false;
        for (; j < recs.size() && recs[j].a == recs[i].a && recs[j].b == recs[i].b && recs[j].c == recs[i].c; ++j) {
            (recs[j].even ? even : odd)++;
            patch = patch || w.origin[static_cast<size_t>(recs[j].face)] >= 0;
        }
        if (j - i > 1 && (!patched || patch)) {
            long left_even = patched ? std::max(0L, even - odd) : std::min(1L, even);
            long left_odd = patched ? std::max(0L, odd - even) : std::min(1L, odd);
            for (size_t k = i; k < j; ++k) {
                long& left = recs[k].even ? left_even : left_odd;
                if (left > 0) --left;
                else keep[static_cast<size_t>(recs[k].face)] = 0;
            }
        }
        i = j;
    }
    keepFaces(w, keep);
}

// Faces that are there twice, the same way round, are a fault only where
// they leave edges open: a face doubled in an otherwise sound surface. A
// whole part that is there twice closes as well as one does. So they are
// thinned out only if that closes edges.
void thinDoubles(Work& w) {
    clean(w, Doubles::Keep);
    Work trial{w.v, w.faces, w.origin, {}, w.turned, w.removed};
    clean(trial, Doubles::Thin);
    if (trial.faces.size() == w.faces.size() || countOpen(trial.faces) >= countOpen(w.faces)) return;
    w.faces = std::move(trial.faces);
    w.origin = std::move(trial.origin);
    w.turned = std::move(trial.turned);
    w.removed = trial.removed;
}

// ---- Sheets: faces joined across edges that just two faces share ----

struct Links {
    std::vector<std::array<int32_t, 3>> next;  // the face across each edge, or -1
    std::vector<std::array<uint8_t, 3>> same;  // it runs along the edge the same way: one of the two is turned
};

Links linkFaces(const std::vector<Face>& faces) {
    Links l;
    l.next.assign(faces.size(), {-1, -1, -1});
    l.same.assign(faces.size(), {0, 0, 0});
    const std::vector<HalfEdge> he = halfEdges(faces);
    for (size_t i = 0; i < he.size();) {
        size_t j = i;
        while (j < he.size() && he[j].key == he[i].key) ++j;
        if (j - i == 2 && he[i].face != he[i + 1].face) {
            const HalfEdge& x = he[i];
            const HalfEdge& y = he[i + 1];
            const uint8_t s = x.forward == y.forward;
            l.next[static_cast<size_t>(x.face)][x.corner] = y.face;
            l.same[static_cast<size_t>(x.face)][x.corner] = s;
            l.next[static_cast<size_t>(y.face)][y.corner] = x.face;
            l.same[static_cast<size_t>(y.face)][y.corner] = s;
        }
        i = j;
    }
    return l;
}

// Turns faces so that each sheet runs one way round, turning the fewer.
void orient(Work& w) {
    const size_t n = w.faces.size();
    const Links l = linkFaces(w.faces);
    std::vector<int8_t> turn(n, -1);
    std::vector<int32_t> queue;
    for (size_t seed = 0; seed < n; ++seed) {
        if (turn[seed] >= 0) continue;
        queue.assign(1, static_cast<int32_t>(seed));
        turn[seed] = 0;
        size_t turned = 0;
        for (size_t qi = 0; qi < queue.size(); ++qi) {
            const auto f = static_cast<size_t>(queue[qi]);
            turned += static_cast<size_t>(turn[f]);
            for (int k = 0; k < 3; ++k) {
                const int32_t g = l.next[f][k];
                // A sheet that can't run one way round (a Möbius band) keeps
                // the clash where it is first met.
                if (g < 0 || turn[static_cast<size_t>(g)] >= 0) continue;
                turn[static_cast<size_t>(g)] = static_cast<int8_t>(turn[f] ^ l.same[f][k]);
                queue.push_back(g);
            }
        }
        const bool others = turned * 2 > queue.size();
        for (int32_t f : queue)
            if ((turn[static_cast<size_t>(f)] != 0) != others) w.turn(static_cast<size_t>(f));
    }
}

std::vector<int32_t> sheetsOf(const std::vector<Face>& faces, int32_t* count) {
    const Links l = linkFaces(faces);
    std::vector<int32_t> sheet(faces.size(), -1);
    std::vector<int32_t> queue;
    int32_t n = 0;
    for (size_t seed = 0; seed < faces.size(); ++seed) {
        if (sheet[seed] >= 0) continue;
        queue.assign(1, static_cast<int32_t>(seed));
        sheet[seed] = n;
        for (size_t qi = 0; qi < queue.size(); ++qi)
            for (int k = 0; k < 3; ++k) {
                const int32_t g = l.next[static_cast<size_t>(queue[qi])][k];
                if (g < 0 || sheet[static_cast<size_t>(g)] >= 0) continue;
                sheet[static_cast<size_t>(g)] = n;
                queue.push_back(g);
            }
        ++n;
    }
    *count = n;
    return sheet;
}

// ---- Open edges, as loops ----

struct Loop {
    std::vector<Id> v;  // its corners, in the order the missing faces run round it
    int32_t face = -1;  // a face along it
};

// The edges that are missing, each running against an edge that has no
// partner, split into loops that pass no point twice.
std::vector<Loop> openLoops(const Work& w) {
    struct Need {
        Id from, to;
        int32_t face;
    };
    std::vector<Need> need;
    {
        const std::vector<HalfEdge> he = halfEdges(w.faces);
        for (size_t i = 0; i < he.size();) {
            size_t j = i;
            long fwd = 0, back = 0;
            int32_t fwd_face = -1, back_face = -1;
            for (; j < he.size() && he[j].key == he[i].key; ++j) {
                if (he[j].forward) {
                    if (fwd++ == 0) fwd_face = he[j].face;
                } else {
                    if (back++ == 0) back_face = he[j].face;
                }
            }
            const Id lo = static_cast<Id>(he[i].key >> 32), hi = static_cast<Id>(he[i].key & 0xffffffffu);
            for (long r = back; r < fwd; ++r) need.push_back({hi, lo, fwd_face});
            for (long r = fwd; r < back; ++r) need.push_back({lo, hi, back_face});
            i = j;
        }
    }
    std::vector<Loop> loops;
    if (need.empty()) return loops;
    std::sort(need.begin(), need.end(), [](const Need& p, const Need& q) {
        if (p.from != q.from) return p.from < q.from;
        if (p.to != q.to) return p.to < q.to;
        return p.face < q.face;
    });
    // The points on open edges, numbered; as many missing edges leave each as
    // reach it, so a walk along them can always go on until it closes.
    std::vector<Id> ids;
    ids.reserve(need.size());
    for (const Need& e : need) ids.push_back(e.from);
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    auto index = [&](Id v) { return static_cast<int32_t>(std::lower_bound(ids.begin(), ids.end(), v) - ids.begin()); };
    std::vector<int32_t> first(ids.size() + 1, 0), to(need.size());
    for (size_t e = 0; e < need.size(); ++e) {
        ++first[static_cast<size_t>(index(need[e].from)) + 1];
        const int32_t t = index(need[e].to);
        // A point that edges reach but none leave can't be on a closed chain.
        to[e] = t < static_cast<int32_t>(ids.size()) && ids[static_cast<size_t>(t)] == need[e].to ? t : -1;
    }
    for (size_t i = 0; i < ids.size(); ++i) first[i + 1] += first[i];
    std::vector<int32_t> cursor(first.begin(), first.end() - 1);
    std::vector<int32_t> at(ids.size(), -1);  // where on the walk a point is
    std::vector<int32_t> path, path_edge;
    for (size_t s = 0; s < ids.size(); ++s) {
        while (cursor[s] < first[s + 1]) {
            path.assign(1, static_cast<int32_t>(s));
            path_edge.clear();
            at[s] = 0;
            auto cur = static_cast<int32_t>(s);
            while (cursor[static_cast<size_t>(cur)] < first[static_cast<size_t>(cur) + 1]) {
                const int32_t e = cursor[static_cast<size_t>(cur)]++;
                const int32_t t = to[static_cast<size_t>(e)];
                if (t < 0) break;
                if (at[static_cast<size_t>(t)] < 0) {
                    at[static_cast<size_t>(t)] = static_cast<int32_t>(path.size());
                    path.push_back(t);
                    path_edge.push_back(e);
                    cur = t;
                    continue;
                }
                // Back at a point of the walk: that stretch is a loop.
                const auto from = static_cast<size_t>(at[static_cast<size_t>(t)]);
                Loop loop;
                loop.face = need[static_cast<size_t>(from < path_edge.size() ? path_edge[from] : e)].face;
                for (size_t i = from; i < path.size(); ++i) loop.v.push_back(ids[static_cast<size_t>(path[i])]);
                for (size_t i = from + 1; i < path.size(); ++i) at[static_cast<size_t>(path[i])] = -1;
                path.resize(from + 1);
                path_edge.resize(from);
                if (loop.v.size() >= 3) loops.push_back(std::move(loop));
                cur = t;
            }
            for (int32_t p : path) at[static_cast<size_t>(p)] = -1;
        }
    }
    return loops;
}

// ---- Loops that run close to each other ----

struct Near {
    int32_t la, lb;  // two loops, la < lb
    int32_t pa, pb;  // where they are nearest: a corner of each
    double d2;       // how near, squared
    double along;    // > 0: they run the same way where they are near
    double cover;    // how much of the shorter one runs along the other, 0..1
    double reach;    // how far apart the corners where they are near lie
    double widest;   // the widest of those near distances
};

std::vector<Near> nearLoops(const std::vector<Vec3>& v, const std::vector<Loop>& loops, double gap) {
    struct Spot {
        uint64_t cell;
        int32_t loop, pos;
    };
    BBox3 box;
    for (const Loop& l : loops)
        for (Id i : l.v) box.extend(v[static_cast<size_t>(i)]);
    const Vec3 size = box.size();
    // 21 bits a side for the cell numbers.
    const double cell = std::max({gap, size.x / 1e6, size.y / 1e6, size.z / 1e6, 1e-12});
    auto coord = [&](double x, double lo) { return static_cast<int64_t>(std::floor((x - lo) / cell)); };
    auto key = [](int64_t x, int64_t y, int64_t z) {
        return (static_cast<uint64_t>(x) << 42) | (static_cast<uint64_t>(y) << 21) | static_cast<uint64_t>(z);
    };
    std::vector<Spot> spots;
    for (size_t l = 0; l < loops.size(); ++l)
        for (size_t p = 0; p < loops[l].v.size(); ++p) {
            const Vec3& q = v[static_cast<size_t>(loops[l].v[p])];
            spots.push_back({key(coord(q.x, box.min.x), coord(q.y, box.min.y), coord(q.z, box.min.z)),
                             static_cast<int32_t>(l), static_cast<int32_t>(p)});
        }
    std::sort(spots.begin(), spots.end(), [](const Spot& a, const Spot& b) {
        if (a.cell != b.cell) return a.cell < b.cell;
        if (a.loop != b.loop) return a.loop < b.loop;
        return a.pos < b.pos;
    });
    auto point = [&](int32_t l, int32_t p) -> const Vec3& {
        return v[static_cast<size_t>(loops[static_cast<size_t>(l)].v[static_cast<size_t>(p)])];
    };
    auto heading = [&](int32_t l, int32_t p) {
        const auto n = static_cast<int32_t>(loops[static_cast<size_t>(l)].v.size());
        return normalized(point(l, (p + 1) % n) - point(l, (p + n - 1) % n));
    };
    std::vector<Near> raw;
    const double g2 = gap * gap;
    for (const Spot& s : spots) {
        const Vec3& q = point(s.loop, s.pos);
        const int64_t cx = coord(q.x, box.min.x), cy = coord(q.y, box.min.y), cz = coord(q.z, box.min.z);
        int found = 0;
        for (int64_t dx = -1; dx <= 1; ++dx)
            for (int64_t dy = -1; dy <= 1; ++dy)
                for (int64_t dz = -1; dz <= 1; ++dz) {
                    if (cx + dx < 0 || cy + dy < 0 || cz + dz < 0) continue;
                    const uint64_t k = key(cx + dx, cy + dy, cz + dz);
                    auto it = std::lower_bound(spots.begin(), spots.end(), k,
                                               [](const Spot& a, uint64_t c) { return a.cell < c; });
                    // Each pair once, from its lower loop; a crowd of loops
                    // at one spot is linked well enough by a few of them.
                    for (; it != spots.end() && it->cell == k && found < 16; ++it) {
                        if (it->loop <= s.loop) continue;
                        const double d2 = norm2(point(it->loop, it->pos) - q);
                        if (d2 > g2) continue;
                        raw.push_back({s.loop, it->loop, s.pos, it->pos, d2,
                                       dot(heading(s.loop, s.pos), heading(it->loop, it->pos)), 0.0, 0.0, 0.0});
                        ++found;
                    }
                }
    }
    std::sort(raw.begin(), raw.end(), [](const Near& a, const Near& b) {
        if (a.la != b.la) return a.la < b.la;
        if (a.lb != b.lb) return a.lb < b.lb;
        if (a.d2 != b.d2) return a.d2 < b.d2;
        if (a.pa != b.pa) return a.pa < b.pa;
        return a.pb < b.pb;
    });
    std::vector<Near> out;
    std::vector<int32_t> on_a, on_b;
    for (size_t i = 0; i < raw.size();) {
        Near best = raw[i];
        double along = 0, widest = 0;
        BBox3 box_a, box_b;
        on_a.clear();
        on_b.clear();
        size_t j = i;
        for (; j < raw.size() && raw[j].la == raw[i].la && raw[j].lb == raw[i].lb; ++j) {
            along += raw[j].along;
            widest = std::max(widest, raw[j].d2);
            on_a.push_back(raw[j].pa);
            on_b.push_back(raw[j].pb);
            box_a.extend(point(raw[j].la, raw[j].pa));
            box_b.extend(point(raw[j].lb, raw[j].pb));
        }
        auto share = [&](std::vector<int32_t>& at, int32_t loop) {
            std::sort(at.begin(), at.end());
            const auto corners = static_cast<double>(std::unique(at.begin(), at.end()) - at.begin());
            return corners / static_cast<double>(loops[static_cast<size_t>(loop)].v.size());
        };
        best.along = along;
        best.cover = std::max(share(on_a, best.la), share(on_b, best.lb));
        best.reach = std::max(norm(box_a.size()), norm(box_b.size()));
        best.widest = std::sqrt(widest);
        out.push_back(best);
        i = j;
    }
    return out;
}

// Two sheets that meet along a seam run opposite ways along it, as two faces
// do along an edge. Where they run the same way one sheet is inside out:
// it is turned. Returns how many faces were turned.
//
// This is for surfaces that were never joined, and is kept to them:
//
// - Only a seam counts: half of the shorter loop's corners, and two at
//   least, lying along the other, over a stretch eight times as long as
//   the two are apart. Two holes that merely come close say nothing of
//   which way round their parts should be.
// - Only an open sheet is turned: one whose faces don't nearly add up to a
//   closed surface (the sum of their area vectors is a fifth or more of
//   their area; for a closed surface it is nothing). A part that is closed
//   but for some holes has an inside of its own, and its volume says which
//   way round it goes. Turned on a hint from a neighbour's hole, it would
//   print as a hole where it overlaps another part.
//
// Open sheets follow a closed one they border; among themselves the smaller
// side is turned.
size_t alignAcrossSeams(Work& w, const std::vector<Loop>& loops, const std::vector<Near>& near) {
    if (near.empty()) return 0;
    int32_t count = 0;
    const std::vector<int32_t> sheet = sheetsOf(w.faces, &count);
    const auto n = static_cast<size_t>(count);
    std::vector<Vec3> lean(n);
    std::vector<double> area(n, 0.0);
    std::vector<size_t> faces_in(n, 0);
    for (size_t f = 0; f < w.faces.size(); ++f) {
        const Face& t = w.faces[f];
        const Vec3 a = cross(w.v[static_cast<size_t>(t[1])] - w.v[static_cast<size_t>(t[0])],
                             w.v[static_cast<size_t>(t[2])] - w.v[static_cast<size_t>(t[0])]) * 0.5;
        const auto s = static_cast<size_t>(sheet[f]);
        lean[s] += a;
        area[s] += norm(a);
        ++faces_in[s];
    }
    auto open = [&](int32_t s) { return norm(lean[static_cast<size_t>(s)]) >= 0.2 * area[static_cast<size_t>(s)]; };
    struct Rel {
        int32_t a, b;
        bool turn;
    };
    std::vector<Rel> rel;
    for (const Near& e : near) {
        const size_t na = loops[static_cast<size_t>(e.la)].v.size(), nb = loops[static_cast<size_t>(e.lb)].v.size();
        const int32_t fa = loops[static_cast<size_t>(e.la)].face, fb = loops[static_cast<size_t>(e.lb)].face;
        if (fa < 0 || fb < 0 || e.along == 0 || e.cover < 0.5) continue;
        if (e.cover * static_cast<double>(std::min(na, nb)) < 2 || e.reach <= 8 * e.widest) continue;
        const int32_t sa = sheet[static_cast<size_t>(fa)], sb = sheet[static_cast<size_t>(fb)];
        if (sa == sb || (!open(sa) && !open(sb))) continue;
        rel.push_back({sa, sb, e.along > 0});
        rel.push_back({sb, sa, e.along > 0});
    }
    if (rel.empty()) return 0;
    std::sort(rel.begin(), rel.end(), [](const Rel& p, const Rel& q) {
        if (p.a != q.a) return p.a < q.a;
        if (p.b != q.b) return p.b < q.b;
        return p.turn < q.turn;
    });
    std::vector<int8_t> turn(n, -1);
    std::vector<int32_t> queue;
    std::vector<char> flip_sheet(n, 0);
    bool any = false;
    // From `seed`, through open sheets only.
    auto spread = [&](int32_t seed) {
        queue.assign(1, seed);
        turn[static_cast<size_t>(seed)] = 0;
        for (size_t qi = 0; qi < queue.size(); ++qi) {
            const int32_t s = queue[qi];
            auto it = std::lower_bound(rel.begin(), rel.end(), s, [](const Rel& r, int32_t x) { return r.a < x; });
            for (; it != rel.end() && it->a == s; ++it) {
                if (turn[static_cast<size_t>(it->b)] >= 0 || !open(it->b)) continue;
                turn[static_cast<size_t>(it->b)] = static_cast<int8_t>(turn[static_cast<size_t>(s)] ^ (it->turn ? 1 : 0));
                queue.push_back(it->b);
            }
        }
    };
    // Closed sheets stay as they are, and the open ones they border follow.
    for (const Rel& r : rel) {
        if (open(r.a) || turn[static_cast<size_t>(r.a)] >= 0) continue;
        spread(r.a);
        for (int32_t s : queue)
            if (turn[static_cast<size_t>(s)] == 1) flip_sheet[static_cast<size_t>(s)] = 1, any = true;
    }
    // The rest among themselves.
    for (const Rel& r : rel) {
        if (turn[static_cast<size_t>(r.a)] >= 0) continue;
        spread(r.a);
        size_t side[2] = {0, 0};
        for (int32_t s : queue) side[turn[static_cast<size_t>(s)]] += faces_in[static_cast<size_t>(s)];
        if (side[0] == 0 || side[1] == 0) continue;  // all run the right way
        const int8_t smaller = side[1] <= side[0] ? 1 : 0;
        for (int32_t s : queue)
            if (turn[static_cast<size_t>(s)] == smaller) flip_sheet[static_cast<size_t>(s)] = 1, any = true;
    }
    if (!any) return 0;
    size_t flipped = 0;
    for (size_t f = 0; f < w.faces.size(); ++f)
        if (flip_sheet[static_cast<size_t>(sheet[f])]) {
            w.turn(f);
            ++flipped;
        }
    return flipped;
}

// Loops near each other become one, joined through their nearest corners
// there and back, so that they are patched together: the patch of least
// area then zips a seam's two sides, and still caps two holes that merely
// lie close.
std::vector<std::vector<Id>> joinLoops(const std::vector<Loop>& loops, std::vector<Near> near) {
    const size_t n = loops.size();
    std::sort(near.begin(), near.end(), [](const Near& a, const Near& b) {
        if (a.d2 != b.d2) return a.d2 < b.d2;
        if (a.la != b.la) return a.la < b.la;
        return a.lb < b.lb;
    });
    std::vector<int32_t> root(n);
    std::iota(root.begin(), root.end(), 0);
    auto find = [&](int32_t a) {
        while (root[static_cast<size_t>(a)] != a) a = root[static_cast<size_t>(a)] = root[static_cast<size_t>(root[static_cast<size_t>(a)])];
        return a;
    };
    struct Bridge {
        int32_t pos, other, other_pos;
    };
    std::vector<std::vector<Bridge>> bridges(n);
    for (const Near& e : near) {
        const int32_t a = find(e.la), b = find(e.lb);
        if (a == b) continue;
        root[static_cast<size_t>(std::max(a, b))] = std::min(a, b);
        bridges[static_cast<size_t>(e.la)].push_back({e.pa, e.lb, e.pb});
        bridges[static_cast<size_t>(e.lb)].push_back({e.pb, e.la, e.pa});
    }
    for (auto& b : bridges)
        std::sort(b.begin(), b.end(), [](const Bridge& p, const Bridge& q) {
            if (p.pos != q.pos) return p.pos < q.pos;
            return p.other < q.other;
        });
    std::vector<std::vector<Id>> out;
    std::vector<char> seen(n, 0);
    struct Frame {
        int32_t loop, start, step;  // corners of `loop` given out so far, from `start`
        size_t bridge;              // bridges of the corner at hand looked at so far
    };
    std::vector<Frame> stack;
    for (size_t r = 0; r < n; ++r) {
        if (seen[r]) continue;
        seen[r] = 1;
        std::vector<Id> seq;
        stack.assign(1, Frame{static_cast<int32_t>(r), 0, 0, 0});
        while (!stack.empty()) {
            Frame& f = stack.back();
            const std::vector<Id>& lv = loops[static_cast<size_t>(f.loop)].v;
            const auto len = static_cast<int32_t>(lv.size());
            if (f.step == len) {
                // Round this loop: back to where it was entered, and across
                // to the corner it was entered from.
                const int32_t start = f.start;
                stack.pop_back();
                if (!stack.empty()) {
                    seq.push_back(lv[static_cast<size_t>(start)]);
                    const Frame& up = stack.back();
                    const std::vector<Id>& uv = loops[static_cast<size_t>(up.loop)].v;
                    seq.push_back(uv[static_cast<size_t>((up.start + up.step) % static_cast<int32_t>(uv.size()))]);
                }
                continue;
            }
            const int32_t pos = (f.start + f.step) % len;
            const std::vector<Bridge>& bs = bridges[static_cast<size_t>(f.loop)];
            if (f.bridge == 0) {
                seq.push_back(lv[static_cast<size_t>(pos)]);
                // Skip to this corner's bridges.
                f.bridge = static_cast<size_t>(std::lower_bound(bs.begin(), bs.end(), pos,
                                                                [](const Bridge& b, int32_t p) { return b.pos < p; }) -
                                               bs.begin()) + 1;
            }
            const size_t bi = f.bridge - 1;
            if (bi < bs.size() && bs[bi].pos == pos) {
                ++f.bridge;
                const Bridge b = bs[bi];
                if (!seen[static_cast<size_t>(b.other)]) {
                    seen[static_cast<size_t>(b.other)] = 1;
                    stack.push_back(Frame{b.other, b.other_pos, 0, 0});  // `f` is no longer valid
                }
                continue;
            }
            f.bridge = 0;
            ++f.step;
        }
        out.push_back(std::move(seq));
    }
    return out;
}

// ---- Patching a loop ----

constexpr int kExact = 96;  // loops up to this many corners get the patch of least area

// Drops a corner that follows itself, also round the end.
void tidy(std::vector<Id>& loop) {
    loop.erase(std::unique(loop.begin(), loop.end()), loop.end());
    while (loop.size() > 1 && loop.front() == loop.back()) loop.pop_back();
}

// The triangulation of least area (classic interval dynamic programming),
// with a little weight on edge length so that among equals, as in a flat
// loop, short diagonals win. A corner may come twice; a triangle on it twice
// is no triangle and is left out, its edges cancelling.
void patchExact(const std::vector<Vec3>& v, const std::vector<Id>& loop, double scale, std::vector<Face>& out) {
    const auto n = static_cast<int>(loop.size());
    auto at = [n](int i, int j) { return static_cast<size_t>(i) * static_cast<size_t>(n) + static_cast<size_t>(j); };
    std::vector<double> len(static_cast<size_t>(n) * static_cast<size_t>(n), 0.0);
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            len[at(i, j)] = norm(v[static_cast<size_t>(loop[static_cast<size_t>(i)])] -
                                 v[static_cast<size_t>(loop[static_cast<size_t>(j)])]);
    const double lambda = 1e-6 * scale;
    std::vector<double> cost(len.size(), 0.0);
    std::vector<int32_t> cut(len.size(), -1);
    for (int span = 2; span < n; ++span)
        for (int i = 0; i + span < n; ++i) {
            const int j = i + span;
            const Id a = loop[static_cast<size_t>(i)], c = loop[static_cast<size_t>(j)];
            const Vec3& pa = v[static_cast<size_t>(a)];
            const Vec3 ac = v[static_cast<size_t>(c)] - pa;
            double best = std::numeric_limits<double>::infinity();
            int best_k = i + 1;
            for (int k = i + 1; k < j; ++k) {
                const Id b = loop[static_cast<size_t>(k)];
                double t = cost[at(i, k)] + cost[at(k, j)];
                if (a != b && b != c && a != c)
                    t += 0.5 * norm(cross(v[static_cast<size_t>(b)] - pa, ac)) +
                         lambda * (len[at(i, k)] + len[at(k, j)] + len[at(i, j)]);
                if (t < best) {
                    best = t;
                    best_k = k;
                }
            }
            cost[at(i, j)] = best;
            cut[at(i, j)] = best_k;
        }
    std::vector<std::pair<int, int>> todo{{0, n - 1}};
    while (!todo.empty()) {
        const auto [i, j] = todo.back();
        todo.pop_back();
        if (j - i < 2) continue;
        const int k = cut[at(i, j)];
        const Id a = loop[static_cast<size_t>(i)], b = loop[static_cast<size_t>(k)], c = loop[static_cast<size_t>(j)];
        if (a != b && b != c && a != c) out.push_back({a, b, c});
        todo.emplace_back(i, k);
        todo.emplace_back(k, j);
    }
}

// A short chord that cuts a long loop into two parts of at least a quarter
// of its corners each: the shortest from 64 corners spread round it. Across
// a crack that is a rung over it; over a wide flat hole any chord will do.
// Never along an edge the loop already has, nor from a point to itself:
// where two loops were joined that would only part them again, and each
// would get its own cap.
void shortChord(const std::vector<Vec3>& v, const std::vector<Id>& loop, int* ci, int* cj) {
    const auto n = static_cast<int>(loop.size());
    const int apart = n / 4;
    const int samples = std::min(n, 64);
    double best = std::numeric_limits<double>::infinity();
    int bi = 0, bj = n / 2;
    std::vector<Id> joined;  // the corners an edge of the loop joins to this one
    for (int s = 0; s < samples; ++s) {
        const int i = static_cast<int>(static_cast<int64_t>(s) * n / samples);
        const Id a = loop[static_cast<size_t>(i)];
        const Vec3& p = v[static_cast<size_t>(a)];
        joined.assign(1, a);
        for (int t = 0; t < n; ++t)
            if (loop[static_cast<size_t>(t)] == a) {
                joined.push_back(loop[static_cast<size_t>((t + n - 1) % n)]);
                joined.push_back(loop[static_cast<size_t>((t + 1) % n)]);
            }
        for (int off = apart; off <= n - apart; ++off) {
            const int j = (i + off) % n;
            const Id b = loop[static_cast<size_t>(j)];
            if (std::find(joined.begin(), joined.end(), b) != joined.end()) continue;
            const double d2 = norm2(v[static_cast<size_t>(b)] - p);
            if (d2 < best) {
                best = d2;
                bi = i;
                bj = j;
            }
        }
    }
    *ci = std::min(bi, bj);
    *cj = std::max(bi, bj);
}

void patch(const std::vector<Vec3>& v, std::vector<Id> first, double scale, std::vector<Face>& out) {
    std::vector<std::vector<Id>> todo;
    todo.push_back(std::move(first));
    while (!todo.empty()) {
        std::vector<Id> loop = std::move(todo.back());
        todo.pop_back();
        tidy(loop);
        const auto n = static_cast<int>(loop.size());
        if (n < 3) continue;
        if (n <= kExact) {
            patchExact(v, loop, scale, out);
            continue;
        }
        int i = 0, j = 0;
        shortChord(v, loop, &i, &j);
        // i .. j with the chord back, and j .. round the end .. i with the
        // chord the other way: the two patches share it.
        std::vector<Id> a(loop.begin() + i, loop.begin() + j + 1);
        std::vector<Id> b(loop.begin() + j, loop.end());
        b.insert(b.end(), loop.begin(), loop.begin() + i + 1);
        todo.push_back(std::move(a));
        todo.push_back(std::move(b));
    }
}

// How far across a loop is, near enough: from its first corner to the
// farthest, and from that one to the farthest again.
double across(const std::vector<Vec3>& v, const std::vector<Id>& loop) {
    auto farthest = [&](Id from, double* d) {
        Id best = from;
        *d = 0;
        for (Id i : loop) {
            const double d2 = norm2(v[static_cast<size_t>(i)] - v[static_cast<size_t>(from)]);
            if (d2 > *d) *d = d2, best = i;
        }
        return best;
    };
    double d = 0;
    farthest(farthest(loop.front(), &d), &d);
    return std::sqrt(d);
}

// ---- The pieces that come of it ----

// Pieces are faces joined through shared points. Two things are settled for
// each once it is closed:
//
// - One that holds no volume goes: a surface with no thickness and the
//   patch that closed it.
// - One that had to be patched, or had faces turned, faces outward. Which
//   way round a broken piece should go as a whole, its faces can't say for
//   sure (most may be wrong, and a seam only says how two sheets go
//   together); left inward it would be taken for a hollow wherever it lies
//   inside another part, and print as a hole there. So only a hollow whose
//   shell needed no repair is kept as one; a broken one prints solid.
void settlePieces(Work& w) {
    std::vector<Id> root(w.v.size());
    std::iota(root.begin(), root.end(), 0);
    auto find = [&](Id a) {
        while (root[static_cast<size_t>(a)] != a) a = root[static_cast<size_t>(a)] = root[static_cast<size_t>(root[static_cast<size_t>(a)])];
        return a;
    };
    for (const Face& f : w.faces) {
        const Id a = find(f[0]);
        root[static_cast<size_t>(find(f[1]))] = a;
        root[static_cast<size_t>(find(f[2]))] = a;
    }
    std::vector<double> volume(w.v.size(), 0.0), area(w.v.size(), 0.0);
    std::vector<char> touched(w.v.size(), 0);
    for (size_t i = 0; i < w.faces.size(); ++i) {
        const Face& f = w.faces[i];
        const Vec3& a = w.v[static_cast<size_t>(f[0])];
        const Vec3 b = w.v[static_cast<size_t>(f[1])] - a, c = w.v[static_cast<size_t>(f[2])] - a;
        const Vec3 n = cross(b, c);
        const auto r = static_cast<size_t>(find(f[0]));
        // About the piece's own first point, so that size far from the origin
        // does not drown a thin piece's volume in rounding.
        volume[r] += dot(a - w.v[r], n) / 6.0;
        area[r] += 0.5 * norm(n);
        if (w.turned[i] || w.origin[i] >= 0) touched[r] = 1;
    }
    std::vector<char> keep(w.faces.size(), 1);
    bool any = false;
    for (size_t i = 0; i < w.faces.size(); ++i) {
        const auto r = static_cast<size_t>(find(w.faces[i][0]));
        if (std::abs(volume[r]) <= 1e-9 * std::pow(area[r], 1.5)) {
            keep[i] = 0;
            any = true;
        } else if (touched[r] && volume[r] < 0) {
            w.turn(i);
        }
    }
    if (any) keepFaces(w, keep);
}

}  // namespace

std::size_t countOpenEdges(const TriangleMesh& mesh) { return countOpen(mesh.faces); }

RepairReport repairMesh(TriangleMesh& mesh, const RepairOptions& options) {
    RepairReport r;
    r.open_edges_before = r.open_edges_after = countOpen(mesh.faces);
    if (r.open_edges_before == 0) return r;
    for (const Vec3& p : mesh.vertices)
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return r;  // nothing to go by

    const double diagonal = norm(mesh.bbox().size());
    const double gap = options.seam_gap > 0 ? options.seam_gap : std::min(0.1, 2e-3 * diagonal);
    Work w{mesh.vertices, mesh.faces, std::vector<int32_t>(mesh.faces.size(), -1), {},
           std::vector<char>(mesh.faces.size(), 0), 0};
    thinDoubles(w);
    orient(w);
    for (int pass = 0; pass < 2; ++pass) {
        const std::vector<Loop> loops = openLoops(w);
        if (loops.empty()) break;
        const std::vector<Near> near = nearLoops(w.v, loops, gap);
        if (pass == 0) {
            // Sheets turned to match across their seams change the loops.
            if (alignAcrossSeams(w, loops, near)) {
                thinDoubles(w);
                continue;
            }
        }
        std::vector<Face> added;
        for (std::vector<Id>& loop : joinLoops(loops, near)) {
            tidy(loop);
            if (loop.size() < 3) continue;
            Fill fill;
            for (size_t i = 0; i < loop.size(); ++i)
                fill.perimeter += norm(w.v[static_cast<size_t>(loop[i])] -
                                       w.v[static_cast<size_t>(loop[(i + 1) % loop.size()])]);
            for (Id i : loop) fill.middle += w.v[static_cast<size_t>(i)] / static_cast<double>(loop.size());
            fill.across = across(w.v, loop);
            added.clear();
            patch(w.v, std::move(loop), std::max(fill.across, 1e-9), added);
            const auto id = static_cast<int32_t>(w.fills.size());
            w.fills.push_back(fill);
            w.faces.insert(w.faces.end(), added.begin(), added.end());
            w.origin.insert(w.origin.end(), added.size(), id);
            w.turned.insert(w.turned.end(), added.size(), 0);
        }
        break;
    }
    clean(w, Doubles::Patched);
    settlePieces(w);

    const size_t open = countOpen(w.faces);
    if (w.faces.empty() || open >= r.open_edges_before) return r;  // no better: the file as it is

    // The file's faces first, in their order, then the patches.
    std::vector<Face> faces;
    faces.reserve(w.faces.size());
    for (size_t i = 0; i < w.faces.size(); ++i)
        if (w.origin[i] < 0) {
            faces.push_back(w.faces[i]);
            r.faces_flipped += w.turned[i] != 0;
        }
    std::vector<double> area(w.fills.size(), 0.0);
    std::vector<Vec3> facing(w.fills.size());
    std::vector<char> used(w.fills.size(), 0);
    for (size_t i = 0; i < w.faces.size(); ++i) {
        if (w.origin[i] < 0) continue;
        const Face& f = w.faces[i];
        faces.push_back(f);
        const auto id = static_cast<size_t>(w.origin[i]);
        used[id] = 1;
        const Vec3 a = cross(w.v[static_cast<size_t>(f[1])] - w.v[static_cast<size_t>(f[0])],
                             w.v[static_cast<size_t>(f[2])] - w.v[static_cast<size_t>(f[0])]) * 0.5;
        area[id] += norm(a);
        facing[id] += a;
        ++r.patch_faces;
    }
    bool first = true;
    for (size_t id = 0; id < w.fills.size(); ++id) {
        if (!used[id]) continue;
        r.patch_area += area[id];
        if (first || area[id] > r.biggest_patch_area) {
            first = false;
            r.biggest_patch_area = area[id];
            r.biggest_patch_across = w.fills[id].across;
            r.biggest_patch_at = w.fills[id].middle;
            r.biggest_patch_facing = normalized(facing[id]);
        }
        // A strip of this area along half the loop's length is this wide.
        const double width = w.fills[id].perimeter > 0 ? 2 * area[id] / w.fills[id].perimeter : 0;
        if (width <= gap) {
            ++r.gaps_closed;
            r.widest_gap = std::max(r.widest_gap, width);
        } else {
            ++r.holes_filled;
            r.largest_hole = std::max(r.largest_hole, w.fills[id].across);
        }
    }
    r.open_edges_after = open;
    r.faces_removed = w.removed;
    mesh.faces = std::move(faces);

    // Points no face uses any more (a dropped surface's) would still count
    // towards the model's size.
    if (w.removed > 0) {
        std::vector<Id> remap(mesh.vertices.size(), -1);
        for (const Face& f : mesh.faces)
            for (Id i : f) remap[static_cast<size_t>(i)] = 0;
        Id n = 0;
        for (size_t i = 0; i < remap.size(); ++i)
            if (remap[i] == 0) {
                mesh.vertices[static_cast<size_t>(n)] = mesh.vertices[i];
                remap[i] = n++;
            }
        mesh.vertices.resize(static_cast<size_t>(n));
        for (Face& f : mesh.faces)
            for (Id& i : f) i = remap[static_cast<size_t>(i)];
    }
    return r;
}

}  // namespace fruit
