// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Solid geometry on closed meshes, for the model tools (Combine, Cut, Split,
// Label, Collisions). All lengths are millimetres, in the same frame as the
// TriangleMesh given.
//
// The booleans and plane cuts run on Manifold (Apache-2.0). It needs closed
// meshes, so every function says plainly in `message` when an input is not
// one (holes, loose edges, edges shared by three faces, no volume) and does
// nothing else. These functions use no threads. A Solid or a TriangleMesh
// may be read by several threads at once only if none of them changes it;
// do not share one Solid between threads (Manifold evaluates lazily).

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "geometry/BBox.h"
#include "mesh/TriangleMesh.h"

namespace fruit::ops {

// A mesh that has been checked and converted for the operations below, so
// that a part tested against many others is converted once.
class Solid {
public:
    Solid() = default;
    // Welds vertices (1e-6 mm), turns a wholly inside-out mesh round, and
    // checks that it is a closed solid. If not, valid() is false and
    // message() says why.
    explicit Solid(const TriangleMesh& mesh);

    bool valid() const { return valid_; }
    // Why it is not valid, or a note about what was changed (e.g. "its faces
    // pointed inwards, so they were turned round"). Empty when nothing to say.
    const std::string& message() const { return message_; }
    bool empty() const;      // valid but with no volume (e.g. two parts that do not meet)
    double volume() const;   // mm3
    BBox3 bbox() const;
    TriangleMesh toMesh() const;  // closed, merged vertices, faces counter-clockwise from outside

private:
    struct Impl;
    friend struct SolidAccess;
    std::shared_ptr<const Impl> impl_;
    bool valid_ = false;
    std::string message_;
};

// The shape `polygons` (millimetres in XY; outer outlines counter-clockwise,
// holes clockwise) pushed up from z = 0 to z = height. Invalid if the
// outlines cannot make a solid.
Solid extrude(const std::vector<std::vector<Vec2>>& polygons, double height);

// Result of an operation that makes one mesh. `ok` is false (and `mesh`
// empty) when an input could not be used or nothing came out where something
// was expected; `message` says which, in plain words. An ok result can still
// carry a note (e.g. "the two parts do not touch, so nothing is left").
struct MeshResult {
    TriangleMesh mesh;
    bool ok = false;
    std::string message;
};

// Round trip through the solid representation: merges vertices, drops
// degenerate faces. Fails, saying why, when the mesh is not a closed solid.
MeshResult cleanMesh(const TriangleMesh& mesh);

// Why `mesh` is not a closed solid in words ("12 edges are open"), or an empty
// string when it is one. Cheap: no conversion.
std::string whyNotSolid(const TriangleMesh& mesh);

// Booleans. Parts that only share faces (touch without overlapping) come out
// as expected: union fuses them, intersect is empty.
Solid unite(const Solid& a, const Solid& b);
Solid subtract(const Solid& a, const Solid& b);  // a minus b
Solid intersect(const Solid& a, const Solid& b);

MeshResult unionMeshes(const TriangleMesh& a, const TriangleMesh& b);
MeshResult subtractMeshes(const TriangleMesh& a, const TriangleMesh& b);  // a minus b
MeshResult intersectMeshes(const TriangleMesh& a, const TriangleMesh& b);

// A plane through `point`; `normal` points to the "above" side. It need not
// be unit length. Z cuts: normal (0,0,1); vertical cuts: (1,0,0) or (0,1,0).
struct Plane {
    Vec3 point{0, 0, 0};
    Vec3 normal{0, 0, 1};
};

struct CutResult {
    TriangleMesh above;  // on the side the normal points to; cut face filled
    TriangleMesh below;  // the other side; cut face filled
    bool ok = false;
    std::string message;  // also says when the plane misses the mesh (one side is empty)
};

CutResult cutByPlane(const TriangleMesh& mesh, const Plane& plane);
std::pair<Solid, Solid> cutByPlane(const Solid& solid, const Plane& plane);  // {above, below}

// One mesh per disconnected piece, largest volume first (ties: lowest
// vertex first). Shells with a negative volume (the cavity of a hollowed
// model) stay with the piece that holds them; a lone inside-out shell is
// turned round. Needs no closed mesh: loose sheets come out as pieces too.
std::vector<TriangleMesh> splitShells(const TriangleMesh& mesh);
// How many pieces splitShells would give.
int countPieces(const TriangleMesh& mesh);

struct CollisionResult {
    bool ok = false;           // false: an input was not a closed solid (see message)
    std::string message;
    bool overlaps = false;     // overlap volume above the threshold
    double overlap_volume = 0; // mm3, measured only when the bounding boxes meet
    // Smallest distance between the two surfaces when they do not overlap,
    // looked for up to `gap_search` mm; -1 when not asked for or farther
    // apart than that. 0 when they overlap or touch.
    double gap = -1;
};

// Do `a` and `b` overlap by more than `volume_threshold` mm3? With
// gap_search > 0 also the smallest gap (see above).
CollisionResult checkCollision(const Solid& a, const Solid& b, double volume_threshold = 1e-3,
                               double gap_search = 0);
CollisionResult checkCollision(const TriangleMesh& a, const TriangleMesh& b,
                               double volume_threshold = 1e-3, double gap_search = 0);

}  // namespace fruit::ops
