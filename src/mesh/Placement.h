#pragma once

#include "mesh/TriangleMesh.h"

namespace fruit {

// Rotate (degrees, X then Y then Z), then translate so the model is centred
// on the plate in XY and its lowest point sits at `elevation` mm.
void placeOnPlate(TriangleMesh& mesh, const Vec3& rotation_deg, double elevation);

}  // namespace fruit
