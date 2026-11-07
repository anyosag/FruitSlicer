// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The job being worked on: the model as loaded, what the check found, and
// where it sits. Placement edits are undoable.

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "Orient.h"
#include "core/JobControl.h"
#include "mesh/ModelCheck.h"
#include "mesh/ModelLoad.h"
#include "mesh/Placement.h"
#include "printer/PrinterProfile.h"

namespace gui {

// The Orient step's choices and results for this job.
struct OrientState {
    std::vector<fruit::Vec3> visible;  // sides people will see, model coordinates
    int settings = 0;                  // bumped whenever `visible` changes
    // How it sits now, measured for (revision, settings).
    OrientMetrics now;
    int now_revision = -1, now_settings = -1;
    // The search's options, found for (revision, settings).
    std::shared_ptr<const OrientSearch> results;
    int results_revision = -1, results_settings = -1;
    bool decided = false;  // an option was used, or the current one kept

    bool nowFresh(int revision) const { return now_revision == revision && now_settings == settings; }
    bool resultsFresh(int revision) const {
        return results && results_revision == revision && results_settings == settings;
    }
};

class Document {
public:
    std::string path;  // file it came from
    std::string name;  // file name without folder and extension
    fruit::LoadedModel model;
    fruit::MeshCheck check;      // what loading found
    std::size_t low_points = 0;  // mid-air spots as first placed
    fruit::PrinterProfile printer;
    int geometry_id = 0;         // unique per load: the renderer re-uploads when it changes
    OrientState orient;

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
