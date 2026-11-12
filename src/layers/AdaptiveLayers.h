// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Adaptive layers: where the model's outline doesn't change (straight
// vertical walls), neighbouring thin layers of the print file are merged
// into one thicker layer, with a longer exposure. Everything before the
// print file is written stays on equal thin layers; this only plans and
// builds the merged layers.

#include <cstddef>
#include <vector>

#include "core/JobControl.h"
#include "geometry/Polygon.h"
#include "layers/LayerStack.h"

namespace fruit {

// The printer's limit on one layer's height, mm.
inline constexpr double kMaxLayerHeight = 0.2;
// The largest thickest-layer multiplier a resin can ask for (of the thin height).
inline constexpr int kMaxAdaptiveMultiplier = 8;
// The resin's light depth Dp, mm: how deep light hardens it. FruitSlicer's
// typical value until it is measured, and the range the setting allows.
inline constexpr double kDefaultLightDepth = 0.10, kMinLightDepth = 0.03, kMaxLightDepth = 0.5;
// How far one layer's outline may differ from the group's first and still
// merge, mm (about one printer pixel).
inline constexpr double kAdaptiveTolerance = 0.02;

// The thickest layer, in thin layers, for a multiplier asked for: the
// largest power of two that is at most `multiplier` and keeps the layer at
// or under kMaxLayerHeight. At least 1 (no merging).
int maxAdaptiveCount(double thin_height, int multiplier);
// The heights layers may have: thin_height, then doubled, up to the thickest.
std::vector<double> adaptiveHeights(double thin_height, int multiplier);
// The multiplier (1, 2, 4 or 8) whose height is nearest `mm`, among the
// heights this thin height allows. Ties go to the thinner.
int snapAdaptiveMultiplier(double thin_height, double mm);
// Exposure of a layer `height` thick: normal_exposure × exp((height − thin)/light_depth),
// rounded to 0.01 s; the normal exposure itself for the thin height.
double adaptiveExposure(double normal_exposure, double thin_height, double height, double light_depth);

struct AdaptiveOptions {
    double thin_height = 0.05;      // the print file's layer height
    int multiplier = 1;             // thickest layer in thin layers (1: no merging)
    double tolerance_mm = kAdaptiveTolerance;
    std::size_t first_mergeable = 0;  // layers before this index are never merged
};

// `count` thin layers from `first`, printed as one.
struct LayerGroup {
    std::size_t first = 0, count = 1;
};

// Whether two outlines match: each fits inside the other grown by `tolerance_mm`.
// Two empty outlines match; an empty and a non-empty one don't.
bool outlinesMatch(const Paths& a, const Paths& b, double tolerance_mm);

// Groups every one of `total` thin layers, in order. `model` holds the
// model's own outline per thin layer (no supports; a layer it doesn't reach
// counts as empty). A group of n = 2, 4, 8 … layers needs every outline in
// it to match the first's; the thickest allowed is tried first, and a layer
// is at most twice as thick as the one before (getting thinner is not limited).
std::vector<LayerGroup> planAdaptiveLayers(const LayerStack& model, std::size_t total, const AdaptiveOptions& o,
                                           unsigned threads = 0, const JobControl& ctl = JobControl::none());

// One layer of the print: its image is the union of its thin layers'.
struct AdaptiveLayer {
    LayerGroup group;
    double thickness = 0;  // mm
    double top_z = 0;      // mm, cumulative
    Paths slice;
};
// The union of the full slices of `g`'s thin layers.
Paths mergedSlice(const LayerStack& full, const LayerGroup& g);
std::vector<AdaptiveLayer> buildAdaptiveLayers(const LayerStack& full, const std::vector<LayerGroup>& groups,
                                               double thin_height);
// How many of `groups` are made of more than one thin layer.
std::size_t mergedCount(const std::vector<LayerGroup>& groups);

}  // namespace fruit
