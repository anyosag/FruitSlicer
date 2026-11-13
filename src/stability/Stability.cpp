// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "stability/Stability.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

#include "core/Parallel.h"

namespace fruit {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kMinLever = 0.05;  // mm
constexpr double kSliver = kSliverContact;  // mm²: under the contact of a 0.2 mm tip (0.031 mm²)
constexpr double kListFrom = 0.05;  // a joint is listed from a chance of 1 in 20
constexpr double kWorthLosing = 1.0;  // mm³ of model: less than this lost still counts as printed whole
constexpr double kBorrowedMargin = 1.0 / 3;  // a borrowed limit, as a share of where half fail
constexpr double kLen = 10.0;      // mm: a sheet's slope is counted as its rise over this length
constexpr size_t kMaxSheets = 16;  // more parts of one piece in one layer than this are taken as one stiff sheet

// ---------------------------------------------------------------- small matrices
//
// A sheet (one layer part of the model) moves as a rigid plate: it rises by w
// at its centre and tilts, so the point (x, y) from its centre rises by
// w + gx·x + gy·y. What it is asked to carry is a force and that force's
// first moments about the centre. A 3 × 3 stiffness ties the two together.

using M3 = std::array<std::array<double, 3>, 3>;

struct Load {
    double n = 0, mx = 0, my = 0;  // force, and force × offset in x and y
};
struct Move {
    double w = 0, gx = 0, gy = 0;
};

void addTo(M3& a, const M3& b) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) a[i][j] += b[i][j];
}

