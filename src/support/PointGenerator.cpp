#include "support/PointGenerator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <queue>
#include <sstream>
#include <unordered_map>

#include "layers/IslandGraph.h"
#include "layers/LayerStack.h"
#include "mesh/AabbTree.h"
#include "mesh/MeshAnalysis.h"

namespace fruit {

namespace {

constexpr double kInward = 0.01;   // mm: nudge endpoints into the solid for through-material tests
constexpr double kEdgeInset = 0.02; // mm: edge samples sit just inside the face

struct Sample {
    Vec3 pos, normal;
    int layer = 0, part = -1;
    bool island = false;
    bool excluded = false;
    bool needs_cover = true;  // false: only a place where capacity tips may go
    int tip_class = 0;
    double cost = 1.0;
    int tip = -1;  // tip placed on this sample, if any
};

struct Tip {
    Vec3 pos, normal;
    int layer = 0, part = -1;
    int tip_class = -1;  // -1 for manual tips with a custom diameter
    double diameter = 0.4;
    PointCategory cat = PointCategory::Overhang;
    bool pinned = false;
    double cost = 1.0;
    double thickness = 0;
    bool removed = false;
};

// Hash grid over XY. Iteration order is deterministic (cells in fixed order,
// insertion order inside a cell).
class XYGrid {
public:
    explicit XYGrid(double cell) : cell_(cell) {}
    void insert(int id, const Vec2& p) { map_[key(ix(p.x), ix(p.y))].push_back(id); }
    template <class F>
    void query(const Vec2& p, double r, F&& f) const {
        const int64_t x0 = ix(p.x - r), x1 = ix(p.x + r), y0 = ix(p.y - r), y1 = ix(p.y + r);
        for (int64_t x = x0; x <= x1; ++x)
            for (int64_t y = y0; y <= y1; ++y) {
                auto it = map_.find(key(x, y));
                if (it == map_.end()) continue;
                for (int id : it->second) f(id);
            }
    }

private:
    int64_t ix(double v) const { return static_cast<int64_t>(std::floor(v / cell_)); }
    static uint64_t key(int64_t x, int64_t y) {
        return (static_cast<uint64_t>(x + (1LL << 31)) << 32) ^ static_cast<uint64_t>(y + (1LL << 31));
    }
    double cell_;
    std::unordered_map<uint64_t, std::vector<int>> map_;
};

class Generator {
public:
    Generator(const TriangleMesh& mesh, const SupportConfig& cfg, const PointGenOptions& opts)
        : mesh_(mesh),
          cfg_(cfg),
          opts_(opts),
          tip_grid_(std::max(cfg.coverage.maxRadius(), 1.0)),
          sample_grid_(std::max(cfg.sample_min_spacing, 0.05)) {}

    PointGenResult run();

private:
    // ---- setup / geometry helpers
    void setup();
    double capacityOf(const Tip& t) const { return cfg_.capacity.capacity(t.diameter); }
    bool segmentInside(const Vec3& a, const Vec3& na, const Vec3& b, const Vec3& nb) const;
    bool covers(const Tip& t, const Sample& s) const;
    bool coveredByExisting(const Sample& s) const;
    void project(const Vec2& xy, double z, Vec3& pos, Vec3& normal) const;
    int classFor(double max_diameter, int preferred) const;
    double peelFactor(const Vec2& xy) const;

    // ---- samples and tips
    int makeSample(const Vec2& xy, int layer, int part, bool island, bool decimate = true,
                   bool needs_cover = true, double spacing = -1);
    void sampleRegion(const Paths& region, int layer, int part, bool island, std::vector<int>& out);
    int addTip(int sample, PointCategory cat, bool pinned);
    int addTipAt(const Vec3& pos, const Vec3& normal, int layer, int part, int cls, PointCategory cat, bool pinned);

    // ---- per layer
    void processIsland(int part, std::vector<int>& layer_samples,
                       std::vector<std::pair<int, std::vector<int>>>& large_islands);
    void processOverhang(int part, std::vector<int>& layer_samples);
    Paths anchorFor(int part) const;
    bool mergesSoon(int part, int layers) const;
    void coverLayer(const std::vector<int>& layer_samples);
    void enforceIslandMinimum(int part, const std::vector<int>& island_samples);
    void enforceCapacity(int layer);
    void stabilize(int layer);

    // ---- union-find over parts
    int find(int a) {
        while (uf_[a] != a) a = uf_[a] = uf_[uf_[a]];
        return a;
    }
    void unite(int a, int b);

    // ---- final passes
    void finalize();
    void warn(const std::string& code, const std::string& msg, int layer, const Vec3& where) {
        result_.warnings.push_back({code, msg, layer, where});
    }

    const TriangleMesh& mesh_;
    const SupportConfig& cfg_;
    const PointGenOptions& opts_;
    AabbTree tree_;
    LayerStack layers_;
    IslandGraph graph_;
    SharpEdgeIndex edges_;
    CostField costf_;
    std::vector<MinimumRegion> minima_;
    double s_off_ = 0;
    int anchor_layers_ = 1;

