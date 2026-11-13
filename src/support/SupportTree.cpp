// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "support/SupportTree.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>

#include "mesh/Primitives.h"
#include "ops/MeshOps.h"

namespace fruit {

namespace {

double shaftFor(double tip_d) { return tip_d <= 0.3 ? 0.8 : (tip_d <= 0.45 ? 1.0 : 1.2); }
double penetrationFor(double tip_d, const TreeConfig& cfg) {
    if (cfg.penetration_table.empty()) return std::clamp(0.5 * tip_d, 0.08, 0.2);
    // Nearest measured diameter.
    const auto* best = &cfg.penetration_table.front();
    for (const auto& e : cfg.penetration_table)
        if (std::abs(e.first - tip_d) < std::abs(best->first - tip_d)) best = &e;
    return best->second;
}

// The lowest point of a head as appendPinhead draws it: the tip ball or the
// joint ball at the junction.
double headBottom(const Head& h) {
    const Vec3 d = normalized(h.dir);
    const double r_tip = h.spec.tip_diameter / 2, r_back = h.spec.shaft_diameter / 2;
    const Vec3 front = h.contact + d * (r_tip - h.spec.penetration);
    return std::min(front.z - r_tip, h.junction.z - r_back);
}

// A head that stands on the plate (Head::on_plate), along its own
// direction: the tip ball, kept above the plate, then the cone down to where
// its slanted end just meets the plate, and an upright foot under that end.
// Draws it into `m` when given; returns the foot's middle, on the plate.
struct PlateHeadShape {
    Vec3 d;            // its direction (unit)
    Vec3 front, end;   // the tip ball's centre; where the cone ends above its foot
    double r_tip = 0, r_end = 0;
};
PlateHeadShape plateHeadShape(const Head& h) {
    PlateHeadShape s;
    s.d = normalized(h.dir);  // heads point down, within the tilt limit
    const double down = std::max(-s.d.z, 1e-6), slant = std::sqrt(std::max(0.0, 1 - down * down));
    const double r_back = h.spec.shaft_diameter / 2;
    s.r_tip = h.spec.tip_diameter / 2;
    s.front = h.contact + s.d * (s.r_tip - h.spec.penetration);
    if (s.front.z < s.r_tip) s.front = s.front - s.d * ((s.r_tip - s.front.z) / down);  // deeper into the part, not the plate
    s.end = s.front;
    s.r_end = s.r_tip;
    const double end_z = r_back * slant;  // the slanted end's lowest edge on the plate
    if (s.front.z > end_z) {
        s.end = s.front + s.d * ((s.front.z - end_z) / down);
        s.r_end = r_back;
    }
    return s;
}
Vec3 plateHead(const Head& h, TriangleMesh* m, const GeometryQuality& q) {
    const PlateHeadShape s = plateHeadShape(h);
    if (m) appendSphere(*m, s.front, s.r_tip, q.segments(s.r_tip));
    if (m && s.r_end > s.r_tip) appendFrustum(*m, s.front, s.r_tip, s.end, s.r_end, q.segments(s.r_end));
    const Vec3 foot{s.end.x, s.end.y, 0};
    if (m && s.end.z > 1e-9) appendFrustum(*m, foot, s.r_end, s.end, s.r_end, q.segments(s.r_end));
    return foot;
}

class Builder {
public:
    Builder(const TriangleMesh& m, const TreeConfig& c, double min_raft)
        : mesh_(m), cfg_(c), tree_(m), first_layers_(std::max(min_raft, 0.5)) {
        out_.raft_top = cfg_.raft == RaftStyle::None ? 0.0 : std::max(cfg_.raft_thickness, min_raft);
    }