M3 times(const M3& a, const M3& b) {
    M3 c{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) c[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    return c;
}

// A spring of stiffness k standing at (dx, dy) from the centre.
M3 springAt(double k, double dx, double dy) {
    const double v[3] = {1, dx, dy};
    M3 m{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m[i][j] = k * v[i] * v[j];
    return m;
}

// A sheet of area `a` joined over a patch whose centre is (dx, dy) from the
// sheet's centre, with the patch's second moments about its own centre.
M3 patchAt(double a, double dx, double dy, double sxx, double syy, double sxy) {
    M3 m = springAt(a, dx, dy);
    m[1][1] += sxx;
    m[2][2] += syy;
    m[1][2] += sxy;
    m[2][1] += sxy;
    return m;
}

// Eigenvalues (largest first) and vectors of a symmetric 2 × 2.
struct Axes {
    double l1 = 0, l2 = 0;
    Vec2 v1{1, 0}, v2{0, 1};
};
Axes axesOf(double a, double b, double d) {  // [a b; b d]
    Axes e;
    const double mean = (a + d) / 2, diff = (a - d) / 2, r = std::sqrt(diff * diff + b * b);
    e.l1 = mean + r;
    e.l2 = mean - r;
    // By the angle, which stays sound when the matrix is all but diagonal
    // (the vector (b, l1 - a) is then two rounding errors).
    const double theta = 0.5 * std::atan2(2 * b, a - d);
    e.v1 = {std::cos(theta), std::sin(theta)};
    e.v2 = {-e.v1.y, e.v1.x};
    return e;
}

// Two stiffnesses one after the other (the load goes through both).
double inSeries(double a, double b) {
    if (std::isinf(a)) return b;
    if (std::isinf(b)) return a;
    return a + b > 0 ? a * b / (a + b) : 0.0;
}

// A hold as a spring at its centre of stiffness plus a turning stiffness
// about that centre.
struct Hold {
    double k = 0;
    Vec2 e;   // the centre of stiffness, from the sheet's centre
    Axes r;   // turning stiffness about it
    double floor = 0;  // a turning stiffness at or under this is none at all
};
Hold holdOf(const M3& m) {
    Hold h;
    h.k = m[0][0];
    if (!(h.k > 1e-14)) {
        h.k = 0;
        return h;
    }
    h.e = {m[0][1] / h.k, m[0][2] / h.k};
    h.r = axesOf(m[1][1] - h.k * h.e.x * h.e.x, m[1][2] - h.k * h.e.x * h.e.y, m[2][2] - h.k * h.e.y * h.e.y);
    // Pins in a line, or one pin alone, cannot resist a turn about themselves:
    // the piece pivots there. A lever shorter than about a small tip's own
    // half-width (0.05 mm) is no lever either: tips nearly in a line would
    // otherwise be asked for forces no tip could give, where in fact the
    // piece turns on them.
    h.floor = kMinLever * kMinLever * h.k;
    return h;
}

// How a sheet held by `m` moves under `q`. A turn the hold cannot resist is
// let go: the piece pivots and the pull follows it. False with no hold at all.
bool settle(const M3& m, const Load& q, Move& out) {
    const Hold h = holdOf(m);
    if (h.k <= 0) return false;
    const Vec2 mom{q.mx - q.n * h.e.x, q.my - q.n * h.e.y};  // about the centre of stiffness
    Vec2 g{};
    if (h.r.l1 > h.floor) g += h.r.v1 * (dot(mom, h.r.v1) / h.r.l1);
    if (h.r.l2 > h.floor) g += h.r.v2 * (dot(mom, h.r.v2) / h.r.l2);
    out.gx = g.x;
    out.gy = g.y;
    out.w = q.n / h.k - dot(g, h.e);
    return true;
}

// The inverse of a symmetric matrix that is positive or zero (n × n, row by
// row), by Cholesky. A direction it has no stiffness in is left out: nothing
// moves along it.
void invertSym(std::vector<double>& a, int n) {
    const auto at = [n](int i, int j) { return static_cast<size_t>(i) * static_cast<size_t>(n) + static_cast<size_t>(j); };
    std::vector<double> l(a.size(), 0.0), inv(a.size(), 0.0);
    std::vector<char> out(static_cast<size_t>(n), 0);
    for (int j = 0; j < n; ++j) {
        double d = a[at(j, j)];
        for (int k = 0; k < j; ++k) d -= l[at(j, k)] * l[at(j, k)];
        if (!(d > 1e-10 * a[at(j, j)])) {
            out[static_cast<size_t>(j)] = 1;
            for (int k = 0; k < j; ++k) l[at(j, k)] = 0;
            l[at(j, j)] = 1;
            continue;
        }
        l[at(j, j)] = std::sqrt(d);
        for (int i = j + 1; i < n; ++i) {
            double v = a[at(i, j)];
            for (int k = 0; k < j; ++k) v -= l[at(i, k)] * l[at(j, k)];
            l[at(i, j)] = v / l[at(j, j)];
        }
    }
    // The inverse of the lower triangle, column by column.
    for (int j = 0; j < n; ++j) {
        inv[at(j, j)] = 1 / l[at(j, j)];
        for (int i = j + 1; i < n; ++i) {
            double v = 0;
            for (int k = j; k < i; ++k) v -= l[at(i, k)] * inv[at(k, j)];
            inv[at(i, j)] = v / l[at(i, i)];
        }
    }
    for (int i = 0; i < n; ++i)
        for (int j = 0; j <= i; ++j) {
            double v = 0;
            if (!out[static_cast<size_t>(i)] && !out[static_cast<size_t>(j)])
                for (int k = i; k < n; ++k) v += inv[at(k, i)] * inv[at(k, j)];
            a[at(i, j)] = a[at(j, i)] = v;
        }
}

// ---------------------------------------------------------------- polygons

// A point inside `e` (the middle of its longest run along a line across it).
bool pointInside(const ExPolygon& e, Vec2& out) {
    const BBox2 b = e.bbox();
    const Paths ps = e.toPaths();
    for (double f : {0.5, 0.25, 0.75, 0.375, 0.625, 0.125, 0.875}) {
        const double y = b.min.y + (b.max.y - b.min.y) * f;
        double best = 0;
        for (const auto& [t0, t1] : lineSpans(ps, {b.min.x - 1.0, y}, {1.0, 0.0}))
            if (t1 - t0 > best) {
                best = t1 - t0;
                out = {b.min.x - 1.0 + (t0 + t1) / 2, y};
            }
        if (best > 1e-6) return true;
    }
    return false;
}

// Whether `p` is inside paths as Clipper returns them (outer rings one way
// round, holes the other).
bool inPaths(const Paths& ps, const Vec2& p) {
    const Point64 q = toPoint(p);
    int count = 0;
    for (const Path& path : ps) {
        if (path.size() < 3) continue;
        if (Clipper2Lib::PointInPolygon(q, path) == Clipper2Lib::PointInPolygonResult::IsOutside) continue;
        count += area(path) >= 0 ? 1 : -1;
    }
    return count > 0;
}

// How far `p` is from the outline `path` (closed).
double distanceToOutline(const Vec2& p, const Path& path) {
    double best = kInf;
    for (size_t i = 0; i < path.size(); ++i) {
        const Vec2 a = toVec(path[i]), b = toVec(path[(i + 1) % path.size()]);
        const Vec2 ab = b - a;
        const double len2 = dot(ab, ab);
        const double t = len2 > 0 ? std::clamp(dot(p - a, ab) / len2, 0.0, 1.0) : 0.0;
        best = std::min(best, norm(p - (a + ab * t)));
    }
    return best;
}

// Everything within r of a part: its outline grown by r, less what of each
// hole stays further than r from that hole's own edge. Shrinking a hole past
// where it closes leaves an inside-out scrap behind (growing the part with
// its holes in one go does, and so does a band along the hole's edge), so a
// hole too narrow to hold such a point is left out before it is shrunk, and
// what comes back is kept only where it really is that far from the edge.
Paths grownBy(const ExPolygon& e, double r) {
    const Paths outer = offset(Paths{e.contour}, r);
    Paths left;
    for (const Path& h : e.holes) {
        const Vec2 size = bbox(h).size();
        if (std::min(size.x, size.y) <= 2 * r) continue;
        Path solid = h;
        if (area(solid) < 0) std::reverse(solid.begin(), solid.end());
        for (const ExPolygon& piece : toExPolygons(offset(Paths{solid}, -r))) {
            Vec2 in;
            if (!pointInside(piece, in) || distanceToOutline(in, h) < r - 0.01) continue;
            const Paths ps = piece.toPaths();
            left.insert(left.end(), ps.begin(), ps.end());
        }
    }
    return left.empty() ? outer : difference(outer, left);
}

// ---------------------------------------------------------------- the print's parts

enum class Kind : std::uint8_t {
    Dead,  // one end was never held, or a support brushing past the model below its tip
    Model,     // model on model
    Rod,       // support on support
    Tip,       // model resting on a support
    Foot,      // support standing on the model
};

struct Link {
    int child = -1, parent = -1;
    Kind kind = Kind::Dead;
    int geom = -1;      // Model: index of its bending numbers
    double area = 0;    // mm² the two parts share
    Vec2 c;             // the centre of that area
    double eff = 0;     // Rod: the area a straight pull is spread over, the rod's lean allowed for
    double holds = 0;   // mm² of layer at which half such joints break, pulled straight
    double k = 0;       // Rod, Tip, Foot: stiffness of this way down
};

struct Bending {
    M3 p{};          // the joint as a patch of resin one layer thick, in the sheets' own measure (buildSheets)
    double sxx = 0, syy = 0, sxy = 0;  // second moments of the shared area about its centre
    Axes ax;         // the same as axes
    double ext1 = 0, ext2 = 0;  // how far it reaches from its centre along each axis
};

struct Worst {
    float u = 0, load = 0, stress = 0;
    float near = 0;  // a tip: how much of `load` was thin model near it, given to it outright
    std::int32_t step = -1;
    RiskCause cause = RiskCause::None;
};

// How close a joint was while one layer printed (kept only when it matters).
struct Seen {
    std::int32_t link, step;
    float u;
};

struct Cup {
    int part;
    double force, dx, dy;
};

// One joint that can break, for the replays.
struct Breakable {
    int link, child, parent;
    double holds;
};

// A joint's load as the print goes on: a layer at which it reached a new high.
struct High {
    int step;
    double u;
};

// A piece of the model that hangs together through the model's own joints.
struct Body {
    int up = -1;   // the piece it later became part of
    int born = 0;  // the layer at which it came about
};

// A piece of the print that hangs together through joints too lightly loaded
// to break. Pieces join as layers are added; `born` is the layer at which
// this one came about, `up` the piece it later became part of.
struct Piece {
    int up = -1;
    int born = 0;
    std::vector<int> kids;
    std::vector<int> parts;  // the parts added to it while it was the whole piece, lowest first
};

struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed) {}
    std::uint64_t next() {  // splitmix64
        std::uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    double unit() { return (static_cast<double>(next() >> 11) + 0.5) * (1.0 / 9007199254740992.0); }  // (0, 1)
};

class Analysis {
public:
    Analysis(const StabilityInput& in, const StabilityConfig& cfg, const JobControl& ctl)
        : in_(in), cfg_(cfg), ctl_(ctl) {
        if (!in.print) throw std::invalid_argument("stability: no layers given");
        h_ = in.print->layer_height;
        if (!(h_ > 0)) throw std::invalid_argument("stability: layer height must be positive");
        if (in.model && in.model->size() && std::abs(in.model->layer_height - h_) > 1e-9)
            throw std::invalid_argument("stability: the model's layers have a different height");
        usable_ = std::clamp(cfg.usable_fraction, 0.05, 1.0);
        steep_ = std::max(cfg.steepness, 1.0);
        const double d = cfg.capacity.reference_diameter > 0 ? cfg.capacity.reference_diameter : 0.4;
        // What a tip's contact stands per mm², in multiples of the pull per mm².
        per_mm2_ = cfg.capacity.capacity(d) / usable_ / (kPi * d * d / 4);
        bulk_ = per_mm2_ * std::max(cfg.bulk_factor, 1e-6);
        // A joint is worth a strength of its own from a chance of 1 in 10,000;
        // its load is remembered from half of that load on.
        floor_u_ = std::pow(-std::log2(1 - 1e-4), 1 / steep_);
        seen_floor_ = static_cast<float>(floor_u_ / 2);
    }

    StabilityResult run();

private:
    void buildParts();
    void buildLinks();
    void buildBodies();
    void sizeJoints();
    void findHeads();
    void buildRods();
    void buildDepths();
    void buildSheets();
    void buildPulls();
    void replayPulls();
    void finishParts();
    void findRuns();
    void findSkins();
    void runReplays();
    void findSpots();
    void listTips();
    void summarise();
    double lostAbove(int part) const;

    struct Scratch;
    void step(int k, Scratch& st) const;

    double tipHolds(double diameter) const { return cfg_.capacity.capacity(diameter) / usable_; }
    double hingeFactor(const Vec2& xy) const;
    int layerOf(int n) const { return part_[n].layer; }
    bool ground(int n) const { return layerOf(n) == 0; }

    const StabilityInput& in_;
    const StabilityConfig& cfg_;
    const JobControl& ctl_;
    double h_ = 0.05, usable_ = 0.7, steep_ = 7, per_mm2_ = 0, bulk_ = 0, floor_u_ = 0;
    float seen_floor_ = 0;

    // One connected part of one layer: the model's, or the supports' outside the model.
    struct Part {
        int layer = 0;
        const ExPolygon* shape = nullptr;
        BBox2 box;
        std::vector<int> parents, children;  // the model's or supports' own first, then where the two meet
        const ExPolygon& poly() const { return *shape; }
    };
    std::shared_ptr<const IslandGraph> model_graph_, support_graph_;  // own the shapes
    std::vector<Part> part_;
    std::vector<std::vector<int>> in_layer_;
    std::vector<int> same_;                        // by part: how many of its parents are of its own kind
    std::vector<std::vector<AreaMoments>> touch_;  // by part: where it meets each parent of the other kind
    int n_ = 0, layers_ = 0, models_ = 0, nodes_ = 0;  // the model's parts come first: [0, models_)
    std::vector<Vec2> centre_;
    std::vector<double> area_, lit_, perimeter_;  // lit_: the area the film pulls on
    std::vector<char> model_, held_;

    std::vector<int> link_begin_;  // links of part n: [link_begin_[n], link_begin_[n + 1])
    std::vector<Link> links_;
    std::vector<Bending> bend_;

    // Support parts: the area of all the joints that rest on each. Several
    // joints onto one rod share that rod's give, in proportion to their areas.
    std::vector<double> fan_;
    std::vector<double> rod_k_;  // support parts: stiffness of everything below
    bool has_feet_ = false;      // some support stands on the model
    // By support part: the joint it stands on the model with, if its
    // stiffest way down ends on the model (-1: that way reaches the plate).
    std::vector<int> anchor_;

    // The model's own layers as a network. Every layer part is a stiff sheet
    // with three freedoms: how far it rises at the middle of the print, and
    // its two slopes (per kLen mm). Every joint between two layers is a patch
    // of resin one layer thick. The sheets of one layer that hang together
    // through the layers below are solved together, so a pull finds every way
    // through the model: down, and round through a joint at the side. A tip
    // is a spring under the sheet it touches, as stiff as its support.
    struct Group {
        int first = 0, count = 0;    // its sheets' freedoms: nodes first .. first + count
        std::vector<double> below;   // how it gives against all that is below it: for when its layer is the newest
        std::vector<double> within;  // the same with the joints above it: for when it is not
        // Turns that nothing below resists (a piece on one tip, or on tips in
        // a line): the piece pivots, and the pull follows it.
        std::vector<std::array<double, 3>> free;
    };
    struct Joint {
        int link, a, b;  // the sheet above, the sheet below (-1: the plate)
    };
    std::vector<int> node_;                   // by part: its three freedoms, or -1
    std::vector<int> node_end_;               // by layer: how many nodes there are up to and including it
    std::vector<std::vector<Group>> groups_;  // by layer
    std::vector<std::vector<Joint>> joints_;  // by the upper sheet's layer
    Vec2 origin_;                             // the middle of the model
    std::vector<char> grounded_;              // by piece (bodies_): stands on the plate somewhere

    std::vector<Body> bodies_;
    std::vector<int> body_home_;        // by model part
    std::vector<int> tips_, tip_end_;   // tips in layer order; how many there are up to each layer
    std::vector<int> plate_joints_;     // joints of the model onto the plate
    int bodyAt(int part, int k) const {
        int x = body_home_[part];
        while (bodies_[x].up >= 0 && bodies_[bodies_[x].up].born <= k) x = bodies_[x].up;
        return x;
    }
    void samplePart(int id, std::vector<Vec2>& out) const;
    // Support parts inside a plain rod (one part below, which has only this
    // one above): a load goes straight to the rod's foot.
    std::vector<int> rests_;  // by part: how many joints rest on it
    std::size_t tips_found_ = 0;
    std::vector<int> tip_group_;       // by joint: which tip it is a joint of
    std::vector<char> tip_follows_;    // by joint: not the first joint of its tip
    std::vector<double> group_holds_;  // by tip: what it holds
    std::vector<double> group_area_;   // by tip: all it touches the model with
    std::vector<int> group_low_;       // by tip: the layer of its lowest joint
    // By tip: where it acts. A tip is a pin: the joints it meets the model
    // in, layer on layer, pull at one point and cannot turn the model.
    std::vector<Vec2> group_at_;
    const Vec2& tipAt(int link) const { return group_at_[tip_group_[link]]; }
    // How much model lies under each point that a part's pull is shared out
    // from (the points samplePart gives, in that order): layers in a row.
    std::vector<std::vector<std::uint16_t>> depth_;
    // And how far each of those points is from thick model in its own part
    // (mm; none: infinite). Thin model next to thick model hangs from that.
    std::vector<std::vector<float>> away_;
    std::vector<char> head_;  // by joint: part of the head under a tip, whose strength is the tip's
    std::vector<char> sliver_;  // by joint: smaller than the smallest tip
    std::vector<int> down_, foot_, weakest_;
    std::vector<double> weakest_eff_;

    std::vector<double> pull_;
    std::vector<std::vector<Cup>> cups_;

    // A plain run of joints, one part on one part with nothing else joining
    // (a stem, a rod), breaks once, at its weakest: `run_best_` is that joint.
    std::vector<int> run_of_, run_best_;
    std::vector<char> run_contact_;  // the run ends in a tip (or is a support's foot on the model)
    std::vector<Worst> worst_;        // by link
    // By link: mm³ of the model lost with it, averaged over the replays in which it let go, and over all of them.
    std::vector<float> lost_with_, lost_per_print_;
    double tips_cost_chance_ = 0, tips_cost_volume_ = 0;
    std::vector<Seen> seen_;          // by link, then layer
    std::vector<int> worst_link_;     // by part: its most loaded joint
    mutable std::vector<float> by_layer_;  // each layer written by the one thread replaying it
    double whole_ = 1;

    StabilityResult out_;
};

double Analysis::hingeFactor(const Vec2& xy) const {
    // As the planner has it: a straight ramp across the plate, full at the far edge.
    if (cfg_.peel_bias <= 0 || norm(cfg_.peel_axis) == 0) return 1.0;
    const Vec2 a = normalized(cfg_.peel_axis);
    const double half = std::max(std::abs(a.x) * cfg_.plate_half.x + std::abs(a.y) * cfg_.plate_half.y, 1.0);
    const double t = std::clamp(dot(xy, a) / half, -1.0, 1.0);
    return 1.0 + cfg_.peel_bias * 0.5 * (t + 1.0);
}

// ---------------------------------------------------------------- parts and joints

// The print's parts. With the model's own layers given, the model's parts
// and the supports' are kept apart, and a tip is exactly the area where a
// support lies under the model. (Outlines joined into one would not do:
// where supports stand close enough to fuse into a block against the model,
// the block and the model are one outline, and the whole block would pass
// for one tip.) A support is kept whole, the part of each tip that is sunk
// into the model included: cut off at the model's surface, what is left of a
// tip there is a thin crescent that would pass for a weak neck.
void Analysis::buildParts() {
    const LayerStack& all = *in_.print;
    layers_ = static_cast<int>(all.size());
    const LayerStack* model = in_.model && in_.model->size() ? in_.model : nullptr;
    if (model && in_.model_graph) {
        if (in_.model_graph->layerCount() != model->size())
            throw std::invalid_argument("stability: the model's parts given are not its layers'");
        model_graph_ = in_.model_graph;
    } else {
        ctl_.step("parts", 0);
        model_graph_ = std::make_shared<const IslandGraph>(model ? *model : all, cfg_.link_grow);
    }
    LayerStack rest;  // the supports: each layer less the model
    if (model) {
        rest.layer_height = h_;
        rest.sample_z = all.sample_z;
        rest.slices.resize(all.size());
        parallelFor(all.size(), cfg_.threads, [&](size_t L) {
            if (L >= model->size() || model->slices[L].empty()) {
                rest.slices[L] = all.slices[L];
                return;
            }
            Paths out = difference(all.slices[L], model->slices[L]);
            // Not the shavings left where the two outlines run together.
            out.erase(std::remove_if(out.begin(), out.end(),
                                     [](const Path& path) {
                                         const double a = std::abs(area(path));
                                         return a < 1e-6 || a < 0.002 * perimeter(path);
                                     }),
                      out.end());
            rest.slices[L] = std::move(out);
        });
        ctl_.check();
        support_graph_ = std::make_shared<const IslandGraph>(in_.supports ? *in_.supports : rest, cfg_.link_grow);
    }
    const LayerStack& sup = in_.supports ? *in_.supports : rest;
    ctl_.step("parts", 1);

    const int nm = static_cast<int>(model_graph_->parts().size());
    const int ns = support_graph_ ? static_cast<int>(support_graph_->parts().size()) : 0;
    n_ = nm + ns;
    models_ = nm;
    part_.assign(n_, {});
    in_layer_.assign(layers_, {});
    model_.assign(n_, 0);
    auto take = [&](const IslandGraph& g, int first, bool is_model) {
        for (int i = 0; i < static_cast<int>(g.parts().size()); ++i) {
            const LayerPart& p = g.part(i);
            Part& q = part_[first + i];
            q.layer = p.layer;
            q.shape = &p.shape;
            q.box = p.box;
            q.parents = p.parents;
            q.children = p.children;
            for (int& x : q.parents) x += first;
            for (int& x : q.children) x += first;
            model_[first + i] = is_model;
        }
    };
    take(*model_graph_, 0, true);
    if (support_graph_) take(*support_graph_, nm, false);
    for (int L = 0; L < layers_; ++L) {
        if (static_cast<size_t>(L) < model_graph_->layerCount())
            for (int i : model_graph_->partsInLayer(static_cast<size_t>(L))) in_layer_[L].push_back(i);
        if (support_graph_ && static_cast<size_t>(L) < support_graph_->layerCount())
            for (int i : support_graph_->partsInLayer(static_cast<size_t>(L))) in_layer_[L].push_back(nm + i);
    }
    same_.assign(n_, 0);
    for (int i = 0; i < n_; ++i) same_[i] = static_cast<int>(part_[i].parents.size());
    touch_.assign(n_, {});

    centre_.assign(n_, {});
    area_.assign(n_, 0);
    lit_.assign(n_, 0);
    perimeter_.assign(n_, 0);
    const bool want_shape = cfg_.shape_effect > 0;
    // Where the model lies on a support, and where a support stands on the model.
    struct Touch {
        int child, parent;
        AreaMoments m;
    };
    std::vector<std::vector<Touch>> touches(layers_);
    auto at = [&](size_t layer, bool is_model, const Vec2& x) {
        for (int id : in_layer_[layer]) {
            if ((model_[id] != 0) != is_model) continue;
            const Part& p = part_[id];
            if (x.x < p.box.min.x || x.x > p.box.max.x || x.y < p.box.min.y || x.y > p.box.max.y) continue;
            if (p.poly().contains(x)) return id;
        }
        return -1;
    };
    parallelFor(static_cast<size_t>(layers_), cfg_.threads, [&](size_t L) {
        for (int id : in_layer_[L]) {
            const Part& p = part_[id];
            const Paths ps = p.poly().toPaths();
            const AreaMoments m = areaMoments(ps);
            area_[id] = m.area > 0 ? m.area : p.poly().area();
            centre_[id] = m.area > 0 ? m.centroid : p.box.center();
            if (want_shape)
                for (const Path& path : ps) perimeter_[id] += perimeter(path);
            // What the film pulls on: all of a model part; of a support part
            // only what lies outside the model (the rest is lit as model).
            if (model_[id] || !in_.supports) lit_[id] = area_[id];
        }
        if (!model) return;
        if (in_.supports && L < rest.slices.size())
            for (const ExPolygon& e : toExPolygons(rest.slices[L])) {
                Vec2 x;
                if (!pointInside(e, x)) continue;
                const int id = at(L, false, x);
                if (id >= 0) lit_[id] += e.area();
            }
    });
    // The supports that reach the plate through supports. The others stand
    // on the model, on a small tip of their own turned upside down.
    std::vector<char> rooted(n_, 0);
    for (int L = 0; L < layers_; ++L)
        for (int id : in_layer_[L]) {
            if (model_[id] || area_[id] < cfg_.pixel_area) continue;
            rooted[id] = L == 0;
            for (size_t j = 0; j < part_[id].parents.size() && !rooted[id]; ++j) rooted[id] = rooted[part_[id].parents[j]];
        }
    if (model) parallelFor(static_cast<size_t>(layers_), cfg_.threads, [&](size_t L) {
        if (L == 0) return;
        auto meet = [&](const Paths& above, const Paths& below, bool above_is_model) {
            if (above.empty() || below.empty()) return;
            for (const ExPolygon& e : toExPolygons(intersection(above, below))) {
                const AreaMoments m = areaMoments(e.toPaths());
                Vec2 x;
                if (!(m.area > 1e-7) || !pointInside(e, x)) continue;
                const int child = at(L, above_is_model, x), parent = at(L - 1, !above_is_model, x);
                if (child < 0 || parent < 0) continue;
                // A support that reaches the plate does not stand on the
                // model: where it lies over the model it is passing it by.
                if (!above_is_model && rooted[child]) continue;
                touches[L].push_back({child, parent, m});
            }
        };
        const Paths none;
        const Paths& sup_here = L < sup.size() ? sup.slices[L] : none;
        const Paths& sup_below = L - 1 < sup.size() ? sup.slices[L - 1] : none;
        meet(L < model->size() ? model->slices[L] : none, sup_below, true);
        meet(sup_here, L - 1 < model->size() ? model->slices[L - 1] : none, false);
    });
    // A support that stands on the model does so with the first 0.6 mm of
    // it: its tip, turned upside down and sunk in. Higher up it is passing
    // the model by like any other.
    {
        std::vector<int> start(n_, -1), touching(n_, -1);
        for (int L = 1; L < layers_; ++L) {
            for (const Touch& t : touches[L])
                if (!model_[t.child]) touching[t.child] = L;
            for (int id : in_layer_[L]) {
                if (model_[id] || rooted[id]) continue;
                int from = -1;
                for (int q : part_[id].parents)
                    if (start[q] >= 0 && (from < 0 || start[q] < from)) from = start[q];
                if (from < 0 && touching[id] == L) from = L;
                start[id] = from;
            }
            // (Nor does the model over that sunk tip hang from it: the two are one solid there.)
            auto foot = [&](int part) { return start[part] >= 0 && (layerOf(part) - start[part]) * h_ <= 0.6; };
            touches[L].erase(std::remove_if(touches[L].begin(), touches[L].end(),
                                            [&](const Touch& t) {
                                                return model_[t.child] ? !rooted[t.parent] && foot(t.parent) : !foot(t.child);
                                            }),
                             touches[L].end());
        }
    }
    for (int L = 0; L < layers_; ++L)
        for (const Touch& t : touches[L]) {
            Part& c = part_[t.child];
            size_t j = static_cast<size_t>(same_[t.child]);
            while (j < c.parents.size() && c.parents[j] != t.parent) ++j;
            if (j == c.parents.size()) {
                c.parents.push_back(t.parent);
                part_[t.parent].children.push_back(t.child);
                touch_[t.child].push_back(t.m);
                continue;
            }
            // The same two parts meeting in a second place: one joint of both.
            AreaMoments& a = touch_[t.child][j - same_[t.child]];
            const double sum = a.area + t.m.area;
            const Vec2 c2 = (a.centroid * a.area + t.m.centroid * t.m.area) / sum;
            const Vec2 da = a.centroid - c2, db = t.m.centroid - c2;
            a.sxx += a.area * da.x * da.x + t.m.sxx + t.m.area * db.x * db.x;
            a.syy += a.area * da.y * da.y + t.m.syy + t.m.area * db.y * db.y;
            a.sxy += a.area * da.x * da.y + t.m.sxy + t.m.area * db.x * db.y;
            a.centroid = c2;
            a.area = sum;
        }
    // Held when it printed: on the plate, or on something that was held. A
    // part smaller than one printer pixel is not printed at all (a corner of
    // a faceted tip, a shaving): it holds nothing and nothing holds it.
    held_.assign(n_, 0);
    for (int L = 0; L < layers_; ++L)
        for (int id : in_layer_[L]) {
            if (area_[id] < cfg_.pixel_area) continue;
            if (L == 0) {
                held_[id] = 1;
                continue;
            }
            for (int p : part_[id].parents)
                if (held_[p]) {
                    held_[id] = 1;
                    break;
                }
        }
}

void Analysis::buildLinks() {
    link_begin_.assign(n_ + 1, 0);
    for (int i = 0; i < n_; ++i) link_begin_[i + 1] = link_begin_[i] + static_cast<int>(part_[i].parents.size());
    links_.assign(link_begin_[n_], {});
    std::vector<int> geom_of(links_.size(), -1);
    int geoms = 0;
    for (int i = 0; i < n_; ++i) {
        const auto& parents = part_[i].parents;
        for (size_t j = 0; j < parents.size(); ++j) {
            Link& l = links_[link_begin_[i] + j];
            l.child = i;
            l.parent = parents[j];
            if (!held_[i] || !held_[l.parent]) l.kind = Kind::Dead;
            else if (model_[i] && model_[l.parent]) l.kind = Kind::Model;
            else if (!model_[i] && !model_[l.parent]) l.kind = Kind::Rod;
            else l.kind = model_[i] ? Kind::Tip : Kind::Foot;
            if (l.kind == Kind::Model) geom_of[link_begin_[i] + j] = geoms++;
        }
    }
    bend_.assign(geoms, {});
    parallelFor(static_cast<size_t>(layers_), cfg_.threads, [&](size_t L) {
        for (int id : in_layer_[L]) {
            const Part& p = part_[id];
            if (p.parents.empty()) continue;
            const Paths mine = p.poly().toPaths();
            for (size_t j = 0; j < p.parents.size(); ++j) {
                const int li = link_begin_[id] + static_cast<int>(j);
                Link& l = links_[li];
                if (j >= static_cast<size_t>(same_[id])) {  // model on support, or support on model: already measured
                    const AreaMoments& m = touch_[id][j - same_[id]];
                    l.area = m.area;
                    l.c = m.centroid;
                    continue;
                }
                const Paths theirs = part_[l.parent].poly().toPaths();
                Paths shared = intersection(mine, theirs);
                AreaMoments m = areaMoments(shared);
                if (!(m.area > 1e-9)) {  // joined only by the cure's reach past the outline
                    shared = intersection(mine, offset(theirs, cfg_.link_grow));
                    m = areaMoments(shared);
                }
                if (!(m.area > 1e-9)) {
                    m = AreaMoments{};
                    m.area = 1e-9;
                    m.centroid = (centre_[id] + centre_[l.parent]) * 0.5;
                }
                l.area = m.area;
                l.c = m.centroid;
                l.geom = geom_of[li];
                if (l.geom < 0) continue;
                Bending& b = bend_[l.geom];
                b.ax = axesOf(m.sxx, m.sxy, m.syy);
                for (const Path& path : shared)
                    for (const Point64& q : path) {
                        const Vec2 d = toVec(q) - m.centroid;
                        b.ext1 = std::max(b.ext1, std::abs(dot(d, b.ax.v1)));
                        b.ext2 = std::max(b.ext2, std::abs(dot(d, b.ax.v2)));
                    }
                b.sxx = m.sxx;
                b.syy = m.syy;
                b.sxy = m.sxy;
            }
        }
    });
}

// The pieces of the model that hang together through the model's own joints,
// as they join up layer by layer. Tips share a layer's pull within a piece.
void Analysis::buildBodies() {
    bodies_.clear();
    grounded_.clear();
    body_home_.assign(n_, -1);
    std::vector<int> top;
    auto find = [&](int x) {
        while (top[x] != x) x = top[x] = top[top[x]];
        return x;
    };
    std::vector<int> roots;
    for (int L = 0; L < layers_; ++L)
        for (int id : in_layer_[L]) {
            if (!held_[id] || !model_[id]) continue;
            roots.clear();
            for (int i = link_begin_[id]; i < link_begin_[id + 1]; ++i) {
                if (links_[i].kind != Kind::Model) continue;
                const int r = find(body_home_[links_[i].parent]);
                if (std::find(roots.begin(), roots.end(), r) == roots.end()) roots.push_back(r);
            }
            int mine;
            if (roots.size() == 1) {
                mine = roots[0];
            } else {
                mine = static_cast<int>(bodies_.size());
                bodies_.push_back({-1, L});
                char on_plate = L == 0;
                for (int r : roots) on_plate = on_plate || grounded_[r];
                grounded_.push_back(on_plate);
                top.push_back(mine);
                for (int r : roots) {
                    bodies_[r].up = mine;
                    top[r] = mine;
                }
            }
            body_home_[id] = mine;
        }
    // Tips, and where the model stands on the plate, in layer order.
    tips_.clear();
    plate_joints_.clear();
    tip_end_.assign(layers_, 0);
    for (int L = 1; L < layers_; ++L) {
        for (int id : in_layer_[L])
            for (int i = link_begin_[id]; i < link_begin_[id + 1]; ++i) {
                if (links_[i].kind == Kind::Tip && links_[i].k > 0) tips_.push_back(i);
                if (links_[i].kind == Kind::Model && ground(links_[i].parent)) plate_joints_.push_back(i);
            }
        tip_end_[L] = static_cast<int>(tips_.size());
    }
}

void Analysis::sizeJoints() {
    // Where a support and the model meet: a tip under the model, or the
    // small upside-down tip a support stands on the model with. Those given
    // by their makers are found by where they touch.
    std::unordered_map<std::uint64_t, std::vector<int>> grid;
    auto cell = [](double v) { return static_cast<std::int64_t>(std::floor(v)); };
    auto key = [](std::int64_t x, std::int64_t y, std::int64_t z) {
        return (static_cast<std::uint64_t>(x + 4096) << 42) ^ (static_cast<std::uint64_t>(y + 4096) << 21) ^
               static_cast<std::uint64_t>(z + 4096);
    };
    for (size_t t = 0; t < in_.tips.size(); ++t) {
        const Vec3& p = in_.tips[t].at;
        grid[key(cell(p.x), cell(p.y), cell(p.z))].push_back(static_cast<int>(t));
    }
    auto contact = [](const Link& l) { return l.kind == Kind::Tip || l.kind == Kind::Foot; };
    std::vector<int> tip_of(links_.size(), -1);
    std::vector<double> touching(in_.tips.size(), 0);
    for (size_t i = 0; i < links_.size(); ++i) {
        const Link& l = links_[i];
        if (!contact(l) || grid.empty()) continue;
        const double z = layerOf(l.child) * h_;
        double best_d = 1e300;
        const std::int64_t cx = cell(l.c.x), cy = cell(l.c.y), cz = cell(z);
        for (std::int64_t x = cx - 1; x <= cx + 1; ++x)
            for (std::int64_t y = cy - 1; y <= cy + 1; ++y)
                for (std::int64_t zz = cz - 1; zz <= cz + 1; ++zz) {
                    const auto it = grid.find(key(x, y, zz));
                    if (it == grid.end()) continue;
                    for (int t : it->second) {
                        const TipContact& tc = in_.tips[t];
                        if (tc.foot != (l.kind == Kind::Foot)) continue;  // a tip holds one way up only
                        const double dxy = distance(tc.at.xy(), l.c), dz = std::abs(tc.at.z - z);
                        if (dxy > 0.6 + tc.diameter / 2 || dz > 0.6 + h_) continue;
                        const double d = dxy * dxy + dz * dz;
                        if (d < best_d) {
                            best_d = d;
                            tip_of[i] = t;
                        }
                    }
                }
        if (tip_of[i] >= 0) touching[tip_of[i]] += l.area;
    }
    // One tip may meet the model in several joints, layer on layer. They are
    // one tip: together they hold what the tip holds, and they let go
    // together. Tips nobody told us about are told apart by their supports
    // (joints along one support are one tip) and sized by the largest of
    // their joints.
    const int given = static_cast<int>(in_.tips.size());
    tip_group_.assign(links_.size(), -1);
    tip_follows_.assign(links_.size(), 0);
    std::vector<int> ends_in(n_, -1), stands_on(n_, -1);  // by support part: the tip over it, the tip under it
    std::vector<double> top, all;  // of the tips found here: their largest joint, and all their joints together
    std::vector<int> low, high;    // and the layers they run from and to
    auto found = [&](std::vector<int>& of, int part) {
        int g = of[part];
        for (int q = 0; g < 0 && q < same_[part]; ++q) g = of[part_[part].parents[q]];
        if (g < 0) {
            g = static_cast<int>(top.size());
            top.push_back(0);
            all.push_back(0);
            low.push_back(layers_);
            high.push_back(-1);
        }
        of[part] = g;
        return g;
    };
    for (int L = 1; L < layers_; ++L)
        for (int id : in_layer_[L])
            for (int i = link_begin_[id]; i < link_begin_[id + 1]; ++i) {
                const Link& l = links_[i];
                if (!contact(l)) continue;
                if (tip_of[i] >= 0) {
                    tip_group_[i] = tip_of[i];
                    continue;
                }
                const int g = l.kind == Kind::Tip ? found(ends_in, l.parent) : found(stands_on, l.child);
                tip_group_[i] = given + g;
                top[g] = std::max(top[g], l.area);
                all[g] += l.area;
                low[g] = std::min(low[g], L);
                high[g] = std::max(high[g], L);
            }
    group_holds_.assign(given + top.size(), 0);
    group_area_.assign(given + top.size(), 0);
    std::vector<char> led(group_holds_.size(), 0);
    tips_found_ = 0;
    for (int t = 0; t < given; ++t) {
        if (!(touching[t] > 0)) continue;
        // The tip as it was made, unless it touches with clearly less than itself.
        const double d = in_.tips[t].diameter, nominal = kPi * d * d / 4;
        group_holds_[t] = tipHolds(d) * (touching[t] >= 0.75 * nominal ? 1.0 : touching[t] / nominal);
        group_area_[t] = touching[t];
        ++tips_found_;
    }
    // A tip sunk into the model shows its whole width within a few layers:
    // its largest joint is its size. A support that lies along the model for
    // longer than a tip is tall is no tip but a seam: each of its joints
    // holds by itself, with its own area.
    for (size_t g = 0; g < top.size(); ++g) {
        if ((high[g] - low[g]) * h_ > 0.6) continue;
        group_holds_[given + g] = tipHolds(2 * std::sqrt(top[g] / kPi));
        group_area_[given + g] = top[g];
        ++tips_found_;
    }
    for (size_t i = 0; i < links_.size(); ++i) {
        const int g = tip_group_[i] - given;
        if (g < 0 || !((high[static_cast<size_t>(g)] - low[static_cast<size_t>(g)]) * h_ > 0.6)) continue;
        tip_group_[i] = static_cast<int>(group_holds_.size());
        group_holds_.push_back(tipHolds(2 * std::sqrt(links_[i].area / kPi)));
        group_area_.push_back(links_[i].area);
    }
    led.assign(group_holds_.size(), 0);
    group_low_.assign(group_holds_.size(), layers_);
    for (size_t i = 0; i < links_.size(); ++i)
        if (tip_group_[i] >= 0) group_low_[tip_group_[i]] = std::min(group_low_[tip_group_[i]], layerOf(links_[i].child));
    sliver_.assign(links_.size(), 0);
    for (size_t i = 0; i < links_.size(); ++i)
        sliver_[i] = (tip_group_[i] >= 0 ? group_area_[tip_group_[i]] : links_[i].area) < kSliver;
    group_at_.assign(group_holds_.size(), Vec2{});
    {
        std::vector<double> weight(group_holds_.size(), 0);
        for (size_t i = 0; i < links_.size(); ++i) {
            if (tip_group_[i] < 0) continue;
            group_at_[tip_group_[i]] += links_[i].c * links_[i].area;
            weight[tip_group_[i]] += links_[i].area;
        }
        for (size_t g = 0; g < group_at_.size(); ++g)
            if (weight[g] > 0) group_at_[g] = group_at_[g] / weight[g];
    }
    for (size_t i = 0; i < links_.size(); ++i) {
        Link& l = links_[i];
        switch (l.kind) {
            case Kind::Dead: break;
            case Kind::Model: l.holds = bulk_ * l.area; break;
            case Kind::Rod: break;  // once the rod's lean is known (buildRods)
            case Kind::Tip:
            case Kind::Foot:
                l.holds = group_holds_[tip_group_[i]];
                tip_follows_[i] = led[tip_group_[i]];  // the first joint of a tip stands for it in the replays
                led[tip_group_[i]] = 1;
                break;
        }
    }
}

// Next to each tip, along its own support as far as that runs alone: the
// tips plate measures a tip with its head, so the head has no strength of
// its own to lose.
void Analysis::findHeads() {
    head_.assign(links_.size(), 0);
    for (const Link& tip : links_) {
        if (tip.kind == Kind::Tip) {  // down from a tip under the model
            for (int at = tip.parent; same_[at] == 1 && !head_[link_begin_[at]] && links_[link_begin_[at]].kind == Kind::Rod;) {
                const int below = links_[link_begin_[at]].parent;
                int rods = 0;
                for (int c : part_[below].children) rods += held_[c] && !model_[c];
                if (rods != 1) break;  // other supports join in here
                head_[link_begin_[at]] = 1;
                at = below;
            }
        } else if (tip.kind == Kind::Foot) {  // up from the tip a support stands on
            for (int at = tip.child;;) {
                int next = -1, rods = 0;
                for (int c : part_[at].children)
                    if (held_[c] && !model_[c]) {
                        ++rods;
                        next = c;
                    }
                if (rods != 1 || same_[next] != 1) break;  // other supports join in here
                const int i = link_begin_[next];
                if (head_[i] || links_[i].kind != Kind::Rod) break;
                head_[i] = 1;
                at = next;
            }
        }
    }
}

// How stiffly each support holds: a rod that pulls only along its length,
// down to the plate or to the model it stands on (taken as firm under it).
void Analysis::buildRods() {
    findHeads();
    fan_.assign(n_, 0);
    rests_.assign(n_, 0);
    for (const Link& l : links_) {
        if (l.kind == Kind::Dead) continue;
        ++rests_[l.parent];
        if (!model_[l.parent]) fan_[l.parent] += l.area;
    }
    // What a joint onto a support part finds under it: its share of that part's hold.
    auto under = [&](const Link& l) {
        if (ground(l.parent)) return kInf;
        return fan_[l.parent] > 0 ? rod_k_[l.parent] * (l.area / fan_[l.parent]) : 0.0;
    };
    rod_k_.assign(n_, 0);
    down_.assign(n_, -1);
    foot_.assign(n_, -1);
    weakest_.assign(n_, -1);
    weakest_eff_.assign(n_, kInf);
    has_feet_ = false;
    anchor_.assign(n_, -1);
    // Every joint spans one layer, so with one stiffness for all the resin a
    // joint's own stiffness is simply its area.
    for (int L = 1; L < layers_; ++L)
        for (int id : in_layer_[L]) {
            if (!held_[id]) continue;
            const int b0 = link_begin_[id], b1 = link_begin_[id + 1];
            if (model_[id]) {
                for (int i = b0; i < b1; ++i)
                    if (links_[i].kind == Kind::Tip) links_[i].k = inSeries(links_[i].area, under(links_[i]));
                continue;
            }
            double sum = 0;
            for (int i = b0; i < b1; ++i) {
                Link& l = links_[i];
                if (l.kind == Kind::Rod) {
                    // A leaning rod: its cut is wider than the rod, it is
                    // longer than it is tall, and only part of its pull is
                    // upward, so it gives more. Its strength is its own
                    // cross-section's: the planner's tip table does not
                    // know a head's lean, and neither does this.
                    const double t = std::min(1.0, distance(centre_[id], centre_[l.parent]) / h_);
                    const double lean = 1 + t * t;
                    l.eff = l.area / std::sqrt(lean);
                    l.holds = bulk_ * l.eff;
                    l.k = inSeries(l.area / (lean * lean), under(l));
                } else if (l.kind == Kind::Foot) {
                    l.k = l.area;
                    has_feet_ = true;
                }
                sum += l.k;
            }
            rod_k_[id] = sum;
            {
                // What it stands on: where its stiffest way down ends.
                double most = 0;
                for (int i = b0; i < b1; ++i) {
                    const Link& l = links_[i];
                    if (!(l.k > most)) continue;
                    most = l.k;
                    anchor_[id] = l.kind == Kind::Foot ? i : ground(l.parent) ? -1 : anchor_[l.parent];
                }
            }
            if (b1 - b0 == 1 && links_[b0].kind == Kind::Rod && rests_[links_[b0].parent] == 1) {
                const int p = links_[b0].parent;
                down_[id] = p;
                const double own = head_[b0] ? kInf : links_[b0].eff;
                if (down_[p] >= 0 && own > weakest_eff_[p]) {
                    weakest_eff_[id] = weakest_eff_[p];
                    weakest_[id] = weakest_[p];
                } else {
                    weakest_eff_[id] = own;
                    weakest_[id] = b0;
                }
                foot_[id] = down_[p] >= 0 ? foot_[p] : p;
            }
        }
}

// How thick the model is under each point that a part's pull is shared out
// from: for how many layers in a row, down from the part's own, there is
// model at that point. Counted on a lattice of points 0.25 mm apart.
void Analysis::buildDepths() {
    depth_.assign(static_cast<size_t>(models_), {});
    away_.assign(static_cast<size_t>(models_), {});
    BBox2 box;
    for (int id = 0; id < models_; ++id) box.extend(part_[id].box);
    if (box.empty()) return;
    const double g = 0.25;
    const Vec2 size = box.size();
    const int nx = static_cast<int>(std::ceil(size.x / g)) + 1, ny = static_cast<int>(std::ceil(size.y / g)) + 1;
    struct Run {
        int row, from, to;
    };
    const auto cells = static_cast<size_t>(nx) * static_cast<size_t>(ny);
    std::vector<std::uint16_t> deep(cells, 0);
    std::vector<int> last(cells, -2);
    std::vector<Vec2> points;
    std::vector<float> grid;
    const int chunk = 64;
    std::vector<std::vector<Run>> runs(chunk);
    for (int first = 0; first < layers_; first += chunk) {
        const int count = std::min(chunk, layers_ - first);
        // Which lattice points are in the model, layer by layer...
        parallelFor(static_cast<size_t>(count), cfg_.threads, [&](size_t c) {
            std::vector<Run>& out = runs[c];
            out.clear();
            for (int id : in_layer_[first + static_cast<int>(c)]) {
                if (!model_[id]) continue;
                const Part& p = part_[id];
                const Paths ps = p.poly().toPaths();
                const int r0 = std::max(0, static_cast<int>(std::floor((p.box.min.y - box.min.y) / g - 0.5)));
                const int r1 = std::min(ny - 1, static_cast<int>(std::ceil((p.box.max.y - box.min.y) / g - 0.5)));
                for (int r = r0; r <= r1; ++r)
                    for (const auto& [t0, t1] : lineSpans(ps, {box.min.x, box.min.y + (r + 0.5) * g}, {1.0, 0.0})) {
                        const int from = std::max(0, static_cast<int>(std::ceil(t0 / g - 0.5)));
                        const int to = std::min(nx - 1, static_cast<int>(std::floor(t1 / g - 0.5)));
                        if (to >= from) out.push_back({r, from, to});
                    }
            }
        });
        ctl_.check();
        // ...and for how many layers in a row each has been.
        for (int c = 0; c < count; ++c) {
            const int L = first + c;
            for (const Run& r : runs[static_cast<size_t>(c)])
                for (int i = r.from; i <= r.to; ++i) {
                    const size_t at = static_cast<size_t>(r.row) * static_cast<size_t>(nx) + static_cast<size_t>(i);
                    deep[at] = last[at] == L - 1 ? static_cast<std::uint16_t>(std::min(65535, deep[at] + 1)) : 1;
                    last[at] = L;
                }
            for (int id : in_layer_[L]) {
                if (!model_[id]) continue;
                samplePart(id, points);
                std::vector<std::uint16_t>& out = depth_[id];
                out.resize(points.size());
                for (size_t j = 0; j < points.size(); ++j) {
                    const int i = static_cast<int>(std::floor((points[j].x - box.min.x) / g));
                    const int r = static_cast<int>(std::floor((points[j].y - box.min.y) / g));
                    // The lattice point nearest it, or (at an edge) the deepest of those around that one.
                    std::uint16_t best = 0;
                    for (int ring = 0; ring <= 1 && best == 0; ++ring)
                        for (int dr = -ring; dr <= ring; ++dr)
                            for (int di = -ring; di <= ring; ++di) {
                                const int rr = r + dr, ii = i + di;
                                if (rr < 0 || rr >= ny || ii < 0 || ii >= nx) continue;
                                const size_t at = static_cast<size_t>(rr) * static_cast<size_t>(nx) + static_cast<size_t>(ii);
                                if (last[at] == L) best = std::max(best, deep[at]);
                            }
                    out[j] = std::max<std::uint16_t>(best, 1);
                }
                // How far each point is from thick model: across the points'
                // own grid (the one samplePart lays over the part's box).
                std::vector<float>& far = away_[id];
                far.assign(points.size(), std::numeric_limits<float>::infinity());
                const Part& p = part_[id];
                const Vec2 size = p.box.size();
                const double cell = std::clamp(std::sqrt(std::max(area_[id], 0.0)) / 12, 0.3, 1.5);
                const int gx = std::max(1, static_cast<int>(std::ceil(size.x / cell))), gy = std::max(1, static_cast<int>(std::ceil(size.y / cell)));
                const double sx = size.x / gx, sy = size.y / gy, sd = std::hypot(sx, sy);
                grid.assign(static_cast<size_t>(gx) * static_cast<size_t>(gy), std::numeric_limits<float>::infinity());
                auto cellOf = [&](const Vec2& q) {
                    const int ix = std::clamp(static_cast<int>((q.x - p.box.min.x) / (sx > 0 ? sx : 1)), 0, gx - 1);
                    const int iy = std::clamp(static_cast<int>((q.y - p.box.min.y) / (sy > 0 ? sy : 1)), 0, gy - 1);
                    return static_cast<size_t>(iy) * static_cast<size_t>(gx) + static_cast<size_t>(ix);
                };
                bool any = false;
                for (size_t j = 0; j < points.size(); ++j)
                    if (out[j] * h_ >= cfg_.slab) {
                        grid[cellOf(points[j])] = 0;
                        any = true;
                    }
                if (!any) continue;
                auto relax = [&](int ix, int iy, int dx, int dy, double step) {
                    const int jx = ix + dx, jy = iy + dy;
                    if (jx < 0 || jx >= gx || jy < 0 || jy >= gy) return;
                    float& here = grid[static_cast<size_t>(iy) * static_cast<size_t>(gx) + static_cast<size_t>(ix)];
                    const float there = grid[static_cast<size_t>(jy) * static_cast<size_t>(gx) + static_cast<size_t>(jx)] + static_cast<float>(step);
                    if (there < here) here = there;
                };
                for (int iy = 0; iy < gy; ++iy)
                    for (int ix = 0; ix < gx; ++ix) {
                        relax(ix, iy, -1, 0, sx);
                        relax(ix, iy, 0, -1, sy);
                        relax(ix, iy, -1, -1, sd);
                        relax(ix, iy, 1, -1, sd);
                    }
                for (int iy = gy - 1; iy >= 0; --iy)
                    for (int ix = gx - 1; ix >= 0; --ix) {
                        relax(ix, iy, 1, 0, sx);
                        relax(ix, iy, 0, 1, sy);
                        relax(ix, iy, 1, 1, sd);
                        relax(ix, iy, -1, 1, sd);
                    }
                for (size_t j = 0; j < points.size(); ++j) far[j] = grid[cellOf(points[j])];
            }
        }
    }
}

// The model's layers as a network (see Group), worked out once from the
// plate up: how the sheets of every layer give against all that is below
// them. Each layer's pull is then passed down and back up through it (step).
void Analysis::buildSheets() {
    BBox2 box;
    for (int id = 0; id < models_; ++id)
        if (held_[id]) box.extend(part_[id].box);
    origin_ = box.empty() ? Vec2{} : box.center();
    node_.assign(n_, -1);
    node_end_.assign(layers_, 0);
    groups_.assign(layers_, {});
    joints_.assign(layers_, {});
    // Every joint as a patch, and what is joined to each sheet from above.
    std::vector<M3> up(static_cast<size_t>(models_), M3{});
    const double s2 = kLen * kLen;
    for (const Link& l : links_) {
        if (l.kind != Kind::Model) continue;
        Bending& b = bend_[l.geom];
        b.p = patchAt(l.area, (l.c.x - origin_.x) / kLen, (l.c.y - origin_.y) / kLen, b.sxx / s2, b.syy / s2, b.sxy / s2);
        if (!ground(l.parent)) addTo(up[l.parent], b.p);
    }
    auto add = [](std::vector<double>& m, int n, int a, int b, const M3& x, double sign) {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                m[static_cast<size_t>(3 * a + r) * static_cast<size_t>(n) + static_cast<size_t>(3 * b + c)] += sign * x[r][c];
    };
    struct In {
        int a, b;  // the sheet above (within its group), the sheet below (within its group)
        const M3* p;
    };
    std::vector<int> slot(bodies_.size(), -1), slot_layer(bodies_.size(), -1);
    std::vector<int> group_of;  // by node: its group in its layer
    std::vector<std::vector<int>> sheets;
    std::vector<std::vector<In>> into;
    int nodes = 0;
    for (int L = 1; L < layers_; ++L) {
        // The layer's sheets, piece by piece (as the pieces stand with this layer on).
        sheets.clear();
        for (int id : in_layer_[L]) {
            if (!held_[id] || !model_[id]) continue;
            const int body = bodyAt(id, L);
            if (slot_layer[body] != L) {
                slot_layer[body] = L;
                slot[body] = static_cast<int>(sheets.size());
                sheets.emplace_back();
            }
            sheets[static_cast<size_t>(slot[body])].push_back(id);
        }
        std::vector<Group>& gs = groups_[L];
        gs.resize(sheets.size());
        for (size_t gi = 0; gi < sheets.size(); ++gi) {
            Group& g = gs[gi];
            g.first = nodes;
            // A piece in very many parts in one layer (a lattice) is taken as
            // one stiff sheet across them.
            const bool one = sheets[gi].size() > kMaxSheets;
            g.count = one ? 1 : static_cast<int>(sheets[gi].size());
            for (size_t i = 0; i < sheets[gi].size(); ++i) node_[sheets[gi][i]] = nodes + (one ? 0 : static_cast<int>(i));
            nodes += g.count;
            group_of.resize(static_cast<size_t>(nodes), static_cast<int>(gi));
        }
        node_end_[L] = nodes;
        const std::vector<Group>& below = groups_[L - 1];
        for (size_t gi = 0; gi < sheets.size(); ++gi) {
            Group& g = gs[gi];
            const int n = 3 * g.count;
            std::vector<double> t(static_cast<size_t>(n) * static_cast<size_t>(n), 0.0);
            into.assign(below.size(), {});
            for (int id : sheets[gi]) {
                const int a = node_[id] - g.first;
                for (int i = link_begin_[id]; i < link_begin_[id + 1]; ++i) {
                    const Link& l = links_[i];
                    if (l.kind == Kind::Tip && l.k > 0)
                        add(t, n, a, a, springAt(l.k, (tipAt(i).x - origin_.x) / kLen, (tipAt(i).y - origin_.y) / kLen), 1);
                    if (l.kind != Kind::Model) continue;
                    const M3& p = bend_[l.geom].p;
                    add(t, n, a, a, p, 1);
                    const int b = ground(l.parent) ? -1 : node_[l.parent];
                    joints_[L].push_back({i, node_[id], b});
                    if (b >= 0) {
                        const int q = group_of[static_cast<size_t>(b)];
                        into[static_cast<size_t>(q)].push_back({a, b - below[static_cast<size_t>(q)].first, &p});
                    }
                }
            }
            // Less what the layer below gives: joint · its give · joint, for
            // every two joints into the same piece of that layer.
            for (size_t q = 0; q < below.size(); ++q) {
                const std::vector<In>& in = into[q];
                if (in.empty()) continue;
                const int nq = 3 * below[q].count;
                const std::vector<double>& w = below[q].within;
                for (const In& x : in)
                    for (const In& y : in) {
                        M3 give;
                        for (int r = 0; r < 3; ++r)
                            for (int c = 0; c < 3; ++c)
                                give[r][c] = w[static_cast<size_t>(3 * x.b + r) * static_cast<size_t>(nq) + static_cast<size_t>(3 * y.b + c)];
                        add(t, n, x.a, y.a, times(times(*x.p, give), *y.p), -1);
                    }
            }
            double trace = 0;
            for (int i = 0; i < n; ++i) {
                trace += t[static_cast<size_t>(i) * static_cast<size_t>(n) + static_cast<size_t>(i)];
                for (int j = i + 1; j < n; ++j) {
                    double& x = t[static_cast<size_t>(i) * static_cast<size_t>(n) + static_cast<size_t>(j)];
                    double& y = t[static_cast<size_t>(j) * static_cast<size_t>(n) + static_cast<size_t>(i)];
                    x = y = (x + y) / 2;
                }
            }
            // How the piece as a whole is held: all its sheets of this layer
            // moving as one. A turn it is not held against is let go.
            M3 whole{};
            for (int a = 0; a < g.count; ++a)
                for (int b = 0; b < g.count; ++b)
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 3; ++c)
                            whole[r][c] += t[static_cast<size_t>(3 * a + r) * static_cast<size_t>(n) + static_cast<size_t>(3 * b + c)];
            const double scale[3] = {1, kLen, kLen};  // to mm
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) whole[r][c] *= scale[r] * scale[c];
            const Hold h = holdOf(whole);
            g.free.clear();
            if (h.k > 0) {
                if (!(h.r.l1 > h.floor)) g.free.push_back({-dot(h.r.v1, h.e), h.r.v1.x * kLen, h.r.v1.y * kLen});
                if (!(h.r.l2 > h.floor)) g.free.push_back({-dot(h.r.v2, h.e), h.r.v2.x * kLen, h.r.v2.y * kLen});
            }
            g.below = t;
            for (const auto& z : g.free) {
                const double hard = 1e3 * (trace > 0 ? trace / n : 1.0) / (z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
                for (int a = 0; a < g.count; ++a)
                    for (int b = 0; b < g.count; ++b)
                        for (int r = 0; r < 3; ++r)
                            for (int c = 0; c < 3; ++c)
                                g.below[static_cast<size_t>(3 * a + r) * static_cast<size_t>(n) + static_cast<size_t>(3 * b + c)] +=
                                    hard * z[static_cast<size_t>(r)] * z[static_cast<size_t>(c)];
            }
            invertSym(g.below, n);
            g.within = std::move(t);
            for (int id : sheets[gi]) add(g.within, n, node_[id] - g.first, node_[id] - g.first, up[id], 1);
            invertSym(g.within, n);
        }
        if ((L & 63) == 0) ctl_.check();
    }
    nodes_ = nodes;
}

