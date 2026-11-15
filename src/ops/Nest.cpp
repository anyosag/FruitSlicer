// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "ops/Nest.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>

#include "core/Parallel.h"
#include "ops/Arrange.h"

namespace fruit::ops {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Run {
    int x0, x1;  // inclusive, cells from the pivot's cell
};

// A part's outline at one turn, on the grid, grown by half the gap: its rows
// of runs, relative to the cell its pivot is in.
struct Mask {
    int y0 = 0, y1 = -1;  // rows, relative to the pivot's cell
    int x0 = 0, x1 = -1;  // the widest it reaches either way
    std::vector<std::vector<Run>> rows;  // rows[y - y0]
    std::size_t cells = 0;
    // The part's mesh turned by this much about its pivot: the middle of its
    // rectangle, and the rectangle of mesh and supports together (mm, from the pivot).
    Vec2 mesh_mid;
    double ex0 = 0, ex1 = 0, ey0 = 0, ey1 = 0;
};

// What every run shares: the grid, and each part's masks by turn.
struct Prepared {
    NestOptions o;
    double inset = 0;  // the masks may come this close to the plate's edge (they carry half the gap)
    int W = 0, H = 0;  // grid cells
    double gx0 = 0, gy0 = 0;  // the grid's lower-left corner, plate coordinates
    // Each part's turns tried (degrees; [0] = 0): every `angle_step`, and the turn
    // that gives it its smallest rectangle with its quarter turns.
    std::vector<std::vector<double>> angles;
    std::vector<std::vector<int>> quarter;     // indices into its angles: the quarter turns of none and of its best
    std::vector<std::vector<Mask>> masks;      // [part][angle]; empty for a part that can't be drawn
    std::vector<Vec2> pivot;                   // each part's pivot now: its mesh rectangle's middle
    std::vector<bool> usable;
    std::vector<double> big;                   // each part's biggest layer (its outline's area when not given)
};

Vec2 rotate(const Vec2& p, double deg) {
    const double r = deg * kPi / 180, c = std::cos(r), s = std::sin(r);
    return {p.x * c - p.y * s, p.x * s + p.y * c};
}

// The convex hull of `pts`, counter-clockwise (monotone chain).
std::vector<Vec2> hullOf(std::vector<Vec2> pts) {
    std::sort(pts.begin(), pts.end(), [](const Vec2& a, const Vec2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    if (pts.size() < 3) return pts;
    auto cross = [](const Vec2& o, const Vec2& a, const Vec2& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };
    std::vector<Vec2> h(2 * pts.size());
    std::size_t k = 0;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
        h[k++] = pts[i];
    }
    for (std::size_t i = pts.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && cross(h[k - 2], h[k - 1], pts[i - 1]) <= 0) --k;
        h[k++] = pts[i - 1];
    }
    h.resize(k - 1);
    return h;
}

// The part's outline (mesh seen from above, its disks) on a grid half the
// nesting cell, as the centres of its filled cells, from its pivot.
std::vector<Vec2> fineCells(const NestPart& p, const Vec2& pivot, double f) {
    double R = 0;
    for (const Vec3& v : p.mesh->vertices) R = std::max(R, std::hypot(v.x - pivot.x, v.y - pivot.y));
    for (const NestDisk& d : p.disks) R = std::max(R, std::hypot(d.at.x - pivot.x, d.at.y - pivot.y) + d.radius);
    const int n = static_cast<int>(std::ceil(R / f)) + 2;  // cells either way from the pivot
    const int N = 2 * n + 1;
    std::vector<std::uint8_t> g(static_cast<std::size_t>(N) * N, 0);
    auto cellOf = [&](double v) { return static_cast<int>(std::floor(v / f + 0.5)) + n; };
    auto set = [&](int x, int y) {
        if (x >= 0 && y >= 0 && x < N && y < N) g[static_cast<std::size_t>(y) * N + x] = 1;
    };
    for (const Face& fc : p.mesh->faces) {
        Vec2 q[3];
        for (int k = 0; k < 3; ++k) {
            const Vec3& v = p.mesh->vertices[fc[k]];
            q[k] = {v.x - pivot.x, v.y - pivot.y};
            set(cellOf(q[k].x), cellOf(q[k].y));  // a sliver narrower than a cell still counts
        }
        const double area = (q[1].x - q[0].x) * (q[2].y - q[0].y) - (q[2].x - q[0].x) * (q[1].y - q[0].y);
        if (std::abs(area) < 1e-12) continue;
        const int bx0 = cellOf(std::min({q[0].x, q[1].x, q[2].x})), bx1 = cellOf(std::max({q[0].x, q[1].x, q[2].x}));
        const int by0 = cellOf(std::min({q[0].y, q[1].y, q[2].y})), by1 = cellOf(std::max({q[0].y, q[1].y, q[2].y}));
        for (int y = by0; y <= by1; ++y)
            for (int x = bx0; x <= bx1; ++x) {
                const double X = (x - n) * f, Y = (y - n) * f;
                const double w0 = ((q[1].x - X) * (q[2].y - Y) - (q[2].x - X) * (q[1].y - Y)) / area;
                const double w1 = ((q[2].x - X) * (q[0].y - Y) - (q[0].x - X) * (q[2].y - Y)) / area;
                const double w2 = 1 - w0 - w1;
                if (w0 >= -1e-9 && w1 >= -1e-9 && w2 >= -1e-9) set(x, y);
            }
    }
    for (const NestDisk& d : p.disks) {
        const double cx = d.at.x - pivot.x, cy = d.at.y - pivot.y;
        const int r = static_cast<int>(std::ceil(d.radius / f));
        const int ix = cellOf(cx), iy = cellOf(cy);
        for (int y = iy - r; y <= iy + r; ++y)
            for (int x = ix - r; x <= ix + r; ++x)
                if (std::hypot((x - n) * f - cx, (y - n) * f - cy) <= d.radius) set(x, y);
    }
    std::vector<Vec2> out;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x)
            if (g[static_cast<std::size_t>(y) * N + x]) out.push_back({(x - n) * f, (y - n) * f});
    return out;
}