    SupportTree run(const std::vector<SupportPoint>& pts, const JobControl& ctl);

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
    // A shaft of radius `r` straight down from `from` keeps its gap to the
    // model all the way to where its foot starts. Rays first (cheap, and they
    // see anything under the shaft down to the raft), then the distance along
    // the shaft itself: four rays miss a wall that passes between them.
    bool shaftClear(const Vec3& from, double r) const {
        const double bottom = out_.raft_top;
        if (from.z <= bottom) return true;
        if (tree_.anyHit(from, {0, 0, -1}, from.z - bottom)) return false;
        const double rr = r + cfg_.clearance;
        for (int k = 0; k < 4; ++k) {
            const double a = kPi / 2 * k + kPi / 4;
            const Vec3 o = from + Vec3{std::cos(a) * rr, std::sin(a) * rr, 0};
            if (tree_.anyHit(o, {0, 0, -1}, from.z - bottom)) return false;
        }
        const Vec3 foot_top{from.x, from.y, bottom + cfg_.base_height};
        return from.z <= foot_top.z || segmentClear(from, foot_top, r);
    }
    // The gap a pillar's foot keeps to the model.
    double footGap() const {
        return cfg_.raft == RaftStyle::None ? std::max(cfg_.clearance, cfg_.foot_clearance) : cfg_.clearance;
    }
    // The widest foot (its diameter where it stands) a pillar of shaft radius
    // `r` can have at `xy` and keep that gap: the tree's base_diameter where
    // there is room, less beside the model, under 2r where not even a straight
    // foot fits. The foot is a cone from that width up to the shaft's.
    double footRoom(const Vec2& xy, double r) const {
        const double z0 = out_.raft_top, h = cfg_.base_height, gap = footGap();
        double d = cfg_.base_diameter;
        for (double z = z0; z < z0 + h - 1e-9; z += 0.05) {
            const double t = (z - z0) / h;
            const double room = tree_.closest({xy.x, xy.y, z}).distance - gap;  // the radius allowed at this height
            d = std::min(d, 2 * (room - r * t) / (1 - t));
        }
        return std::max(0.0, d);
    }
    // A pillar can stand under `from`: its shaft is clear and a foot fits.
    bool dropClear(const Vec3& from, double r) const {
        return shaftClear(from, r) && footRoom(from.xy(), r) >= 2 * r - 1e-9;
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
    // Where a pillar can stand on the model below `from`: straight down, else
    // the nearest spot a 45° walk reaches (as for the raft). Its shaft keeps
    // the gap to the model all the way down, and the face it lands on is no
    // steeper than max_anchor_slope_deg, so the foot sits on it. `top` is the
    // pillar's top; `foot` and `foot_n` are where it meets the model.
    bool modelStand(const Vec3& from, double r, Vec3& top, Vec3& foot, Vec3& foot_n) const {
        if (cfg_.max_model_anchor_drop <= 0) return false;
        const double min_up = std::cos(deg2rad(cfg_.max_anchor_slope_deg));
        auto stands = [&](const Vec3& q) {
            const auto hit = tree_.raycast(q, {0, 0, -1}, 1e9);
            if (!hit) return false;
            // Room for a foot under the shaft; and the limit is on how far below
            // the head the model is, so a walk down can't get around it.
            if (q.z - hit->point.z <= 1.0 || from.z - hit->point.z > cfg_.max_model_anchor_drop) return false;
            const Vec3 n = mesh_.faceNormal(hit->face);
            if (n.z < min_up) return false;
            // The shaft ends a head's length above the model; below that its
            // foot narrows to the contact.
            const Vec3 shaft_end{q.x, q.y, hit->point.z + 1.0 + r};
            if (shaft_end.z < q.z && !segmentClear(q, shaft_end, r)) return false;
            top = q;
            foot = hit->point;
            foot_n = n;
            return true;
        };
        if (stands(from)) return true;
        double best = 1e9;
        Vec3 bt, bf, bn;
        for (int k = 0; k < 16; ++k) {
            const double az = 2 * kPi * k / 16;
            const Vec3 step{std::cos(az), std::sin(az), -1.0};
            for (double d = 1.0; d <= cfg_.max_route_length && d < best; d += 1.0) {
                const Vec3 q = from + step * d;
                if (!segmentClear(from, q, r)) break;
                if (stands(q)) {
                    best = d;
                    bt = top;
                    bf = foot;
                    bn = foot_n;
                    break;
                }
            }
        }
        if (best == 1e9) return false;
        top = bt;
        foot = bf;
        foot_n = bn;
        return true;
    }
    void warn(const std::string& code, const std::string& msg, const Vec3& where) {
        out_.warnings.push_back({code, msg, -1, where});
    }

    // The directions a head may take at a face with outward normal `n`:
    // `first`, then a fan sorted by closeness to it.
    std::vector<Vec3> headDirections(const Vec3& first) const {
        std::vector<Vec3> cands{first};
        for (double tilt : {0.0, 10.0, 20.0, 30.0, 40.0, cfg_.max_head_tilt_deg})
            for (int k = 0; k < 16; ++k) {
                const double az = 2 * kPi * k / 16, t = deg2rad(tilt);
                cands.push_back({std::sin(t) * std::cos(az), std::sin(t) * std::sin(az), -std::cos(t)});
            }
        std::stable_sort(cands.begin() + 1, cands.end(),
                         [&](const Vec3& a, const Vec3& b) { return dot(a, first) > dot(b, first); });
        return cands;
    }
    static Vec3 junctionOf(const Vec3& contact, const Vec3& d, const PinheadSpec& s) {
        const Vec3 front = contact + d * (s.tip_diameter / 2 - s.penetration);
        return front + d * (s.tip_diameter / 2 + s.head_length + s.shaft_diameter / 2);
    }
    // How far to the side the model is from a head that stands on the plate,
    // within the first layers' height, beyond the head's own surface. Sideways
    // only (what is above it is the face it holds), and not the last stretch
    // below its tip, which runs beside that face.
    double plateHeadGap(const Head& h) const {
        const PlateHeadShape s = plateHeadShape(h);
        const double reach = std::max(s.r_end, s.r_tip) + footGap() + 0.05;
        double gap = 1e9;
        auto look = [&](const Vec3& q, double r) {
            for (int k = 0; k < 16; ++k) {
                const double a = 2 * kPi * k / 16;
                const auto hit = tree_.raycast(q, {std::cos(a), std::sin(a), 0}, reach);
                if (hit) gap = std::min(gap, distance(q, hit->point) - r);
            }
        };
        for (double z = 0.02; z <= std::min(s.end.z, first_layers_); z += 0.05) look({s.end.x, s.end.y, z}, s.r_end);
        const double len = distance(s.front, s.end);
        for (double t = len; t >= 0.25; t -= 0.05) {
            const Vec3 q = s.front + s.d * t;
            if (q.z > first_layers_) break;
            look(q, s.r_tip + (s.r_end - s.r_tip) * (len > 0 ? t / len : 1.0));
        }
        return gap;
    }
    void settlePlateHead(const SupportPoint& p, Head& h);

    bool placeHead(const SupportPoint& p, Head& h, bool& drop_ok);
    bool placeStub(const SupportPoint& p, int point);
    // A head for `p` at its own size and what it stands on (a pillar, a
    // bridge to one, a route to the raft, or the model below). False, with
    // nothing kept, when there is none; `why` is the warning's code then.
    bool supportTip(const SupportPoint& p, int pi, const char*& why);
    // The same at the planned size, then smaller ones; then a little way
    // across the same face. True when one of them got a head.
    bool supportSmallerOrMoved(const SupportPoint& p, int pi);
    bool attachToPillar(int head);
    // `head`'s junction lies in the junction of a head that has a way down to
    // the raft (the two are one ball in the print): it shares that junction
    // and that way down. False when there is no such head.
    bool shareJunction(int head);
    // A support that stands on the model within kSameFootSpot of `foot`, or -1.
    int standsOnSpot(const Vec3& foot) const;
    // `head` joins pillar `pillar`, which stands on the model where its own
    // pillar (from `stand` down) would have stood. False when it can't.
    bool joinOnModel(int head, int pillar, const Vec3& stand);
    // `top`: where the pillar starts when that isn't the head's junction (a
    // bridge leads there).
    void newPillar(int head, bool on_model, const Vec3& anchor = {}, const Vec3& anchor_n = {},
                   const Vec3* top = nullptr);
    void brace();
    void braceGroup(bool on_model);
    void raft();
    void mergePillars();

    const TriangleMesh& mesh_;
    const TreeConfig& cfg_;
    AabbTree tree_;
    // How high the long-exposure layers reach (and a little above): what
    // stands on the plate there cures wider than drawn.
    double first_layers_ = 0.5;
    SupportTree out_;
};

bool Builder::placeHead(const SupportPoint& p, Head& h, bool& drop_ok) {
    PinheadSpec s;
    s.tip_diameter = p.tip_diameter;
    s.penetration = penetrationFor(p.tip_diameter, cfg_);
    s.head_length = cfg_.head_length;
    s.shaft_diameter = shaftFor(p.tip_diameter);
    const Vec3 n = normalized(p.normal);
    const Vec3 preferred = headDirection(n, cfg_.max_head_tilt_deg);

    // Candidate directions: the preferred one first, then a fan sorted by
    // closeness to it. A head must leave the surface (dir · n > cos 80°).
    const std::vector<Vec3> cands = headDirections(preferred);
    const double min_dot = std::cos(deg2rad(80.0));
    int fallback = -1;
    for (size_t i = 0; i < cands.size(); ++i) {
        const Vec3& d = cands[i];
        if (dot(d, n) < min_dot) continue;
        for (double len : {cfg_.head_length, cfg_.head_length * 0.6}) {
            PinheadSpec si = s;
            si.head_length = len;
            if (!headClear(p.pos, d, si)) continue;
            const Vec3 j = junctionOf(p.pos, d, si);
            if (shaftClear(j, si.shaft_diameter / 2)) {
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

// A head that stands on the plate itself: like a pillar's foot, it keeps the
// wider gap to the model beside it down where the first layers cure wide. It
// leans away from a wall if another direction has the room, else its base
// narrows, down to the tip's own width; with no room even then it is reported.
void Builder::settlePlateHead(const SupportPoint& p, Head& h) {
    const double need = footGap() - 1e-9;
    double best_gap = plateHeadGap(h);
    if (best_gap >= need) return;
    const Vec3 n = normalized(p.normal);
    const double min_dot = std::cos(deg2rad(80.0));
    Head best = h;
    for (const Vec3& d : headDirections(normalized(h.dir))) {
        if (dot(d, n) < min_dot || !headClear(p.pos, d, h.spec)) continue;
        Head c = h;
        c.dir = d;
        c.junction = junctionOf(p.pos, d, c.spec);
        if (headBottom(c) >= 0) continue;  // would no longer reach the plate itself
        const double g = plateHeadGap(c);
        if (g >= need) {
            h = c;
            return;
        }
        if (g > best_gap) {
            best = c;
            best_gap = g;
        }
    }
    h = best;
    const double tip = h.spec.tip_diameter;
    for (double d = h.spec.shaft_diameter - 0.2; d > tip + 1e-9; d -= 0.2) {
        Head c = h;
        c.spec.shaft_diameter = d;
        if (plateHeadGap(c) >= need) {
            h = c;
            return;
        }
    }
    h.spec.shaft_diameter = tip;  // a straight column
    const double g = plateHeadGap(h);
    if (g < need) {
        std::ostringstream m;
        m << "a tip just above the plate stands " << std::max(0.0, g) << " mm from the model beside it; the first "
          << "layers cure wider than drawn, so it may stick to the model there";
        warn("foot_near_model", m.str(), {h.contact.x, h.contact.y, 0});
    }
}

// No head fits at `p`: when the model is just below, a straight column as wide
// as the tip, sunk into both, holds it from there.
bool Builder::placeStub(const SupportPoint& p, int point) {
    if (cfg_.max_stub_length <= 0) return false;
    const double d = p.tip_diameter, r = d / 2, pen = penetrationFor(d, cfg_);
    const Vec3 start = p.pos - Vec3{0, 0, 0.02};  // just clear of the face the tip touches
    const auto hit = tree_.raycast(start, {0, 0, -1}, cfg_.max_stub_length);
    if (!hit) return false;
    const double gap = p.pos.z - hit->point.z;
    if (gap < 0.05) return false;                          // touching: it prints joined on
    if (mesh_.faceNormal(hit->face).z < 0.2) return false;  // a wall, not a face to stand on
    // Nothing beside the column on its way down.
    for (double t : {0.25, 0.5, 0.75}) {
        const Vec3 q{p.pos.x, p.pos.y, p.pos.z - gap * t};
        for (int k = 0; k < 8; ++k) {
            const double a = 2 * kPi * k / 8;
            if (tree_.anyHit(q, {std::cos(a), std::sin(a), 0}, r + 0.05)) return false;
        }
    }
    Head h;
    h.point = point;
    h.contact = p.pos;
    h.dir = {0, 0, -1};
    h.junction = hit->point;
    h.spec.tip_diameter = d;
    h.spec.penetration = pen;
    h.spec.shaft_diameter = d;
    h.spec.head_length = gap;
    h.stub = true;
    out_.heads.push_back(h);
    out_.stubs.push_back({p.pos + Vec3{0, 0, pen}, hit->point - Vec3{0, 0, pen}, d});
    std::ostringstream m;
    m << "no room for a support head: a " << gap << " mm column from the model just below holds the tip; leaves a "
      << "small mark there";
    warn("anchor_on_model", m.str(), hit->point);
    return true;
}

// Two junctions closer than this (mm) are one ball in the print: a head whose
// junction is that near another's stands in the same place.
constexpr double kSameJunction = 0.25;

bool Builder::shareJunction(int hi) {
    const Head& h = out_.heads[hi];
    int with = -1, on = -1;
    double nearest = kSameJunction;
    for (size_t pi = 0; pi < out_.pillars.size(); ++pi) {
        const Pillar& P = out_.pillars[pi];
        if (P.on_model || P.tips >= cfg_.max_tips_per_trunk) continue;
        for (int g : P.heads) {
            const double d = distance(out_.heads[static_cast<size_t>(g)].junction, h.junction);
            if (g != hi && d < nearest) {
                nearest = d;
                with = g;
                on = static_cast<int>(pi);
            }
        }
    }
    if (with < 0) return false;
    // (That head's way down is known to be clear; this one adds nothing to it but the step across.)
    const Vec3 to = out_.heads[static_cast<size_t>(with)].junction;
    if (nearest > 1e-9) out_.bridges.push_back({h.junction, to, h.spec.shaft_diameter});
    Pillar& P = out_.pillars[static_cast<size_t>(on)];
    P.heads.push_back(hi);
    ++P.tips;
    return true;
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
    P.lowest_attach = std::min(P.lowest_attach, best_attach.z);
    P.heads.push_back(hi);
    ++P.bridges;
    ++P.tips;
    P.diameter = std::max(P.diameter, h.spec.shaft_diameter) + 0.1;
    return true;
}

// Two supports that stand on the model closer than this (mm, foot to foot)
// are one in the print: their feet, 0.4 mm contacts that widen to the shaft
// within 1 mm, fuse just above the surface, and what both hold comes down on
// one spot. (At 1 mm apart each was found to carry its own tip's load.)
constexpr double kSameFootSpot = 0.75;

int Builder::standsOnSpot(const Vec3& foot) const {
    int best = -1;
    double best_d = kSameFootSpot;
    for (size_t pi = 0; pi < out_.pillars.size(); ++pi) {
        const Pillar& P = out_.pillars[pi];
        if (!P.on_model) continue;
        const double d = distance(P.anchor_contact, foot);
        if (d < best_d) {
            best_d = d;
            best = static_cast<int>(pi);
        }
    }
    return best;
}

bool Builder::joinOnModel(int hi, int pi, const Vec3& stand) {
    const Head& h = out_.heads[hi];
    Pillar& P = out_.pillars[pi];
    if (P.tips >= cfg_.max_tips_per_trunk) return false;
    const double r = h.spec.shaft_diameter / 2;
    const double d = distance(stand.xy(), P.xy);
    // Down at 45° onto its shaft. A support too short to have one is head and
    // foot only: then across, to where its shaft would start.
    const Vec3 at{P.xy.x, P.xy.y, std::max(stand.z - d, std::min(stand.z, P.bottom))};
    if (d > 1e-9 && !segmentClear(stand, at, r)) return false;
    const Vec3 shaft_top{P.xy.x, P.xy.y, std::max(P.top, P.bottom)};
    if (at.z > shaft_top.z && !segmentClear(shaft_top, at, P.diameter / 2)) return false;
    if (distance(stand, h.junction) > 1e-9) out_.bridges.push_back({h.junction, stand, h.spec.shaft_diameter});
    if (distance(stand, at) > 1e-9) out_.bridges.push_back({stand, at, h.spec.shaft_diameter});
    P.top = std::max(P.top, at.z);
    P.lowest_attach = std::min(P.lowest_attach, at.z);
    P.heads.push_back(hi);
    ++P.bridges;
    ++P.tips;
    // The foot is pulled as hard as its tips are together: as much contact as they have.
    const double mine = std::max(0.4, h.spec.tip_diameter);
    P.foot_tip = std::sqrt(P.foot_tip * P.foot_tip + mine * mine);
    return true;
}

void Builder::newPillar(int hi, bool on_model, const Vec3& anchor, const Vec3& anchor_n, const Vec3* top) {
    const Head& h = out_.heads[hi];
    const Vec3 t = top ? *top : h.junction;
    Pillar P;
    P.xy = t.xy();
    P.top = t.z;
    P.diameter = h.spec.shaft_diameter;
    P.heads.push_back(hi);
    P.on_model = on_model;
    if (on_model) {
        P.anchor_contact = anchor;
        P.anchor_normal = anchor_n;
        P.bottom = anchor.z + 1.0 + P.diameter / 2;  // a reversed pinhead below
        // The foot is pulled as hard as the tip is: as wide a contact, sunk as deep.
        P.foot_tip = std::max(0.4, h.spec.tip_diameter);
    } else {
        P.bottom = out_.raft_top + cfg_.base_height;
    }
    out_.pillars.push_back(P);
}

void Builder::brace() {
    braceGroup(false);  // the pillars on the raft, with each other
    braceGroup(true);   // those standing on the model, with each other
}

void Builder::braceGroup(bool on_model) {
    // Minimum spanning tree over the group's pillars (Prim, O(n²)).
    std::vector<int> idx;
    for (size_t i = 0; i < out_.pillars.size(); ++i)
        if (out_.pillars[i].on_model == on_model && !out_.pillars[i].merged) idx.push_back(static_cast<int>(i));
    // What a pillar stands on, and where its shaft starts above that.
    auto ground = [&](const Pillar& P) { return P.on_model ? P.anchor_contact.z : out_.raft_top; };
    auto shaftFoot = [&](const Pillar& P) { return P.on_model ? P.bottom : out_.raft_top + cfg_.base_height; };
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
        const double z0 = std::max(shaftFoot(A), shaftFoot(B)) + 1.0;
        if (std::max(A.top - ground(A), B.top - ground(B)) < cfg_.brace_min_height || h - z0 < best[v]) continue;
        // No-trap rule: pillars, brace and what they stand on form a closed
        // loop. If any model material passes between the two pillars below the
        // braces, that loop would encircle it and the supports could not come off.
        bool fenced = false;
        for (double z = std::max(ground(A), ground(B)) + 0.25; z <= h && !fenced; z += 0.25) {
            const Vec3 a{A.xy.x, A.xy.y, z}, b{B.xy.x, B.xy.y, z};
            if (tree_.anyHit(a, b - a, distance(a, b))) fenced = true;
        }
        if (fenced) {
            ++out_.braces_skipped_trap;
            continue;
        }
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

void Builder::mergePillars() {
    if (!cfg_.merge_pillars) return;
    const double z_floor = out_.raft_top + cfg_.base_height + 1.0;
    auto active = [&](const Pillar& P) { return !P.on_model && !P.merged; };
    while (true) {
        // Closest mergeable pair first (deterministic: ties by index).
        int ba = -1, bb = -1;
        double bd = 1e300;
        Vec3 bm;
        double bzs = 0;
        for (size_t i = 0; i < out_.pillars.size(); ++i) {
            const Pillar& A = out_.pillars[i];
            if (!active(A)) continue;
            for (size_t j = i + 1; j < out_.pillars.size(); ++j) {
                const Pillar& B = out_.pillars[j];
                if (!active(B)) continue;
                const double d = distance(A.xy, B.xy);
                if (d > cfg_.max_merge_distance || d >= bd || d < 1e-6) continue;
                if (A.tips + B.tips > cfg_.max_tips_per_trunk) continue;
                // Branches start below the flex zone of the lower pillar and
                // meet at 45°.
                // ... and below every bridge already attached to either shaft.
                const double zs = std::min({std::min(A.top, B.top) - cfg_.flex_zone, A.lowest_attach - 0.5,
                                            B.lowest_attach - 0.5});
                const Vec2 mid = (A.xy + B.xy) * 0.5;
                const Vec3 m{mid.x, mid.y, zs - d / 2};
                if (m.z < z_floor) continue;
                const double r_new = std::min(cfg_.max_trunk_diameter, std::hypot(A.diameter, B.diameter)) / 2;
                if (!segmentClear({A.xy.x, A.xy.y, zs}, m, A.diameter / 2)) continue;
                if (!segmentClear({B.xy.x, B.xy.y, zs}, m, B.diameter / 2)) continue;
                if (!dropClear(m, r_new)) continue;
                ba = static_cast<int>(i);
                bb = static_cast<int>(j);
                bd = d;
                bm = m;
                bzs = zs;
            }
        }
        if (ba < 0) break;
        Pillar& A = out_.pillars[ba];
        Pillar& B = out_.pillars[bb];
        out_.branches.push_back({{A.xy.x, A.xy.y, bzs}, bm, A.diameter});
        out_.branches.push_back({{B.xy.x, B.xy.y, bzs}, bm, B.diameter});
        A.merged = B.merged = true;
        A.bottom = bzs;
        B.bottom = bzs;
        Pillar C;
        C.xy = bm.xy();
        C.top = bm.z;
        C.bottom = out_.raft_top + cfg_.base_height;
        C.diameter = std::min(cfg_.max_trunk_diameter, std::hypot(A.diameter, B.diameter));
        C.base_diameter = std::max(C.diameter, std::min(cfg_.base_diameter, footRoom(C.xy, C.diameter / 2)));
        C.tips = A.tips + B.tips;
        out_.pillars.push_back(C);
    }
}

void Builder::raft() {
    if (cfg_.raft == RaftStyle::None) return;
    // Join pads along a minimum spanning tree so the raft is one piece.
    std::vector<int> idx;
    for (size_t i = 0; i < out_.pillars.size(); ++i)
        if (!out_.pillars[i].on_model && !out_.pillars[i].merged) idx.push_back(static_cast<int>(i));
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

bool Builder::supportTip(const SupportPoint& p, int pi, const char*& why) {
    Head h;
    bool drop_ok = false;
    if (!placeHead(p, h, drop_ok)) {
        why = "head_blocked";
        return false;
    }
    h.point = pi;
    const int hi = static_cast<int>(out_.heads.size());
    out_.heads.push_back(h);
    if (headBottom(h) < 0) {
        // Its head alone would reach through the plate, let alone a
        // pillar under it: it stands on the plate.
        Head& ph = out_.heads.back();
        ph.on_plate = true;
        settlePlateHead(p, ph);
        ph.junction = plateHead(ph, nullptr, cfg_.quality);
        return true;
    }
    if (attachToPillar(hi)) return true;
    // Straight down, on a foot as wide as the room beside the model allows.
    const double shaft_r = h.spec.shaft_diameter / 2;
    const double room = drop_ok ? footRoom(h.junction.xy(), shaft_r) : 0;
    if (drop_ok && room >= 2 * shaft_r - 1e-9) {
        newPillar(hi, false);
        out_.pillars.back().base_diameter = std::min(cfg_.base_diameter, room);
        return true;
    }
    Vec3 route_end;
    if (groundRoute(h.junction, shaft_r, cfg_.max_route_length, route_end)) {
        // Diagonal bridge to a new pillar standing where the drop is clear.
        out_.bridges.push_back({h.junction, route_end, h.spec.shaft_diameter});
        Pillar P;
        P.xy = route_end.xy();
        P.top = route_end.z;
        P.bottom = out_.raft_top + cfg_.base_height;
        P.diameter = h.spec.shaft_diameter;
        P.base_diameter = std::min(cfg_.base_diameter, footRoom(P.xy, shaft_r));
        P.heads.push_back(hi);
        P.bridges = 1;
        // The bridge lands on its top: a later head may raise the top, and a
        // merge into a trunk must still start below where this bridge lands.
        P.lowest_attach = route_end.z;
        out_.pillars.push_back(P);
        return true;
    }
    if (drop_ok) {
        // Its shaft is clear but nowhere in reach has room for a foot (the
        // model is right beside it down at the plate): a straight foot, at
        // the shaft's own gap, and it is reported.
        newPillar(hi, false);
        out_.pillars.back().base_diameter = h.spec.shaft_diameter;
        std::ostringstream m;
        m << "support's foot stands " << room / 2 + footGap() - shaft_r << " mm or less from the model on the "
          << "plate; the first layers cure wider than drawn, so it may stick to the model there";
        warn("foot_near_model", m.str(), {h.junction.x, h.junction.y, out_.raft_top});
        return true;
    }
    // Its junction lies in the junction of a head that has a way down: a
    // support standing on the model here would print fused to that one, and
    // being the shorter and stiffer of the two it would be pulled by both
    // tips on a foot made for one. It goes down the same way instead.
    if (shareJunction(hi)) return true;
    // Stand on the model below: straight down, or on the nearest spot a
    // short walk reaches where the shaft clears the model.
    Vec3 stand, foot, foot_n;
    if (modelStand(h.junction, h.spec.shaft_diameter / 2, stand, foot, foot_n)) {
        // Where another support already stands on that spot, this tip joins
        // it: two feet on one spot are one foot, and it would carry both
        // tips on a contact made for one.
        if (const int there = standsOnSpot(foot); there >= 0 && joinOnModel(hi, there, stand)) return true;
        const bool walked = distance(stand, h.junction) > 1e-9;
        if (walked) out_.bridges.push_back({h.junction, stand, h.spec.shaft_diameter});
        newPillar(hi, true, foot, foot_n, &stand);
        if (walked) out_.pillars.back().bridges = 1;
        std::ostringstream m;
        m << "support stands on the model (drop " << stand.z - foot.z << " mm); leaves a mark on an upper surface";
        warn("anchor_on_model", m.str(), foot);
        return true;
    }
    out_.heads.pop_back();
    why = "no_route";
    return false;
}

bool Builder::supportSmallerOrMoved(const SupportPoint& p, int pi) {
    // A smaller head is narrower, and so is its shaft: it may find a way down
    // where the planned size found none (beside a wall, inside a cup). Better
    // a smaller tip than none: the planner counted on one here.
    // The planned size (but not where it was planned: tried already), then 0.4 and 0.3 mm below it.
    std::vector<double> sizes{p.tip_diameter};
    for (const double d : {0.4, 0.3})
        if (d < sizes.back() - 1e-9) sizes.push_back(d);
    const auto atSizes = [&](const SupportPoint& at, double moved) {
        for (const double d : sizes) {
            if (moved == 0 && d == p.tip_diameter) continue;
            SupportPoint q = at;
            q.tip_diameter = d;
            const char* w = nullptr;
            if (!supportTip(q, pi, w)) continue;
            Head& h = out_.heads.back();
            h.smaller = d < p.tip_diameter - 1e-9;
            h.moved = moved;
            out_.smaller_heads += h.smaller;
            out_.moved_heads += moved > 0;
            return true;
        }
        return false;
    };
    if (atSizes(p, 0)) return true;
    // Then the same face a little way off, nearest first: within a tip's
    // reach of where it was planned, so it still holds what it was for.
    const Vec3 n = normalized(p.normal);
    const Vec3 a = normalized(std::abs(n.z) < 0.9 ? cross(n, Vec3{0, 0, 1}) : cross(n, Vec3{1, 0, 0}));
    const Vec3 b = cross(n, a);
    for (const double r : {0.5, 1.0, 1.5})
        for (int k = 0; k < 8; ++k) {
            const double ang = k * 3.14159265358979323846 / 4;
            const Vec3 off = (a * std::cos(ang) + b * std::sin(ang)) * r;
            const auto hit = tree_.raycast(p.pos + off + n * 0.6, n * -1.0, 1.2);
            if (!hit) continue;
            const Vec3 hn = mesh_.faceNormal(static_cast<size_t>(hit->face));
            // The same face, still facing down.
            if (dot(hn, n) < 0.8 || hn.z > -0.2) continue;
            SupportPoint q = p;
            q.pos = hit->point;
            q.normal = hn;
            if (atSizes(q, distance(q.pos, p.pos))) return true;
        }
    return false;
}

SupportTree Builder::run(const std::vector<SupportPoint>& pts, const JobControl& ctl) {
    // Highest points first: their pillars are the longest and lower heads
    // bridge into them.
    std::vector<int> order(pts.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return pts[a].pos.z > pts[b].pos.z; });
    const double np = static_cast<double>(std::max<size_t>(order.size(), 1));
    size_t done = 0;
    for (int pi : order) {
        if ((done++ & 15) == 0) ctl.step("tree", 0.7 * static_cast<double>(done) / np);
        const SupportPoint& p = pts[pi];
        const char* why = nullptr;
        if (supportTip(p, pi, why)) continue;
        if (supportSmallerOrMoved(p, pi)) continue;
        // No head at all: a short column to the model just below, if there is one.
        if (std::string(why) == "head_blocked" && placeStub(p, pi)) continue;
        ++out_.dropped_points;
        if (std::string(why) == "head_blocked") warn("head_blocked", "no collision-free head direction; point dropped", p.pos);
        else warn("no_route", "head has no clear path to the raft or a short drop onto the model; point dropped", p.pos);
    }
    ctl.step("tree", 0.7);
    mergePillars();
    ctl.step("tree", 0.85);
    brace();
    ctl.step("tree", 0.95);
    raft();
    ctl.report("tree", 1);
    return std::move(out_);
}

}  // namespace

double tipPenetration(double tip_d, const TreeConfig& cfg) { return penetrationFor(tip_d, cfg); }

void splitByTree(const SupportTree& tree, const std::vector<SupportPoint>& points, std::vector<SupportPoint>& built,
                 std::vector<SupportPoint>& dropped) {
    std::vector<char> has_head(points.size(), 0);
    for (const Head& h : tree.heads)
        if (h.point >= 0 && static_cast<size_t>(h.point) < points.size()) has_head[h.point] = 1;
    built.clear();
    dropped.clear();
    for (size_t i = 0; i < points.size(); ++i) (has_head[i] ? built : dropped).push_back(points[i]);
}

void splitByFooting(const SupportTree& tree, const std::vector<SupportPoint>& points,
                    std::vector<SupportPoint>& from_raft, std::vector<TiedTip>& tied) {
    // A head on a pillar that stands on the model is tied; every other head
    // reaches the raft (through its own pillar, a bridge or a trunk) or the plate.
    std::vector<int> foot_of(tree.heads.size(), -1);
    for (size_t pi = 0; pi < tree.pillars.size(); ++pi)
        if (tree.pillars[pi].on_model)
            for (int h : tree.pillars[pi].heads)
                if (h >= 0 && static_cast<size_t>(h) < foot_of.size()) foot_of[h] = static_cast<int>(pi);
    from_raft.clear();
    tied.clear();
    for (size_t hi = 0; hi < tree.heads.size(); ++hi) {
        const int pt = tree.heads[hi].point;
        if (pt < 0 || static_cast<size_t>(pt) >= points.size()) continue;
        if (tree.heads[hi].stub) tied.push_back({points[pt], tree.heads[hi].junction});
        else if (foot_of[hi] >= 0) tied.push_back({points[pt], tree.pillars[foot_of[hi]].anchor_contact});
        else from_raft.push_back(points[pt]);
    }
}

double SupportTree::fullRaftInset(const TreeConfig& cfg) const {
    // 45°: as far in as the slab is thick, but never past the brim and margin,
    // so each trunk's foot stays on the top.
    return std::max(0.0, std::min(raft_top, cfg.pad_brim + cfg.full_raft_margin));
}

Paths SupportTree::fullRaftOutline(const TreeConfig& cfg, double inset) const {
    if (cfg.raft != RaftStyle::Full) return {};
    const double r = cfg.base_diameter / 2 + cfg.pad_brim + cfg.full_raft_margin;
    const int n = std::max(16, cfg.quality.segments(r));
    Paths discs;
    for (const Pillar& P : pillars) {
        if (P.on_model || P.merged) continue;
        Path d;
        for (int i = 0; i < n; ++i) {
            const double a = 2 * M_PI * i / n;
            d.push_back(toPoint({P.xy.x + r * std::cos(a), P.xy.y + r * std::sin(a)}));
        }
        discs.push_back(std::move(d));
    }
    if (discs.empty()) return {};
    Paths u = unionNonZero(discs);
    // Closing: fill gaps narrower than full_raft_gap, so the slab follows the
    // supports' footprint instead of spanning empty plate.
    if (cfg.full_raft_gap > 0) u = offset(offset(u, cfg.full_raft_gap / 2), -cfg.full_raft_gap / 2);
    // Closing and simplifying can leave a hair-thin spur of no area: drop
    // anything too small to be a pad (it would print as a loose piece).
    auto tidy = [](Paths p) {
        p.erase(std::remove_if(p.begin(), p.end(), [](const Path& q) { return std::abs(area(q)) < 0.5; }), p.end());
        return p;
    };
    u = tidy(simplify(u, 0.02));
    if (inset > 0) u = tidy(simplify(offset(u, -inset), 0.02));
    return u;
}

TriangleMesh SupportTree::mesh(const TreeConfig& cfg) const {
    TriangleMesh m;
    const GeometryQuality& q = cfg.quality;
    for (const Head& h : heads) {
        if (h.stub) continue;  // drawn as its column, below
        if (h.on_plate)
            plateHead(h, &m, q);
        else
            appendPinhead(m, h.contact, h.dir, h.spec, q);
    }
    for (const Rod& r : stubs) appendFrustum(m, r.a, r.diameter / 2, r.b, r.diameter / 2, q.segments(r.diameter / 2));
    for (const Rod& r : bridges) {
        appendFrustum(m, r.a, r.diameter / 2, r.b, r.diameter / 2, q.segments(r.diameter / 2));
        appendSphere(m, r.b, r.diameter / 2, q.segments(r.diameter / 2));
    }
    for (const Pillar& P : pillars) {
        const Vec3 top{P.xy.x, P.xy.y, P.top}, bottom{P.xy.x, P.xy.y, P.bottom};
        appendShaft(m, top, bottom, P.diameter, q, true);
        if (P.merged) {
            appendSphere(m, bottom, P.diameter / 2, q.segments(P.diameter / 2));
        } else if (P.on_model) {
            // Reversed pinhead: a contact as wide as the tip above, sunk as deep
            // into the surface below (it is pulled as hard as the tip is).
            PinheadSpec a;
            a.tip_diameter = P.foot_tip;
            a.penetration = penetrationFor(P.foot_tip, cfg);
            // Junction of the reversed head lands on the pillar bottom.
            a.head_length = std::max(0.1, P.bottom - P.anchor_contact.z - (a.tip_diameter - a.penetration) - P.diameter / 2);
            a.shaft_diameter = P.diameter;
            appendPinhead(m, P.anchor_contact, {0, 0, 1}, a, q);
        } else {
            const double base = P.base_diameter > 0 ? std::max(P.base_diameter, P.diameter) : cfg.base_diameter;
            appendBaseCone(m, bottom, P.diameter, base, std::max(0.0, raft_top - 0.1), q);
            if (cfg.raft == RaftStyle::Pads) {  // (Full has one slab for all, below)
                const double r = cfg.base_diameter / 2 + cfg.pad_brim;
                appendFrustum(m, {P.xy.x, P.xy.y, 0}, r, {P.xy.x, P.xy.y, raft_top}, std::max(0.5, r - 0.4),
                              q.segments(r));
            }
        }
    }
    for (const Rod& r : braces) appendFrustum(m, r.a, r.diameter / 2, r.b, r.diameter / 2, q.segments(r.diameter / 2));
    for (const Rod& r : branches) {
        appendFrustum(m, r.a, r.diameter / 2, r.b, r.diameter / 2, q.segments(r.diameter / 2));
        appendSphere(m, r.b, r.diameter / 2, q.segments(r.diameter / 2));
    }
    if (cfg.raft == RaftStyle::Full && raft_top > 0) {
        // One slab, built as stacked prisms of the outline, each pulled in a
        // little more than the one below: a ~45° edge for the scraper. They
        // overlap the cones and links; the slicer fills overlaps as a union.
        const double inset = fullRaftInset(cfg);
        const int steps = std::max(1, static_cast<int>(std::ceil(raft_top / 0.2 - 1e-9)));
        const double h = raft_top / steps;
        for (int k = 0; k < steps; ++k) {
            const double z0 = k * h;
            const Paths outline = fullRaftOutline(cfg, inset * z0 / raft_top);
            for (const ExPolygon& ex : toExPolygons(outline)) {
                std::vector<std::vector<Vec2>> polys;
                auto add = [&](const Path& p) {
                    std::vector<Vec2> v;
                    for (const Point64& q : p) v.push_back(toVec(q));
                    polys.push_back(std::move(v));
                };
                add(ex.contour);
                for (const Path& hole : ex.holes) add(hole);
                const ops::Solid prism = ops::extrude(polys, h);
                if (!prism.valid() || prism.empty()) continue;
                TriangleMesh pm = prism.toMesh();
                for (Vec3& v : pm.vertices) v.z += z0;
                m.append(pm);
            }
        }
    }
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
                             double min_raft_thickness, const JobControl& ctl) {
    Builder b(model, cfg, min_raft_thickness);
    return b.run(points, ctl);
}

}  // namespace fruit