void Analysis::buildPulls() {
    pull_.assign(n_, 0);
    for (int i = 0; i < n_; ++i) {
        if (!held_[i]) continue;
        double f = lit_[i] * hingeFactor(centre_[i]);
        if (cfg_.shape_effect > 0 && perimeter_[i] > 0) {
            // Pan et al. 2017: at equal area the pull rises with area ÷ outline,
            // from 58 % for a thin outline to all of it at 5 mm.
            const double fill = std::min(1.0, area_[i] / perimeter_[i] / 5.0);
            f *= 1.0 - std::min(cfg_.shape_effect, 1.0) * 0.42 * (1.0 - fill);
        }
        pull_[i] = f;
    }
    cups_.assign(layers_, {});
    if (cfg_.cup_pull <= 0) return;
    HollowReport own;
    const HollowReport* hollows = in_.hollows;
    if (!hollows) {
        own = analyzeHollows(*in_.print);
        hollows = &own;
    }
    for (int L = 1; L < layers_ && static_cast<size_t>(L) < hollows->cup_regions.size(); ++L) {
        if (hollows->cup_regions[L].empty()) continue;
        for (const ExPolygon& e : toExPolygons(hollows->cup_regions[L])) {
            const AreaMoments m = areaMoments(e.toPaths());
            Vec2 at;
            if (!(m.area > 1e-6) || !pointInside(e, at)) continue;
            // The part whose hole this is: the smallest outline around the point.
            int ring = -1;
            double ring_area = kInf;
            const Point64 q = toPoint(at);
            for (int id : in_layer_[L]) {
                const Part& p = part_[id];
                if (!held_[id] || p.poly().holes.empty()) continue;
                if (Clipper2Lib::PointInPolygon(q, p.poly().contour) == Clipper2Lib::PointInPolygonResult::IsOutside)
                    continue;
                if (p.poly().contains(at)) continue;  // in its resin, not in a hole of it
                const double outer = std::abs(area(p.poly().contour));
                if (outer < ring_area) {
                    ring_area = outer;
                    ring = id;
                }
            }
            if (ring < 0) continue;
            cups_[L].push_back({ring, cfg_.cup_pull * m.area * hingeFactor(m.centroid), m.centroid.x - centre_[ring].x,
                                m.centroid.y - centre_[ring].y});
        }
    }
}

