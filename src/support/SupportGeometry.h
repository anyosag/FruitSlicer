// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Support primitives tessellated into closed shells. Shapes overlap each other
// and the model on purpose (tip penetration); the slicer's NonZero union
// merges them.
//
// Tessellation keeps chord error at or below `chord_error` (default 7 µm, half
// a 14 µm Saturn 16K pixel), e.g. ~9 segments for a 0.2 mm tip.

#include "mesh/TriangleMesh.h"

namespace fruit {

struct PinheadSpec {
    double tip_diameter = 0.4;   // contact sphere diameter
    double penetration = 0.2;    // depth the contact sphere sinks into the part
    double head_length = 1.0;    // cone length between contact and shaft (flex zone starts here)
    double shaft_diameter = 1.0; // diameter where the head meets the shaft
};

struct GeometryQuality {
    double chord_error = 0.007;
    int segments(double radius) const;
};

// Pinhead touching the part at `contact`, extending along unit `dir` (away
// from the part, usually downward). Returns the junction point where a shaft
// or bridge attaches.
Vec3 appendPinhead(TriangleMesh& m, const Vec3& contact, const Vec3& dir, const PinheadSpec& s,
                   const GeometryQuality& q = {});

// Vertical (or slanted) shaft between two points. `top_joint` adds a sphere
// at `top` (skip it when a pinhead junction sphere is already there).
void appendShaft(TriangleMesh& m, const Vec3& top, const Vec3& bottom, double diameter,
                 const GeometryQuality& q = {}, bool top_joint = true);

// Cone widening a shaft onto the raft: from radius r_shaft at `top` to
// r_base at height `base_z`.
void appendBaseCone(TriangleMesh& m, const Vec3& top, double shaft_diameter, double base_diameter,
                    double base_z, const GeometryQuality& q = {});

// Direction a head leaves the part: the outward surface normal, tilted
// towards straight down until it is at most `max_angle_deg` from vertical.
Vec3 headDirection(const Vec3& outward_normal, double max_angle_deg = 45.0);

// Closed hexahedron from 8 corners: v[0..3] bottom face, v[4..7] top face,
// both counter-clockwise seen from above. Used for wedges and sheared blocks.
void appendHexahedron(TriangleMesh& m, const Vec3 (&v)[8]);

// Raft slab from z0 to z1 covering `min`..`max` in XY, with the top edge
// chamfered inward by `chamfer` mm so a scraper can get under it.
void appendRaft(TriangleMesh& m, const Vec2& min, const Vec2& max, double z0, double z1,
                double chamfer = 0.0);

}  // namespace fruit
