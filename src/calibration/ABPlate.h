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

#include <nlohmann/json.hpp>

#include "mesh/TriangleMesh.h"
#include "printer/PrinterProfile.h"

namespace fruit {

struct ABPlateOptions {
    int copies = 8;
    uint32_t seed = 1;
    double margin = 4.0;  // mm between neighbouring footprints
};

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

}  // namespace fruit
