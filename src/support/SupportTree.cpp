#include "support/SupportTree.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>

#include "mesh/Primitives.h"

namespace fruit {

namespace {

double shaftFor(double tip_d) { return tip_d <= 0.3 ? 0.8 : (tip_d <= 0.45 ? 1.0 : 1.2); }
double penetrationFor(double tip_d) { return std::clamp(0.5 * tip_d, 0.08, 0.2); }

class Builder {
public:
    Builder(const TriangleMesh& m, const TreeConfig& c, double min_raft)
        : mesh_(m), cfg_(c), tree_(m) {
        out_.raft_top = cfg_.raft == RaftStyle::None ? 0.0 : std::max(cfg_.raft_thickness, min_raft);
    }

    SupportTree run(const std::vector<SupportPoint>& pts);

private:
    // Distance-based clearance along a segment (radius r around it).
    bool segmentClear(const Vec3& a, const Vec3& b, double r, double skip_start = 0) const {
        const double len = distance(a, b);
        const int n = std::max(2, static_cast<int>(std::ceil(len / 0.25)));
        for (int i = 0; i <= n; ++i) {
            const double t = static_cast<double>(i) / n;
            if (t * len < skip_start) continue;
            const Vec3 q = a + (b - a) * t;
            if (tree_.closest(q).distance < r + cfg_.clearance) return false;
        }
        return true;
    }
    bool dropClear(const Vec3& from, double r) const {
        const double bottom = out_.raft_top;
        if (from.z <= bottom) return true;
        if (tree_.anyHit(from, {0, 0, -1}, from.z - bottom)) return false;
        const double rr = r + cfg_.clearance;
        for (int k = 0; k < 4; ++k) {
            const double a = kPi / 2 * k + kPi / 4;
            const Vec3 o = from + Vec3{std::cos(a) * rr, std::sin(a) * rr, 0};
            if (tree_.anyHit(o, {0, 0, -1}, from.z - bottom)) return false;
        }
        return true;
    }
    bool headClear(const Vec3& contact, const Vec3& dir, const PinheadSpec& s) const {
        // Rays along the cone surface, starting just outside the part: they
        // see walls and neighbouring features but not the surface the tip
        // sits on (which a distance test would always report as a hit).
        const Vec3 front = contact + dir * (s.tip_diameter / 2 - s.penetration);
        const Vec3 j = front + dir * (s.tip_diameter / 2 + s.head_length + s.shaft_diameter / 2);
        const Vec3 ref = std::abs(dir.z) < 0.9 ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
        const Vec3 u = normalized(cross(ref, dir)), v = cross(dir, u);
        const Vec3 start = contact + dir * (s.tip_diameter / 2 + 0.05);
        const double r0 = s.tip_diameter / 2, r1 = s.shaft_diameter / 2 + cfg_.clearance;
        for (int k = -1; k < 8; ++k) {
            Vec3 off0{}, off1{};
            if (k >= 0) {
                const double a = 2 * kPi * k / 8;
                const Vec3 radial = u * std::cos(a) + v * std::sin(a);
                off0 = radial * r0;
                off1 = radial * r1;
            }
            const Vec3 a = start + off0, b = j + off1;
            const Vec3 d = b - a;
            if (tree_.anyHit(a, d, norm(d))) return false;
        }
        return tree_.closest(j).distance >= s.shaft_diameter / 2 + cfg_.clearance;
    }
    // Walk down at 45° from `from` in 16 directions until a clear vertical
    // drop to the raft is found (PrusaSlicer-style ground route search).
    bool groundRoute(const Vec3& from, double r, double max_reach, Vec3& to) const {
        double best = 1e9;
        for (int k = 0; k < 16; ++k) {
            const double az = 2 * kPi * k / 16;
            const Vec3 step{std::cos(az), std::sin(az), -1.0};
            for (double d = 1.0; d <= max_reach && d < best; d += 1.0) {
                const Vec3 q = from + step * d;
                if (q.z < out_.raft_top + cfg_.base_height + 1.0) break;
                if (!segmentClear(from, q, r)) break;
                if (dropClear(q, r)) {
                    best = d;
                    to = q;
                    break;
                }
            }
        }
        return best < 1e9;
    }
    void warn(const std::string& code, const std::string& msg, const Vec3& where) {
        out_.warnings.push_back({code, msg, -1, where});
    }

    bool placeHead(const SupportPoint& p, Head& h, bool& drop_ok);
    bool attachToPillar(int head);
    void newPillar(int head, bool on_model, const Vec3& anchor = {}, const Vec3& anchor_n = {});
    void brace();
    void raft();

