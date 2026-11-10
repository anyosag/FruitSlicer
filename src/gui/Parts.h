// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The parts of a job: every model on the plate, each with its own mesh and
// its own placement. No UI here.
//
// How a job uses them
//   A job (Document) holds one part or more, in order, and one of them is
//   selected: the tools (Move, Rotate, Scale, On plate, Repair...) act on
//   the selected part. What prints is every part placed, together:
//   Document::placed() is all the parts' placed meshes in one mesh, so the
//   supports, the hollows and drain holes, the layers and the print file see
//   one model with several shells (they already cope with that: the slicer
//   merges overlapping shells with a NonZero union). The parts stay separate
//   for editing.
//
//   The lift and the raft are the job's: every part stands at the same
//   height above the plate (Placement::elevation and on_plate are kept the
//   same on every part; changing them on one changes them on all).
//
//   Tip edits and drain holes placed by hand are kept in their part's own
//   coordinates and name the part (TipEdit::part, DrainHole::part), so they
//   follow it when it moves. While their part is off the job (removed, and
//   not yet put back by undo) they are left out. The sides people see
//   (OrientState) are kept for each part.
//
// The API, on Document (see Document.h):
//   parts(), part() (the selected one), findPart(id), partIndex(id),
//   selectedId(), select(id)                    list and select
//   addPart(NewPart)                            add one (undoable), returns its id
//   removePart(id)                              remove one (undoable; never the last)
//   replaceMesh(id, model)                      a part's mesh changed (undoable)
//   replaceParts(parts)                         all of them at once (undoable)
//   editParts(fn)                               any change to the list as ONE undo step
//   placed(), placedBox(), partOfFace(face)     the job as it prints
//   Part::placed(), Part::footprint(),          each part placed, and what it
//   Part::footprintBox()                        covers seen from above
//   frames()                                    each part's model -> printer transform
// Every change to a part's mesh or placement, adding and removing, gives the
// job a new revision(): supports, hollow plans and layers made for the old
// one are out of date, as after any other model edit, and undo brings the
// old revision (and with it what was made for it) back.

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "geometry/Polygon.h"
#include "geometry/Transform.h"
#include "mesh/ModelCheck.h"
#include "mesh/ModelLoad.h"
#include "mesh/Placement.h"
#include "printer/PrinterProfile.h"

namespace gui {

// A part's mesh as loaded (and repaired), in its own coordinates, with what
// checking it found. Never changed once made: parts, undo steps and the
// workers' copies share it.
struct PartModel {
    fruit::LoadedModel model;  // the mesh, and what the repair did to it
    fruit::MeshCheck check;    // what the check found
    int geometry_id = 0;       // unique: the 3D view uploads each one once
};

// A model checked, with a new geometry id.
std::shared_ptr<const PartModel> makePartModel(fruit::LoadedModel m);

// What a part is, as an edit sets it.
struct PartState {
    int id = -1;                             // stable in the job; given by the job
    std::string name;                        // shown in the Parts list
    std::string path;                        // the file it came from ("" when made in the app)
    std::shared_ptr<const PartModel> model;  // its mesh
    fruit::Placement placement;              // where it sits (the lift is the job's)
};

// A part to add: Document::addPart gives it an id.
struct NewPart {
    std::string name, path;
    std::shared_ptr<const PartModel> model;
    fruit::Placement placement;
};

// One part in the job, with what follows from its placement.
class Part {
public:
    int id() const { return s_.id; }
    const std::string& name() const { return s_.name; }
    const std::string& path() const { return s_.path; }
    const PartState& state() const { return s_; }
    const std::shared_ptr<const PartModel>& shared() const { return s_.model; }
    const fruit::LoadedModel& model() const { return s_.model->model; }
    const fruit::TriangleMesh& mesh() const { return s_.model->model.mesh; }  // its own coordinates
    const fruit::MeshCheck& check() const { return s_.model->check; }
    int geometryId() const { return s_.model->geometry_id; }
    const fruit::Placement& placement() const { return s_.placement; }
    // Names this part's mesh and placement: a new number for every change to
    // either, the same again when undo goes back. Numbers are never shared
    // between parts.
    int revision() const { return revision_; }
    const fruit::Transform& transform() const { return transform_; }  // own -> printer
    const fruit::BBox3& box() const { return box_; }                  // its placed outline
    const fruit::PlateFit& fit() const { return fit_; }               // alone on the plate, where it is
    // Placed, in printer coordinates: made when first asked for.
    const fruit::TriangleMesh& placed() const;
    // What it covers seen from above: the convex outline of its placed mesh
    // (printer coordinates, counter-clockwise), and its rectangle.
    const std::vector<fruit::Vec2>& footprint() const;
    fruit::BBox2 footprintBox() const;

private:
    friend class Document;
    void update(const fruit::PrinterProfile& printer);  // transform, box and fit; the mesh later
    PartState s_;
    int revision_ = 0;
    fruit::Transform transform_;
    fruit::BBox3 box_;
    fruit::PlateFit fit_;
    mutable std::shared_ptr<const fruit::TriangleMesh> placed_;
    mutable std::shared_ptr<const std::vector<fruit::Vec2>> footprint_;
};

// A working copy of the job's parts, for Document::editParts: change it
// freely; the job takes it as one undo step.
class PartsEdit {
public:
    std::vector<PartState>& parts() { return parts_; }
    PartState* find(int id);
    // Adds a part at the end and returns its new id.
    int add(NewPart p);
    bool remove(int id);
    int selected = -1;  // the part selected once it is done (an id; another is chosen if it is gone)

private:
    friend class Document;
    std::vector<PartState> parts_;
    int next_id_ = 0;
};

// Each part's model -> printer transform. Things kept in a part's own
// coordinates (tip edits, drain holes) find their part's frame here.
class PartFrames {
public:
    PartFrames() = default;
    // One frame for every part: a job with one model, and tests. Implicit,
    // so code that has one transform passes it as it is.
    PartFrames(const fruit::Transform& all) : all_(all) {}  // NOLINT(google-explicit-constructor)
    void add(int part, const fruit::Transform& t) { frames_.emplace_back(part, t); }
    // Null when that part is not on the plate.
    const fruit::Transform* find(int part) const;

private:
    std::vector<std::pair<int, fruit::Transform>> frames_;
    std::optional<fruit::Transform> all_;
};

// Room left between parts placed side by side, mm: their rafts and the
// supports at their edges need it.
inline constexpr double kPartGap = 5.0;

// Where a part `size` (its outline seen from above, mm) can go on the plate
// without overlapping any of `taken` (other parts' outlines, printer
// coordinates), `gap` mm clear of them and inside the plate's margins: the
// free spot nearest the middle of what is taken (the plate's centre when
// nothing is). Off the plate, to the right of everything, when there is no
// room.
fruit::Vec2 freeSpot(const std::vector<fruit::BBox2>& taken, fruit::Vec2 size, const fruit::PrinterProfile& printer,
                     double margin, double gap = kPartGap);

// The name a part from `path` gets: its file name without the folder and
// extension ("Part" when there is none), with " 2", " 3"... after it when
// another part already has it.
std::string partName(const std::string& path, const std::vector<std::string>& taken);

}  // namespace gui
