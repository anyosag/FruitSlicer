// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Where a model sits on the build plate.

#include "geometry/Transform.h"
#include "mesh/TriangleMesh.h"

namespace fruit {

// How the user placed a model: first turned the other way round along any
// of its own axes (`mirror_*`, a mirror image: the triangles are turned
// inside out with it, so the outside stays outside), then uniform scale, then rotation about X,
// then Y, then Z (degrees), then the middle of its outline put `offset` mm
// from the plate's centre in XY (0, 0: centred) with its lowest point
// `elevation` mm above the plate (0 when it stands on the plate).
struct Placement {
    double scale = 1.0;
    Vec3 rotation_deg{0, 0, 0};
    bool mirror_x = false, mirror_y = false, mirror_z = false;  // along the model's own axes, before the rest
    double elevation = 5.0;  // lift above the plate (raft + supports underneath)
    bool on_plate = false;   // flat on the plate: no lift, no raft
    // Height added under a model that stands on the plate (no raft), mm: the
    // printer squashes its first layers, so the part would print short.
    // Used only when on_plate; added as whole layers (heightCorrectionLayers).
    double height_correction = 0.10;
    Vec2 offset{0, 0};       // where on the plate, from its centre (+X right, +Y back)

    bool mirrored() const { return mirror_x || mirror_y || mirror_z; }
    // A mirror image of the file (an odd number of flips), not just turned.
    bool mirrorImage() const { return ((mirror_x ? 1 : 0) + (mirror_y ? 1 : 0) + (mirror_z ? 1 : 0)) % 2 == 1; }
    double lift() const { return on_plate ? 0.0 : elevation; }
    // Layers of `layer_height` added under the model: 0 with a raft.
    int heightCorrectionLayers(double layer_height) const;
    // Their thickness, mm (the correction rounded to whole layers).
    double heightCorrectionAdded(double layer_height) const { return heightCorrectionLayers(layer_height) * layer_height; }
    // How far the model's lowest point is raised in the print: the lift with a
    // raft, the height correction's layers (at the job's 0.05 mm) without one.
    double heightUnder() const { return on_plate ? heightCorrectionAdded(0.05) : elevation; }
    bool centred() const { return offset.x == 0 && offset.y == 0; }
    bool operator==(const Placement&) const = default;
};

// Largest height correction the app offers, mm.
inline constexpr double kMaxHeightCorrection = 0.5;

// The mirror along the model's own axes in `p` (a mirror turns the way round
// a triangle's corners go: apply it with TriangleMesh::transform).
Transform mirrorTransform(const Placement& p);
// What turns a model's directions into the plate's: the mirror, then the
// rotation (no scale). The same for the three sizes of its outline.
Transform placementRotation(const Placement& p);

// Rotate (degrees, X then Y then Z), then translate so the model is centred
// on the plate in XY and its lowest point sits at `elevation` mm. Returns the
// transform that was applied.
Transform placeOnPlate(TriangleMesh& mesh, const Vec3& rotation_deg, double elevation);

// The model-to-printer transform for `p`, computed from `model`'s vertices
// (its outline's middle at `p.offset`).
Transform placementTransform(const TriangleMesh& model, const Placement& p);

// `model` (in its own coordinates) placed on the plate per `p`.
TriangleMesh placeModel(const TriangleMesh& model, const Placement& p);

}  // namespace fruit