// The fine cells turned by `deg`, on the nesting grid, grown by `grow` mm.
Mask maskAt(const std::vector<Vec2>& fine, double deg, double cell, double grow) {
    Mask m;
    if (fine.empty()) return m;
    const double r = deg * kPi / 180, c = std::cos(r), s = std::sin(r);
    int x0 = std::numeric_limits<int>::max(), x1 = std::numeric_limits<int>::min(), y0 = x0, y1 = x1;
    std::vector<std::pair<int, int>> hit;
    hit.reserve(fine.size());
    for (const Vec2& p : fine) {
        const int x = static_cast<int>(std::floor((p.x * c - p.y * s) / cell + 0.5));
        const int y = static_cast<int>(std::floor((p.x * s + p.y * c) / cell + 0.5));
        hit.push_back({x, y});
        x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    }
    const int k = static_cast<int>(std::ceil(grow / cell)) + 1;
    const int W = x1 - x0 + 1 + 2 * k, H = y1 - y0 + 1 + 2 * k;
    std::vector<std::uint8_t> core(static_cast<std::size_t>(W) * H, 0), grown(core.size(), 0);
    auto at = [&](std::vector<std::uint8_t>& g, int x, int y) -> std::uint8_t& {
        return g[static_cast<std::size_t>(y - y0 + k) * W + (x - x0 + k)];
    };
    for (const auto& [x, y] : hit) at(core, x, y) = 1;
    // Grow by a disk of `grow`: from the cells on the edge only (the rest is inside anyway).
    std::vector<int> half(2 * k + 1);
    for (int dy = -k; dy <= k; ++dy) {
        const double rr = grow / cell;
        // Rounded up: the grown outline is never smaller than `grow` (-1: this row is out of reach).
        const double h2 = rr * rr - double(dy) * dy;
        half[dy + k] = h2 < 0 ? -1 : static_cast<int>(std::ceil(std::sqrt(h2)));
    }
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            if (!at(core, x, y)) continue;
            at(grown, x, y) = 1;
            const bool edge = !at(core, x - 1, y) || !at(core, x + 1, y) || !at(core, x, y - 1) || !at(core, x, y + 1);
            if (!edge || k == 0) continue;
            for (int dy = -k; dy <= k; ++dy) {
                const int w = half[dy + k];
                if (w < 0) continue;
                std::uint8_t* row = &at(grown, x - w, y + dy);
                std::fill(row, row + 2 * w + 1, std::uint8_t{1});
            }
        }
    m.y0 = y0 - k, m.y1 = y1 + k, m.x0 = x0 - k, m.x1 = x1 + k;
    m.rows.resize(static_cast<std::size_t>(H));
    for (int y = 0; y < H; ++y) {
        const std::uint8_t* row = &grown[static_cast<std::size_t>(y) * W];
        for (int x = 0; x < W;) {
            if (!row[x]) {
                ++x;
                continue;
            }
            const int a = x;
            while (x < W && row[x]) ++x;
            m.rows[static_cast<std::size_t>(y)].push_back({a + x0 - k, x - 1 + x0 - k});
            m.cells += static_cast<std::size_t>(x - a);
        }
    }
    return m;
}

