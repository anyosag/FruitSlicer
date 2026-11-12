// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The job being worked on: its parts (each model on the plate: its mesh as
// loaded, what the check found, where it sits) and what is made for them.
// Edits to the parts are undoable.

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Layers.h"
#include "Orient.h"
#include "Parts.h"
#include "Print.h"
#include "Supports.h"
#include "core/JobControl.h"
#include "mesh/ModelCheck.h"
#include "mesh/ModelLoad.h"
#include "mesh/Placement.h"
#include "printer/PrinterProfile.h"

namespace gui {

// The Rotate tool's choices and results for one part. Revisions here are
// the part's own (Part::revision).
struct OrientState {
    std::vector<fruit::Vec3> visible;  // sides people will see, the part's own coordinates
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
    double scroll_carry = 0;  // trackpad scrolling over the chart that has not yet made a whole layer

    // Cut for these supports, with `base_layers` added under a model on the
    // plate (the height correction, a print setting: it changes the layers,
    // not the supports).
    bool fresh(const std::shared_ptr<const SupportResult>& supports, std::size_t base_layers) const {
        return result && supports && for_supports == supports && result->base_layers == base_layers;
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
    std::vector<int> parts;                // the parts' ids, in order
    std::vector<std::string> model_paths;  // where each part's model file is
    int revision = -1;                     // the parts' meshes and placements
    int orient_settings = -1;              // sides people will see
    std::vector<bool> orient_decided;      // each part's
    const void* supports = nullptr;  // the supports result, by identity
    SupportChoices choices;
    std::vector<TipEdit> edits;
    bool hollow_applied = false;  // the Hollow screen
    HollowChoices hollow;
    std::vector<fruit::DrainHole> holes;
    std::string sample_path;
    PrintEdits print;
    bool operator==(const JobStamp&) const = default;
};

// The job: its parts on the plate (see Parts.h), and what is made for them
// all together (supports, hollows, layers, the print file). Edits to the
// parts are undoable.
class Document {
public:
    Document() = default;
    // Copies (a worker's snapshot) get the placed meshes made first, on the
    // thread that copies, so no copy ever makes them lazily.
    Document(const Document& o) {
        o.prepare();
        *this = o;
    }
    Document& operator=(const Document&) = default;
    Document(Document&&) = default;
    Document& operator=(Document&&) = default;

    std::string name;            // the job's: its first model's file name without folder and extension
    std::size_t low_points = 0;  // mid-air spots of the first part as first placed (the health dialog)
    fruit::PrinterProfile printer;
    HollowState hollow;
    SupportState supports;
    LayerState layers;
    PrintState print;
    std::string job_path;               // the .fruitjob it was saved to or opened from ("" if never saved)
    std::optional<JobStamp> saved;      // as it was then
    JobStamp stamp() const;
    // The layers are cut for the current supports and the print settings'
    // height correction.
    bool layersFresh() const;
    bool unsaved() const { return !saved || !(*saved == stamp()); }

    // ---- The parts (see Parts.h) ----
    const std::vector<Part>& parts() const { return parts_; }
    std::size_t partCount() const { return parts_.size(); }
    // The selected part: the tools act on it. There is always one once a
    // model is placed.
    const Part& part() const { return parts_[static_cast<std::size_t>(selected_)]; }
    int selectedId() const { return parts_.empty() ? -1 : part().id(); }
    const Part* findPart(int id) const;
    int partIndex(int id) const;  // -1 when it is not in the job
    // Selects a part (not an edit: nothing to undo).
    void select(int id);
    // Adds a part (undoable) and selects it; its placement's lift is the
    // job's. Returns its id.
    int addPart(NewPart p);
    // Removes a part (undoable); false for the last one: a job keeps one.
    bool removePart(int id);
    // A part's mesh changed (undoable): its placement stays.
    bool replaceMesh(int id, fruit::LoadedModel m);
    bool replaceMesh(int id, std::shared_ptr<const PartModel> m);
    // Every part at once (undoable): the first is selected.
    void replaceParts(std::vector<NewPart> parts);
    // Any change to the parts as one undo step: `fn` gets a working copy of
    // the list. Nothing is recorded, and false comes back, when it is left
    // as it was. A part whose lift changed gives every part that lift.
    bool editParts(const std::function<void(PartsEdit&)>& fn);
    // Each part's model -> printer transform.
    PartFrames frames() const;