// ---------------------------------------------------------------- one layer's pull

struct Analysis::Scratch {
    std::vector<double> f;     // support parts: the pull through them
    std::vector<int> stamp;
    int tick = 0;              // one number for every pass down the supports
    std::vector<std::vector<int>> todo;  // by layer
    std::vector<Worst> worst;
    std::vector<Seen> seen;
    // The pull each tip was given this layer.
    std::vector<double> tip_pull;
    std::vector<int> tip_stamp;
    // The same, tip by tip (a tip may have several joints).
    std::vector<double> group_pull;
    std::vector<int> group_stamp;
    // The pieces of the model that are pulled this layer, and what holds each.
    std::vector<int> body_slot, body_stamp, used;
    struct Group {
        std::vector<int> tops, tips, plate;
        Load given;       // the pulls given outright to its tips, less what the tips got: a turn at most
        double pull = 0;  // on its newest layer
    };
    std::vector<Group> groups;
    // Where a support that stands on the model pulls on it.
    struct Foot {
        int sheet;
        double f;
        Vec2 at;
    };
    std::vector<Foot> feet;
    std::vector<int> foot_links;  // the joints supports stand on the model with, as they are pulled this layer
    std::vector<Vec2> points;
    struct Reach {
        Vec2 at;
        double below, lnk;  // how far under the newest layer it touches the model (mm), ln of its stiffness
    };
    std::vector<Reach> reach;
    std::vector<double> weight;
    // What each tip's own give draws (it is a spring), as last worked out.
    std::vector<double> tip_more, group_more;
    std::vector<int> more_stamp;
    int solved = 0;
    // Where a tip's support stands on the model: how far the model rose
    // under its foot, as last worked out, and how much of that the tip's
    // spring has been let off.
    std::vector<double> tip_foot, tip_back;
    std::vector<double> load, y, z, u, t;  // the sheets' loads and moves, three numbers a sheet
    Scratch(int parts, int layers, size_t links, size_t bodies, size_t groups, int nodes)
        : f(parts, 0.0), stamp(parts, -1), todo(layers), worst(links), tip_pull(links, 0.0), tip_stamp(links, -1),
          group_pull(groups, 0.0), group_stamp(groups, -1), body_slot(bodies, -1), body_stamp(bodies, -1),
          tip_more(links, 0.0), group_more(groups, 0.0), more_stamp(groups, -1), tip_foot(links, 0.0),
          tip_back(links, 0.0), load(3 * static_cast<size_t>(nodes)),
          y(load.size()), z(load.size()), u(load.size()), t(load.size()) {}
};