// The plate's cells, taken or free, with each row's running count for quick
// "is anything taken in this run" questions.
class Plate {
public:
    Plate(int W, int H) : W_(W), H_(H), taken_(static_cast<std::size_t>(W) * H, 0), sum_(static_cast<std::size_t>(W + 1) * H, 0) {}
    bool fits(const Mask& m, int gx, int gy) const {
        if (gx + m.x0 < 0 || gx + m.x1 >= W_ || gy + m.y0 < 0 || gy + m.y1 >= H_) return false;
        for (int r = 0; r < static_cast<int>(m.rows.size()); ++r) {
            const int y = gy + m.y0 + r;
            const int* s = &sum_[static_cast<std::size_t>(y) * (W_ + 1)];
            for (const Run& run : m.rows[static_cast<std::size_t>(r)])
                if (s[gx + run.x1 + 1] - s[gx + run.x0] != 0) return false;
        }
        return true;
    }
    void take(const Mask& m, int gx, int gy) {
        for (int r = 0; r < static_cast<int>(m.rows.size()); ++r) {
            const int y = gy + m.y0 + r;
            std::uint8_t* row = &taken_[static_cast<std::size_t>(y) * W_];
            for (const Run& run : m.rows[static_cast<std::size_t>(r)])
                std::fill(row + gx + run.x0, row + gx + run.x1 + 1, std::uint8_t{1});
            int* s = &sum_[static_cast<std::size_t>(y) * (W_ + 1)];
            for (int x = 0; x < W_; ++x) s[x + 1] = s[x] + row[x];
        }
    }

private:
    int W_, H_;
    std::vector<std::uint8_t> taken_;
    std::vector<int> sum_;
};

struct Spot {
    bool found = false;
    int gx = 0, gy = 0, angle = 0;
    long far = 0, left = 0;  // how far its far edge is from the side filled from, and its left edge
};

// Bottom-left fill from the back (+Y, `from_back`) or the front: the spot whose
// far edge is nearest the side filled from, then furthest left, then the least turn.
Spot bestSpot(const Prepared& pr, const Plate& plate, int part, const std::vector<int>& angles, bool from_back) {
    Spot best;
    for (const int ai : angles) {
        const Mask& m = pr.masks[static_cast<std::size_t>(part)][static_cast<std::size_t>(ai)];
        if (m.rows.empty()) continue;
        const int gy_first = from_back ? pr.H - 1 - m.y1 : -m.y0;
        const int gy_last = from_back ? -m.y0 : pr.H - 1 - m.y1;
        const int step = from_back ? -1 : 1;
        for (int gy = gy_first; from_back ? gy >= gy_last : gy <= gy_last; gy += step) {
            const long far = from_back ? long(pr.H - 1) - (gy + m.y0) : long(gy + m.y1);
            if (best.found && far > best.far) break;
            for (int gx = -m.x0; gx + m.x1 < pr.W; ++gx) {
                const long left = gx + m.x0;
                if (best.found && far == best.far && left >= best.left) break;
                if (!plate.fits(m, gx, gy)) continue;
                const bool better = !best.found || far < best.far || (far == best.far && left < best.left) ||
                                    (far == best.far && left == best.left &&
                                     std::abs(std::remainder(pr.angles[part][ai], 360.0)) <
                                         std::abs(std::remainder(pr.angles[part][best.angle], 360.0)));
                if (better) best = {true, gx, gy, ai, far, left};
                break;  // the first in this row is the furthest left
            }
        }
    }
    return best;
}

