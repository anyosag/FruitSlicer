// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Orbit camera, Z up (plate coordinates in mm).

#include "view3d/Math.h"

namespace gui {

enum class ViewPreset { Front, Side, Angled, Below };

class Camera {
public:
    // Turn by mouse movement in pixels.
    void orbit(float dx, float dy);
    // Move the view so the plate follows the cursor (`viewport_h` in pixels).
    void pan(float dx, float dy, float viewport_h);
    // Positive steps zoom in.
    void zoom(float steps);
    // Fit a box (centre + radius) in view, keeping the direction.
    void frame(V3 center, float radius);
    void preset(ViewPreset p);

    V3 eye() const;
    M4 view() const;
    M4 proj(float aspect) const;
    V3 target() const { return target_; }
    float distance() const { return distance_; }

private:
    float yaw_deg_ = -60;   // around Z, from +X
    float pitch_deg_ = 25;  // above the plate
    float distance_ = 320;
    float fov_deg_ = 30;
    V3 target_{0, 0, 15};
};

}  // namespace gui
