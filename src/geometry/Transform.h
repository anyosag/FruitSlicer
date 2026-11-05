#pragma once

#include "geometry/Vec.h"

namespace fruit {

// Affine transform: p' = R * p + t, with R a 3x3 matrix (row-major).
struct Transform {
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    Vec3 t{};

    static Transform identity() { return {}; }
    static Transform translation(const Vec3& d);
    // Rotation about X, then Y, then Z (degrees), i.e. R = Rz * Ry * Rx.
    static Transform rotationXYZ(double rx_deg, double ry_deg, double rz_deg);

    Vec3 apply(const Vec3& p) const;
    Vec3 applyLinear(const Vec3& v) const;  // rotation/scale only
    Transform then(const Transform& next) const;  // returns next ∘ this
    double determinant() const;
};

}  // namespace fruit
