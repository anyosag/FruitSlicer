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
    std::vector<TipEdit> edits;                 // made by hand, in model coordinates
    std::shared_ptr<const SupportResult> result;
    int result_revision = -1, result_settings = -1;  // what it was made for
    int selected_tip = -1;                      // index into result->points

    // Made for this placement, these picks and these choices.
    bool fresh(int revision, int orient_settings) const {
        return result && result_revision == revision && result_settings == orient_settings &&
               result->choices == choices && result->edits == edits;
    }
};

// The Hollow screen's choices and plan for this job (canvas board "Hollow").
// Nothing changes what prints until they are applied.
struct HollowState {
    HollowChoices choices;
    std::vector<fruit::DrainHole> manual;    // holes placed by hand, model coordinates
    bool applied = false;                    // what prints follows these choices
    int settings = 0;                        // bumped whenever what prints would change
    std::shared_ptr<const HollowPlan> plan;  // the latest, for the screen
    int plan_revision = -1;

    bool planFresh(int revision) const {
        return plan && plan_revision == revision && plan->choices == choices && plan->manual == manual;
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
    std::vector<TipEdit> edits;
    bool hollow_applied = false;  // the Hollow screen
    HollowChoices hollow;
    std::vector<fruit::DrainHole> holes;
    std::string sample_path;
    double exposure = 0, bottom_exposure = 0;
    int bottom_layers = -1, transition_layers = -1;
    bool operator==(const JobStamp&) const = default;
};

class Document {
public:
    Document() = default;
    // Copies (a worker's snapshot) get the placed mesh made first, on the
    // thread that copies, so no copy ever makes it lazily.
    Document(const Document& o) {
        o.placed();
        *this = o;
    }
    Document& operator=(const Document&) = default;
    Document(Document&&) = default;
    Document& operator=(Document&&) = default;

    std::string path;  // file it came from
    std::string name;  // file name without folder and extension
    fruit::LoadedModel model;
    fruit::MeshCheck check;      // what loading found
    std::size_t low_points = 0;  // mid-air spots as first placed
    fruit::PrinterProfile printer;
    int geometry_id = 0;         // unique per load: the renderer re-uploads when it changes
    OrientState orient;
    HollowState hollow;
    SupportState supports;
    LayerState layers;
    PrintState print;
    std::string job_path;               // the .fruitjob it was saved to or opened from ("" if never saved)
    std::optional<JobStamp> saved;      // as it was then
    JobStamp stamp() const;
    bool unsaved() const { return !saved || !(*saved == stamp()); }

    const fruit::Placement& placement() const { return placement_; }
    const fruit::Transform& transform() const { return transform_; }  // model -> printer
    const fruit::TriangleMesh& placed() const;                         // printer coordinates
    const fruit::BBox3& placedBox() const { return placed_box_; }      // its outline, kept up to date while dragging
    const fruit::PlateFit& fit() const { return fit_; }
    // Names the placement (and model) shown now: a new number for every
    // change, and the same number again when undo or redo goes back to a
    // placement. Results carry it, so what was made for a placement counts
    // again once it is back.
    int revision() const { return revision_; }
    // What supports are made for besides the placement: the sides people
    // see and, once applied, the Hollow screen's choices.
    int supportSettings() const { return orient.settings * 100003 + (hollow.applied ? 1 + hollow.settings : 0); }

    // Sets the placement without recording it (loading, the health dialog).
    void place(const fruit::Placement& p);
    // An edit the user can undo (nothing happens when it changes nothing).
    void edit(const fruit::Placement& p);
    // The mirror image of the model along the plate's axis (0 X, 1 Y, 2 Z):
    // an undoable edit like any other. Everything that acts on "the model" goes
    // through here, so it can later point at the selected part.
    void mirror(int axis);
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    void undo();
    void redo();
    // Dragging a handle in the 3D view: begin, any number of previews (the
    // transform and the fit follow at once; the placed mesh is made again
    // only when asked for), then end, which is one undo step for the whole
    // drag, or cancel, which puts it back as it was.
    void beginDrag();
    void preview(const fruit::Placement& p);
    void endDrag();
    void cancelDrag();
    bool dragging() const { return drag_from_.has_value(); }
    // The model file itself was replaced: everything made for the old one is
    // out of date, and undo can't go back past it.
    void modelChanged();

private:
    struct Placed {
        fruit::Placement placement;
        int revision = 0;
    };
    void set(const fruit::Placement& p);  // placement, transform, outline and fit; the mesh later
    fruit::Placement placement_;
    fruit::Transform transform_;
    mutable fruit::TriangleMesh placed_;
    mutable bool placed_stale_ = true;
    fruit::BBox3 placed_box_;
    fruit::PlateFit fit_;
    int revision_ = 0, last_revision_ = 0;
    std::vector<Placed> undo_, redo_;
    std::optional<Placed> drag_from_;
};

// A new value for Document::geometry_id.
int newGeometryId();

// Reads, checks and places a model file (worker thread).
std::shared_ptr<Document> loadDocument(const std::string& path, const fruit::PrinterProfile& printer,
                                       const fruit::JobControl& ctl);

// A small bracket with a shelf: used by scripts and the self-test.
fruit::TriangleMesh demoModel();
// The bracket with faults, for the repair's screens and tests:
// "worn": a slit in the post, three faces of the upright turned, one face
//         there twice (repaired, nothing worth a look);
// "open": the post's top and the shelf's top missing (repaired with big
//         patches);
// "loose": not the bracket but a 20 mm cube as six squares 0.7 mm apart, too
//         far to be seams (can't be repaired).
fruit::TriangleMesh demoFaultyModel(const std::string& kind);

}  // namespace gui