    std::vector<Sample> samples_;
    std::vector<Tip> tips_;
    XYGrid tip_grid_, sample_grid_;

    struct Comp {
        double capacity = 0;
        std::vector<int> tips;
        std::vector<int> pool;  // samples where extra tips may go
        double last_stab_z = -1e9;
        double last_warn_z = -1e9;
        bool exempt = false;  // skipped tiny island not yet merged: no capacity demand
    };
    std::vector<int> uf_;
    std::vector<Comp> comp_;
    std::vector<std::vector<int>> manual_by_layer_;
    std::vector<int> tips_on_part_;
    std::vector<int> skipped_islands_;
    PointGenResult result_;
};

// ---------------------------------------------------------------- helpers

bool Generator::segmentInside(const Vec3& a, const Vec3& na, const Vec3& b, const Vec3& nb) const {
    const Vec3 pa = a - na * kInward, pb = b - nb * kInward;
    const Vec3 d = pb - pa;
    const double len = norm(d);
    if (len < 1e-6) return true;
    return !tree_.anyHit(pa, d, len - 1e-7, 1e-7);
}

bool Generator::covers(const Tip& t, const Sample& s) const {
    if (t.removed) return false;
    if (t.pos.z > s.pos.z + 1e-6) return false;
    const double dz = s.pos.z - t.pos.z;
    const double r = cfg_.coverage.radius(std::min(dz, t.thickness));
    if (norm2((s.pos - t.pos).xy()) > r * r) return false;
    return segmentInside(t.pos, t.normal, s.pos, s.normal);
}

bool Generator::coveredByExisting(const Sample& s) const {
    bool hit = false;
    tip_grid_.query(s.pos.xy(), cfg_.coverage.maxRadius(), [&](int ti) {
        if (!hit && covers(tips_[ti], s)) hit = true;
    });
    return hit;
}

void Generator::project(const Vec2& xy, double z, Vec3& pos, Vec3& normal) const {
    const Vec3 q{xy.x, xy.y, z};
    const ClosestPoint cp = tree_.closest(q);
    if (cp.face >= 0 && cp.distance < 0.3) {
        pos = cp.point;
        normal = mesh_.faceNormal(cp.face);
    } else {
        pos = q;
        normal = {0, 0, -1};
    }
}

int Generator::classFor(double max_diameter, int preferred) const {
    int best = 0;
    for (int c = 0; c < static_cast<int>(cfg_.tip_classes.size()); ++c)
        if (c <= preferred && cfg_.tip_classes[c].diameter <= max_diameter + 1e-9) best = c;
    return best;
}

double Generator::peelFactor(const Vec2& xy) const {
    if (cfg_.peel_bias <= 0 || norm(cfg_.peel_axis) == 0) return 1.0;
    // Linear ramp across the plate: +bias at the far (high-stress) side.
    const double half = 100.0;
    const double t = std::clamp(dot(xy, normalized(cfg_.peel_axis)) / half, -1.0, 1.0);
    return 1.0 + cfg_.peel_bias * 0.5 * (t + 1.0);
}

// ---------------------------------------------------------------- samples

int Generator::makeSample(const Vec2& xy, int layer, int part, bool island, bool decimate, bool needs_cover,
                          double spacing) {
    Sample s;
    project(xy, layers_.layerBottom(layer), s.pos, s.normal);
    if (s.normal.z > (needs_cover ? 0.2 : -0.05)) return -1;  // not a downward-facing surface
    if (decimate) {
        bool close = false;
        const double sp = spacing > 0 ? spacing : cfg_.sample_min_spacing;
        const double d2 = sp * sp;
        sample_grid_.query(s.pos.xy(), sp, [&](int si) {
            // Surfaces that need coverage are only thinned against each other,
            // never against coarse capacity candidates.
            if (needs_cover && !samples_[si].needs_cover) return;
            if (!close && norm2(samples_[si].pos - s.pos) < d2) close = true;
        });
        if (close) return -1;
    }
    s.needs_cover = needs_cover;
    s.layer = layer;
    s.part = part;
    s.island = island;
    s.excluded = costf_.excluded(s.pos);
    const double r_default = cfg_.tip_classes[cfg_.default_tip].diameter / 2;
    const bool delicate = costf_.isCosmetic(s.pos, s.normal) || costf_.nearEdge(s.pos, r_default);
    s.tip_class = island ? cfg_.minima_tip : (delicate ? cfg_.edge_tip : cfg_.default_tip);
    s.cost = costf_.cost(s.pos, s.normal, cfg_.tip_classes[s.tip_class].diameter / 2);
    const int id = static_cast<int>(samples_.size());
    samples_.push_back(s);
    sample_grid_.insert(id, s.pos.xy());
    const int root = find(part);
    comp_[root].pool.push_back(id);
    return id;
}

void Generator::sampleRegion(const Paths& region, int layer, int part, bool island, std::vector<int>& out) {
    for (const ExPolygon& e : toExPolygons(region)) {
        Paths ring = offset(e.toPaths(), -kEdgeInset);
        if (ring.empty()) ring = e.toPaths();
        for (const Path& path : ring) {
            if (path.size() < 2) continue;
            double carry = 0;  // distance to the next emitted point
            for (size_t i = 0; i < path.size(); ++i) {
                const Vec2 a = toVec(path[i]), b = toVec(path[(i + 1) % path.size()]);
                const double len = distance(a, b);
                double t = carry;
                while (t <= len) {
                    const int id = makeSample(a + (b - a) * (len > 0 ? t / len : 0), layer, part, island);
                    if (id >= 0) out.push_back(id);
                    t += cfg_.edge_step;
                }
                carry = t - len;
            }
        }
        // Interior grid aligned to global multiples of grid_step (deterministic).
        const BBox2 b = e.bbox();
        const double g = cfg_.grid_step;
        for (double x = std::ceil(b.min.x / g) * g; x <= b.max.x; x += g)
            for (double y = std::ceil(b.min.y / g) * g; y <= b.max.y; y += g)
                if (e.contains({x, y})) {
                    const int id = makeSample({x, y}, layer, part, island);
                    if (id >= 0) out.push_back(id);
                }
    }
}

int Generator::addTipAt(const Vec3& pos, const Vec3& normal, int layer, int part, int cls, PointCategory cat,
                        bool pinned) {
    Tip t;
    t.pos = pos;
    t.normal = normal;
    t.layer = layer;
    t.part = part;
    t.tip_class = cls;
    t.diameter = cfg_.tip_classes[cls].diameter;
    t.cat = cat;
    t.pinned = pinned;
    t.cost = costf_.cost(pos, normal, t.diameter / 2);
    t.thickness = localThickness(tree_, pos, normal, 100.0);
    const int id = static_cast<int>(tips_.size());
    tips_.push_back(t);
    tip_grid_.insert(id, pos.xy());
    const int root = find(part);
    comp_[root].capacity += capacityOf(t);
    comp_[root].tips.push_back(id);
    ++tips_on_part_[part];
    return id;
}

int Generator::addTip(int si, PointCategory cat, bool pinned) {
    Sample& s = samples_[si];
    const int id = addTipAt(s.pos, s.normal, s.layer, s.part, s.tip_class, cat, pinned);
    tips_[id].cost = s.cost;
    s.tip = id;
    return id;
}

// ---------------------------------------------------------------- per layer

void Generator::unite(int a, int b) {
    a = find(a);
    b = find(b);
    if (a == b) return;
    // Keep the lower id as root for determinism; merge the smaller lists in.
    if (b < a) std::swap(a, b);
    Comp& A = comp_[a];
    Comp& B = comp_[b];
    A.capacity += B.capacity;
    A.tips.insert(A.tips.end(), B.tips.begin(), B.tips.end());
    A.pool.insert(A.pool.end(), B.pool.begin(), B.pool.end());
    A.last_stab_z = std::max(A.last_stab_z, B.last_stab_z);
    A.exempt = A.exempt && B.exempt;
    B = Comp{};
    uf_[b] = a;
}

Paths Generator::anchorFor(int part) const {
    // Ancestors exactly anchor_layers_ below, plus island roots met earlier.
    std::vector<int> frontier{part}, roots;
    for (int d = 0; d < anchor_layers_ && !frontier.empty(); ++d) {
        std::vector<int> next;
        for (int p : frontier) {
            const auto& parents = graph_.part(p).parents;
            if (parents.empty()) roots.push_back(p);
            next.insert(next.end(), parents.begin(), parents.end());
        }
        std::sort(next.begin(), next.end());
        next.erase(std::unique(next.begin(), next.end()), next.end());
        frontier = std::move(next);
    }
    if (frontier.empty() && roots.empty()) return {};
    Paths all;
    for (int p : frontier) {
        const Paths ps = graph_.part(p).shape.toPaths();
        all.insert(all.end(), ps.begin(), ps.end());
    }
    for (int p : roots) {
        if (p == part) continue;
        const Paths ps = graph_.part(p).shape.toPaths();
        all.insert(all.end(), ps.begin(), ps.end());
    }
    return unionNonZero(all);
}

bool Generator::mergesSoon(int part, int layers) const {
    std::vector<int> frontier{part};
    for (int d = 0; d < layers && !frontier.empty(); ++d) {
        std::vector<int> next;
        for (int p : frontier)
            for (int c : graph_.part(p).children) {
                if (graph_.part(c).parents.size() > 1) return true;
                next.push_back(c);
            }
        frontier = std::move(next);
    }
    return false;
}

void Generator::processIsland(int part, std::vector<int>& layer_samples,
                              std::vector<std::pair<int, std::vector<int>>>& large_islands) {
    const LayerPart& P = graph_.part(part);
    const int L = P.layer;
    if ((P.area < cfg_.tiny_island_area && mergesSoon(part, cfg_.tiny_island_merge_layers)) ||
        (P.area < cfg_.absorb_island_area && mergesSoon(part, cfg_.absorb_island_layers))) {
        // Reported once, aggregated, in finalize().
        skipped_islands_.push_back(part);
        comp_[find(part)].exempt = true;
        return;
    }
    // Island length (max distance between outline points, subsampled).
    std::vector<Vec2> pts;
    const Path& c = P.shape.contour;
    const size_t stride = std::max<size_t>(1, c.size() / 200);
    for (size_t i = 0; i < c.size(); i += stride) pts.push_back(toVec(c[i]));
    double length = 0;
    Vec2 ea, eb;
    for (size_t i = 0; i < pts.size(); ++i)
        for (size_t j = i + 1; j < pts.size(); ++j) {
            const double d = distance(pts[i], pts[j]);
            if (d > length) { length = d; ea = pts[i]; eb = pts[j]; }
        }
    const Vec2 size = P.box.size();
    const double width = std::max(0.0, std::min(size.x, size.y));

    // A mesh minimum inside this island is where it truly starts.
    const MinimumRegion* minimum = nullptr;
    const double h = layers_.layer_height;
    for (const auto& m : minima_) {
        if (m.z > layers_.layerTop(L) + 1e-9) break;
        if (m.z < layers_.layerBottom(L) - h) continue;
        if (P.shape.contains(m.point.xy())) {
            minimum = &m;
            break;
        }
    }

    auto insideOrNear = [&](Vec2 q) {
        if (P.shape.contains(q)) return q;
        // Walk towards the bbox centre until inside.
        const Vec2 ctr = P.box.center();
        for (int k = 1; k <= 20; ++k) {
            const Vec2 r = q + (ctr - q) * (k / 20.0);
            if (P.shape.contains(r)) return r;
        }
        return toVec(P.shape.contour.front());
    };

    if (length <= cfg_.island_one_tip_length) {
        const int cls = classFor(std::max(0.6 * width, cfg_.tip_classes.front().diameter), cfg_.minima_tip);
        Vec3 pos, n;
        if (minimum) {
            pos = minimum->point;
            const ClosestPoint cp = tree_.closest(pos);
            n = cp.face >= 0 ? mesh_.faceNormal(cp.face) : Vec3{0, 0, -1};
        } else {
            Vec2 ctr{};
            for (const auto& q : pts) ctr += q;
            ctr = insideOrNear(ctr / static_cast<double>(std::max<size_t>(1, pts.size())));
            project(ctr, layers_.layerBottom(L), pos, n);
        }
        addTipAt(pos, n, L, part, cls, PointCategory::Island, true);
        return;
    }
    if (length <= cfg_.island_two_tip_length) {
        const int cls = classFor(std::max(0.45 * width, cfg_.tip_classes.front().diameter), cfg_.minima_tip);
        for (double f : {0.25, 0.75}) {
            const Vec2 q = insideOrNear(ea + (eb - ea) * f);
            Vec3 pos, n;
            project(q, layers_.layerBottom(L), pos, n);
            addTipAt(pos, n, L, part, cls, PointCategory::Island, true);
        }
        return;
    }
    // Large island: cover its whole first layer like an overhang, then make
    // sure it has at least `large_island_min_tips` tips.
    std::vector<int> mine;
    sampleRegion(P.shape.toPaths(), L, part, true, mine);
    layer_samples.insert(layer_samples.end(), mine.begin(), mine.end());
    large_islands.push_back({part, std::move(mine)});
    if (P.area > cfg_.warn_flat_area) {
        std::ostringstream m;
        m << "island starts with a flat " << P.area << " mm² face in one layer; tilting the part 10-45° would "
          << "spread it over many layers";
        warn("flat_face", m.str(), L, {P.box.center().x, P.box.center().y, layers_.layerBottom(L)});
    }
}

void Generator::processOverhang(int part, std::vector<int>& layer_samples) {
    const LayerPart& P = graph_.part(part);
    Paths below;
    for (int q : P.parents) {
        const Paths ps = graph_.part(q).shape.toPaths();
        below.insert(below.end(), ps.begin(), ps.end());
    }
    const Paths below_union = unionNonZero(below);
    // Any newly exposed underside (even self-supporting) is a place where
    // capacity tips can go; sampled coarsely and never needing coverage.
    {
        const Paths band = difference(P.shape.toPaths(), offset(below_union, 0.002));
        if (!band.empty())
            for (const ExPolygon& e : toExPolygons(band))
                for (const Path& path : e.toPaths()) {
                    const double step = cfg_.capacity_candidate_step;
                    double carry = 0;
                    for (size_t i = 0; i < path.size(); ++i) {
                        const Vec2 a = toVec(path[i]), b = toVec(path[(i + 1) % path.size()]);
                        const double len = distance(a, b);
                        double t = carry;
                        while (t <= len) {
                            makeSample(a + (b - a) * (len > 0 ? t / len : 0), P.layer, part, false, true, false, step);
                            t += step;
                        }
                        carry = t - len;
                    }
                }
    }
    Paths N = difference(P.shape.toPaths(), offset(below_union, s_off_));
    if (N.empty()) return;
    N = offset(offset(N, -0.002), 0.002);  // drop numerical slivers
    if (std::abs(area(N)) < 1e-4) return;
    const double new_area = std::abs(area(N));
    if (new_area > cfg_.warn_flat_area) {
        std::ostringstream m;
        m << "flat overhang of " << new_area << " mm² appears in a single layer; tilting the part 10-45° "
          << "would spread it over many layers";
        warn("flat_face", m.str(), P.layer, {P.box.center().x, P.box.center().y, layers_.layerBottom(P.layer)});
    }
    // Material within `self_supported_reach` of what existed `anchor_window`
    // below holds itself up.
    const Paths anchor = anchorFor(part);
    if (!anchor.empty()) N = difference(N, offset(anchor, cfg_.self_supported_reach));
    if (N.empty() || std::abs(area(N)) < 1e-4) return;
    sampleRegion(N, P.layer, part, false, layer_samples);
}

void Generator::coverLayer(const std::vector<int>& layer_samples) {
    std::vector<int> U;
    for (int si : layer_samples)
        if (samples_[si].needs_cover && !coveredByExisting(samples_[si])) U.push_back(si);
    if (U.empty()) return;

    // Candidate tips sit on the uncovered samples themselves.
    const double r = cfg_.coverage.maxRadius();
    XYGrid local(std::max(r, 1.0));
    for (size_t i = 0; i < U.size(); ++i) local.insert(static_cast<int>(i), samples_[U[i]].pos.xy());
    std::vector<std::vector<int>> cover(U.size());
    for (size_t i = 0; i < U.size(); ++i) {
        const Sample& c = samples_[U[i]];
        if (c.excluded) continue;
        Tip t;
        t.pos = c.pos;
        t.normal = c.normal;
        t.thickness = localThickness(tree_, c.pos, c.normal, 100.0);
        local.query(c.pos.xy(), r, [&](int j) {
            if (covers(t, samples_[U[j]])) cover[i].push_back(j);
        });
    }
    std::vector<char> covered(U.size(), 0);
    size_t remaining = U.size();
    if (opts_.strategy == CoverStrategy::FirstFit) {
        for (size_t i = 0; i < U.size(); ++i) {
            if (covered[i] || cover[i].empty()) continue;
            const Sample& s = samples_[U[i]];
            addTip(U[i], s.island ? PointCategory::Island : PointCategory::Overhang, false);
            for (int j : cover[i]) covered[j] = 1;
        }
        return;
    }
    // Lazy greedy on gain / cost; ties broken by index (deterministic).
    using Entry = std::pair<double, int>;  // (score, -index)
    auto score = [&](size_t i) {
        int g = 0;
        for (int j : cover[i]) g += !covered[j];
        return g / samples_[U[i]].cost;
    };
    std::priority_queue<Entry> pq;
    for (size_t i = 0; i < U.size(); ++i)
        if (!cover[i].empty()) pq.push({score(i), -static_cast<int>(i)});
    while (remaining > 0 && !pq.empty()) {
        auto [sc, neg] = pq.top();
        pq.pop();
        const size_t i = static_cast<size_t>(-neg);
        const double now = score(i);
        if (now <= 0) continue;
        if (now + 1e-12 < sc) {
            pq.push({now, neg});
            continue;
        }
        const Sample& s = samples_[U[i]];
        addTip(U[i], s.island ? PointCategory::Island : PointCategory::Overhang, false);
        for (int j : cover[i])
            if (!covered[j]) { covered[j] = 1; --remaining; }
    }
}

void Generator::enforceIslandMinimum(int part, const std::vector<int>& island_samples) {
    while (tips_on_part_[part] < cfg_.large_island_min_tips) {
        // Farthest island sample from the island's existing tips.
        int best = -1;
        double best_d = -1;
        for (int si : island_samples) {
            const Sample& s = samples_[si];
            if (s.tip >= 0 || s.excluded) continue;
            double d = 1e9;
            for (int ti : comp_[find(part)].tips)
                if (tips_[ti].part == part && !tips_[ti].removed) d = std::min(d, distance(tips_[ti].pos.xy(), s.pos.xy()));
            if (d / s.cost > best_d) { best_d = d / s.cost; best = si; }
        }
        if (best < 0) break;
        addTip(best, PointCategory::Island, true);
    }
}

void Generator::enforceCapacity(int layer) {
    std::vector<std::pair<int, double>> demand;  // (root, demand)
    for (int p : graph_.partsInLayer(layer)) {
        const LayerPart& P = graph_.part(p);
        const int root = find(p);
        const double d = P.area * peelFactor(P.box.center());
        auto it = std::find_if(demand.begin(), demand.end(), [&](auto& e) { return e.first == root; });
        if (it == demand.end()) demand.push_back({root, d});
        else it->second += d;
    }
    for (auto& [root, need] : demand) {
        Comp& C = comp_[root];
        if (C.exempt || C.capacity + 1e-9 >= need) continue;
        // Add tips at pool samples farthest from the component's tips.
        while (C.capacity + 1e-9 < need) {
            int best = -1;
            double best_score = -1;
            for (int si : C.pool) {
                const Sample& s = samples_[si];
                if (s.tip >= 0 || s.excluded) continue;
                double d = 1e9;
                for (int ti : C.tips)
                    if (!tips_[ti].removed) d = std::min(d, distance(tips_[ti].pos.xy(), s.pos.xy()));
                const double sc = d / s.cost;
                if (sc > best_score) { best_score = sc; best = si; }
            }
            if (best < 0) break;
            addTip(best, PointCategory::Capacity, false);
        }
        // Then upgrade the smallest auto tips.
        while (C.capacity + 1e-9 < need) {
            int best = -1;
            for (int ti : C.tips) {
                const Tip& t = tips_[ti];
                if (t.removed || t.tip_class < 0 || t.tip_class + 1 >= static_cast<int>(cfg_.tip_classes.size()))
                    continue;
                if (best < 0 || t.diameter < tips_[best].diameter) best = ti;
            }
            if (best < 0) break;
            Tip& t = tips_[best];
            C.capacity -= capacityOf(t);
            ++t.tip_class;
            t.diameter = cfg_.tip_classes[t.tip_class].diameter;
            C.capacity += capacityOf(t);
        }
        if (C.capacity + 1e-9 < need) {
            const double z = layers_.layerBottom(layer);
            if (z - C.last_warn_z >= 1.0) {
                C.last_warn_z = z;
                BBox2 where;
                for (int p : graph_.partsInLayer(layer))
                    if (find(p) == root) where.extend(graph_.part(p).box);
                std::ostringstream m;
                m << "layer area " << need << " mm² exceeds what its tips can hold (" << C.capacity
                  << " mm²); tilt the part or use larger tips";
                warn("capacity_deficit", m.str(), layer, {where.center().x, where.center().y, z});
            }
        }
    }
}

void Generator::stabilize(int layer) {
    const double z = layers_.layerBottom(layer);
    std::vector<int> roots;
    for (int p : graph_.partsInLayer(layer)) {
        const int r = find(p);
        if (std::find(roots.begin(), roots.end(), r) == roots.end()) roots.push_back(r);
    }
    for (int root : roots) {
        Comp& C = comp_[root];
        std::vector<const Tip*> live;
        for (int ti : C.tips)
            if (!tips_[ti].removed) live.push_back(&tips_[ti]);
        if (live.empty()) continue;
        double zlow = 1e9;
        BBox2 spread;
        Vec2 ctr{};
        for (const Tip* t : live) {
            zlow = std::min(zlow, t->pos.z);
            spread.extend(t->pos.xy());
            ctr += t->pos.xy();
        }
        ctr = ctr / static_cast<double>(live.size());
        const double H = z - zlow;
        const double span = norm(spread.size());
        if (H < cfg_.stabilize_min_height) continue;
        if (live.size() >= 3 && span * cfg_.slenderness >= H) continue;
        if (z - C.last_stab_z < 2.0) continue;
        // Side tip on the outline point farthest from the tips' centroid.
        Vec2 best_pt;
        int best_part = -1;
        double best_d = -1;
        for (int p : graph_.partsInLayer(layer)) {
            if (find(p) != root) continue;
            for (const auto& q : graph_.part(p).shape.contour) {
                const Vec2 v = toVec(q);
                const double d = distance(v, ctr);
                if (d > best_d) { best_d = d; best_pt = v; best_part = p; }
            }
        }
        if (best_part < 0) continue;
        Vec3 pos, n;
        project(best_pt, z + layers_.layer_height / 2, pos, n);
        if (costf_.excluded(pos)) continue;
        addTipAt(pos, n, layer, best_part, cfg_.edge_tip, PointCategory::Stabilization, true);
        C.last_stab_z = z;
    }
}

// ---------------------------------------------------------------- run

void Generator::setup() {
    tree_.build(mesh_);
    layers_ = sliceModel(mesh_, cfg_.layer_height);
    graph_ = IslandGraph(layers_, cfg_.link_grow);
    edges_ = SharpEdgeIndex(mesh_, cfg_.sharp_edge_angle_deg);
    costf_.cosmetic = opts_.cosmetic;
    costf_.edges = &edges_;
    costf_.cosmetic_weight = cfg_.cosmetic_weight;
    costf_.edge_weight = cfg_.edge_weight;
    costf_.edge_margin = cfg_.edge_margin;
    minima_ = findMinima(mesh_);
    s_off_ = cfg_.layer_height * std::tan(deg2rad(cfg_.critical_angle_deg));
    anchor_layers_ = std::max(1, static_cast<int>(std::lround(cfg_.anchor_window / cfg_.layer_height)));
    const size_t np = graph_.parts().size();
    uf_.resize(np);
    std::iota(uf_.begin(), uf_.end(), 0);
    comp_.assign(np, Comp{});
    tips_on_part_.assign(np, 0);
    manual_by_layer_.assign(layers_.size(), {});
    for (size_t i = 0; i < opts_.manual.size(); ++i) {
        const auto& m = opts_.manual[i];
        const size_t L = std::min(layers_.layerAt(m.pos.z), layers_.size() ? layers_.size() - 1 : 0);
        manual_by_layer_[L].push_back(static_cast<int>(i));
    }
}

PointGenResult Generator::run() {
    const auto t0 = std::chrono::steady_clock::now();
    setup();
    PointGenStats& st = result_.stats;
    st.layers = layers_.size();
    st.parts = graph_.parts().size();
    st.islands = graph_.islands().size();

    const int stab_every = std::max(1, static_cast<int>(std::lround(0.5 / cfg_.layer_height)));
    for (size_t L = 0; L < layers_.size(); ++L) {
        const int layer = static_cast<int>(L);
        double layer_area = 0;
        std::vector<int> layer_samples;
        std::vector<std::pair<int, std::vector<int>>> large_islands;
        for (int p : graph_.partsInLayer(L)) {
            layer_area += graph_.part(p).area;
            for (int q : graph_.part(p).parents) unite(p, q);
        }
        if (layer_area > st.peak_layer_area) {
            st.peak_layer_area = layer_area;
            st.peak_layer = layer;
        }
        // Manual (pinned) tips belong to the part they touch.
        for (int mi : manual_by_layer_[L]) {
            const SupportPoint& m = opts_.manual[mi];
            const ClosestPoint cp = tree_.closest(m.pos);
            const Vec3 pos = cp.face >= 0 ? cp.point : m.pos;
            const Vec3 n = cp.face >= 0 ? mesh_.faceNormal(cp.face) : Vec3{0, 0, -1};
            int part = -1;
            double best = 1e9;
            for (int p : graph_.partsInLayer(L)) {
                const double d = graph_.part(p).shape.contains(pos.xy()) ? 0 : distance(graph_.part(p).box.center(), pos.xy());
                if (d < best) { best = d; part = p; }
            }
            if (part < 0) {
                warn("manual_point_unattached", "manual point is not on the model", layer, m.pos);
                continue;
            }
            const int id = addTipAt(pos, n, layer, part, 0, PointCategory::Manual, true);
            tips_[id].diameter = m.tip_diameter;
            tips_[id].tip_class = -1;
            Comp& C = comp_[find(part)];
            C.capacity += capacityOf(tips_[id]) - cfg_.capacity.capacity(cfg_.tip_classes[0].diameter);
        }
        for (int p : graph_.partsInLayer(L)) {
            if (graph_.part(p).parents.empty()) processIsland(p, layer_samples, large_islands);
            else processOverhang(p, layer_samples);
        }
        coverLayer(layer_samples);
        for (const auto& [p, isl] : large_islands) enforceIslandMinimum(p, isl);
        enforceCapacity(layer);
        if (layer % stab_every == 0) stabilize(layer);
    }
    if (st.peak_layer_area > cfg_.warn_layer_area) {
        std::ostringstream m;
        m << "peak layer area " << st.peak_layer_area << " mm² at layer " << st.peak_layer
          << "; large cross-sections raise peel force — consider tilting";
        warn("peak_area", m.str(), st.peak_layer, {0, 0, layers_.layerBottom(st.peak_layer)});
    }
    finalize();
    st.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return std::move(result_);
}

void Generator::finalize() {
    PointGenStats& st = result_.stats;
    st.samples = static_cast<size_t>(std::count_if(samples_.begin(), samples_.end(),
                                                   [](const Sample& s) { return s.needs_cover; }));
    st.greedy_points = tips_.size();

    // Coverage relation sample -> tips.
    std::vector<std::vector<int>> coverers(samples_.size());
    std::vector<std::vector<int>> covered_by_tip(tips_.size());
    for (size_t si = 0; si < samples_.size(); ++si) {
        if (!samples_[si].needs_cover) continue;
        tip_grid_.query(samples_[si].pos.xy(), cfg_.coverage.maxRadius(), [&](int ti) {
            if (covers(tips_[ti], samples_[si])) {
                coverers[si].push_back(ti);
                covered_by_tip[ti].push_back(static_cast<int>(si));
            }
        });
    }
    std::vector<int> cover_count(samples_.size());
    for (size_t si = 0; si < samples_.size(); ++si) cover_count[si] = static_cast<int>(coverers[si].size());

    // Capacity replay: (layer, component) pairs with their slack, and the
    // pairs each tip contributes to.
    std::vector<double> slack;
    std::vector<std::vector<int>> tip_pairs(tips_.size());
    {
        std::vector<int> uf(graph_.parts().size());
        std::iota(uf.begin(), uf.end(), 0);
        std::vector<double> cap(uf.size(), 0);
        std::vector<std::vector<int>> members(uf.size());
        std::vector<char> exempt(uf.size(), 0);
        for (int p : skipped_islands_) exempt[p] = 1;
        auto f = [&](int a) {
            while (uf[a] != a) a = uf[a] = uf[uf[a]];
            return a;
        };
        std::vector<std::vector<int>> tips_by_layer(layers_.size());
        for (size_t ti = 0; ti < tips_.size(); ++ti) tips_by_layer[tips_[ti].layer].push_back(static_cast<int>(ti));
        for (size_t L = 0; L < layers_.size(); ++L) {
            for (int p : graph_.partsInLayer(L))
                for (int q : graph_.part(p).parents) {
                    int a = f(p), b = f(q);
                    if (a == b) continue;
                    if (b < a) std::swap(a, b);
                    cap[a] += cap[b];
                    exempt[a] = exempt[a] && exempt[b];
                    members[a].insert(members[a].end(), members[b].begin(), members[b].end());
                    members[b].clear();
                    uf[b] = a;
                }
            for (int ti : tips_by_layer[L]) {
                const int r = f(tips_[ti].part);
                cap[r] += capacityOf(tips_[ti]);
                members[r].push_back(ti);
            }
            std::vector<std::pair<int, double>> demand;
            for (int p : graph_.partsInLayer(L)) {
                const int r = f(p);
                const double d = graph_.part(p).area * peelFactor(graph_.part(p).box.center());
                auto it = std::find_if(demand.begin(), demand.end(), [&](auto& e) { return e.first == r; });
                if (it == demand.end()) demand.push_back({r, d});
                else it->second += d;
            }
            for (auto& [r, d] : demand) {
                if (exempt[r]) continue;
                const int pair = static_cast<int>(slack.size());
                slack.push_back(cap[r] - d);
                for (int ti : members[r]) tip_pairs[ti].push_back(pair);
            }
        }
    }

    // Island minimum counts (large islands).
    std::vector<int> island_tips(graph_.parts().size(), 0);
    for (const Tip& t : tips_)
        if (t.cat == PointCategory::Island) ++island_tips[t.part];

    if (opts_.prune) {
        std::vector<int> order;
        for (size_t ti = 0; ti < tips_.size(); ++ti)
            if (!tips_[ti].pinned) order.push_back(static_cast<int>(ti));
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            if (tips_[a].cost != tips_[b].cost) return tips_[a].cost > tips_[b].cost;
            // Later (higher) greedy picks first: they were chosen with less information.
            return a > b;
        });
        for (int ti : order) {
            Tip& t = tips_[ti];
            bool ok = true;
            for (int si : covered_by_tip[ti])
                if (cover_count[si] < 2) { ok = false; break; }
            if (!ok) continue;
            const double c = capacityOf(t);
            for (int pi : tip_pairs[ti])
                if (slack[pi] - c < -1e-9) { ok = false; break; }
            if (!ok) continue;
            if (t.cat == PointCategory::Island && island_tips[t.part] <= cfg_.large_island_min_tips) continue;
            t.removed = true;
            ++st.pruned;
            for (int si : covered_by_tip[ti]) --cover_count[si];
            for (int pi : tip_pairs[ti]) slack[pi] -= c;
            if (t.cat == PointCategory::Island) --island_tips[t.part];
        }
    }

