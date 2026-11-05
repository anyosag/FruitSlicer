#include "mesh/Placement.h"

namespace fruit {

void placeOnPlate(TriangleMesh& mesh, const Vec3& rotation_deg, double elevation) {
    if (rotation_deg.x != 0 || rotation_deg.y != 0 || rotation_deg.z != 0)
        mesh.transform(Transform::rotationXYZ(rotation_deg.x, rotation_deg.y, rotation_deg.z));
    const BBox3 b = mesh.bbox();
    const Vec3 c = b.center();
    mesh.translate({-c.x, -c.y, elevation - b.min.z});
}

}  // namespace fruit