    const TriangleMesh& mesh_;
    const TreeConfig& cfg_;
    AabbTree tree_;
    SupportTree out_;
};

bool Builder::placeHead(const SupportPoint& p, Head& h, bool& drop_ok) {
    PinheadSpec s;
    s.tip_diameter = p.tip_diameter;
    s.penetration = penetrationFor(p.tip_diameter);
    s.head_length = cfg_.head_length;
    s.shaft_diameter = shaftFor(p.tip_diameter);
    const Vec3 n = normalized(p.normal);
    const Vec3 preferred = headDirection(n, cfg_.max_head_tilt_deg);

    // Candidate directions: the preferred one first, then a fan sorted by
    // closeness to it. A head must leave the surface (dir · n > cos 80°).
    std::vector<Vec3> cands{preferred};
    for (double tilt : {0.0, 10.0, 20.0, 30.0, 40.0, cfg_.max_head_tilt_deg})
        for (int k = 0; k < 16; ++k) {
            const double az = 2 * kPi * k / 16, t = deg2rad(tilt);
            cands.push_back({std::sin(t) * std::cos(az), std::sin(t) * std::sin(az), -std::cos(t)});
        }
    std::stable_sort(cands.begin() + 1, cands.end(),
                     [&](const Vec3& a, const Vec3& b) { return dot(a, preferred) > dot(b, preferred); });
    const double min_dot = std::cos(deg2rad(80.0));
    int fallback = -1;
    for (size_t i = 0; i < cands.size(); ++i) {
        const Vec3& d = cands[i];
        if (dot(d, n) < min_dot) continue;
        for (double len : {cfg_.head_length, cfg_.head_length * 0.6}) {
            PinheadSpec si = s;
            si.head_length = len;
            if (!headClear(p.pos, d, si)) continue;
            const Vec3 front = p.pos + d * (si.tip_diameter / 2 - si.penetration);
            const Vec3 j = front + d * (si.tip_diameter / 2 + si.head_length + si.shaft_diameter / 2);
            if (dropClear(j, si.shaft_diameter / 2)) {
                h = Head{-1, p.pos, d, j, si};
                drop_ok = true;
                return true;
            }
            if (fallback < 0) {
                fallback = static_cast<int>(i);
                h = Head{-1, p.pos, d, j, si};
            }
        }
    }
    drop_ok = false;
    return fallback >= 0;
}

bool Builder::attachToPillar(int hi) {
    Head& h = out_.heads[hi];
    int best = -1;
    double best_d = 1e9;
    Vec3 best_attach;
    for (size_t pi = 0; pi < out_.pillars.size(); ++pi) {
        Pillar& P = out_.pillars[pi];
        if (P.on_model || P.bridges >= cfg_.max_heads_per_pillar - 1) continue;
        const double d = distance(P.xy, h.junction.xy());
        if (d > cfg_.max_bridge_length || d >= best_d) continue;
        // 45° descent from the junction to the pillar axis.
        const Vec3 attach{P.xy.x, P.xy.y, h.junction.z - d};
        if (attach.z < out_.raft_top + cfg_.base_height + 1.0) continue;
        if (!segmentClear(h.junction, attach, h.spec.shaft_diameter / 2)) continue;
        // Extending the pillar upwards must also be clear.
        if (attach.z > P.top && !segmentClear({P.xy.x, P.xy.y, P.top}, attach, P.diameter / 2)) continue;
        best = static_cast<int>(pi);
        best_d = d;
        best_attach = attach;
    }
    if (best < 0) return false;
    Pillar& P = out_.pillars[best];
    out_.bridges.push_back({h.junction, best_attach, h.spec.shaft_diameter});
    P.top = std::max(P.top, best_attach.z);
    P.heads.push_back(hi);
    ++P.bridges;
    P.diameter = std::max(P.diameter, h.spec.shaft_diameter) + 0.1;
    return true;
}

void Builder::newPillar(int hi, bool on_model, const Vec3& anchor, const Vec3& anchor_n) {
    const Head& h = out_.heads[hi];
    Pillar P;
    P.xy = h.junction.xy();
    P.top = h.junction.z;
    P.diameter = h.spec.shaft_diameter;
    P.heads.push_back(hi);
    P.on_model = on_model;
    if (on_model) {
        P.anchor_contact = anchor;
        P.anchor_normal = anchor_n;
        P.bottom = anchor.z + 1.0 + P.diameter / 2;  // small reversed pinhead below
    } else {
        P.bottom = out_.raft_top + cfg_.base_height;
    }
    out_.pillars.push_back(P);
}

void Builder::brace() {
    // Minimum spanning tree over raft-standing pillars (Prim, O(n²)).
    std::vector<int> idx;
    for (size_t i = 0; i < out_.pillars.size(); ++i)
        if (!out_.pillars[i].on_model) idx.push_back(static_cast<int>(i));
    const size_t n = idx.size();
    if (n < 2) return;
    std::vector<double> best(n, 1e300);
    std::vector<int> parent(n, -1);
    std::vector<char> in(n, 0);
    best[0] = 0;
    for (size_t it = 0; it < n; ++it) {
        int u = -1;
        for (size_t i = 0; i < n; ++i)
            if (!in[i] && (u < 0 || best[i] < best[u])) u = static_cast<int>(i);
        in[u] = 1;
        for (size_t v = 0; v < n; ++v) {
            if (in[v]) continue;
            const double d = distance(out_.pillars[idx[u]].xy, out_.pillars[idx[v]].xy);
            if (d < best[v]) { best[v] = d; parent[v] = u; }
        }
    }
    for (size_t v = 1; v < n; ++v) {
        if (parent[v] < 0 || best[v] > cfg_.brace_max_distance || best[v] < 1e-6) continue;
        const Pillar& A = out_.pillars[idx[parent[v]]];
        const Pillar& B = out_.pillars[idx[v]];
        const double h = std::min(A.top, B.top) - cfg_.flex_zone;
        const double z0 = out_.raft_top + cfg_.base_height + 1.0;
        if (std::max(A.top, B.top) - out_.raft_top < cfg_.brace_min_height || h - z0 < best[v]) continue;
        // Zig-zag at 45°, levels at least `brace_step` apart.
        bool from_a = true;
        for (double z = z0; z + best[v] <= h; z += std::max(best[v], cfg_.brace_step)) {
            const Vec3 a{(from_a ? A : B).xy.x, (from_a ? A : B).xy.y, z};
            const Vec3 b{(from_a ? B : A).xy.x, (from_a ? B : A).xy.y, z + best[v]};
            if (segmentClear(a, b, cfg_.brace_diameter / 2)) out_.braces.push_back({a, b, cfg_.brace_diameter});
            from_a = !from_a;
        }
    }
}

void Builder::raft() {
    if (cfg_.raft == RaftStyle::None) return;
    // Join pads along a minimum spanning tree so the raft is one piece.
    std::vector<int> idx;
    for (size_t i = 0; i < out_.pillars.size(); ++i)
        if (!out_.pillars[i].on_model) idx.push_back(static_cast<int>(i));
    const size_t n = idx.size();
    if (n < 2) return;
    std::vector<double> best(n, 1e300);
    std::vector<int> parent(n, -1);
    std::vector<char> in(n, 0);
    best[0] = 0;
    for (size_t it = 0; it < n; ++it) {
        int u = -1;
        for (size_t i = 0; i < n; ++i)
            if (!in[i] && (u < 0 || best[i] < best[u])) u = static_cast<int>(i);
        in[u] = 1;
        for (size_t v = 0; v < n; ++v) {
            if (in[v]) continue;
            const double d = distance(out_.pillars[idx[u]].xy, out_.pillars[idx[v]].xy);
            if (d < best[v]) { best[v] = d; parent[v] = u; }
        }
    }
    const double zc = out_.raft_top / 2;
    for (size_t v = 1; v < n; ++v) {
        const Vec2 a = out_.pillars[idx[parent[v]]].xy, b = out_.pillars[idx[v]].xy;
        out_.raft_links.push_back({{a.x, a.y, zc}, {b.x, b.y, zc}, out_.raft_top * 0.9});
    }
}

SupportTree Builder::run(const std::vector<SupportPoint>& pts) {
    // Highest points first: their pillars are the longest and lower heads
    // bridge into them.
    std::vector<int> order(pts.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return pts[a].pos.z > pts[b].pos.z; });
    for (int pi : order) {
        const SupportPoint& p = pts[pi];
        Head h;
        bool drop_ok = false;
        if (!placeHead(p, h, drop_ok)) {
            ++out_.dropped_points;
            warn("head_blocked", "no collision-free head direction; point dropped", p.pos);
            continue;
        }
        h.point = pi;
        const int hi = static_cast<int>(out_.heads.size());
        out_.heads.push_back(h);
        if (attachToPillar(hi)) continue;
        if (drop_ok) {
            newPillar(hi, false);
            continue;
        }
        Vec3 route_end;
        if (groundRoute(h.junction, h.spec.shaft_diameter / 2, cfg_.max_route_length, route_end)) {
            // Diagonal bridge to a new pillar standing where the drop is clear.
            out_.bridges.push_back({h.junction, route_end, h.spec.shaft_diameter});
            Pillar P;
            P.xy = route_end.xy();
            P.top = route_end.z;
            P.bottom = out_.raft_top + cfg_.base_height;
            P.diameter = h.spec.shaft_diameter;
            P.heads.push_back(hi);
            P.bridges = 1;
            out_.pillars.push_back(P);
            continue;
        }
        // Stand on the model below, if the drop is short enough.
        auto hit = tree_.raycast(h.junction, {0, 0, -1}, 1e9);
        if (hit && h.junction.z - hit->point.z <= cfg_.max_model_anchor_drop &&
            h.junction.z - hit->point.z > 1.0) {
            newPillar(hi, true, hit->point, mesh_.faceNormal(hit->face));
            std::ostringstream m;
            m << "support stands on the model (drop " << h.junction.z - hit->point.z << " mm); leaves a mark on an upper surface";
            warn("anchor_on_model", m.str(), hit->point);
            continue;
        }
        out_.heads.pop_back();
        ++out_.dropped_points;
        warn("no_route", "head has no clear path to the raft or a short drop onto the model; point dropped", p.pos);
    }
    brace();
    raft();
    return std::move(out_);
}

}  // namespace

