// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "support/Raft.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "mesh/Primitives.h"
#include "ops/MeshOps.h"
#include "support/SupportGeometry.h"
#include "support/SupportTree.h"

namespace fruit {

namespace {

using nlohmann::json;

double round9(double x) { return std::round(x * 1e9) / 1e9; }
bool near(double a, double b, double eps = 1e-6) { return std::abs(a - b) <= eps; }

constexpr double kLayer = 0.1;         // mm: a spec raft is built in layers this thick
constexpr double kArcTol = 0.004;      // mm: arcs of the layers' offsets
constexpr double kPlanFillet = 1.0;    // mm: joints rounded in plan (bar to pad, link to frame)
constexpr double kInnerCorner = 0.5;   // mm: a window's inner corners, in plan
constexpr double kRoofCorner = 0.4;    // mm: a window's roof corners
constexpr double kFarEdge = 1e3;       // mm: the top surface runs on, inward, past any edge
constexpr double kLobeArcMax = 75.0;   // degrees: a lobe that curves more is a corner, no window there

const char* kindName(RaftKind k) { return k == RaftKind::Full ? "full" : "pads"; }
const char* windowsName(RaftWindows w) {
    switch (w) {
        case RaftWindows::Few: return "few";
        case RaftWindows::Some: return "some";
        case RaftWindows::EveryTip: return "every_tip";
        default: return "none";
    }
}

// A polyline (d, z) with corner `i` rounded by radius r[i] (its ends stay
// sharp). Each radius is cut down to fit the two edges beside it.
std::vector<Vec2> filleted(const std::vector<Vec2>& v, const std::vector<double>& r, int per90 = 6) {
    std::vector<Vec2> out;
    const size_t n = v.size();
    for (size_t i = 0; i < n; ++i) {
        if (i == 0 || i + 1 == n || r[i] <= 0) {
            out.push_back(v[i]);
            continue;
        }
        const Vec2 V = v[i], a = v[i - 1] - V, b = v[i + 1] - V;
        const double la = norm(a), lb = norm(b);
        if (la < 1e-9 || lb < 1e-9) {
            out.push_back(V);
            continue;
        }
        const Vec2 u = a * (1 / la), w = b * (1 / lb);
        const double ang = std::acos(std::clamp(dot(u, w), -1.0, 1.0));
        if (ang < 1e-6 || ang > kPi - 1e-6) {
            out.push_back(V);
            continue;
        }
        double t = r[i] / std::tan(ang / 2);
        t = std::min({t, 0.45 * la, 0.45 * lb});
        const double rr = t * std::tan(ang / 2);
        const Vec2 T1 = V + u * t, T2 = V + w * t;
        const Vec2 bis = normalized(u + w);
        const Vec2 C = V + bis * (rr / std::sin(ang / 2));
        const double a1 = std::atan2(T1.y - C.y, T1.x - C.x), a2 = std::atan2(T2.y - C.y, T2.x - C.x);
        double da = a2 - a1;
        while (da > kPi) da -= 2 * kPi;
        while (da < -kPi) da += 2 * kPi;
        const int m = std::max(2, static_cast<int>(std::abs(da) / (kPi / 2) * per90) + 1);
        for (int k = 0; k <= m; ++k) {
            const double t2 = a1 + da * k / m;
            out.push_back({C.x + rr * std::cos(t2), C.y + rr * std::sin(t2)});
        }
    }
    return out;
}

// Where the polyline crosses height z: its d values, in order.
std::vector<double> crossings(const std::vector<Vec2>& p, double z) {
    std::vector<double> c;
    for (size_t i = 0; i + 1 < p.size(); ++i) {
        const Vec2 a = p[i], b = p[i + 1];
        if (a.y == b.y) continue;
        if ((a.y - z) * (b.y - z) > 0) continue;
        const double t = (z - a.y) / (b.y - a.y);
        if (t < 0 || t >= 1) continue;
        c.push_back(a.x + t * (b.x - a.x));
    }
    std::sort(c.begin(), c.end());
    return c;
}

// The layer of an edge at height z: `outline` pulled in by where the profile
// says the material is (between pairs of crossings, the last running on inward).
Paths edgeLayer(const Paths& outline, const std::vector<Vec2>& prof, double z) {
    const std::vector<double> c = crossings(prof, z);
    Paths out;
    for (size_t i = 0; i < c.size(); i += 2) {
        Paths a = offset(outline, -c[i], kArcTol);
        if (i + 1 < c.size()) a = difference(a, offset(outline, -c[i + 1], kArcTol));
        out = out.empty() ? a : unionOf(out, a);
    }
    return out;
}

Path circle(const Vec2& c, double r, int n) {
    Path p;
    p.reserve(n);
    for (int i = 0; i < n; ++i) {
        const double a = 2 * kPi * i / n;
        p.push_back(toPoint({c.x + r * std::cos(a), c.y + r * std::sin(a)}));
    }
    return p;
}

Path bar(const Vec2& a, const Vec2& b, double hw) {
    const Vec2 d = normalized(b - a), n{-d.y * hw, d.x * hw};
    return {toPoint(a - n), toPoint(b - n), toPoint(b + n), toPoint(a + n)};
}

Paths tidy(Paths p) {
    p.erase(std::remove_if(p.begin(), p.end(), [](const Path& q) { return std::abs(area(q)) < 0.5; }), p.end());
    return p;
}

// The trunks' discs of radius r, closed over gaps narrower than `gap`: the
// outline a Full raft (or a frame) follows round the supports.
Paths closedOutline(const std::vector<Vec2>& trunks, double r, double gap, const GeometryQuality& q) {
    Paths discs;
    const int n = std::max(16, q.segments(r));
    for (const Vec2& t : trunks) discs.push_back(circle(t, r, n));
    if (discs.empty()) return {};
    Paths u = unionNonZero(discs);
    if (gap > 0) u = offset(offset(u, gap / 2), -gap / 2);
    return tidy(simplify(u, 0.02));
}

// Windows at the tips of lobes: an outline stretch that is the arc of one
// trunk's disc, curving no more than a corner does; then as many as fit
// `spacing` apart along each outline (0: all of them).
std::vector<RaftWindow> pickWindows(const Paths& outline, const std::vector<Vec2>& trunks, double r, double spacing) {
    std::vector<RaftWindow> out;
    if (spacing < 0) return out;
    for (const Path& path : outline) {
        if (area(path) <= 0 || path.size() < 3) continue;
        std::vector<Vec2> v;
        for (const Point64& p : path) v.push_back(toVec(p));
        std::vector<double> s(v.size() + 1, 0.0);
        for (size_t i = 0; i < v.size(); ++i) s[i + 1] = s[i] + norm(v[(i + 1) % v.size()] - v[i]);
        const double L = s.back();
        struct Tip {
            double s;
            RaftWindow w;
        };
        std::vector<Tip> tips;
        for (const Vec2& t : trunks) {
            std::vector<double> ang;
            std::vector<size_t> idx;
            for (size_t i = 0; i < v.size(); ++i)
                if (std::abs(norm(v[i] - t) - r) < 0.03) {
                    ang.push_back(std::atan2(v[i].y - t.y, v[i].x - t.x));
                    idx.push_back(i);
                }
            if (ang.size() < 2) continue;
            double sx = 0, sy = 0;
            for (double a : ang) sx += std::cos(a), sy += std::sin(a);
            const double a0 = std::atan2(sy, sx);
            double lo = 1e9, hi = -1e9, best = 1e9;
            size_t at = idx[0];
            for (size_t k = 0; k < ang.size(); ++k) {
                double rel = ang[k] - a0;
                while (rel > kPi) rel -= 2 * kPi;
                while (rel < -kPi) rel += 2 * kPi;
                lo = std::min(lo, rel), hi = std::max(hi, rel);
                if (std::abs(rel) < best) best = std::abs(rel), at = idx[k];
            }
            if ((hi - lo) * 180 / kPi > kLobeArcMax) continue;
            tips.push_back({s[at], {t, {std::cos(a0), std::sin(a0)}, r}});
        }
        std::sort(tips.begin(), tips.end(), [](const Tip& a, const Tip& b) { return a.s < b.s; });
        if (spacing == 0) {
            for (const Tip& t : tips) out.push_back(t.w);
            continue;
        }
        const double gap = 0.8 * spacing;
        std::vector<size_t> best;
        for (size_t start = 0; start < tips.size(); ++start) {
            std::vector<size_t> pick{start};
            for (size_t k = 1; k < tips.size(); ++k) {
                const size_t c = (start + k) % tips.size();
                const double sc = std::fmod(tips[c].s - tips[start].s + L, L);
                const double last = std::fmod(tips[pick.back()].s - tips[start].s + L, L);
                if (sc - last >= gap && L - sc >= gap) pick.push_back(c);
            }
            if (pick.size() > best.size()) best = pick;
        }
        for (size_t i : best) out.push_back(tips[i].w);
    }
    return out;
}

// One window at height z, in plan: from outside the raft in to `depth` past
// the base edge, its inner corners rounded; near the roof it narrows and its
// back pulls out, so the roof meets the walls in a curve.
Path windowAt(const RaftWindow& w, double base_inset, double wh, double z) {
    const double ra = std::min(kRoofCorner, 0.45 * wh);
    double shrink = 0;
    if (z > wh - ra) {
        const double dz = z - (wh - ra);
        shrink = ra - std::sqrt(std::max(0.0, ra * ra - dz * dz));
    }
    const double hw = kWindowWidth / 2 - shrink;
    const double r_in = w.radius - base_inset - (kWindowDepth - shrink);
    const double r_out = w.radius + std::max(0.0, -base_inset) + 1.0;
    const double rc = std::min({kInnerCorner, hw * 0.95, (r_out - r_in) / 2});
    const Vec2 n = w.dir, t{-n.y, n.x};
    std::vector<Vec2> loc{{-hw, r_out}, {-hw, r_in + rc}};
    for (int k = 1; k < 6; ++k) {
        const double a = kPi + kPi / 2 * k / 6;
        loc.push_back({-hw + rc + rc * std::cos(a), r_in + rc + rc * std::sin(a)});
    }
    loc.push_back({-hw + rc, r_in});
    loc.push_back({hw - rc, r_in});
    for (int k = 1; k < 6; ++k) {
        const double a = 1.5 * kPi + kPi / 2 * k / 6;
        loc.push_back({hw - rc + rc * std::cos(a), r_in + rc + rc * std::sin(a)});
    }
    loc.push_back({hw, r_in + rc});
    loc.push_back({hw, r_out});
    Path p;
    for (const Vec2& q : loc) p.push_back(toPoint(w.trunk + n * q.y + t * q.x));
    if (area(p) < 0) std::reverse(p.begin(), p.end());
    return p;
}

// A rounded foot: the corner between the raft's top and a trunk's base cone,
// filled with a concave curve of radius `rf`, as rings stacked up the curve.
void appendFootFillet(TriangleMesh& m, const Vec2& c, double h, double foot_r, double shaft_r, double cone_h,
                      double rf, const GeometryQuality& q) {
    // The base cone runs from foot_r at h − 0.1 up to shaft_r at h + cone_h.
    const double zb = h - 0.1, zt = h + cone_h;
    if (zt <= zb) return;
    const double rh = foot_r + (shaft_r - foot_r) * (h - zb) / (zt - zb);
    const Vec2 u = normalized(Vec2{shaft_r - foot_r, zt - zb});  // up the cone, (r, z)
    const double ang = std::acos(std::clamp(u.x, -1.0, 1.0));     // between the raft's top (outward) and the cone
    if (ang <= 1e-3 || ang >= kPi - 1e-3) return;
    const double t = rf / std::tan(ang / 2);
    const Vec2 corner{rh, h};
    const Vec2 T1 = corner + Vec2{t, 0}, T2 = corner + u * t;
    const Vec2 bis = normalized(Vec2{1, 0} + u);
    const Vec2 C = corner + bis * (rf / std::sin(ang / 2));
    const double a1 = std::atan2(T1.y - C.y, T1.x - C.x), a2 = std::atan2(T2.y - C.y, T2.x - C.x);
    double da = a2 - a1;
    while (da > kPi) da -= 2 * kPi;
    while (da < -kPi) da += 2 * kPi;
    std::vector<Vec2> ring{{T1.x, h - 0.05}};
    for (int k = 0; k <= 4; ++k) ring.push_back({C.x + rf * std::cos(a1 + da * k / 4), C.y + rf * std::sin(a1 + da * k / 4)});
    const int seg = q.segments(T1.x);
    for (size_t i = 0; i + 1 < ring.size(); ++i)
        if (ring[i + 1].y > ring[i].y + 1e-6)
            appendFrustum(m, {c.x, c.y, ring[i].y}, ring[i].x, {c.x, c.y, ring[i + 1].y}, ring[i + 1].x, seg);
}

void appendLayer(TriangleMesh& m, const Paths& layer, double z0, double dz) {
    for (const ExPolygon& ex : toExPolygons(layer)) {
        std::vector<std::vector<Vec2>> polys;
        auto add = [&](const Path& p) {
            std::vector<Vec2> v;
            v.reserve(p.size());
            for (const Point64& q : p) v.push_back(toVec(q));
            polys.push_back(std::move(v));
        };
        add(ex.contour);
        for (const Path& hole : ex.holes) add(hole);
        const ops::Solid prism = ops::extrude(polys, dz);
        if (!prism.valid() || prism.empty()) continue;
        TriangleMesh pm = prism.toMesh();
        for (Vec3& v : pm.vertices) v.z += z0;
        m.append(pm);
    }
}

std::vector<Vec2> raftTrunks(const SupportTree& tree) {
    std::vector<Vec2> t;
    for (const Pillar& P : tree.pillars)
        if (!P.on_model && !P.merged) t.push_back(P.xy);
    return t;
}

// Builds (into `m` when given) or only measures a spec raft.
RaftShape specRaft(TriangleMesh* m, const SupportTree& tree, const TreeConfig& tc) {
    RaftShape shape;
    if (!tc.raft_spec || tree.raft_top <= 0) return shape;
    const RaftSpec& s = *tc.raft_spec;
    const double H = tree.raft_top;
    const double foot = tc.base_diameter / 2;
    const GeometryQuality& q = tc.quality;
    shape.trunks = raftTrunks(tree);
    if (shape.trunks.empty()) return shape;
    const bool full = s.kind == RaftKind::Full;
    const bool edge = full || s.frame;
    const double edge_h = full ? H : s.frame_height;
    const double pad_r = foot + s.reach;
    const double lobe_r = full ? pad_r : pad_r + s.frame_gap + s.frame_width;
    const std::vector<Vec2> prof = edge ? raftEdgeProfile(s, H) : std::vector<Vec2>{};
    if (edge) shape.top = closedOutline(shape.trunks, lobe_r, s.gap_fill, q);
    const double base_inset = edge ? sideInset(edge_h, full ? s.side_angle : s.frame_angle) : 0.0;
    // Bars: the fewest that make one piece (Builder::raft's), and on a grid every neighbour too.
    std::vector<std::pair<Vec2, Vec2>> bars;
    for (const Rod& r : tree.raft_links) bars.push_back({r.a.xy(), r.b.xy()});
    if (!full && s.join == PadJoin::Grid) {
        const auto& T = shape.trunks;
        std::vector<double> nearest(T.size(), 1e300);
        for (size_t i = 0; i < T.size(); ++i)
            for (size_t j = 0; j < T.size(); ++j)
                if (i != j) nearest[i] = std::min(nearest[i], norm(T[i] - T[j]));
        for (size_t i = 0; i < T.size(); ++i)
            for (size_t j = i + 1; j < T.size(); ++j)
                if (norm(T[i] - T[j]) <= 1.25 * std::min(nearest[i], nearest[j])) bars.push_back({T[i], T[j]});
    }
    // Links from the pads beside the frame to its inner wall.
    std::vector<std::pair<Vec2, Vec2>> links;
    if (!full && s.frame && !shape.top.empty()) {
        std::vector<Vec2> wall;
        for (const Path& p : offset(shape.top, -s.frame_width, kArcTol))
            for (const Point64& pt : p) wall.push_back(toVec(pt));
        for (const Vec2& t : shape.trunks) {
            double best = 1e300;
            Vec2 at;
            for (const Vec2& w : wall)
                if (const double d = norm(w - t); d < best) best = d, at = w;
            if (best < pad_r + s.frame_gap + 0.6) links.push_back({t, at + normalized(at - t) * 0.6});
        }
    }
    if (edge && s.windows != RaftWindows::None)
        shape.windows = pickWindows(shape.top, shape.trunks, lobe_r, windowSpacing(s.windows));
    const double wh = windowHeight(s);
    // Bars and links: full width from the plate, their top edge rounded.
    const double bar_h = full ? 0.8 * H : s.bar_pct / 100.0 * H;
    const double bar_w = s.bar_width;
    const double edge_r = s.rounded ? 0.25 : 0.0;
    const std::vector<Vec2> pad_prof = filleted({{sideInset(H, s.side_angle), 0}, {0, H}, {kFarEdge, H}}, {0, edge_r, 0});
    const std::vector<Vec2> bar_prof = filleted({{0, 0}, {0, bar_h}, {kFarEdge, bar_h}}, {0, edge_r, 0});
    const std::vector<Vec2> link_prof = filleted({{0, 0}, {0, H}, {kFarEdge, H}}, {0, edge_r, 0});
    double top_z = H;
    if (edge) top_z = std::max(top_z, edge_h + s.lip_height);
    const int n = std::max(1, static_cast<int>(std::ceil(top_z / kLayer - 1e-9)));
    const double dz = top_z / n;
    for (int k = 0; k < n; ++k) {
        const double z0 = k * dz, zm = z0 + dz / 2;
        Paths layer = edge ? edgeLayer(shape.top, prof, zm) : Paths{};
        Paths add;
        if (!full) {
            const std::vector<double> c = crossings(pad_prof, zm);
            if (!c.empty()) {
                const double r = pad_r - c[0];
                if (r > 0.05)
                    for (const Vec2& t : shape.trunks) add.push_back(circle(t, r, std::max(16, q.segments(r))));
            }
            const std::vector<double> lc = crossings(link_prof, zm);
            if (!lc.empty())
                for (const auto& [a, b] : links) add.push_back(bar(a, b, std::max(0.05, bar_w / 2 - lc[0])));
        }
        const std::vector<double> bc = crossings(bar_prof, zm);
        if (!bc.empty())
            for (const auto& [a, b] : bars) add.push_back(bar(a, b, std::max(0.05, bar_w / 2 - bc[0])));
        if (!add.empty()) layer = layer.empty() ? unionNonZero(add) : unionOf(layer, unionNonZero(add));
        if (s.rounded && !full) layer = offset(offset(layer, kPlanFillet, kArcTol), -kPlanFillet, kArcTol);
        if (!shape.windows.empty() && zm < wh) {
            Paths cut;
            for (const RaftWindow& w : shape.windows) cut.push_back(windowAt(w, base_inset, wh, zm));
            // (Their mouths are left as cut: rounding them layer by layer took slivers off the low layers that
            // the higher ones, leaning out, kept, so those started in mid-air.)
            layer = difference(layer, unionNonZero(cut));
        }
        layer = tidy(layer);
        if (k == 0) shape.base = layer;
        shape.volume += std::abs(area(layer)) * dz;
        if (m) appendLayer(*m, layer, z0, dz);
    }
    shape.height = top_z;
    shape.pieces = static_cast<int>(toExPolygons(shape.base).size());
    if (m && s.rounded)
        for (const Pillar& P : tree.pillars) {
            if (P.on_model || P.merged) continue;
            const double base = P.base_diameter > 0 ? std::max(P.base_diameter, P.diameter) : tc.base_diameter;
            appendFootFillet(*m, P.xy, H, base / 2, P.diameter / 2, P.bottom - H, 0.4, q);
        }
    return shape;
}

}  // namespace

double windowSpacing(RaftWindows w) {
    switch (w) {
        case RaftWindows::Few: return 30.0;
        case RaftWindows::Some: return 18.0;
        case RaftWindows::EveryTip: return 0.0;
        default: return -1.0;
    }
}

double sideInset(double height, double angle_deg) {
    if (near(angle_deg, 90.0, 1e-9)) return 0.0;
    return round9(height / std::tan(deg2rad(angle_deg)));
}

double lipWidth(double lip_height) { return lip_height > 0 ? std::max(0.6, 1.25 * lip_height) : 0.0; }

double windowHeight(const RaftSpec& s) {
    return s.window_pct / 100.0 * (s.kind == RaftKind::Pads ? s.frame_height : s.thickness);
}

RaftSpec leastResinSpec() { return RaftSpec{}; }

RaftSpec plainBaseSpec() {
    RaftSpec s;
    s.kind = RaftKind::Full;
    s.reach = 0.8;
    s.thickness = 1.0;
    s.side_angle = 135;
    return s;
}

const std::vector<RaftPreset>& builtInRaftPresets() {
    static const std::vector<RaftPreset> v = [] {
        std::vector<RaftPreset> p;
        RaftSpec e;
        e.kind = RaftKind::Full;
        e.reach = 1.8;
        e.thickness = 2.0;
        e.side_angle = 60;
        e.rounded = true;
        e.lip_height = 0.8;
        e.windows = RaftWindows::Some;
        e.window_pct = 60;
        p.push_back({"Easy release", "Tapered side, rounded lip, windows", e});
        RaftSpec h = e;
        h.reach = 3.0;
        h.side_angle = 75;
        h.windows = RaftWindows::Few;
        h.window_pct = 50;
        p.push_back({"Strong hold", "Steeper side, wider reach, few windows", h});
        p.push_back({"Plain base", "FruitSlicer's first Full raft", plainBaseSpec()});
        p.push_back({"Least resin", "A pad under each trunk, joined by bars", leastResinSpec()});
        RaftSpec f;
        f.kind = RaftKind::Pads;
        f.reach = 0.8;
        f.side_angle = kFramedPadsSideAngle;
        f.rounded = true;
        f.lip_height = 0.8;
        f.windows = RaftWindows::Some;
        f.window_pct = 60;
        f.bar_width = 2.0;
        f.bar_pct = 100;
        f.join = PadJoin::Grid;
        f.frame = true;
        p.push_back({"Pads with frame", "Pads inside a frame with windows", f});
        return p;
    }();
    return v;
}

const RaftPreset* builtInRaftPreset(const std::string& name) {
    for (const RaftPreset& p : builtInRaftPresets())
        if (p.name == name) return &p;
    return nullptr;
}

json raftSpecToJson(const RaftSpec& s) {
    return json{{"kind", kindName(s.kind)},
                {"reach", s.reach},
                {"thickness", s.thickness},
                {"side_angle", s.side_angle},
                {"rounded", s.rounded},
                {"gap_fill", s.gap_fill},
                {"lip_height", s.lip_height},
                {"windows", windowsName(s.windows)},
                {"window_pct", s.window_pct},
                {"bar_width", s.bar_width},
                {"bar_pct", s.bar_pct},
                {"join", s.join == PadJoin::Grid ? "grid" : "fewest"},
                {"frame", s.frame},
                {"frame_width", s.frame_width},
                {"frame_gap", s.frame_gap},
                {"frame_height", s.frame_height},
                {"frame_angle", s.frame_angle}};
}

RaftSpec raftSpecFromJson(const json& j) {
    RaftSpec s;
    if (!j.is_object()) return s;
    auto num = [&](const char* k, double& v) {
        if (auto it = j.find(k); it != j.end() && it->is_number()) v = it->get<double>();
    };
    auto flag = [&](const char* k, bool& v) {
        if (auto it = j.find(k); it != j.end() && it->is_boolean()) v = it->get<bool>();
    };
    auto word = [&](const char* k) -> std::string {
        if (auto it = j.find(k); it != j.end() && it->is_string()) return it->get<std::string>();
        return {};
    };
    if (word("kind") == "full") s.kind = RaftKind::Full;
    num("reach", s.reach);
    num("thickness", s.thickness);
    num("side_angle", s.side_angle);
    flag("rounded", s.rounded);
    num("gap_fill", s.gap_fill);
    num("lip_height", s.lip_height);
    const std::string w = word("windows");
    s.windows = w == "few" ? RaftWindows::Few : w == "some" ? RaftWindows::Some : w == "every_tip" ? RaftWindows::EveryTip
                                                                                               : RaftWindows::None;
    num("window_pct", s.window_pct);
    num("bar_width", s.bar_width);
    num("bar_pct", s.bar_pct);
    if (word("join") == "grid") s.join = PadJoin::Grid;
    flag("frame", s.frame);
    num("frame_width", s.frame_width);
    num("frame_gap", s.frame_gap);
    num("frame_height", s.frame_height);
    num("frame_angle", s.frame_angle);
    return s;
}

RaftSpec legacyRaftSpec(bool full, const TreeConfig& tc) {
    if (!full) {
        RaftSpec s = leastResinSpec();
        s.thickness = tc.raft_thickness;
        s.reach = round9(tc.pad_brim - 0.4);
        // The old pads were 0.4 mm wider at the plate, whatever their thickness.
        if (!near(s.thickness, 1.0, 1e-12)) s.side_angle = 180.0 - std::atan(s.thickness / 0.4) * 180.0 / kPi;
        s.bar_width = tc.link_width;
        s.gap_fill = tc.full_raft_gap;
        return s;
    }
    RaftSpec s = plainBaseSpec();
    s.thickness = tc.raft_thickness;
    s.gap_fill = tc.full_raft_gap;
    s.bar_width = tc.link_width;  // where the slab's pieces are too far apart to fill, bars join them
    const double past = tc.pad_brim + tc.full_raft_margin;   // at the plate
    const double in = std::max(0.0, std::min(s.thickness, past));
    s.side_angle = near(in, s.thickness, 1e-12) ? 135.0 : 180.0 - std::atan(s.thickness / std::max(in, 1e-6)) * 180.0 / kPi;
    s.reach = round9(past - in);
    return s;
}

void applyRaftSpec(const RaftSpec& s, TreeConfig& tc) {
    tc.raft_spec.reset();
    if (tc.raft == RaftStyle::None) return;
    tc.raft_thickness = s.thickness;
    tc.full_raft_gap = s.gap_fill;
    tc.link_width = s.bar_width;
    const double in = sideInset(s.thickness, s.side_angle);
    if (s.kind == RaftKind::Pads && !s.rounded && !s.frame && s.join == PadJoin::Fewest && near(s.bar_pct, 80.0) &&
        near(in, -0.4)) {
        tc.raft = RaftStyle::Pads;
        tc.pad_brim = round9(s.reach + 0.4);
        return;
    }
    if (s.kind == RaftKind::Full && !s.rounded && s.lip_height <= 0 && s.windows == RaftWindows::None &&
        near(-in, s.thickness)) {
        tc.raft = RaftStyle::Full;
        tc.full_raft_margin = round9(s.reach + s.thickness - tc.pad_brim);
        return;
    }
    tc.raft = s.kind == RaftKind::Full ? RaftStyle::Full : RaftStyle::Pads;
    tc.raft_spec = s;
}

double raftPastFoot(const TreeConfig& tc) {
    if (!tc.raft_spec) return tc.pad_brim + (tc.raft == RaftStyle::Full ? tc.full_raft_margin : 0.0);
    const RaftSpec& s = *tc.raft_spec;
    if (s.kind == RaftKind::Pads && s.frame)
        return s.reach + s.frame_gap + s.frame_width + std::max(0.0, -sideInset(s.frame_height, s.frame_angle));
    return s.reach + std::max(0.0, -sideInset(s.thickness, s.side_angle));
}

std::vector<Vec2> raftEdgeProfile(const RaftSpec& s, double height) {
    const bool frame = s.kind == RaftKind::Pads && s.frame;
    if (s.kind == RaftKind::Pads && !frame)
        return filleted({{sideInset(height, s.side_angle), 0}, {0, height}, {kFarEdge, height}},
                        {0, s.rounded ? 0.25 : 0.0, 0});
    const double h = frame ? s.frame_height : height;
    const double in = sideInset(h, frame ? s.frame_angle : s.side_angle);
    const double lip = s.lip_height, lw = lipWidth(lip);
    const double r_edge = s.rounded ? (lip > 0 ? 0.3 : 0.4) : 0.0;
    const double rt = s.rounded ? std::min(lw / 2, 0.6 * lip) : 0.0;
    const double ri = s.rounded ? std::min(0.3, 0.35 * lip) : 0.0;
    std::vector<Vec2> v{{in, 0}, {0, h}};
    std::vector<double> r{0, r_edge};
    if (lip > 0) {
        v.insert(v.end(), {{0, h + lip}, {lw, h + lip}, {lw, h}});
        r.insert(r.end(), {rt, rt, ri});
    }
    if (frame) {
        v.insert(v.end(), {{s.frame_width, h}, {s.frame_width, 0}});
        r.insert(r.end(), {s.rounded ? 0.3 : 0.0, 0});
    } else {
        v.push_back({kFarEdge, h});
        r.push_back(0);
    }
    return filleted(v, r);
}

void appendSpecRaft(TriangleMesh& m, const SupportTree& tree, const TreeConfig& tc, RaftShape* shape) {
    RaftShape s = specRaft(&m, tree, tc);
    if (shape) *shape = std::move(s);
}

RaftShape specRaftShape(const SupportTree& tree, const TreeConfig& tc) { return specRaft(nullptr, tree, tc); }

}  // namespace fruit
