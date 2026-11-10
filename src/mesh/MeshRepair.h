// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Making a broken mesh printable.
//
// A mesh slices into layers with a clear inside and outside when every edge
// is used as often one way round as the other: then the outlines cut from it
// close, and the slicer's non-zero union says what is solid. Edges where
// that fails are "open": at holes, along seams that were never joined, and
// where a face is turned the wrong way.
//
// The repair never moves a point of the model. It only
//
// - drops faces that are there twice;
// - turns faces so that neighbours run the same way round, in each sheet of
//   faces joined edge to edge, and across a seam between two open sheets;
//   a piece that needed repair is then turned to face outward as a whole;
// - adds faces: over each hole, and across each gap between open edges that
//   run close to each other. The patch over a loop of open edges is the one
//   with the least area (found exactly for loops of up to 96 corners; longer
//   loops are first cut along short chords). For a flat hole that is the
//   flat cap, and for a crack it is the strip that zips it shut. Loops that
//   come within `seam_gap` of each other are patched together, so the two
//   sides of an unjoined seam are zipped rather than each capped;
// - drops loose surfaces with no thickness: closed by their patch they hold
//   no volume, and where the patch lands on the surface itself the two
//   cancel.
//
// Faces it adds go last in the mesh, so they can be told apart.
//
// A hollow inside a model is a shell that faces inward, and stays one only
// if that shell needed no repair: a piece the repair had to close can't be
// told from one that is simply inside out, which printed as a hollow would
// leave a hole in whatever it overlaps. A broken hollow prints solid.

#include <cstddef>

#include "mesh/TriangleMesh.h"

namespace fruit {

struct RepairOptions {
    // Open edges this near each other count as one seam. 0: from the model's
    // size: 2 ‰ of its diagonal, at most 0.1.
    //
    // Like every length here and in the report, this is in the file's own
    // units: mm, unless the model is scaled afterwards, when the report's
    // sizes scale with it.
    double seam_gap = 0;
};

struct RepairReport {
    std::size_t open_edges_before = 0;  // as loaded
    std::size_t open_edges_after = 0;   // 0: watertight
    std::size_t faces_flipped = 0;      // faces of the file that now run the other way round
    std::size_t faces_removed = 0;      // doubled faces and loose surfaces with no thickness
    std::size_t gaps_closed = 0;        // cracks and seams: patches narrower than the seam gap
    double widest_gap = 0;              // the widest one's width, averaged along it
    std::size_t holes_filled = 0;       // wider patches
    double largest_hole = 0;            // across
    double patch_area = 0;              // of all faces added
    std::size_t patch_faces = 0;        // how many: the last faces of the mesh
    double biggest_patch_area = 0;      // of the one patch with the most
    double biggest_patch_across = 0;
    Vec3 biggest_patch_at;              // its middle (model coordinates)
    Vec3 biggest_patch_facing;          // the way it faces, outward, as the repair leaves it

    bool changed() const { return faces_flipped || faces_removed || patch_faces; }
    bool watertight() const { return open_edges_after == 0; }
};

// Edges not used as often one way round as the other (0 for a mesh that
// slices cleanly, however many faces meet at an edge).
std::size_t countOpenEdges(const TriangleMesh& mesh);

// Repairs `mesh` in place. A mesh without open edges is left untouched; so
// is one the repair could not improve.
RepairReport repairMesh(TriangleMesh& mesh, const RepairOptions& options = {});

}  // namespace fruit