    // The selected part's placement, and its fit alone on the plate.
    const fruit::Placement& placement() const { return part().placement(); }
    const fruit::PlateFit& fit() const { return part().fit(); }
    // The job as it prints: every part placed, in order, in one mesh
    // (printer coordinates), and its outline (kept up to date while
    // dragging). The mesh is made when first asked for.
    const fruit::TriangleMesh& placed() const;
    // The same mesh, shared: for a cache that must tell when it is another one (the pick tree).
    std::shared_ptr<const fruit::TriangleMesh> placedShared() const {
        placed();
        return placed_;
    }
    const fruit::BBox3& placedBox() const { return box_; }
    // The part (an index into parts()) a face of placed() belongs to.
    int partOfFace(std::size_t face) const;
    // Where that part's faces begin in placed(): a face there is its mesh's
    // face `face - faceStart(part)`.
    std::size_t faceStart(int part_index) const;
    // The part (an id) whose surface is nearest a point (printer
    // coordinates): the one a tip or a hole there is on.
    int partNear(const fruit::Vec3& at) const;
    // Every part fits the plate where it is; as fit().fits_centred, for every part.
    bool fits() const;
    bool fitsCentred() const;
    // A part that doesn't fit where it is: the selected one if it doesn't,
    // else the first; null when they all fit.
    const Part* partOff() const;

    // The Rotate tool's choices and results: the selected part's, a part's.
    OrientState& orient() { return orient_[selectedId()]; }
    const OrientState& orient() const { return orientOf(selectedId()); }
    OrientState& orientOf(int id) { return orient_[id]; }
    const OrientState& orientOf(int id) const;
    // The sides people will see on every part, in printer directions: what
    // supports are planned for (they are the same for every part's faces).
    std::vector<fruit::Vec3> visibleInPrinter() const;
    bool orientDecided() const;  // for every part

    // Names the parts' meshes and placements shown now: a new number for
    // every change, and the same number again when undo or redo goes back.
    // Results carry it, so what was made for them counts again once it is
    // back.
    int revision() const { return revision_; }
    // What supports are made for besides the parts: the sides people see
    // and, once applied, the Hollow screen's choices.
    int supportSettings() const { return orientSettings() * 100003 + (hollow.applied ? 1 + hollow.settings : 0); }
    int orientSettings() const;  // changes whenever any part's sides do

    // Sets the selected part's placement without recording it (loading, the
    // health dialog).
    void place(const fruit::Placement& p);
    // An edit of the selected part's placement the user can undo (nothing
    // happens when it changes nothing).
    void edit(const fruit::Placement& p);
    // The selected part as its mirror image along the plate's axis (0 X, 1 Y,
    // 2 Z): an undoable edit of its placement like any other. Its middle and
    // its lift stay; the other parts are left as they are.
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
    // Undo can't go back past now (a model file replaced under the job).
    void clearHistory();

private:
    struct Snapshot {
        std::vector<Part> parts;  // without their placed meshes
        int selected = 0;
        int revision = 0;
    };
    Snapshot snapshot() const;
    void restore(Snapshot s);
    void record(Snapshot before);         // an undo step
    void changed();                       // outline, fit, the placed mesh later
    void setPlacement(std::size_t i, const fruit::Placement& p);  // and the lift on every part
    void prepare() const;                 // every placed mesh made
    std::vector<Part> parts_;
    int selected_ = 0;  // index into parts_
    int next_id_ = 0;
    std::map<int, OrientState> orient_;
    mutable std::shared_ptr<const fruit::TriangleMesh> placed_;
    mutable std::vector<std::size_t> face_start_;  // where each part's faces begin in placed_
    fruit::BBox3 box_;
    int revision_ = 0, last_revision_ = 0;
    std::vector<Snapshot> undo_, redo_;
    std::optional<Snapshot> drag_from_;
};

// A new value for Document::geometry_id.
int newGeometryId();

// Whether `seen` was taken from `job` (what a tool or a check kept is for this
// job). Never true for another job, even one that came to sit at the same
// address after the first was closed: its address and revision can repeat.
inline bool sameJob(const std::weak_ptr<const Document>& seen, const std::shared_ptr<const Document>& job) {
    return job && !seen.owner_before(job) && !job.owner_before(seen);
}

// Reads, checks and places a model file (worker thread): a job with that
// one part.
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
