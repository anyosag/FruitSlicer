// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "slice/MeshSlicer.h"

#include "core/Parallel.h"

#include <algorithm>
#include <atomic>
#include <thread>
#include <unordered_map>

namespace fruit {

namespace {

struct Segment {
    uint64_t from_key, to_key;
    Point64 from, to;
};

uint64_t edgeKey(int32_t a, int32_t b) {
    const auto lo = static_cast<uint64_t>(std::min(a, b));
    const auto hi = static_cast<uint64_t>(std::max(a, b));
    return (hi << 32) | lo;
}

// Intersection of edge (a,b) with plane z, computed from the endpoints in
// vertex-index order so both faces sharing the edge get the same point.
Point64 edgePoint(const TriangleMesh& m, int32_t a, int32_t b, double z) {
    if (a > b) std::swap(a, b);
    const Vec3& p = m.vertices[a];
    const Vec3& q = m.vertices[b];
    const double t = (z - p.z) / (q.z - p.z);
    return toPoint({p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t});
}

Paths sliceLayer(const TriangleMesh& m, const std::vector<int32_t>& candidates, double z,
                 size_t& open_chains) {
    std::vector<Segment> segs;
    for (int32_t fi : candidates) {
        const Face& f = m.faces[fi];
        bool above[3];
        int n_above = 0;
        for (int k = 0; k < 3; ++k) {
            above[k] = m.vertices[f[k]].z > z;  // on-plane vertices count as below
            n_above += above[k];
        }
        if (n_above == 0 || n_above == 3) continue;
        // Walk edges a->b in face order. The segment runs from the crossing on
        // the above->below edge to the crossing on the below->above edge, which
        // puts the solid on the left (outer contours CCW).
        int32_t down_a = -1, down_b = -1, up_a = -1, up_b = -1;
        for (int k = 0; k < 3; ++k) {
            const int k2 = (k + 1) % 3;
            if (above[k] && !above[k2]) { down_a = f[k]; down_b = f[k2]; }
            if (!above[k] && above[k2]) { up_a = f[k]; up_b = f[k2]; }
        }
        Segment s{edgeKey(down_a, down_b), edgeKey(up_a, up_b), edgePoint(m, down_a, down_b, z),
                  edgePoint(m, up_a, up_b, z)};
        segs.push_back(s);
    }
    if (segs.empty()) return {};

    std::unordered_multimap<uint64_t, size_t> by_start;
    by_start.reserve(segs.size() * 2);
    for (size_t i = 0; i < segs.size(); ++i) by_start.emplace(segs[i].from_key, i);
    std::vector<bool> used(segs.size(), false);

    Paths loops;
    for (size_t start = 0; start < segs.size(); ++start) {
        if (used[start]) continue;
        Path loop;
        size_t cur = start;
        bool closed = false;
        while (true) {
            used[cur] = true;
            const Point64& p = segs[cur].from;
            if (loop.empty() || loop.back() != p) loop.push_back(p);
            const uint64_t next_key = segs[cur].to_key;
            if (next_key == segs[start].from_key) { closed = true; break; }
            size_t next = SIZE_MAX;
            auto range = by_start.equal_range(next_key);
            for (auto it = range.first; it != range.second; ++it)
                if (!used[it->second]) { next = it->second; break; }
            if (next == SIZE_MAX) {
                loop.push_back(segs[cur].to);
                break;
            }
            cur = next;
        }
        if (!closed) ++open_chains;
        if (loop.size() >= 2 && loop.front() == loop.back()) loop.pop_back();
        if (loop.size() >= 3) loops.push_back(std::move(loop));
    }
    return unionNonZero(loops);
}

}  // namespace

std::vector<Paths> sliceMesh(const TriangleMesh& mesh, const std::vector<double>& zs,
                             SliceStats* stats, unsigned threads, const JobControl& ctl) {
    std::vector<Paths> out(zs.size());
    if (mesh.faces.empty() || zs.empty()) return out;

    // Faces sorted by min z; each layer scans faces whose z-range spans it.
    const auto nf = static_cast<int32_t>(mesh.faces.size());
    std::vector<int32_t> order(nf);
    std::vector<double> zmin(nf), zmax(nf);
    for (int32_t i = 0; i < nf; ++i) {
        const Face& f = mesh.faces[i];
        zmin[i] = std::min({mesh.vertices[f[0]].z, mesh.vertices[f[1]].z, mesh.vertices[f[2]].z});
        zmax[i] = std::max({mesh.vertices[f[0]].z, mesh.vertices[f[1]].z, mesh.vertices[f[2]].z});
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](int32_t a, int32_t b) {
        return zmin[a] < zmin[b] || (zmin[a] == zmin[b] && a < b);
    });

    // Heights in ascending order; each thread sweeps one contiguous chunk with
    // an active face set, so total work is O(faces + crossings) per thread.
    std::vector<size_t> layer_order(zs.size());
    for (size_t i = 0; i < zs.size(); ++i) layer_order[i] = i;
    std::sort(layer_order.begin(), layer_order.end(),
              [&](size_t a, size_t b) { return zs[a] < zs[b] || (zs[a] == zs[b] && a < b); });

    threads = std::min<unsigned>(resolveThreads(threads), static_cast<unsigned>(zs.size()));
    const size_t chunk = (zs.size() + threads - 1) / threads;
    std::atomic<size_t> open_total{0}, done{0};
    const double total = static_cast<double>(zs.size());
    auto worker = [&](size_t begin, size_t end, const std::atomic<bool>& stop) {
        std::vector<int32_t> active, candidates;
        size_t open = 0, cursor = 0;
        for (size_t k = begin; k < end; ++k) {
            if (stop) return;
            ctl.check();
            const size_t li = layer_order[k];
            const double z = zs[li];
            while (cursor < order.size() && zmin[order[cursor]] <= z) active.push_back(order[cursor++]);
            candidates.clear();
            size_t keep = 0;
            for (int32_t f : active) {
                if (zmax[f] <= z) continue;  // permanently below this and later layers
                active[keep++] = f;
                candidates.push_back(f);
            }
            active.resize(keep);
            out[li] = sliceLayer(mesh, candidates, z, open);
            ctl.report("slice", static_cast<double>(++done) / total);
        }
        open_total += open;
    };
    runThreads(threads, [&](unsigned t, const std::atomic<bool>& stop) {
        const size_t b = t * chunk, e = std::min(zs.size(), b + chunk);
        if (b < e) worker(b, e, stop);
    });
    ctl.check();
    if (stats) stats->open_chains = open_total;
    return out;
}

Paths sliceMeshAt(const TriangleMesh& mesh, double z) { return sliceMesh(mesh, {z}, nullptr, 1)[0]; }

}  // namespace fruit