// Points spread evenly over a part, for sharing its pull out: the middles of
// equal cells across its box that lie in it.
void Analysis::samplePart(int id, std::vector<Vec2>& out) const {
    out.clear();
    const Part& p = part_[id];
    const Vec2 size = p.box.size();
    const double g = std::clamp(std::sqrt(std::max(area_[id], 0.0)) / 12, 0.3, 1.5);
    const int nx = std::max(1, static_cast<int>(std::ceil(size.x / g))), ny = std::max(1, static_cast<int>(std::ceil(size.y / g)));
    for (int iy = 0; iy < ny; ++iy)
        for (int ix = 0; ix < nx; ++ix) {
            const Vec2 q{p.box.min.x + (ix + 0.5) * size.x / nx, p.box.min.y + (iy + 0.5) * size.y / ny};
            if (p.poly().contains(q)) out.push_back(q);
        }
    if (out.empty()) out.push_back(centre_[id]);
}

void Analysis::step(int k, Scratch& st) const {
    float layer_worst = 0;
    bool counting = true;
    auto record = [&](int link, double u, double load, double stress, RiskCause cause, double near = 0) {
        if (!counting) return;
        const auto uf = static_cast<float>(u);
        // (A head's strength is its tip's. What goes through it is kept only
        // to be passed down its rod afterwards. And a sliver where two
        // outlines barely meet does not speak for the layer.)
        if (!head_[link]) {
            if (uf > layer_worst && !sliver_[link]) layer_worst = uf;
            if (uf >= seen_floor_) st.seen.push_back({link, k, uf});
        }
        Worst& w = st.worst[link];
        if (uf > w.u) {
            w.u = uf;
            w.load = static_cast<float>(load);
            w.stress = static_cast<float>(stress);
            w.near = static_cast<float>(near);
            w.step = k;
            w.cause = cause;
        }
    };
    auto intoRod = [&](int n, double f) {
        if (f == 0 || ground(n)) return;
        if (st.stamp[n] != st.tick) {
            st.stamp[n] = st.tick;
            st.f[n] = 0;
            st.todo[layerOf(n)].push_back(n);
        }
        st.f[n] += f;
    };
    // What a tip is given outright (below), joint by joint and tip by tip.
    auto giveTip = [&](int i, double f) {
        if (st.tip_stamp[i] != k) {
            st.tip_stamp[i] = k;
            st.tip_pull[i] = 0;
        }
        st.tip_pull[i] += f;
        const int tip = tip_group_[i];
        if (st.group_stamp[tip] != k) {
            st.group_stamp[tip] = k;
            st.group_pull[tip] = 0;
        }
        st.group_pull[tip] += f;
    };
    // All a tip's joint is pulled by: what it was given, and what its own give draws.
    auto pullOf = [&](int i) { return (st.tip_stamp[i] == k ? st.tip_pull[i] : 0.0) + st.tip_more[i]; };

    // ---- the pieces of the model this layer pulls on, and what holds each
    st.used.clear();
    auto group = [&](int body) -> Scratch::Group& {
        if (st.body_stamp[body] != k) {
            st.body_stamp[body] = k;
            st.body_slot[body] = static_cast<int>(st.used.size());
            st.used.push_back(body);
            if (st.groups.size() < st.used.size()) st.groups.emplace_back();
            Scratch::Group& g = st.groups[st.used.size() - 1];
            g.tops.clear();
            g.tips.clear();
            g.plate.clear();
            g.given = {};
            g.pull = 0;
        }
        return st.groups[st.body_slot[body]];
    };
    // What holds the pieces from slot `from` on: their tips, and where they stand on the plate.
    auto gather = [&](size_t from) {
        if (st.used.size() <= from || k < 1) return;
        auto slotOf = [&](int part) {
            const int body = bodyAt(part, k);
            return st.body_stamp[body] == k && static_cast<size_t>(st.body_slot[body]) >= from ? st.body_slot[body] : -1;
        };
        for (int t = 0; t < tip_end_[k]; ++t)
            if (const int slot = slotOf(links_[tips_[t]].child); slot >= 0) {
                st.groups[slot].tips.push_back(tips_[t]);
                st.tip_more[tips_[t]] = st.tip_foot[tips_[t]] = st.tip_back[tips_[t]] = 0;
            }
        for (int i : plate_joints_)
            if (const int slot = slotOf(links_[i].child); slot >= 0) st.groups[slot].plate.push_back(i);
    };
    for (int id : in_layer_[k])
        if (held_[id] && model_[id]) group(bodyAt(id, k)).tops.push_back(id);
    gather(0);

    // ---- thin, fresh model: held by the tips near each patch
    //
    // A stiff piece passes a pull to all that holds it, each hold taking
    // what its stiffness draws; that is worked out further down, through
    // the model's joints. A thin slab cannot: it bends, and a patch of it is
    // held by the tips near that patch. So where the model under a patch is
    // thin and a tip is within a fresh skin's reach of it, the patch's pull
    // is given outright to the tips near it: all of it where the model is a
    // layer thick and the tip close by, half at `slab` mm thick, half at
    // that reach, and soon nothing beyond either. (And where thick model of
    // the same part is nearer than any tip, the patch hangs from that
    // instead.) Among those tips it is
    // shared within a length that grows with the thickness, as a plate on
    // springs spreads a load (Hetényi 1946): `share_reach` mm at 1 mm thick,
    // with its 3/4 power. Within that length the slab is taken as stiff, so
    // an off-centre patch still loads the nearer tips more.
    const bool stiff = !(cfg_.share_reach < 1e5);
    const double reach2 = cfg_.skin_half_reach * cfg_.skin_half_reach;
    for (size_t gi = 0; gi < st.used.size(); ++gi) {
        Scratch::Group& g = st.groups[gi];
        if (g.tips.empty()) continue;  // stands on the plate alone, or on nothing
        st.reach.clear();
        for (int i : g.tips) st.reach.push_back({tipAt(i), (k - group_low_[tip_group_[i]] + 1) * h_, std::log(links_[i].k)});
        for (int i : g.plate) st.reach.push_back({links_[i].c, k * h_, std::log(links_[i].area)});
        const size_t nt = g.tips.size(), nh = st.reach.size();
        st.weight.assign(nh, 0);
        // A pull at x, where the model straight under it is `solid` mm thick
        // and thick model of the same part is `away` mm off.
        auto share = [&](const Vec2& x, double pull, double solid, double away) {
            // Only what touches the underside of this same slab can hold a
            // patch of it. A hold further under the newest layer than the
            // slab is thick here, allowing for the tip's own depth and for
            // an underside that drops at 45° on the way to it, is under
            // something else.
            auto under = [&](size_t j, double d2) {
                return stiff || st.reach[j].below <= solid + 0.6 + std::sqrt(d2);
            };
            double inv = 0;
            if (!stiff) {
                double nearest = kInf;
                for (size_t j = 0; j < nt; ++j) {
                    const double d2 = norm2(st.reach[j].at - x);
                    if (d2 < nearest && under(j, d2)) nearest = d2;
                }
                if (!(nearest < kInf)) return;
                const double thick = solid / std::max(cfg_.slab, 1e-6), far = nearest / (reach2 > 0 ? reach2 : 1e-12);
                pull /= (1 + thick * thick * thick * thick) * (1 + far * far);
                // Thin model beside thick model hangs from whichever is
                // nearer, that or a tip.
                if (away < kInf) pull *= away * away * away * away / (away * away * away * away + nearest * nearest);
                if (!(pull > 0) && !(pull < 0)) return;
                const double len = std::max(0.3, cfg_.share_reach * std::pow(solid, 0.75));
                inv = 1 / (3.2 * len * len);
            }
            double top = -kInf;
            for (size_t j = 0; j < nh; ++j) {
                const Scratch::Reach& r = st.reach[j];
                const double d2 = norm2(r.at - x);
                st.weight[j] = under(j, d2) ? r.lnk - d2 * inv : -kInf;
                top = std::max(top, st.weight[j]);
            }
            if (!(top > -kInf)) return;
            M3 hold{};
            for (size_t j = 0; j < nh; ++j) {
                const double w = st.weight[j] - top > -25 ? std::exp(st.weight[j] - top) : 0.0;
                st.weight[j] = w;
                if (w == 0) continue;
                const Vec2 d = st.reach[j].at - x;
                if (j < nt) {
                    addTo(hold, springAt(w, d.x, d.y));
                } else {  // the plate holds over an area: it does not let the piece turn on it
                    const Link& l = links_[g.plate[j - nt]];
                    const Bending& b = bend_[l.geom];
                    const double per = w / l.area;
                    addTo(hold, patchAt(w, d.x, d.y, b.sxx * per, b.syy * per, b.sxy * per));
                }
            }
            Move mv;
            if (!settle(hold, {1, 0, 0}, mv)) return;
            g.given.n += pull;
            g.given.mx += pull * (x.x - origin_.x);
            g.given.my += pull * (x.y - origin_.y);
            for (size_t j = 0; j < nt; ++j) {
                if (st.weight[j] == 0) continue;
                const Vec2 d = st.reach[j].at - x;
                const double f = pull * st.weight[j] * (mv.w + mv.gx * d.x + mv.gy * d.y);
                giveTip(g.tips[j], f);
                g.given.n -= f;
                g.given.mx -= f * (st.reach[j].at.x - origin_.x);
                g.given.my -= f * (st.reach[j].at.y - origin_.y);
            }
        };
        for (int id : g.tops) {
            samplePart(id, st.points);
            const double each = pull_[id] / static_cast<double>(st.points.size());
            for (size_t j = 0; j < st.points.size(); ++j) share(st.points[j], each, depth_[id][j] * h_, away_[id][j]);
        }
        // A cup pulls on the ring around it: as thick as that is at its thickest.
        for (const Cup& c : cups_[k])
            if (model_[c.part] && st.body_stamp[bodyAt(c.part, k)] == k &&
                static_cast<size_t>(st.body_slot[bodyAt(c.part, k)]) == gi)
                share({centre_[c.part].x + c.dx, centre_[c.part].y + c.dy}, c.force,
                      *std::max_element(depth_[c.part].begin(), depth_[c.part].end()) * h_, kInf);
    }

    // ---- down through the supports
    //
    // What a tip is pulled by goes down its support, by each rod's stiffness
    // where rods branch, to the plate or to the model the support stands on.
    // Where supports stand on the model this is run twice: first without
    // keeping score, to learn what they bring into the model; and at the
    // end, for the record.
    auto supports = [&](bool final) {
        counting = final;
        ++st.tick;
        for (int id : in_layer_[k])
            if (held_[id] && !model_[id]) intoRod(id, pull_[id]);
        for (const Cup& c : cups_[k])
            if (!model_[c.part]) intoRod(c.part, c.force);
        for (size_t gi = 0; gi < st.used.size(); ++gi)
            for (int i : st.groups[gi].tips) intoRod(links_[i].parent, pullOf(i));
        for (int L = k; L >= 1; --L) {
            std::vector<int>& todo = st.todo[L];
            for (size_t t = 0; t < todo.size(); ++t) {
                const int id = todo[t];
                const int b0 = link_begin_[id], b1 = link_begin_[id + 1];
                const double f = st.f[id];
                if (down_[id] >= 0) {
                    // The top of a plain rod: everything that came into it here
                    // (a tip, the branches that join in it) goes straight down
                    // to its foot. Noted where it enters and at the rod's
                    // weakest joint below; the joints between are filled in
                    // afterwards (finishParts).
                    if (f > 0) {
                        record(b0, f / links_[b0].holds, f, f / links_[b0].eff, RiskCause::Support);
                        if (weakest_[id] != b0) {
                            const Link& l = links_[weakest_[id]];
                            record(weakest_[id], f / l.holds, f, f / l.eff, RiskCause::Support);
                        }
                    }
                    intoRod(foot_[id], f);
                    continue;
                }
                const double sum = rod_k_[id];
                if (!(sum > 0)) continue;
                for (int i = b0; i < b1; ++i) {
                    const Link& l = links_[i];
                    if (!(l.k > 0)) continue;
                    const double part = f * l.k / sum;
                    if (l.kind == Kind::Rod) {
                        if (part > 0) record(i, part / l.holds, part, part / l.eff, RiskCause::Support);
                        intoRod(l.parent, part);
                    } else if (l.kind == Kind::Foot) {
                        if (final) {
                            if (st.tip_stamp[i] != k) st.foot_links.push_back(i);
                            giveTip(i, part);
                        } else if (part != 0 && !ground(l.parent)) {
                            st.feet.push_back({l.parent, part, tipAt(i)});
                        }
                    }
                }
            }
            todo.clear();
        }
        st.todo[0].clear();
        counting = true;
    };

    // ---- through the model
    //
    // What pulls on the model's sheets: the film on the newest layer (and
    // the suction of a cup), a support standing on the model, and, the other
    // way, each tip with what it was given outright above. Whatever that
    // leaves is carried by the model's joints to all that holds the piece,
    // by stiffness: the plate, and the tips as springs.
    const int nn = k >= 1 ? node_end_[k] : 0;
    bool loaded = false;
    std::fill(st.load.begin(), st.load.begin() + 3 * static_cast<std::ptrdiff_t>(nn), 0.0);
    auto pullOn = [&](int part, double f, const Vec2& at) {
        const int a = node_[part];
        if (a < 0 || f == 0) return;
        double* y = &st.load[3 * static_cast<size_t>(a)];
        y[0] += f;
        y[1] += f * (at.x - origin_.x) / kLen;
        y[2] += f * (at.y - origin_.y) / kLen;
        loaded = true;
    };
    for (size_t gi = 0; gi < st.used.size(); ++gi) {
        Scratch::Group& g = st.groups[gi];
        for (int id : g.tops) {
            pullOn(id, pull_[id], centre_[id]);
            g.pull += pull_[id];
        }
        for (int i : g.tips)
            if (st.tip_stamp[i] == k) pullOn(links_[i].child, -st.tip_pull[i], tipAt(i));
        // What was given outright, patch by patch, is all of those patches'
        // pull but not always where they pull: a patch on one tip turns
        // about it. The turn that is left over is let go, as it is for a
        // piece that pivots: the pull follows it. (On the plate, the plate
        // takes it.)
        if (grounded_[st.used[gi]] || !(g.pull > 0)) continue;
        if (std::abs(g.given.mx) + std::abs(g.given.my) <= 1e-12 * g.pull) continue;
        for (int id : g.tops) {
            double* y = &st.load[3 * static_cast<size_t>(node_[id])];
            y[1] -= pull_[id] / g.pull * g.given.mx / kLen;
            y[2] -= pull_[id] / g.pull * g.given.my / kLen;
        }
    }
    for (const Cup& c : cups_[k])
        if (model_[c.part]) pullOn(c.part, c.force, {centre_[c.part].x + c.dx, centre_[c.part].y + c.dy});

    auto times3 = [](const M3& p, const double* v, double* out) {
        for (int r = 0; r < 3; ++r) out[r] += p[r][0] * v[0] + p[r][1] * v[1] + p[r][2] * v[2];
    };
    auto solveWith = [](const std::vector<double>& inv, int n, const double* in, double* out) {
        for (int i = 0; i < n; ++i) {
            const double* row = &inv[static_cast<size_t>(i) * static_cast<size_t>(n)];
            double v = 0;
            for (int j = 0; j < n; ++j) v += row[j] * in[j];
            out[i] = v;
        }
    };
    // How every sheet moves under those loads, and what each tip's give draws.
    auto solve = [&]() {
        std::copy_n(st.load.begin(), 3 * static_cast<std::ptrdiff_t>(nn), st.y.begin());
        // Up from the plate: each layer's sheets pass what they carry to the layer above...
        for (int L = 1; L <= k; ++L) {
            if (L == k) {
                // The newest layer. Where the piece is free to turn, the pull follows the turn.
                for (const Group& g : groups_[L]) {
                    if (g.free.empty()) continue;
                    const size_t at = 3 * static_cast<size_t>(g.first);
                    double all = 0;
                    for (int a = 0; a < g.count; ++a) all += st.t[at + 3 * static_cast<size_t>(a)];
                    for (const auto& z : g.free) {
                        double turn = 0;
                        for (int a = 0; a < g.count; ++a)
                            for (size_t r = 0; r < 3; ++r) turn += z[r] * st.y[at + 3 * static_cast<size_t>(a) + r];
                        for (int a = 0; a < g.count; ++a) {
                            const double w = all > 0 ? st.t[at + 3 * static_cast<size_t>(a)] / all : 1.0 / g.count;
                            st.y[at + 3 * static_cast<size_t>(a) + 1] -= w * turn * z[1] / (kLen * kLen);
                            st.y[at + 3 * static_cast<size_t>(a) + 2] -= w * turn * z[2] / (kLen * kLen);
                        }
                    }
                }
            }
            for (const Group& g : groups_[L])
                solveWith(L == k ? g.below : g.within, 3 * g.count, &st.y[3 * static_cast<size_t>(g.first)],
                          &st.z[3 * static_cast<size_t>(g.first)]);
            if (L == k) break;
            for (const Joint& j : joints_[L + 1])
                if (j.b >= 0)
                    times3(bend_[links_[j.link].geom].p, &st.z[3 * static_cast<size_t>(j.b)], &st.y[3 * static_cast<size_t>(j.a)]);
        }
        // ...and back down.
        for (const Group& g : groups_[k])
            std::copy_n(&st.z[3 * static_cast<size_t>(g.first)], 3 * g.count, &st.u[3 * static_cast<size_t>(g.first)]);
        for (int L = k - 1; L >= 1; --L) {
            if (groups_[L].empty()) continue;
            const size_t from = 3 * static_cast<size_t>(node_end_[L - 1]), to = 3 * static_cast<size_t>(node_end_[L]);
            std::fill(st.t.begin() + static_cast<std::ptrdiff_t>(from), st.t.begin() + static_cast<std::ptrdiff_t>(to), 0.0);
            for (const Joint& j : joints_[L + 1])
                if (j.b >= 0)
                    times3(bend_[links_[j.link].geom].p, &st.u[3 * static_cast<size_t>(j.a)], &st.t[3 * static_cast<size_t>(j.b)]);
            for (const Group& g : groups_[L]) {
                const size_t at = 3 * static_cast<size_t>(g.first);
                solveWith(g.within, 3 * g.count, &st.t[at], &st.u[at]);
                for (int i = 0; i < 3 * g.count; ++i) st.u[at + static_cast<size_t>(i)] += st.z[at + static_cast<size_t>(i)];
            }
        }
        // A tip is a spring: it pulls back by its stiffness times how far its
        // sheet rose there (less what its spring was let off, below).
        ++st.solved;
        auto rise = [&](int part, const Vec2& at) {
            const int a = node_[part];
            if (a < 0) return 0.0;
            const double* u = &st.u[3 * static_cast<size_t>(a)];
            return u[0] + u[1] * (at.x - origin_.x) / kLen + u[2] * (at.y - origin_.y) / kLen;
        };
        for (size_t gi = 0; gi < st.used.size(); ++gi)
            for (int i : st.groups[gi].tips) {
                const Link& l = links_[i];
                const int on = anchor_[l.parent];
                st.tip_foot[i] = on >= 0 ? rise(links_[on].parent, tipAt(on)) : 0.0;
                const double f = l.k * (rise(l.child, tipAt(i)) - st.tip_back[i]);
                st.tip_more[i] = f;
                const int tip = tip_group_[i];
                if (st.more_stamp[tip] != st.solved) {
                    st.more_stamp[tip] = st.solved;
                    st.group_more[tip] = 0;
                }
                st.group_more[tip] += f;
            }
    };
    if (loaded) {
        // (How much of the newest layer's pull is on each of its sheets, for where a piece is free to turn.)
        std::fill(st.t.begin() + 3 * static_cast<std::ptrdiff_t>(node_end_[k - 1]), st.t.begin() + 3 * static_cast<std::ptrdiff_t>(nn), 0.0);
        for (int id : in_layer_[k])
            if (node_[id] >= 0) st.t[3 * static_cast<size_t>(node_[id])] += pull_[id];
        solve();
    }
    st.feet.clear();
    st.foot_links.clear();
    if (has_feet_) {
        // In the network each tip's spring is fixed at its far end. Where a
        // support stands on the model, that end rose with the model under
        // its foot: the spring is stretched only by how much further the tip
        // rose. (A strut between two parts of one piece is not a way to the
        // plate.) So let each such spring off by its foot's rise, and solve
        // again.
        bool struts = false;
        for (size_t gi = 0; gi < st.used.size() && loaded; ++gi)
            for (int i : st.groups[gi].tips) {
                if (st.tip_foot[i] == 0) continue;
                st.tip_back[i] = st.tip_foot[i];
                pullOn(links_[i].child, links_[i].k * st.tip_back[i], tipAt(i));
                struts = true;
            }
        if (struts) solve();
        supports(false);
        if (!st.feet.empty()) {
            // A support may stand on a piece that nothing else pulls on this layer.
            const size_t before = st.used.size();
            for (const Scratch::Foot& ft : st.feet) group(bodyAt(ft.sheet, k));
            gather(before);
            if (!loaded) std::fill(st.t.begin() + 3 * static_cast<std::ptrdiff_t>(node_end_[k - 1]), st.t.begin() + 3 * static_cast<std::ptrdiff_t>(nn), 0.0);
            for (const Scratch::Foot& ft : st.feet) pullOn(ft.sheet, ft.f, ft.at);
            if (loaded) solve();
        }
    }
    if (loaded) {
        // What each joint carries: its patch times how much further the sheet
        // above it moved than the sheet below.
        for (int L = 1; L <= k; ++L)
            for (const Joint& j : joints_[L]) {
                const Link& l = links_[j.link];
                const Bending& b = bend_[l.geom];
                const double* ua = &st.u[3 * static_cast<size_t>(j.a)];
                double d[3] = {ua[0], ua[1], ua[2]};
                if (j.b >= 0)
                    for (int r = 0; r < 3; ++r) d[r] -= st.u[3 * static_cast<size_t>(j.b) + static_cast<size_t>(r)];
                double q[3] = {0, 0, 0};
                times3(b.p, d, q);
                // About the shared area's own centre: a straight pull and a bending.
                const Vec2 mom{q[1] * kLen - q[0] * (l.c.x - origin_.x), q[2] * kLen - q[0] * (l.c.y - origin_.y)};
                const double straight = q[0] / l.area;
                double bent = 0;
                if (b.ax.l1 > 1e-14) bent += std::abs(dot(mom, b.ax.v1)) / b.ax.l1 * b.ext1;
                if (b.ax.l2 > 1e-14) bent += std::abs(dot(mom, b.ax.v2)) / b.ax.l2 * b.ext2;
                const double stress = straight + bent;
                if (stress > 1e-9 * bulk_)
                    record(j.link, stress / bulk_, q[0], stress, bent > std::abs(straight) ? RiskCause::Bend : RiskCause::Neck);
            }
    }

    // ---- down the supports for the record, and how close every tip is
    supports(true);
    auto note = [&](int i, double own, bool under_model) {
        // All of a tip's joints' pull against what the tip holds.
        const int tip = tip_group_[i];
        const double given = st.group_stamp[tip] == k ? st.group_pull[tip] : 0.0;
        const double whole = given + (st.more_stamp[tip] == st.solved && loaded ? st.group_more[tip] : 0.0);
        if (whole > 0) record(i, whole / links_[i].holds, whole, own / links_[i].area, RiskCause::Tip, under_model ? given : 0.0);
    };
    for (size_t gi = 0; gi < st.used.size(); ++gi)
        for (int i : st.groups[gi].tips) note(i, pullOf(i), true);
    for (int i : st.foot_links) note(i, st.tip_pull[i], false);
    by_layer_[k] = layer_worst;  // one writer per layer: each layer is one thread's
}

