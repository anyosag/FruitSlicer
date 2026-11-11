// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The shape tools' logic: Cut (a plane through a part), Label (letters
// raised or engraved on a part's surface) and Combine (join, subtract or keep
// the overlap of two parts). Plain functions and small states, so the tests
// can check them; no UI here.
//
// The work runs on the plate's coordinates: a part's placed mesh goes in, and
// the result comes back in the part's own coordinates, so a Cut or a Label
// needs nothing special for a part that is scaled, turned or mirrored. Every
// operation is meant to run in a worker (App::cutApply and the others do) and
// checks for cancelling between its steps; Manifold itself can't be stopped
// part way, so a cancelled one finishes and its result is dropped.
//
// How the results go into the job (all one undo step, through
// Document::editParts):
//   Cut      the part goes, and one or two parts of its own take its place:
//            the first stays where it was, the other goes beside the others.
//            (A piece above the cut comes down: its lowest point stands at the
//            job's lift, so a flat cut face sits on the plate.)
//   Label    the part keeps its id and its place; its mesh changes.
//   Combine  both parts go and one takes their place, where A was.

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Parts.h"
#include "core/JobControl.h"
#include "geometry/Vec.h"
#include "Navigation.h"
#include "mesh/TriangleMesh.h"
#include "ops/MeshOps.h"
#include "ops/TextSolid.h"