struct Placed {
    bool placed = false;
    int angle = 0;
    Vec2 pivot_at;  // plate coordinates, mm
};

// The layout moved as a whole by `shift`, scored.
NestLayout score(const Prepared& pr, const std::vector<NestPart>& parts, const std::vector<Placed>& pl, Vec2 shift,
                 const std::string& how) {
    NestLayout L;
    L.how = how;
    L.parts.resize(parts.size());
    double X0 = 1e300, X1 = -1e300, Y0 = 1e300, Y1 = -1e300, load = 0, peel_w = 0, peel = 0;
    Vec2 load_at{0, 0};
    const Vec2 half{pr.o.plate_width / 2, pr.o.plate_depth / 2};
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (!pl[i].placed) continue;
        const Mask& m = pr.masks[i][static_cast<std::size_t>(pl[i].angle)];
        const double deg = pr.angles[i][static_cast<std::size_t>(pl[i].angle)];
        const Vec2 at = pl[i].pivot_at + shift;
        NestPlacement& np = L.parts[i];
        np.placed = true;
        np.turn_deg = std::remainder(deg, 360.0);
        np.offset = at + m.mesh_mid;
        ++L.placed;
        L.turned += std::abs(np.turn_deg) > 1e-9;
        X0 = std::min(X0, at.x + m.ex0), X1 = std::max(X1, at.x + m.ex1);
        Y0 = std::min(Y0, at.y + m.ey0), Y1 = std::max(Y1, at.y + m.ey1);
        const Vec2 c = at + rotate(parts[i].load_at - pr.pivot[i], deg);
        load += parts[i].load;
        load_at = load_at + c * parts[i].load;
        if (pr.o.hinge) {
            const Vec2 h = *pr.o.hinge;
            const double reach = std::abs(h.x) * half.x + std::abs(h.y) * half.y;
            const double t = std::clamp((reach - (c.x * h.x + c.y * h.y)) / (2 * reach), 0.0, 1.0);
            peel += pr.big[i] * t;
            peel_w += pr.big[i];
        }
    }
    // What it covers, at whole cells of the shift: its runs, sorted, hashed (FNV-1a).
    {
        const double cell = std::max(0.1, pr.o.cell);
        std::vector<std::array<int, 3>> runs;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (!pl[i].placed) continue;
            const Mask& m = pr.masks[i][static_cast<std::size_t>(pl[i].angle)];
            const Vec2 at = pl[i].pivot_at + shift;
            const int gx = static_cast<int>(std::lround((at.x - pr.gx0) / cell - 0.5));
            const int gy = static_cast<int>(std::lround((at.y - pr.gy0) / cell - 0.5));
            for (std::size_t r = 0; r < m.rows.size(); ++r)
                for (const Run& run : m.rows[r]) runs.push_back({gy + m.y0 + static_cast<int>(r), gx + run.x0, gx + run.x1});
        }
        std::sort(runs.begin(), runs.end());
        std::uint64_t h = 1469598103934665603ULL;
        for (const auto& run : runs)
            for (const int v : run) {
                h ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(v));
                h *= 1099511628211ULL;
            }
        L.covers = h;
    }
    if (L.placed > 0) {
        L.width = X1 - X0, L.depth = Y1 - Y0;
        if (load > 0) L.balance = std::hypot(load_at.x / load, load_at.y / load);
        if (pr.o.hinge) L.peel = peel_w > 0 ? peel / peel_w : 0.5;
    }
    return L;
}

