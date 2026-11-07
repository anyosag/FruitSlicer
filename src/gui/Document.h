// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The job being worked on: the model as loaded and where it sits.

#include <memory>
#include <string>

#include "core/JobControl.h"
#include "mesh/ModelLoad.h"
#include "mesh/Placement.h"
#include "printer/PrinterProfile.h"

namespace gui {

struct Document {
    std::string path;        // file it came from
    std::string name;        // file name without folder and extension
    fruit::LoadedModel model;
    fruit::Placement placement;
    fruit::TriangleMesh placed;  // printer coordinates
    int revision = 0;            // bumped on every change; results carry it

    void place();  // recompute `placed` from `model` and `placement`
    bool fitsPlate(const fruit::PrinterProfile& printer) const;
};

// Reads and places a model file (worker thread).
std::shared_ptr<Document> loadDocument(const std::string& path, const fruit::JobControl& ctl);

// A small bracket with a shelf: used by scripts and the self-test.
fruit::TriangleMesh demoModel();

}  // namespace gui
