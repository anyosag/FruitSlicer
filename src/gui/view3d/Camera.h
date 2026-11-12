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
    // Turn by angles: `yaw_deg` around the plate's upright axis, `pitch_deg`
    // up over the plate (held just short of straight up or down).
    void turn(float yaw_deg, float pitch_deg);
    // Move the view so the plate follows the cursor (`viewport_h` in pixels).
    void pan(float dx, float dy, float viewport_h);
    // Positive steps zoom in.
    void zoom(float steps);
    // Fit a box (centre + radius) in view, keeping the direction.
    void frame(V3 center, float radius);
    void preset(ViewPreset p);
    // Look at the target from the side `dir` points to (need not be of unit
    // length; kept a little off straight up or down).
    void lookFrom(V3 dir);

    V3 eye() const;
    M4 view() const;
    M4 proj(float aspect) const;
    // The ray through a point of the view, given in normalised device
    // coordinates (-1..1, y up).
    void ray(float ndc_x, float ndc_y, float aspect, V3* origin, V3* dir) const;
    V3 target() const { return target_; }
    float distance() const { return distance_; }
    // Moves the picture sideways by `ndc_x` (normalised device units, + is
    // right) without turning the camera: a panel over the view's left keeps
    // the model centred in what the panel leaves. Projections and rays both
    // follow it.
    void setShift(float ndc_x) { shift_ = ndc_x; }
    float shift() const { return shift_; }

private:
    float yaw_deg_ = -60;   // around Z, from +X
    float pitch_deg_ = 25;  // above the plate
    float distance_ = 320;
    float fov_deg_ = 30;
    V3 target_{0, 0, 15};
    float shift_ = 0;
};

}  // namespace gui