void Analysis::replayPulls() {
    by_layer_.assign(layers_, 0);
    worst_.assign(links_.size(), {});
    const unsigned threads = std::max(1u, std::min<unsigned>(resolveThreads(cfg_.threads), static_cast<unsigned>(std::max(layers_, 1))));
    std::mutex merge;
    runThreads(threads, [&](unsigned t, const std::atomic<bool>& stop) {
        Scratch st(n_, layers_, links_.size(), bodies_.size(), group_holds_.size(), nodes_);
        for (int k = static_cast<int>(t); k < layers_; k += static_cast<int>(threads)) {
            if (stop.load()) return;
            if (t == 0 && (k & 31) == 0) ctl_.step("stress", static_cast<double>(k) / std::max(layers_, 1));
            else if ((k & 31) == 0) ctl_.check();
            step(k, st);
        }
        // The worst of all layers, the earliest layer first among equals, so
        // the answer does not depend on how many threads there were.
        std::lock_guard<std::mutex> lock(merge);
        for (size_t i = 0; i < worst_.size(); ++i) {
            const Worst& w = st.worst[i];
            Worst& g = worst_[i];
            if (w.step < 0) continue;
            if (w.u > g.u || (w.u == g.u && (g.step < 0 || w.step < g.step))) g = w;
        }
        seen_.insert(seen_.end(), st.seen.begin(), st.seen.end());
    });
    std::sort(seen_.begin(), seen_.end(), [](const Seen& a, const Seen& b) {
        return a.link != b.link ? a.link < b.link : a.step < b.step;
    });
    ctl_.step("stress", 1);
}

void Analysis::finishParts() {
    // Down each plain rod: what entered it higher up went through every joint below.
    for (int L = layers_ - 1; L >= 1; --L)
        for (int id : in_layer_[L]) {
            const int p = down_[id];
            if (p < 0 || down_[p] < 0) continue;
            const Worst& above = worst_[link_begin_[id]];
            if (above.step < 0) continue;
            const int j = link_begin_[p];
            const auto u = static_cast<float>(above.load / links_[j].holds);
            if (u > worst_[j].u) {
                Worst& w = worst_[j];
                w.u = u;
                w.load = above.load;
                w.stress = static_cast<float>(above.load / links_[j].eff);
                w.near = 0;
                w.step = above.step;
                w.cause = RiskCause::Support;
            }
        }
    out_.parts.assign(n_, {});
    worst_link_.assign(n_, -1);
    for (int id = 0; id < n_; ++id) {
        PartRisk& r = out_.parts[id];
        r.model = model_[id] != 0;
        r.held = held_[id] != 0;
        if (!r.held) {
            r.cause = RiskCause::Unheld;
            r.lost_chance = 1;
            continue;
        }
        int head = -1;  // a joint in a tip's head: shown for what goes through it, never as a weak one
        for (int i = link_begin_[id]; i < link_begin_[id + 1]; ++i) {
            const Worst& w = worst_[i];
            if (w.step < 0) continue;
            if (head_[i]) {
                if (head < 0 || w.load > worst_[head].load) head = i;
                continue;
            }
            if (!(w.u > r.closeness)) continue;
            r.closeness = w.u;
            r.load = w.load;
            r.stress = w.stress;
            r.at_layer = w.step;
            r.cause = w.cause;
            r.joint = links_[i].c;
            r.joint_area = static_cast<float>(links_[i].kind == Kind::Rod ? links_[i].eff : links_[i].area);
            worst_link_[id] = i;
        }
        if (worst_link_[id] < 0 && head >= 0) {
            r.load = worst_[head].load;
            r.stress = worst_[head].stress;
            r.at_layer = worst_[head].step;
            r.joint = links_[head].c;
            r.joint_area = static_cast<float>(links_[head].eff);
        }
        r.break_chance = static_cast<float>(breakChance(r.closeness, steep_));
    }
}

