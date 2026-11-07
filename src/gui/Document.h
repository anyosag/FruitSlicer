// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The job being worked on: the model as loaded, what the check found, and
// where it sits. Placement edits are undoable.

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Layers.h"
#include "Orient.h"
#include "Print.h"
#include "Supports.h"
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

// The Supports step's choices and result for this job.
struct SupportState {
    SupportChoices choices;                     // for the next run
    std::shared_ptr<const SupportResult> result;
    int result_revision = -1, result_settings = -1;  // what it was made for
    int selected_tip = -1;                      // index into result->points

    // Made for this placement, these picks and these choices.
    bool fresh(int revision, int orient_settings) const {
        return result && result_revision == revision && result_settings == orient_settings &&
               result->choices == choices;
    }
};

// The Layers step: the sliced print and where the viewer is.
struct LayerState {
    std::shared_ptr<const LayerResult> result;
    std::shared_ptr<const SupportResult> for_supports;  // the supports it was cut with
    int current = -1;         // layer shown (0-based); -1 = the biggest
    bool show_below = false;  // the layer below, dimmed, under this one

    bool fresh(const std::shared_ptr<const SupportResult>& supports) const {
        return result && supports && for_supports == supports;
    }
};

// The Print step: the print file's settings and the last save.
struct PrintState {
    PrintSettings settings;
    std::shared_ptr<const SaveResult> saved;   // the last file written and checked
    std::shared_ptr<const LayerResult> saved_from;  // the layers it was written from
    std::string drive;                          // where it went when on a removable drive ("" otherwise)
};

// What a saved job records, to tell whether anything changed since.
struct JobStamp {
    std::string model_path;   // where the model file is
    int revision = -1;        // placement
    int orient_settings = -1;  // sides people will see
    bool orient_decided = false;
    const void* supports = nullptr;  // the supports result, by identity
    SupportChoices choices;
    std::string sample_path;
    double exposure = 0, bottom_exposure = 0;
    int bottom_layers = -1, transition_layers = -1;
    bool operator==(const JobStamp&) const = default;
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
    SupportState supports;
    LayerState layers;
    PrintState print;
    std::string job_path;               // the .fruitjob it was saved to or opened from ("" if never saved)
    std::optional<JobStamp> saved;      // as it was then
    JobStamp stamp() const;
    bool unsaved() const { return !saved || !(*saved == stamp()); }

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

// A new value for Document::geometry_id.
int newGeometryId();

// Reads, checks and places a model file (worker thread).
std::shared_ptr<Document> loadDocument(const std::string& path, const fruit::PrinterProfile& printer,
                                       const fruit::JobControl& ctl);

// A small bracket with a shelf: used by scripts and the self-test.
fruit::TriangleMesh demoModel();

}  // namespace gui