    for (size_t si = 0; si < samples_.size(); ++si)
        if (samples_[si].needs_cover && cover_count[si] == 0) ++st.uncovered;
    for (double s : slack)
        if (s < -1e-9) ++st.capacity_deficits;
    st.tiny_islands_skipped = skipped_islands_.size();
    if (!skipped_islands_.empty()) {
        std::ostringstream m;
        m << skipped_islands_.size() << " small islands merge with the model almost at once (< "
          << cfg_.tiny_island_area << " mm² within " << cfg_.tiny_island_merge_layers << " layers, or < "
          << cfg_.absorb_island_area << " mm² on the next layer) and were left unsupported";
        const LayerPart& first = graph_.part(skipped_islands_.front());
        warn("tiny_islands_skipped", m.str(), first.layer,
             {first.box.center().x, first.box.center().y, layers_.layerBottom(first.layer)});
    }
    if (st.uncovered > 0) {
        std::ostringstream m;
        m << st.uncovered << " surface samples have no covering tip (inside exclusion zones or unreachable)";
        warn("uncovered", m.str(), -1, {});
    }

    for (const Tip& t : tips_) {
        if (t.removed) continue;
        SupportPoint p;
        p.pos = t.pos;
        p.normal = t.normal;
        p.tip_diameter = t.diameter;
        p.category = t.cat;
        p.pinned = t.pinned;
        p.layer = t.layer;
        p.part = t.part;
        p.cost = t.cost;
        p.id = stablePointId(t.pos, t.cat);
        result_.points.push_back(p);
    }
    std::stable_sort(result_.points.begin(), result_.points.end(), [](const SupportPoint& a, const SupportPoint& b) {
        if (a.pos.z != b.pos.z) return a.pos.z < b.pos.z;
        if (a.pos.x != b.pos.x) return a.pos.x < b.pos.x;
        return a.pos.y < b.pos.y;
    });
}

}  // namespace

PointGenResult generateSupportPoints(const TriangleMesh& mesh, const SupportConfig& cfg, const PointGenOptions& opts) {
    Generator g(mesh, cfg, opts);
    return g.run();
}

}  // namespace fruit
