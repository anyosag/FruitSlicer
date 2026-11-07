// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The job being worked on: the model as loaded, what the check found, and
// where it sits. Placement edits are undoable.

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "core/JobControl.h"
#include "mesh/ModelCheck.h"
#include "mesh/ModelLoad.h"
#include "mesh/Placement.h"
#include "printer/PrinterProfile.h"

namespace gui {

class Document {
public:
    std::string path;  // file it came from
    std::string name;  // file name without folder and extension
    fruit::LoadedModel model;
    fruit::MeshCheck check;      // what loading found
    std::size_t low_points = 0;  // mid-air spots as first placed
    fruit::PrinterProfile printer;
    int geometry_id = 0;         // unique per load: the renderer re-uploads when it changes

    const fruit::Placement& placement() const { return placement_; }
    const fruit::Transform& transform() const { return transform_; }  // model -> printer
    const fruit::TriangleMesh& placed() const { return placed_; }      // printer coordinates
    const fruit::PlateFit& fit() const { return fit_; }
    int revision() const { return revision_; }  // bumped on every change; results carry it

    // Sets the placement without recording it (loading, the health dialog).
    void place(const fruit::Placement& p);
    // An edit the user can undo.
    void edit(const fruit::Placement& p);
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    void undo();
    void redo();

private:
    fruit::Placement placement_;
    fruit::Transform transform_;
    fruit::TriangleMesh placed_;
    fruit::PlateFit fit_;
    int revision_ = 0;
    std::vector<fruit::Placement> undo_, redo_;
};

// Reads, checks and places a model file (worker thread).
std::shared_ptr<Document> loadDocument(const std::string& path, const fruit::PrinterProfile& printer,
                                       const fruit::JobControl& ctl);

// A small bracket with a shelf: used by scripts and the self-test.
fruit::TriangleMesh demoModel();

}  // namespace gui
