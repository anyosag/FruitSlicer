// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Same-plate A/B comparison for the M2 go/no-go: N copies of one model in a
// grid, half carrying FruitSlicer's supports (arm A), half bare for the
// vendor slicer's auto-supports (arm B). Arms are shuffled over the cells
// with a fixed seed so plate position, resin batch, film wear and
// temperature affect both arms alike.
//
// Two files are written because a slicer applies auto-supports per object:
// <name>_A.stl (supported copies + an L marker in cell 0) and <name>_B.stl
// (bare copies, standing on the plate). Each copy is the unit of trial.

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "mesh/TriangleMesh.h"
#include "printer/PrinterProfile.h"

namespace fruit {

struct ABPlateOptions {
    int copies = 8;
    uint32_t seed = 1;
    double margin = 4.0;     // mm between neighbouring footprints
    std::string model;       // its name, for the manifest (optional)
    int tips_per_copy = -1;  // FruitSlicer's tips on each A copy (-1: not given)
};

// How copies of a model sit on the plate: each takes the footprint of the
// model with its supports, plus the margin; cell 0 holds the L marker.
struct ABPlateFit {
    double cell_x = 0, cell_y = 0;  // mm
    int rows = 0, cols = 0;
    int capacity = 0;  // copies that fit
};
ABPlateFit abPlateFit(const TriangleMesh& model, const TriangleMesh& supports, const PrinterProfile& printer,
                      double margin = 4.0);

struct ABPlate {
    TriangleMesh supported;  // arm A, printer coordinates
    TriangleMesh bare;       // arm B, printer coordinates
    nlohmann::json manifest;
    std::string csv;
    int rows = 0, cols = 0;
};

// `model` and `supports` are already placed (printer coordinates).
// Throws if the copies do not fit on the plate.
ABPlate makeABPlate(const TriangleMesh& model, const TriangleMesh& supports, const PrinterProfile& printer,
                    const ABPlateOptions& o);

// One copy's score.
struct ABCopyResult {
    int id = 0;
    bool recorded = false;  // a result is given
    bool ok = false;        // printed whole and stayed on
    bool partial = false;   // printed, but part of it failed
    int scar = 0;           // 1..5, 0 = not scored
    double removal_s = -1;  // seconds to clean off its supports; < 0 = not timed
    std::string notes;
};

// The score sheet without the arms, so whoever scores can't see which copy
// had which supports: id,row,col,result,scar,removal_s,notes for every copy
// in `manifest`, with the scores given so far.
std::string blindABCsv(const nlohmann::json& manifest, const std::vector<ABCopyResult>& scores);

// Reads a score sheet: blind, or with an arm column as makeABPlate writes it
// (an arm given there must match the manifest's). Every copy of the
// manifest, in its order. Throws std::runtime_error naming the problem.
std::vector<ABCopyResult> loadABResults(const std::string& manifest_path, const std::string& csv_path);

// One side of an A/B test, from its scores.
struct ABArmStats {
    int copies = 0;                          // on the plate
    int scored = 0;                          // with a result
    int ok = 0, partial = 0, failed = 0;
    int marked = 0;                          // with a mark, 1..5
    double mean_scar = 0;
    int timed = 0;                           // with a clean-up time
    double mean_removal_s = 0;
};

// The two sides compared. Each p is two-sided and exact (or, past about
// 200,000 ways to split the copies, estimated from 200,000 random splits): the
// chance of a difference at least this big if the supports made none. 1 when
// a side has nothing to compare. Each best_p is the smallest p any results
// from these numbers of copies could give: above 0.05, no difference can
// show however the copies turn out.
struct ABSummary {
    ABArmStats a, b;
    double p_ok = 1, p_scar = 1, p_removal = 1;
    double best_p_ok = 1, best_p_scar = 1, best_p_removal = 1;
};
ABSummary summarizeAB(const nlohmann::json& manifest, const std::vector<ABCopyResult>& scores);

// The tests themselves. Fisher's exact test on a 2×2 table (two-sided);
// a permutation test on the difference in means (two-sided).
double fisherExact(int ok_a, int not_a, int ok_b, int not_b);
double permutationTest(const std::vector<double>& a, const std::vector<double>& b);

}  // namespace fruit