// One strength for a plain run of joints, not one per layer, so the answer
// does not depend on the layer height.
void Analysis::findRuns() {
    std::vector<int> live(n_, 0), only(n_, -1);
    for (int id = 0; id < n_; ++id)
        for (int i = link_begin_[id]; i < link_begin_[id + 1]; ++i)
            if (links_[i].kind != Kind::Dead) {
                ++live[id];
                only[id] = i;
            }
    run_of_.assign(links_.size(), -1);
    run_best_.clear();
    run_contact_.clear();
    for (int L = 1; L < layers_; ++L)
        for (int id : in_layer_[L])
            for (int i = link_begin_[id]; i < link_begin_[id + 1]; ++i) {
                const Link& l = links_[i];
                if (l.kind == Kind::Dead || !(l.holds > 0) || head_[i]) continue;
                const bool contact = l.kind == Kind::Tip || l.kind == Kind::Foot;
                int run = -1;
                // Going on from the joint below: the same kind of joint, or a
                // tip on top of its own head (what a tip holds is measured
                // with its head, so the two are one piece of strength).
                if ((l.kind == Kind::Model || l.kind == Kind::Rod) && live[id] == 1 && live[l.parent] == 1 &&
                    rests_[l.parent] == 1 && links_[only[l.parent]].kind == l.kind)
                    run = run_of_[only[l.parent]];
                if (run < 0) {
                    run = static_cast<int>(run_best_.size());
                    run_best_.push_back(i);
                    run_contact_.push_back(contact);
                } else {
                    if (worst_[i].u > worst_[run_best_[run]].u) run_best_[run] = i;
                    run_contact_[run] = run_contact_[run] || contact;
                }
                run_of_[i] = run;
            }
}

// ---------------------------------------------------------------- fresh skins

void Analysis::findSkins() {
    out_.skins.assign(layers_, {});
    const double chances[] = {0.01, 0.05, 0.2, 0.5, 0.9};
    double reach[5];
    for (int j = 0; j < 5; ++j)
        reach[j] = cfg_.skin_half_reach * std::pow(-std::log2(1 - chances[j]), 1 / (2 * steep_));
    if (!(cfg_.skin_half_reach > 0)) return;
    parallelFor(static_cast<size_t>(layers_), cfg_.threads, [&](size_t L) {
        if (L == 0) return;
        for (int id : in_layer_[L]) {
            if (!held_[id] || !model_[id]) continue;
            const Part& p = part_[id];
            Paths below;
            for (int q : p.parents) {
                const Paths ps = part_[q].poly().toPaths();
                below.insert(below.end(), ps.begin(), ps.end());
            }
            const Paths mine = p.poly().toPaths();
            const Paths fresh = difference(mine, below);
            if (std::abs(area(fresh)) < 0.05) continue;
            // A point further than `reach` from what is below lies on a way
            // through fresh skin at least that long.
            if (norm(bbox(fresh).size()) < reach[0]) continue;
            for (int j = 0; j < 5; ++j) {
                Paths grown;
                for (int q : p.parents) {
                    const Paths g = grownBy(part_[q].poly(), reach[j]);
                    grown.insert(grown.end(), g.begin(), g.end());
                }
                Paths far = difference(mine, grown);
                far = offset(offset(far, -0.002), 0.002);  // slivers
                if (std::abs(area(far)) < 1e-3) break;
                out_.skins[L].push_back({id, static_cast<float>(chances[j]), std::move(far)});
            }
        }
        if ((L & 63) == 0) ctl_.check();
    });
    ctl_.step("skins", 1);
}

// ---------------------------------------------------------------- the replays

void Analysis::runReplays() {
    whole_ = 1;
    lost_with_.assign(links_.size(), 0.0f);
    lost_per_print_.assign(links_.size(), 0.0f);
    tips_cost_chance_ = tips_cost_volume_ = 0;
    const int replays = std::max(cfg_.replays, 0);
    out_.stats.replays = replays;

    // What can break: every joint between a support and the model, and any
    // other joint loaded enough to have a chance worth counting.
    std::vector<Breakable> cand;
    std::vector<int> cand_of(links_.size(), -1);
    auto add = [&](int i) {
        if (cand_of[i] >= 0) return;
        const Link& l = links_[i];
        cand_of[i] = static_cast<int>(cand.size());
        // Of a tip's joints the first stands for the tip; the others only go with it.
        cand.push_back({i, l.child, l.parent, tip_follows_[i] ? 0.0 : l.holds});
    };
    for (size_t run = 0; run < run_best_.size(); ++run)
        if (run_contact_[run] || worst_[run_best_[run]].u >= floor_u_) add(run_best_[run]);
    // A part that hangs on several joints: when one of them can go, the
    // others get its load, so they can go too, where that load is anything
    // to them. (To the joint between two whole layers of the model it is
    // nothing: that joint stays, and the model stays one piece there.)
    {
        const size_t first = cand.size();
        std::vector<double> could(n_, 0);  // by part: the most its joints that can go were ever pulled by
        for (size_t c = 0; c < first; ++c) could[cand[c].child] += std::abs(worst_[cand[c].link].load);
        for (size_t c = 0; c < first; ++c) {
            const int child = cand[c].child;
            for (int i = link_begin_[child]; i < link_begin_[child + 1]; ++i) {
                const Link& l = links_[i];
                if (l.kind == Kind::Dead || !(l.holds > 0) || head_[i]) continue;
                if (worst_[i].u + could[child] / l.holds >= floor_u_) add(i);
            }
        }
    }
    std::stable_sort(cand.begin(), cand.end(), [](const Breakable& a, const Breakable& b) { return a.link < b.link; });
    for (size_t c = 0; c < cand.size(); ++c) cand_of[cand[c].link] = static_cast<int>(c);
    const int nc = static_cast<int>(cand.size());
    // The joints of each tip, to let go together.
    std::vector<std::vector<int>> of_tip(group_holds_.size());
    for (int c = 0; c < nc; ++c)
        if (tip_group_[cand[c].link] >= 0) of_tip[tip_group_[cand[c].link]].push_back(c);

    // The pieces that hang together through everything else, as they join up.
    std::vector<Piece> pieces(1);  // 0: the plate
    std::vector<int> top{0}, home(n_, -1);
    auto find = [&](int x) {
        while (top[x] != x) x = top[x] = top[top[x]];
        return x;
    };
    for (int L = 0; L < layers_; ++L)
        for (int id : in_layer_[L]) {
            if (!held_[id]) continue;
            int roots[8];
            int nroots = 0;
            std::vector<int> more;
            auto add = [&](int r) {
                for (int i = 0; i < nroots; ++i)
                    if (roots[i] == r) return;
                for (int m : more)
                    if (m == r) return;
                if (nroots < 8) roots[nroots++] = r;
                else more.push_back(r);
            };
            if (L == 0) add(find(0));
            for (int i = link_begin_[id]; i < link_begin_[id + 1]; ++i) {
                if (links_[i].kind == Kind::Dead || cand_of[i] >= 0) continue;
                add(find(home[links_[i].parent]));
            }
            int mine;
            if (nroots == 0) {
                mine = static_cast<int>(pieces.size());
                pieces.push_back({-1, L, {}, {}});
                top.push_back(mine);
            } else if (nroots == 1 && more.empty()) {
                mine = roots[0];
            } else {
                mine = static_cast<int>(pieces.size());
                Piece m{-1, L, {}, {}};
                for (int i = 0; i < nroots; ++i) m.kids.push_back(roots[i]);
                m.kids.insert(m.kids.end(), more.begin(), more.end());
                pieces.push_back(std::move(m));
                top.push_back(mine);
                for (int kid : pieces[mine].kids) {
                    pieces[kid].up = mine;
                    top[kid] = mine;
                }
            }
            home[id] = mine;
            pieces[mine].parts.push_back(id);
        }
    // The piece something belonged to while layer k printed.
    auto climb = [&](int x, int k) {
        while (pieces[x].up >= 0 && pieces[pieces[x].up].born <= k) x = pieces[x].up;
        return x;
    };

    if (replays == 0 || nc == 0) {
        for (int id = 0; id < n_; ++id) out_.parts[id].lost_chance = held_[id] ? 0.0f : 1.0f;
        return;
    }

    // Each breakable joint's load through the print: the layers at which it reached a new high.
    std::vector<int> high_begin(nc + 1, 0);
    std::vector<High> highs;
    {
        size_t i = 0;  // seen_ and cand are both in joint order
        for (int c = 0; c < nc; ++c) {
            high_begin[c] = static_cast<int>(highs.size());
            while (i < seen_.size() && seen_[i].link < cand[c].link) ++i;
            double top = 0;
            for (; i < seen_.size() && seen_[i].link == cand[c].link; ++i)
                if (seen_[i].u > top) {
                    top = seen_[i].u;
                    highs.push_back({seen_[i].step, top});
                }
        }
        high_begin[nc] = static_cast<int>(highs.size());
    }
    constexpr int kNever = std::numeric_limits<int>::max();
    // The first layer, from `from` on, at which joint c is loaded past `limit`.
    auto firstOver = [&](int c, double limit, int from) {
        if (limit < 0) return from;  // what was moved onto it is already too much
        for (int e = high_begin[c]; e < high_begin[c + 1]; ++e)
            if (highs[e].u > limit) return std::max(highs[e].step, from);
        return kNever;
    };
    // Its own load by the time layer k prints.
    auto loadBy = [&](int c, int k) {
        double top = 0;
        for (int e = high_begin[c]; e < high_begin[c + 1] && highs[e].step <= k; ++e) top = highs[e].u;
        return top;
    };

    std::vector<int> counts(n_, 0), lost_stamp(n_, -1), lost_list;
    std::vector<double> xi(nc), moved_on(nc);
    std::vector<int> when(nc);
    std::vector<char> broken(nc), gone(nc), listed(nc);
    std::vector<int> node_of(pieces.size(), -1), node_stamp(pieces.size(), -1);
    int event = 0, whole = 0;

    struct Edge {
        int to, cand;
        bool up;  // `to` is the piece resting on this one
    };
    std::vector<std::vector<Edge>> adj;
    std::vector<int> node_piece, queue, due;
    std::vector<char> reached, upstream;
    struct Fall {
        int piece, k, by;  // the layer printing when it fell, and the joint whose letting go dropped it
    };
    std::vector<Fall> fallen;
    std::vector<std::pair<int, double>> takers;
    // By joint: the replays in which it let go, and the model that went with it then (mm³, summed).
    std::vector<int> let_go(nc, 0), lost_by;
    std::vector<double> went_with(nc, 0);
    int tips_cost = 0;          // replays in which part of the model went with tips
    double tips_cost_sum = 0;   // and how much, summed over those

    for (int r = 0; r < replays; ++r) {
        if ((r & 15) == 0) ctl_.step("replays", static_cast<double>(r) / replays);
        Rng rng(cfg_.seed * 0x9E3779B97F4A7C15ULL + static_cast<std::uint64_t>(r) * 0xD1B54A32D192ED03ULL + 1);
        due.clear();
        for (int c = 0; c < nc; ++c) {
            // A strength with the curve's own spread, 1 at the middle.
            xi[c] = std::pow(-std::log2(1 - rng.unit()), 1 / steep_);
            moved_on[c] = 0;
            broken[c] = gone[c] = 0;
            when[c] = cand[c].holds > 0 ? firstOver(c, xi[c], 0) : kNever;
            listed[c] = when[c] != kNever;
            if (listed[c]) due.push_back(c);
        }
        fallen.clear();

        for (;;) {
            // The next joint to go, the earliest layer first.
            int x = -1;
            for (int c : due)
                if (!broken[c] && !gone[c] && when[c] != kNever &&
                    (x < 0 || when[c] < when[x] || (when[c] == when[x] && c < x)))
                    x = c;
            if (x < 0) break;
            const int k = when[x];
            broken[x] = 1;
            ++let_go[x];
            if (tip_group_[cand[x].link] >= 0)
                for (int c : of_tip[tip_group_[cand[x].link]]) broken[c] = 1;
            // The pieces as they stand while layer k prints, and what still joins them.
            ++event;
            adj.clear();
            node_piece.clear();
            auto node = [&](int piece) {
                if (node_stamp[piece] != event) {
                    node_stamp[piece] = event;
                    node_of[piece] = static_cast<int>(node_piece.size());
                    node_piece.push_back(piece);
                    adj.emplace_back();
                }
                return node_of[piece];
            };
            const int plate = node(climb(0, k));
            for (int c = 0; c < nc; ++c) {
                if (broken[c] || gone[c] || layerOf(cand[c].child) > k) continue;
                const int a = node(climb(home[cand[c].child], k)), b = node(climb(home[cand[c].parent], k));
                if (a == b) continue;
                adj[a].push_back({b, c, false});
                adj[b].push_back({a, c, true});
            }
            const int above = node(climb(home[cand[x].child], k)), under = node(climb(home[cand[x].parent], k));
            if (above == under) continue;  // the piece holds together around it
            reached.assign(node_piece.size(), 0);
            queue.assign(1, plate);
            reached[plate] = 1;
            for (size_t q = 0; q < queue.size(); ++q)
                for (const Edge& e : adj[queue[q]])
                    if (!reached[e.to]) {
                        reached[e.to] = 1;
                        queue.push_back(e.to);
                    }
            bool fell = false;
            for (size_t v = 0; v < node_piece.size(); ++v) {
                if (reached[v]) continue;
                fell = true;
                fallen.push_back({node_piece[v], k, x});
                for (const Edge& e : adj[v]) gone[e.cand] = 1;
            }
            if (fell || !reached[above]) continue;
            // Still held another way. The pull came down through this joint
            // from the piece above it and from whatever rests on that piece;
            // the other joints leading down out of those pieces take it over,
            // in proportion to what each was carrying.
            upstream.assign(node_piece.size(), 0);
            queue.assign(1, above);
            upstream[above] = 1;
            for (size_t q = 0; q < queue.size(); ++q)
                for (const Edge& e : adj[queue[q]])
                    if (e.up && !upstream[e.to] && e.to != plate) {
                        upstream[e.to] = 1;
                        queue.push_back(e.to);
                    }
            auto carries = [&](int c) { return (loadBy(c, k) + moved_on[c]) * cand[c].holds; };
            const double moved = carries(x);
            if (!(moved > 0)) continue;
            // In proportion to what each was carrying, and, as a layer's pull
            // is shared, more to the ones near the joint that went.
            const Vec2 here = links_[cand[x].link].c;
            const double over = (k - layerOf(cand[x].child) + 1) * h_;
            const double len = std::max(0.3, cfg_.share_reach * std::pow(over, 0.75));
            const double inv = cfg_.share_reach < 1e5 ? 1 / (3.2 * len * len) : 0.0;
            takers.clear();
            double top = -kInf;
            for (int v : queue)
                for (const Edge& e : adj[v]) {
                    if (e.up || upstream[e.to] || !(cand[e.cand].holds > 0)) continue;
                    const double w = std::log(std::max(carries(e.cand), 1e-9 * cand[e.cand].holds)) -
                                     norm2(links_[cand[e.cand].link].c - here) * inv;
                    takers.emplace_back(e.cand, w);
                    top = std::max(top, w);
                }
            double sum = 0;
            for (auto& [c, w] : takers) {
                w = w - top > -25 ? std::exp(w - top) : 0.0;
                sum += w;
            }
            if (!(sum > 0)) continue;
            for (const auto& [c, w] : takers) {
                if (w == 0) continue;
                moved_on[c] += moved * (w / sum) / cand[c].holds;
                when[c] = firstOver(c, xi[c] - moved_on[c], k);
                if (when[c] != kNever && !listed[c]) {
                    listed[c] = 1;
                    due.push_back(c);
                }
            }
        }

        // Everything in a fallen piece, then whatever would have printed on it alone.
        lost_list.clear();
        lost_by.clear();
        for (const Fall& f : fallen) {
            const int k = f.k;
            queue.assign(1, f.piece);
            for (size_t q = 0; q < queue.size(); ++q) {
                const Piece& pc = pieces[queue[q]];
                for (int id : pc.parts) {
                    if (layerOf(id) > k) break;
                    if (lost_stamp[id] != r) {
                        lost_stamp[id] = r;
                        lost_list.push_back(id);
                        lost_by.push_back(f.by);
                    }
                }
                queue.insert(queue.end(), pc.kids.begin(), pc.kids.end());
            }
        }
        double model_lost = 0, with_tips = 0;  // mm³
        for (size_t q = 0; q < lost_list.size(); ++q) {
            const int id = lost_list[q];
            if (model_[id]) {
                model_lost += area_[id] * h_;
                went_with[lost_by[q]] += area_[id] * h_;
                if (tip_group_[cand[lost_by[q]].link] >= 0) with_tips += area_[id] * h_;
            }
            ++counts[id];
            for (int child : part_[id].children) {
                if (lost_stamp[child] == r || !held_[child]) continue;
                // Still on something: a part that is there, through a joint that has not let go.
                bool any = false;
                for (int i = link_begin_[child]; i < link_begin_[child + 1] && !any; ++i) {
                    const int p = links_[i].parent;
                    any = held_[p] && lost_stamp[p] != r && !(cand_of[i] >= 0 && broken[cand_of[i]]);
                }
                if (!any) {
                    lost_stamp[child] = r;
                    lost_list.push_back(child);
                    lost_by.push_back(lost_by[q]);  // what printed on it goes with the same joint
                }
            }
        }
        whole += model_lost < kWorthLosing;
        if (with_tips >= kCountsAsLost) {
            ++tips_cost;
            tips_cost_sum += with_tips;
        }
    }
    whole_ = static_cast<double>(whole) / replays;
    tips_cost_chance_ = static_cast<double>(tips_cost) / replays;
    tips_cost_volume_ = tips_cost > 0 ? tips_cost_sum / tips_cost : 0.0;
    for (int id = 0; id < n_; ++id)
        out_.parts[id].lost_chance = held_[id] ? static_cast<float>(static_cast<double>(counts[id]) / replays) : 1.0f;
    for (int c = 0; c < nc; ++c) {
        if (let_go[c] > 0) lost_with_[cand[c].link] = static_cast<float>(went_with[c] / let_go[c]);
        lost_per_print_[cand[c].link] = static_cast<float>(went_with[c] / replays);
    }
    ctl_.step("replays", 1);
}