bool sameLayout(const NestLayout& a, const NestLayout& b) {
    if (a.placed != b.placed) return false;
    if (a.covers == b.covers) return true;  // the same to look at
    for (std::size_t i = 0; i < a.parts.size(); ++i) {
        const NestPlacement &p = a.parts[i], &q = b.parts[i];
        if (p.placed != q.placed) return false;
        if (!p.placed) continue;
        if (std::abs(std::remainder(p.turn_deg - q.turn_deg, 360.0)) > 0.5) return false;
        if (std::hypot(p.offset.x - q.offset.x, p.offset.y - q.offset.y) > 0.5) return false;
    }
    return true;
}

}  // namespace

std::vector<NestLayout> nestLayouts(const std::vector<NestPart>& parts, const NestOptions& o, const JobControl& ctl) {
    Prepared pr;
    pr.o = o;
    const double cell = std::max(0.1, o.cell);
    pr.inset = std::max(0.0, o.edge_margin - o.gap / 2);
    pr.W = static_cast<int>(std::floor((o.plate_width - 2 * pr.inset) / cell));
    pr.H = static_cast<int>(std::floor((o.plate_depth - 2 * pr.inset) / cell));
    pr.gx0 = -o.plate_width / 2 + pr.inset;
    pr.gy0 = -o.plate_depth / 2 + pr.inset;
    if (pr.W <= 0 || pr.H <= 0 || parts.empty()) return {};
    // Each part's outline, then a mask at every turn: all side by side.
    pr.pivot.resize(parts.size());
    pr.usable.assign(parts.size(), false);
    std::vector<std::vector<Vec2>> fine(parts.size());
    // Half the gap, and a cell and a half for the grid's rounding (outlines sampled at
    // half a cell, then put on the grid, then grown from cell centres).
    const double grow = o.gap / 2 + 1.5 * cell;
    parallelFor(parts.size(), o.threads, [&](std::size_t i) {
        const NestPart& p = parts[i];
        if (!p.mesh || p.mesh->vertices.empty()) return;
        const BBox3 b = p.mesh->bbox();
        pr.pivot[i] = {(b.min.x + b.max.x) / 2, (b.min.y + b.max.y) / 2};
        fine[i] = fineCells(p, pr.pivot[i], cell / 2);
        pr.usable[i] = !fine[i].empty();
    });
    // The turns each part may take: none; or every `angle_step` degrees (0, 90,
    // 180 and 270 among them), and the turn that gives it its smallest
    // rectangle with its quarter turns, so a long part can lie square on.
    pr.angles.assign(parts.size(), {0.0});
    pr.quarter.assign(parts.size(), {0});
    const int n_steps = std::max(4, static_cast<int>(std::round(360.0 / std::max(1.0, o.angle_step))) / 4 * 4);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (!o.turn || !parts[i].may_turn || !pr.usable[i]) continue;
        std::vector<double>& a = pr.angles[i];
        for (int k = 1; k < n_steps; ++k) a.push_back(360.0 * k / n_steps);
        pr.quarter[i] = {0, n_steps / 4, n_steps / 2, 3 * n_steps / 4};
        std::vector<Vec2> pts;
        for (const Vec3& v : parts[i].mesh->vertices) pts.push_back({v.x, v.y});
        for (const NestDisk& d : parts[i].disks)
            for (int k = 0; k < 8; ++k) pts.push_back(d.at + Vec2{std::cos(k * kPi / 4), std::sin(k * kPi / 4)} * d.radius);
        const double best = smallestTurn(hullOf(std::move(pts))).degrees;
        if (std::abs(std::remainder(best, 360.0 / n_steps)) > 1e-6)  // not one of the steps already
            for (int q = 0; q < 4; ++q) {
                pr.quarter[i].push_back(static_cast<int>(a.size()));
                a.push_back(std::fmod(best + 90.0 * q + 360.0, 360.0));
            }
    }
    pr.big.resize(parts.size());
    for (std::size_t i = 0; i < parts.size(); ++i)
        pr.big[i] = parts[i].big_layer > 0 ? parts[i].big_layer : static_cast<double>(fine[i].size()) * cell * cell / 4;
    ctl.report("outlines", 0.1);
    std::vector<std::pair<std::size_t, std::size_t>> jobs;  // (part, turn)
    pr.masks.resize(parts.size());
    for (std::size_t i = 0; i < parts.size(); ++i) {
        pr.masks[i].resize(pr.angles[i].size());
        for (std::size_t a = 0; a < pr.angles[i].size(); ++a) jobs.push_back({i, a});
    }
    std::atomic<std::size_t> done{0};
    parallelFor(jobs.size(), o.threads, [&](std::size_t k) {
        const auto [i, a] = jobs[k];
        if (!pr.usable[i] || ctl.cancelled()) return;
        Mask m = maskAt(fine[i], pr.angles[i][a], cell, grow + parts[i].grow);
        // The mesh's rectangle and the whole outline's, turned, from the pivot.
        double mx0 = 1e300, mx1 = -1e300, my0 = 1e300, my1 = -1e300;
        for (const Vec3& v : parts[i].mesh->vertices) {
            const Vec2 q = rotate({v.x - pr.pivot[i].x, v.y - pr.pivot[i].y}, pr.angles[i][a]);
            mx0 = std::min(mx0, q.x), mx1 = std::max(mx1, q.x), my0 = std::min(my0, q.y), my1 = std::max(my1, q.y);
        }
        m.mesh_mid = {(mx0 + mx1) / 2, (my0 + my1) / 2};
        double ex0 = mx0, ex1 = mx1, ey0 = my0, ey1 = my1;
        for (const NestDisk& d : parts[i].disks) {
            const Vec2 q = rotate(d.at - pr.pivot[i], pr.angles[i][a]);
            ex0 = std::min(ex0, q.x - d.radius), ex1 = std::max(ex1, q.x + d.radius);
            ey0 = std::min(ey0, q.y - d.radius), ey1 = std::max(ey1, q.y + d.radius);
        }
        const double g = parts[i].grow;
        m.ex0 = ex0 - g, m.ex1 = ex1 + g, m.ey0 = ey0 - g, m.ey1 = ey1 + g;
        pr.masks[i][a] = std::move(m);
        const std::size_t d = ++done;
        if (d % 16 == 0) ctl.report("outlines", 0.1 + 0.2 * static_cast<double>(d) / static_cast<double>(jobs.size()));
    });
    ctl.check();

    // The runs: orders of the parts, sides to fill from, how much turning.
    std::vector<std::vector<int>> orders;
    std::vector<std::string> order_names;
    auto byKey = [&](const char* name, auto key) {
        std::vector<int> v(parts.size());
        std::iota(v.begin(), v.end(), 0);
        std::stable_sort(v.begin(), v.end(), [&](int a, int b) { return key(a) > key(b) + 1e-9; });
        orders.push_back(std::move(v));
        order_names.push_back(name);
    };
    byKey("biggest outline first", [&](int i) { return pr.usable[i] ? double(pr.masks[i][0].cells) : 0.0; });
    byKey("heaviest first", [&](int i) { return parts[i].load; });
    byKey("biggest layer first", [&](int i) { return pr.big[static_cast<std::size_t>(i)]; });
    byKey("longest first", [&](int i) {
        const Mask& m = pr.masks[i][0];
        return pr.usable[i] ? double(std::max(m.x1 - m.x0, m.y1 - m.y0)) : 0.0;
    });
    for (int s = 1; s <= 4 && parts.size() > 1; ++s) {
        std::vector<int> v = orders[0];
        std::mt19937 rng(static_cast<std::uint32_t>(s * 7919));
        std::shuffle(v.begin(), v.end(), rng);
        orders.push_back(std::move(v));
        order_names.push_back("shuffled " + std::to_string(s));
    }
    struct Way {
        int order;
        bool from_back;
        int turning;  // 0 none, 1 quarter turns, 2 free
    };
    std::vector<Way> ways;
    for (int t = 0; t < (o.turn ? 3 : 1); ++t)
        for (std::size_t ord = 0; ord < orders.size(); ++ord)
            for (const bool back : {true, false}) ways.push_back({static_cast<int>(ord), back, t});

    std::vector<std::vector<NestLayout>> found(ways.size());
    std::atomic<std::size_t> runs_done{0};
    parallelFor(ways.size(), o.threads, [&](std::size_t w) {
        if (ctl.cancelled()) return;
        const Way& way = ways[w];
        Plate plate(pr.W, pr.H);
        std::vector<Placed> pl(parts.size());
        std::vector<int> keep{0};
        for (const int i : orders[static_cast<std::size_t>(way.order)]) {
            if (!pr.usable[static_cast<std::size_t>(i)]) continue;
            std::vector<int> angles = keep;
            if (way.turning == 1) angles = pr.quarter[static_cast<std::size_t>(i)];
            if (way.turning == 2) {
                angles.resize(pr.angles[static_cast<std::size_t>(i)].size());
                std::iota(angles.begin(), angles.end(), 0);
            }
            const Spot s = bestSpot(pr, plate, i, angles, way.from_back);
            if (!s.found) continue;
            const Mask& m = pr.masks[static_cast<std::size_t>(i)][static_cast<std::size_t>(s.angle)];
            plate.take(m, s.gx, s.gy);
            pl[static_cast<std::size_t>(i)] = {true, s.angle,
                                               {pr.gx0 + (s.gx + 0.5) * cell, pr.gy0 + (s.gy + 0.5) * cell}};
        }
        // How far the layout may move as a whole and stay on the plate.
        double X0 = 1e300, X1 = -1e300, Y0 = 1e300, Y1 = -1e300, load = 0;
        Vec2 load_at{0, 0};
        bool any = false;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (!pl[i].placed) continue;
            any = true;
            const Mask& m = pr.masks[i][static_cast<std::size_t>(pl[i].angle)];
            X0 = std::min(X0, pl[i].pivot_at.x + (m.x0 - 0.5) * cell), X1 = std::max(X1, pl[i].pivot_at.x + (m.x1 + 0.5) * cell);
            Y0 = std::min(Y0, pl[i].pivot_at.y + (m.y0 - 0.5) * cell), Y1 = std::max(Y1, pl[i].pivot_at.y + (m.y1 + 0.5) * cell);
            const Vec2 c = pl[i].pivot_at + rotate(parts[i].load_at - pr.pivot[i], pr.angles[i][static_cast<std::size_t>(pl[i].angle)]);
            load += parts[i].load;
            load_at = load_at + c * parts[i].load;
        }
        if (!any) return;
        const double lo_x = pr.gx0 - X0, hi_x = pr.gx0 + pr.W * cell - X1;
        const double lo_y = pr.gy0 - Y0, hi_y = pr.gy0 + pr.H * cell - Y1;
        auto clampShift = [&](Vec2 s) { return Vec2{std::clamp(s.x, lo_x, hi_x), std::clamp(s.y, lo_y, hi_y)}; };
        const std::string how = order_names[static_cast<std::size_t>(way.order)] +
                                (way.from_back ? ", from the back" : ", from the front") +
                                (way.turning == 0 ? ", not turned" : way.turning == 1 ? ", quarter turns" : ", any turn");
        std::vector<NestLayout>& out = found[w];
        // Its rectangle centred.
        out.push_back(score(pr, parts, pl, clampShift({-(X0 + X1) / 2, -(Y0 + Y1) / 2}), how + ", centred"));
        // Its load centred.
        if (load > 0) out.push_back(score(pr, parts, pl, clampShift(Vec2{0, 0} - load_at * (1 / load)), how + ", load centred"));
        // Brought to the hinge, centred along it.
        if (o.hinge) {
            const Vec2 h = *o.hinge;
            Vec2 s{-(X0 + X1) / 2, -(Y0 + Y1) / 2};
            if (h.x > 0.5) s.x = hi_x;
            if (h.x < -0.5) s.x = lo_x;
            if (h.y > 0.5) s.y = hi_y;
            if (h.y < -0.5) s.y = lo_y;
            out.push_back(score(pr, parts, pl, clampShift(s), how + ", at the hinge"));
        }
        const std::size_t d = ++runs_done;
        ctl.report("layouts", 0.3 + 0.7 * static_cast<double>(d) / static_cast<double>(ways.size()));
    });
    ctl.check();
    std::vector<NestLayout> all;
    for (auto& f : found)
        for (auto& L : f) {
            bool dup = false;
            for (const NestLayout& k : all) dup = dup || sameLayout(k, L);
            if (!dup) all.push_back(std::move(L));
        }
    return all;
}