namespace gui {

class Document;

// ---- What a worker gives back ----

// One part-to-be: a mesh in its own coordinates, and where it came from.
struct ShapePiece {
    std::shared_ptr<const PartModel> model;
    std::string suffix;                // " (top)": what tells it from the others; empty for one
    fruit::Transform own_to_plate;     // where its own coordinates stood on the plate
};

struct ShapeOutcome {
    bool ok = false;
    std::string message;  // plain words: what went wrong, or a note about what was done
    std::vector<ShapePiece> pieces;
};

// `base` with the offset that keeps `piece` where it stood on the plate (its
// middle moves when its outline changes; this puts it back).
fruit::Placement placeInPlace(const ShapePiece& piece, fruit::Placement base);

// ---- Cut ----

enum class CutAxis { Z, X, Y };
enum class CutKeep { Both, Above, Below };  // above: the side the plane's normal points to

struct CutSettings {
    CutAxis axis = CutAxis::Z;
    // Z: mm above where the part stands (the lift is not counted); X and Y:
    // mm from the plate's centre.
    double position = 0;
    CutKeep keep = CutKeep::Both;
    int part = -1;         // the part `position` was set for (an id)
    bool placed = false;   // `position` was set, for `part` and `axis`
    CutAxis placed_axis = CutAxis::Z;
};

// Names for the sides of a cut: Z top / bottom, X right / left, Y back / front.
const char* cutSideWord(CutAxis a, bool above);
// The positions the plane can take through a part, and the middle of them.
struct CutRange {
    double lo = 0, hi = 0;
};
CutRange cutRange(const Part& part, CutAxis axis);
// Puts `position` inside the part's range, in the middle when it has none for
// this part and axis yet.
void cutFit(CutSettings& s, const Part& part);
// The plane in plate coordinates (the normal points to the "above" side).
fruit::ops::Plane cutPlane(const Part& part, const CutSettings& s);
// The plane passes through the part (not along its edge).
bool cutCrosses(const Part& part, const CutSettings& s);

struct CutJob {
    fruit::TriangleMesh placed;  // the part, on the plate
    fruit::Transform own_to_plate;
    fruit::ops::Plane plane;
    CutAxis axis = CutAxis::Z;
    CutKeep keep = CutKeep::Both;
    std::string name;
};
CutJob cutJobFor(const Part& part, const CutSettings& s);
// The pieces, bottom (left, front) first.
ShapeOutcome cutPart(const CutJob& job, const fruit::JobControl& ctl = fruit::JobControl::none());

// Goes into the job: `old` out, the pieces in. Returns the new parts' ids.
std::vector<int> cutIntoParts(PartsEdit& e, const Part& old, const ShapeOutcome& out, const fruit::PrinterProfile& printer);

// ---- Label ----

// Where the letters go: a spot on a part, kept in the part's own coordinates
// so it follows the part when that moves.
struct LabelSpot {
    int part = -1;
    int geometry_id = -1;  // the mesh it was picked on
    fruit::Vec3 point, normal;  // own coordinates; the normal points out of the surface
};

// Letters' outline as it shows in the view before they are applied.
struct LabelPreview {
    std::string key;  // text, height and depth it was made for
    bool ok = false;
    std::string message;
    fruit::TriangleMesh text;  // as ops::makeTextSolid gives it
    std::vector<std::pair<fruit::Vec3, fruit::Vec3>> outline;  // the flat side's edges, in `text`'s coordinates
};

struct LabelSettings {
    std::string text;
    double height = 6.0;  // mm, capital letters
    double depth = 0.8;   // mm
    bool engrave = false;
    std::optional<LabelSpot> spot;
    LabelPreview preview;
};

constexpr std::size_t kLabelMaxChars = 60;
// Limits the fields take.
constexpr double kLabelMinHeight = 1.0, kLabelMaxHeight = 100.0;
constexpr double kLabelMinDepth = 0.1, kLabelMaxDepth = 10.0;

// The letters (made again only when the text, height or depth changed).
const LabelPreview& labelPreview(LabelSettings& s);
// The spot is still on the job as it is: its part is there with the mesh it
// was picked on.
bool labelSpotStillOn(const LabelSettings& s, const Document& doc);
// Where the letters stand on the plate for a spot on `part`: letters upright,
// their tops toward the plate's back where the face is flat on the plate.
fruit::ops::TextPlacement labelPlacement(const Part& part, const LabelSpot& spot, bool engrave);
// The spot for a point on the plate and the surface's outward normal there.
LabelSpot labelSpotAt(const Part& part, const fruit::Vec3& on_plate, const fruit::Vec3& normal_on_plate);
// The preview's outline moved to where the letters would stand.
std::vector<std::pair<fruit::Vec3, fruit::Vec3>> labelOutlineOnPlate(const LabelPreview& p,
                                                                      const fruit::ops::TextPlacement& at);

struct LabelJob {
    fruit::TriangleMesh placed;
    fruit::Transform own_to_plate;
    fruit::ops::TextPlacement place;
    fruit::ops::TextStyle style;
    std::string text, name;
};
LabelJob labelJobFor(const Part& part, const LabelSettings& s);
ShapeOutcome labelPart(const LabelJob& job, const fruit::JobControl& ctl = fruit::JobControl::none());
// The part keeps its id and its place; its mesh is the labelled one.
void relabelPart(PartsEdit& e, int id, const ShapeOutcome& out);

// ---- Combine ----

enum class CombineMode { Join, Subtract, Overlap };  // A and B, A minus B, only where both are

struct CombineSettings {
    CombineMode mode = CombineMode::Join;
    int b = -1;  // the second part (an id); A is the selected part
};
const char* combineVerb(CombineMode m);  // "Join", "Subtract", "Keep the overlap"
// The second part: `s.b` while it is in the job and isn't A; else the only
// other part when there is one; else -1.
int combineSecond(const CombineSettings& s, const Document& doc);

struct CombineJob {
    fruit::TriangleMesh a, b;  // both on the plate
    CombineMode mode = CombineMode::Join;
    std::string a_name, b_name;
};
ShapeOutcome combineParts(const CombineJob& job, const fruit::JobControl& ctl = fruit::JobControl::none());
// Both go, one takes their place: A's lift, and where the result stood.
int combineIntoPart(PartsEdit& e, const Part& a, const Part& b, const ShapeOutcome& out);

// A name for a piece of `base` not already in `taken`.
std::string pieceName(const std::string& base, const std::string& suffix, const std::vector<std::string>& taken);

// ---- What the app keeps for the three tools ----

struct ShapeState {
    CutSettings cut;
    LabelSettings label;
    CombineSettings combine;
    // What the last attempt came to, in words, and the tool it was for.
    std::string note;
    bool note_bad = false;
    Tool note_tool = Tool::None;
    int note_revision = -1;    // the job's revision when it was written: an edit or an undo since hides it
    bool running = false;      // one of them is working in the background
    const void* doc = nullptr; // the job these are for: another job starts them again
    std::string job;           // its name (the job runner's)
};

}  // namespace gui