// ---------------------------------------------------------------- what to look at

// What would be lost if a joint let go: the part half a millimetre up from
// it, not the first sliver of a layer over it.
double Analysis::lostAbove(int part) const {
    const int up = std::max(1, static_cast<int>(std::lround(0.5 / h_)));
    for (int n = 0; n < up; ++n) {
        int next = -1;
        for (int c : part_[part].children)
            if (held_[c] && model_[c] == model_[part] && (next < 0 || area_[c] > area_[next])) next = c;
        if (next < 0) break;
        part = next;
    }
    return static_cast<double>(out_.parts[part].lost_chance);
}

void Analysis::findSpots() {
    std::vector<WeakSpot> all;
    // Joints with a chance worth showing: the weakest of each run, and of
    // several of one kind within reach the worst stands for all.
    std::vector<int> order;
    for (int i : run_best_)
        if (worst_[i].step >= 0 && breakChance(worst_[i].u, steep_) >= kListFrom && !tip_follows_[i])
            order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        if (worst_[a].u != worst_[b].u) return worst_[a].u > worst_[b].u;
        return a < b;
    });
    for (int i : order) {
        const Link& l = links_[i];
        const Worst& w = worst_[i];
        const Vec3 at{l.c.x, l.c.y, layerOf(l.child) * h_};
        // (A tip is all its joints.)
        const double touches = tip_group_[i] >= 0 ? group_area_[tip_group_[i]] : l.area;
        // A joint smaller than the smallest tip, with nothing at risk above
        // it, is a sliver where two outlines barely meet: it may snap and
        // nothing comes of it.
        const double lost = lostAbove(l.child);
        if (touches < kSliver && lost < kListFrom) {
            ++out_.stats.slivers;
            continue;
        }
        auto kin = [](RiskCause a, RiskCause b) {
            auto support = [](RiskCause c) { return c == RiskCause::Tip || c == RiskCause::Support; };
            auto model = [](RiskCause c) { return c == RiskCause::Neck || c == RiskCause::Bend; };
            return a == b || (support(a) && support(b)) || (model(a) && model(b));
        };
        bool merged = false;
        for (WeakSpot& s : all)
            if (kin(s.cause, w.cause) && distance(s.at, at) < 1.5) {
                ++s.count;
                merged = true;
                break;
            }
        if (merged) continue;
        WeakSpot s;
        // Hanging by a sliver of contact is, in plain words, hardly being held.
        s.cause = tip_group_[i] >= 0 && touches < kSliver ? RiskCause::Unheld : w.cause;
        s.at = at;
        s.layer = layerOf(l.child);
        s.part = l.child;
        s.chance = breakChance(w.u, steep_);
        s.lost_chance = lost;
        s.closeness = w.u;
        s.area = l.kind == Kind::Rod ? l.eff : touches;
        s.load = std::abs(w.load);
        s.near = w.near;
        s.holds = l.holds;
        s.lost_volume = lost_with_[i];
        s.at_layer = w.step;
        all.push_back(s);
    }
    // Parts that start in mid-air.
    std::vector<char> counted(n_, 0);
    std::vector<int> stack;
    for (int id = 0; id < n_; ++id) {
        // Only where it starts: what prints on it is unheld with it.
        if (held_[id] || !model_[id]) continue;
        bool on_unheld = false;
        for (int p : part_[id].parents) on_unheld = on_unheld || !held_[p];
        if (on_unheld) continue;
        if (area_[id] < cfg_.speck_area) {
            ++out_.stats.unheld_specks;
            continue;
        }
        ++out_.stats.unheld;
        WeakSpot s;
        s.cause = RiskCause::Unheld;
        s.at = {centre_[id].x, centre_[id].y, layerOf(id) * h_};
        s.layer = layerOf(id);
        s.part = id;
        s.chance = s.lost_chance = 1;
        s.area = area_[id];
        s.at_layer = layerOf(id);
        // All that prints on it and on nothing else goes with it.
        stack.assign(1, id);
        counted[id] = 1;
        while (!stack.empty()) {
            const int at = stack.back();
            stack.pop_back();
            s.lost_volume += area_[at] * h_;
            for (int c : part_[at].children)
                if (!held_[c] && model_[c] && !counted[c]) {
                    counted[c] = 1;
                    stack.push_back(c);
                }
        }
        all.push_back(s);
    }
    // Skins more likely than not to fail: one per part.
    for (int L = 0; L < layers_; ++L) {
        const auto& bands = out_.skins[L];
        for (size_t i = 0; i < bands.size(); ++i) {
            if (bands[i].chance < 0.5f) continue;
            // The last band of this part (the highest chance).
            size_t j = i;
            while (j + 1 < bands.size() && bands[j + 1].part == bands[i].part) ++j;
            const AreaMoments m = areaMoments(bands[i].region);
            WeakSpot s;
            s.cause = RiskCause::Skin;
            s.at = {m.centroid.x, m.centroid.y, L * h_};
            s.layer = L;
            s.part = bands[i].part;
            s.chance = bands[j].chance;
            s.lost_chance = lostAbove(bands[i].part);
            s.area = m.area;
            s.at_layer = L;
            all.push_back(s);
            i = j;
        }
    }
    // What would lose part of the print first, then the likeliest.
    std::stable_sort(all.begin(), all.end(), [](const WeakSpot& a, const WeakSpot& b) {
        const double la = a.chance * a.lost_chance, lb = b.chance * b.lost_chance;
        if (la != lb) return la > lb;
        if (a.chance != b.chance) return a.chance > b.chance;
        if (a.area != b.area) return a.area > b.area;
        return a.at.z < b.at.z;
    });
    out_.spots = std::move(all);
}

// Every tip that touches the model, with the most it was ever pulled by.
void Analysis::listTips() {
    const int given = static_cast<int>(in_.tips.size());
    std::vector<int> lead(group_holds_.size(), -1);
    for (size_t i = 0; i < links_.size(); ++i)
        if (tip_group_[i] >= 0 && !tip_follows_[i] && lead[tip_group_[i]] < 0) lead[tip_group_[i]] = static_cast<int>(i);
    for (size_t g = 0; g < lead.size(); ++g) {
        const int i = lead[g];
        if (i < 0) continue;  // a tip that was given, but touches nothing
        const Link& l = links_[i];
        const Worst& w = worst_[i];
        TipRisk t;
        t.foot = l.kind == Kind::Foot;
        if (static_cast<int>(g) < given) {
            t.contact = static_cast<int>(g);
            t.at = in_.tips[g].at;
        } else {
            t.at = {group_at_[g].x, group_at_[g].y, group_low_[g] * h_};
        }
        t.area = group_area_[g];
        t.holds = l.holds;
        t.lost_chance = lostAbove(l.child);
        t.lost_volume = lost_with_[i];
        t.lost_per_print = lost_per_print_[i];
        if (w.step >= 0) {
            t.closeness = w.u;
            t.chance = breakChance(w.u, steep_);
            t.load = std::abs(w.load);
            t.near = w.near;
            t.at_layer = w.step;
        }
        out_.tips.push_back(t);
    }
    std::stable_sort(out_.tips.begin(), out_.tips.end(), [](const TipRisk& a, const TipRisk& b) {
        if (a.at.z != b.at.z) return a.at.z < b.at.z;
        if (a.at.x != b.at.x) return a.at.x < b.at.x;
        return a.at.y < b.at.y;
    });
}

void Analysis::summarise() {
    StabilityStats& s = out_.stats;
    s.layers = static_cast<size_t>(layers_);
    s.parts = static_cast<size_t>(n_);
    s.whole_chance = whole_;
    s.tips_cost_chance = tips_cost_chance_;
    s.tips_cost_volume = tips_cost_volume_;
    const double limits[4] = {0.01, 0.05, 0.2, 0.5};
    s.tips = tips_found_;
    for (float u : by_layer_) s.worst_closeness = std::max<double>(s.worst_closeness, u);
    for (int L = 0; L < layers_; ++L) {
        const auto& bands = out_.skins[L];
        size_t bi = 0;
        for (int id : in_layer_[L]) {
            if (!model_[id]) continue;
            ++s.model_parts;
            const double vol = area_[id] * h_, pc = out_.partChance(id);
            s.model_volume += vol;
            // This part's skins: nested regions of rising chance, kept in the layer's part order.
            while (bi < bands.size() && bands[bi].part < id) ++bi;
            const size_t b0 = bi;
            size_t b1 = bi;
            while (b1 < bands.size() && bands[b1].part == id) ++b1;
            double skin_loss = 0;
            for (size_t b = b0; b < b1; ++b) {
                const double a = std::abs(area(bands[b].region));
                const double a_next = b + 1 < b1 ? std::abs(area(bands[b + 1].region)) : 0;
                skin_loss += std::max(0.0, a - a_next) * h_ * (1 - pc) * bands[b].chance;
            }
            s.expected_loss += vol * pc + skin_loss;
            for (int t = 0; t < 4; ++t) {
                if (pc >= limits[t]) {
                    s.at_risk[t] += vol;
                    continue;
                }
                for (size_t b = b0; b < b1; ++b)
                    if (1 - (1 - pc) * (1 - bands[b].chance) >= limits[t]) {
                        s.at_risk[t] += std::abs(area(bands[b].region)) * h_;
                        break;
                    }
            }
            bi = b1;
        }
    }
}

StabilityResult Analysis::run() {
    const auto t0 = std::chrono::steady_clock::now();
    buildParts();
    ctl_.check();
    buildLinks();
    sizeJoints();
    buildRods();
    buildBodies();
    buildDepths();
    buildSheets();
    buildPulls();
    ctl_.check();
    replayPulls();
    finishParts();
    findRuns();
    findSkins();
    runReplays();
    out_.model_graph = model_graph_;
    out_.support_graph = support_graph_;
    out_.in_layer = in_layer_;
    out_.where.resize(n_);
    for (int i = 0; i < n_; ++i) out_.where[i] = {part_[i].layer, part_[i].box, area_[i], part_[i].shape};
    out_.layer_height = h_;
    out_.closeness_by_layer = by_layer_;
    findSpots();
    listTips();
    summarise();
    out_.stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return std::move(out_);
}

}  // namespace

StabilityConfig stabilityConfig(const SupportConfig& cfg) {
    StabilityConfig s;
    s.capacity = cfg.capacity;
    s.peel_bias = cfg.peel_bias;
    s.peel_axis = cfg.peel_axis;
    s.plate_half = cfg.plate_half;
    s.link_grow = cfg.link_grow;
    s.speck_area = cfg.absorb_island_area;
    // A fitted table is 0.7 of the load at which half the tips let go. The
    // built-in table has no such load behind it: its numbers are borrowed.
    // They are taken here as a third of that load, a usual margin for a
    // brittle material, and still cautious beside published strengths of
    // resin that was not post-cured (which would put it at 4 to 40 times).
    if (cfg.capacity.table.empty()) s.usable_fraction = kBorrowedMargin;
    // The planner's tip reach on a flat face is a reach it trusts in the same
    // way. A skin's stress goes with the square of its reach, so the reach at
    // which half fail is that one over the root of the trusted share. The
    // job's own spacing choice does not change what the resin spans.
    CoverageModel reach = cfg.coverage;
    reach.density = 1.0;
    s.skin_half_reach = reach.radius(0) / std::sqrt(s.usable_fraction);
    return s;
}

double breakChance(double closeness, double steepness) {
    if (!(closeness > 0)) return 0;
    return 1 - std::exp2(-std::pow(closeness, std::max(steepness, 1.0)));
}

const char* causeName(RiskCause c) {
    switch (c) {
        case RiskCause::None: return "none";
        case RiskCause::Unheld: return "unheld";
        case RiskCause::Skin: return "skin";
        case RiskCause::Tip: return "tip";
        case RiskCause::Neck: return "neck";
        case RiskCause::Bend: return "bend";
        case RiskCause::Support: return "support";
    }
    return "none";
}

int StabilityResult::partAt(std::size_t layer, const Vec2& xy) const {
    if (layer >= in_layer.size()) return -1;
    for (int id : in_layer[layer]) {
        const PrintPart& p = where[static_cast<size_t>(id)];
        if (xy.x < p.box.min.x || xy.x > p.box.max.x || xy.y < p.box.min.y || xy.y > p.box.max.y) continue;
        if (p.shape->contains(xy)) return id;
    }
    return -1;
}

double StabilityResult::partChance(int part) const {
    if (part < 0 || static_cast<size_t>(part) >= parts.size()) return 0;
    return parts[part].held ? parts[part].lost_chance : 1.0;
}

double StabilityResult::chanceAt(std::size_t layer, const Vec2& xy) const {
    const int part = partAt(layer, xy);
    if (part < 0) return 0;
    const double pc = partChance(part);
    double skin = 0;
    if (layer < skins.size())
        for (const SkinBand& b : skins[layer])
            if (b.part == part && b.chance > skin && inPaths(b.region, xy)) skin = b.chance;
    return 1 - (1 - pc) * (1 - skin);
}

double StabilityResult::breakAt(std::size_t layer, const Vec2& xy) const {
    const int part = partAt(layer, xy);
    return part < 0 ? 0.0 : parts[part].break_chance;
}

StabilityResult analyzeStability(const StabilityInput& in, const StabilityConfig& cfg, const JobControl& ctl) {
    Analysis a(in, cfg, ctl);
    return a.run();
}

}  // namespace fruit
