// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Placing the model on the plate: the handles in the 3D view (arrows that
// move it, rings that turn it, after Formlabs PreForm's), the quick fixes
// (a quarter turn, sliding it back onto the plate) and the size the health
// dialog's choice gives. Plain functions, so the tests can check them.

#include <optional>

#include "geometry/Transform.h"
#include "mesh/ModelCheck.h"
#include "mesh/Placement.h"
#include "mesh/TriangleMesh.h"
#include "printer/PrinterProfile.h"

namespace gui {

// The size choice in the health dialog.
enum class SizeChoice { Full, Largest, Custom, Inches };

// The placement the health dialog's choice gives `model` (centred, raised).
// Full size is turned a quarter when only that makes it fit, as the dialog
// says.
fruit::Placement sizeChoicePlacement(const fruit::TriangleMesh& model, const fruit::PrinterProfile& printer,
                                     SizeChoice choice, double custom_percent);

// An angle in degrees as the fields show it: in (-180, 180], with the
// rounding noise of turning it back and forth taken off.
double tidyAngle(double deg);
// `v` to the nearest multiple of `step`.
double snapTo(double v, double step);

// The rotation (degrees about X, then Y, then Z, the plate's axes) after
// turning a model at `rotation_deg` a further `deg` about the plate's axis
// `axis` (0 X, 1 Y, 2 Z), as the fields show it.
fruit::Vec3 turnAbout(const fruit::Vec3& rotation_deg, int axis, double deg);
// The angles X, Y, Z (degrees) of the rotation in `r` (R = Rz · Ry · Rx).
fruit::Vec3 eulerXYZ(const fruit::Transform& r);

// `p` turned a quarter about the vertical, then slid back onto the plate if
// the turn put it over the edge.
fruit::Placement turnedQuarter(const fruit::TriangleMesh& model, const fruit::Placement& p,
                               const fruit::PrinterProfile& printer);
// `p` with its middle moved the least distance that keeps it on the plate
// (unchanged when it is on it, or too big to be).
fruit::Placement slidOnto(const fruit::TriangleMesh& model, const fruit::Placement& p,
                          const fruit::PrinterProfile& printer);

// ---- The handles ----

enum class Handle { None, MoveX, MoveY, TurnX, TurnY, TurnZ };
inline bool isTurn(Handle h) { return h == Handle::TurnX || h == Handle::TurnY || h == Handle::TurnZ; }
inline int handleAxis(Handle h) {
    return h == Handle::MoveX || h == Handle::TurnX ? 0 : h == Handle::MoveY || h == Handle::TurnY ? 1 : 2;
}

// Where the handles go for a model whose outline is `box` (printer
// coordinates): the rings are great circles of a ball round it; the arrows
// lie on the plate (z = 0), pointing out from each side of the outline.
struct HandleFrame {
    fruit::Vec3 centre;    // the ball's centre: the middle of the outline
    double radius = 10;    // the rings' radius
    fruit::Vec2 arrow_gap;  // from the centre to where the X and the Y arrows start
    double arrow_len = 10;
};
HandleFrame handleFrame(const fruit::BBox3& box);

// Where a ray (from `origin` along `dir`) meets the plane through `point`
// square to `normal`; nothing when it runs along the plane or away from it.
std::optional<fruit::Vec3> hitPlane(const fruit::Vec3& origin, const fruit::Vec3& dir, const fruit::Vec3& point,
                                    const fruit::Vec3& normal);
// The angle (degrees) of `p` round `centre`, turning the right-handed way
// about plate axis `axis`, as seen in the plane square to it.
double angleAbout(const fruit::Vec3& centre, int axis, const fruit::Vec3& p);

// A drag in progress: where it started and how far it has gone. Moves are
// measured on the plate, turns in the ring's plane.
struct HandleDrag {
    Handle handle = Handle::None;
    fruit::Placement start;  // the placement when it was grabbed
    HandleFrame frame;       // where the handles were then (they stay put while dragging)
    fruit::Vec3 grab;        // move: where on the plate it was grabbed
    double last_angle = 0;   // turn: the angle last frame, and the turn so far (unwrapped)
    double turned = 0;
    fruit::Placement now;    // what it gives so far
    bool active() const { return handle != Handle::None; }
};
// Starts dragging `h` with the mouse ray (origin, dir); false when the ray
// doesn't meet the handle's plane (seen edge on).
bool startDrag(HandleDrag& d, Handle h, const fruit::Placement& p, const HandleFrame& frame, const fruit::Vec3& origin,
               const fruit::Vec3& dir);
// The placement the drag gives with the mouse ray now. `snap`: moves to
// whole millimetres, turns to 15°; otherwise moves to 0.1 mm, turns to 0.1°.
// Unchanged from the last frame when the ray misses the plane.
fruit::Placement dragTo(HandleDrag& d, const fruit::Vec3& origin, const fruit::Vec3& dir, bool snap);
// The placement a drag of `h` by `amount` (mm along the arrow, or degrees
// about the ring's axis) gives: what dragTo works out, for scripts and tests.
fruit::Placement draggedBy(const fruit::Placement& start, Handle h, double amount, bool snap);

inline constexpr double kTurnSnap = 15.0;  // degrees, with ⇧

}  // namespace gui