std::vector<NestPick> pickLayouts(const std::vector<NestLayout>& layouts) {
    std::vector<NestPick> out;
    if (layouts.empty()) return out;
    int most = 0;
    for (const NestLayout& L : layouts) most = std::max(most, L.placed);
    std::vector<const NestLayout*> c;
    for (const NestLayout& L : layouts)
        if (L.placed == most) c.push_back(&L);
    const bool hinge = c.front()->peel >= 0;
    // Each measure against the spread of all the candidates, 0 (best) to 1.
    auto spread = [&](auto f) {
        double lo = 1e300, hi = -1e300;
        for (const NestLayout* L : c) lo = std::min(lo, f(*L)), hi = std::max(hi, f(*L));
        return std::pair<double, double>{lo, hi};
    };
    const auto [a0, a1] = spread([](const NestLayout& L) { return L.area(); });
    const auto [b0, b1] = spread([](const NestLayout& L) { return L.balance; });
    const auto [p0, p1] = spread([](const NestLayout& L) { return L.peel; });
    const auto [t0, t1] = spread([](const NestLayout& L) { return double(L.turned); });
    auto norm = [](double v, double lo, double hi) { return hi - lo > 1e-12 ? (v - lo) / (hi - lo) : 0.0; };
    auto overall = [&](const NestLayout& L) {
        return 0.35 * norm(L.area(), a0, a1) + 0.35 * norm(L.balance, b0, b1) +
               (hinge ? 0.2 * norm(L.peel, p0, p1) : 0.0) + 0.1 * norm(double(L.turned), t0, t1);
    };
    // Ties go to the earlier layout: the same answer every time.
    auto best = [&](const char* title, auto less) {
        const NestLayout* top = c.front();
        for (const NestLayout* L : c)
            if (less(*L, *top)) top = L;
        for (NestPick& p : out)
            if (sameLayout(p.layout, *top)) {
                p.also.push_back(title);
                return;
            }
        out.push_back({title, {}, *top});
    };
    auto byOverall = [&](const NestLayout& x, const NestLayout& y) { return overall(x) < overall(y) - 1e-12; };
    best("Recommended", byOverall);
    // A measure on which they all score alike picks out nothing: it is left out.
    if (a1 - a0 > 1e-6) best("Most compact", [&](const NestLayout& x, const NestLayout& y) {
        if (std::abs(x.area() - y.area()) > 1e-6) return x.area() < y.area();
        return x.balance < y.balance - 1e-9;
    });
    if (b1 - b0 > 0.05) best("Balanced on the plate", [&](const NestLayout& x, const NestLayout& y) {
        if (std::abs(x.balance - y.balance) > 0.05) return x.balance < y.balance;
        return x.area() < y.area() - 1e-6;
    });
    if (hinge && p1 - p0 > 1e-4)
        best("Gentlest peel", [&](const NestLayout& x, const NestLayout& y) {
            if (std::abs(x.peel - y.peel) > 1e-4) return x.peel < y.peel;
            return x.balance < y.balance - 1e-9;
        });
    else if (!hinge && t1 > t0)
        best("Fewest turns", [&](const NestLayout& x, const NestLayout& y) {
            if (x.turned != y.turned) return x.turned < y.turned;
            return x.area() < y.area() - 1e-6;
        });
    // The room left: the next best overall, each different from those shown.
    std::vector<const NestLayout*> rest = c;
    std::stable_sort(rest.begin(), rest.end(), [&](const NestLayout* x, const NestLayout* y) { return byOverall(*x, *y); });
    for (const NestLayout* L : rest) {
        if (out.size() >= 4) break;
        bool shown = false;
        for (const NestPick& p : out) shown = shown || sameLayout(p.layout, *L);
        if (!shown) out.push_back({"Another way", {}, *L});
    }
    return out;
}

}  // namespace fruit::ops