TriangleMesh SupportTree::mesh(const TreeConfig& cfg) const {
    TriangleMesh m;
    const GeometryQuality& q = cfg.quality;
    for (const Head& h : heads) appendPinhead(m, h.contact, h.dir, h.spec, q);
    for (const Rod& r : bridges) {
        appendFrustum(m, r.a, r.diameter / 2, r.b, r.diameter / 2, q.segments(r.diameter / 2));
        appendSphere(m, r.b, r.diameter / 2, q.segments(r.diameter / 2));
    }
    for (const Pillar& P : pillars) {
        const Vec3 top{P.xy.x, P.xy.y, P.top}, bottom{P.xy.x, P.xy.y, P.bottom};
        appendShaft(m, top, bottom, P.diameter, q, true);
        if (P.on_model) {
            // Reversed pinhead: thin contact sinking 0.1 mm into the surface below.
            PinheadSpec a;
            a.tip_diameter = 0.4;
            a.penetration = 0.1;
            // Junction of the reversed head lands on the pillar bottom.
            a.head_length = std::max(0.1, P.bottom - P.anchor_contact.z - (a.tip_diameter - a.penetration) - P.diameter / 2);
            a.shaft_diameter = P.diameter;
            appendPinhead(m, P.anchor_contact, {0, 0, 1}, a, q);
        } else {
            appendBaseCone(m, bottom, P.diameter, cfg.base_diameter, raft_top - 0.1, q);
            if (cfg.raft == RaftStyle::Pads) {
                const double r = cfg.base_diameter / 2 + cfg.pad_brim;
                appendFrustum(m, {P.xy.x, P.xy.y, 0}, r, {P.xy.x, P.xy.y, raft_top}, std::max(0.5, r - 0.4),
                              q.segments(r));
            }
        }
    }
    for (const Rod& r : braces) appendFrustum(m, r.a, r.diameter / 2, r.b, r.diameter / 2, q.segments(r.diameter / 2));
    // Raft links: flat bars from the plate up, so the raft's first layers are
    // already one piece.
    for (const Rod& r : raft_links) {
        const Vec2 a = r.a.xy(), b = r.b.xy();
        const Vec2 d = normalized(b - a);
        const Vec2 n{-d.y * cfg.link_width / 2, d.x * cfg.link_width / 2};
        const double z1 = raft_top * 0.8;
        const Vec3 v[8] = {{a.x - n.x, a.y - n.y, 0},  {b.x - n.x, b.y - n.y, 0},  {b.x + n.x, b.y + n.y, 0},
                           {a.x + n.x, a.y + n.y, 0},  {a.x - n.x, a.y - n.y, z1}, {b.x - n.x, b.y - n.y, z1},
                           {b.x + n.x, b.y + n.y, z1}, {a.x + n.x, a.y + n.y, z1}};
        appendHexahedron(m, v);
    }
    return m;
}

SupportTree buildSupportTree(const TriangleMesh& model, const std::vector<SupportPoint>& points, const TreeConfig& cfg,
                             double min_raft_thickness) {
    Builder b(model, cfg, min_raft_thickness);
    return b.run(points);
}

}  // namespace fruit
