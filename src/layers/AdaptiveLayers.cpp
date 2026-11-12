// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "layers/AdaptiveLayers.h"

#include <algorithm>
#include <cmath>

#include "core/Parallel.h"

namespace fruit {

int maxAdaptiveCount(double thin_height, int multiplier) {
    int n = 1;
    while (n * 2 <= multiplier && n * 2 <= kMaxAdaptiveMultiplier && thin_height * n * 2 <= kMaxLayerHeight + 1e-9) n *= 2;
    return n;
}

std::vector<double> adaptiveHeights(double thin_height, int multiplier) {
    std::vector<double> v;
    for (int n = 1; n <= maxAdaptiveCount(thin_height, multiplier); n *= 2) v.push_back(thin_height * n);
    return v;
}

int snapAdaptiveMultiplier(double thin_height, double mm) {
    const int top = maxAdaptiveCount(thin_height, kMaxAdaptiveMultiplier);
    int best = 1;
    for (int n = 2; n <= top; n *= 2)
        if (std::abs(thin_height * n - mm) < std::abs(thin_height * best - mm) - 1e-12) best = n;
    return best;
}

double adaptiveExposure(double t0, double h0, double h, double dp) {
    if (h <= h0 + 1e-9) return t0;
    dp = std::clamp(dp, kMinLightDepth, kMaxLightDepth);
    return std::round(t0 * std::exp((h - h0) / dp) * 100.0) / 100.0;
}

bool outlinesMatch(const Paths& a, const Paths& b, double tol) {
    if (a.empty() || b.empty()) return a.empty() && b.empty();
    if (a == b) return true;
    const auto inside = [](const Paths& x, const Paths& grown) { return std::abs(area(difference(x, grown))) < 1e-6; };
    return inside(a, offset(b, tol)) && inside(b, offset(a, tol));
}

std::vector<LayerGroup> planAdaptiveLayers(const LayerStack& model, std::size_t total, const AdaptiveOptions& o,
                                           unsigned threads, const JobControl& ctl) {
    std::vector<LayerGroup> groups;
    const int top = maxAdaptiveCount(o.thin_height, o.multiplier);
    const Paths none;
    auto outline = [&](std::size_t i) -> const Paths& { return i < model.slices.size() ? model.slices[i] : none; };
    // The outlines grown by the tolerance, made once (only where merging may happen).
    std::vector<Paths> grown(top > 1 ? total : 0);
    if (top > 1)
        parallelFor(total, threads, [&](std::size_t i) {
            if (i >= o.first_mergeable && !outline(i).empty()) grown[i] = offset(outline(i), o.tolerance_mm);
        });
    auto fits = [&](const Paths& x, const Paths& g) { return std::abs(area(difference(x, g))) < 1e-6; };
    auto matches = [&](std::size_t a, std::size_t b) {
        const Paths &x = outline(a), &y = outline(b);
        if (x.empty() || y.empty()) return x.empty() && y.empty();
        return x == y || (fits(x, grown[b]) && fits(y, grown[a]));
    };
    // Where the outline changes from one layer to the next by more than the
    // tolerance (a flat face, a step, the model or an island starting or
    // ending), the layer on each side stays thin: a flat face lands at its
    // own height with a thin layer's exposure, and the last layers of the
    // support tips under it are not printed as one slab.
    std::vector<char> thin(total, 0);
    if (top > 1)
        for (std::size_t k = o.first_mergeable + 1; k < total; ++k)
            if (!matches(k - 1, k)) thin[k - 1] = thin[k] = 1;
    std::size_t i = 0, prev = 1;
    while (i < total) {
        ctl.check();
        std::size_t n = 1;
        if (top > 1 && i >= o.first_mergeable && !thin[i]) {
            // How many layers in a row, from here, match this one (at most as many as may be used).
            const std::size_t limit = std::min<std::size_t>({static_cast<std::size_t>(top), prev * 2, total - i});
            std::size_t run = 1;
            while (run < limit && !thin[i + run] && matches(i, i + run)) ++run;
            while (n * 2 <= run) n *= 2;
        }
        groups.push_back({i, n});
        i += n;
        prev = n;
        ctl.report("merge", static_cast<double>(i) / static_cast<double>(std::max<std::size_t>(total, 1)));
    }
    // No sudden thinning either: a layer is at most twice as thick as the one
    // after it. A group that is too thick for what follows is halved, back
    // half first, so the layers step down towards detail (4, 2, 2, 1 rather
    // than 8, 1). Support tips end where the model's outline starts, so this
    // also keeps a tip from being printed as one thick slab.
    for (std::size_t k = groups.size(); k-- > 1;) {
        std::size_t j = k - 1;
        while (groups[j].count > 2 * groups[j + 1].count) {
            const std::size_t half = groups[j].count / 2;
            groups[j].count = half;
            groups.insert(groups.begin() + static_cast<std::ptrdiff_t>(j) + 1, LayerGroup{groups[j].first + half, half});
            ++j;  // the back half against what follows it
        }
    }
    return groups;
}

Paths mergedSlice(const LayerStack& full, const LayerGroup& g) {
    Paths out;
    for (std::size_t k = 0; k < g.count && g.first + k < full.slices.size(); ++k) {
        const Paths& s = full.slices[g.first + k];
        if (s.empty()) continue;
        out = out.empty() ? s : unionOf(out, s);
    }
    return out;
}

std::vector<AdaptiveLayer> buildAdaptiveLayers(const LayerStack& full, const std::vector<LayerGroup>& groups,
                                               double thin_height) {
    std::vector<AdaptiveLayer> v(groups.size());
    for (std::size_t k = 0; k < groups.size(); ++k) {
        v[k].group = groups[k];
        v[k].thickness = static_cast<double>(groups[k].count) * thin_height;
        v[k].top_z = static_cast<double>(groups[k].first + groups[k].count) * thin_height;
        v[k].slice = mergedSlice(full, groups[k]);
    }
    return v;
}

std::size_t mergedCount(const std::vector<LayerGroup>& groups) {
    return static_cast<std::size_t>(
        std::count_if(groups.begin(), groups.end(), [](const LayerGroup& g) { return g.count > 1; }));
}

}  // namespace fruit
