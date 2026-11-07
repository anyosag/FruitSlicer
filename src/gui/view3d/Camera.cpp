// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "view3d/Camera.h"

#include <algorithm>

namespace gui {

namespace {
constexpr float kDeg = 3.14159265358979f / 180.0f;
}

void Camera::orbit(float dx, float dy) {
    yaw_deg_ -= dx * 0.35f;
    pitch_deg_ = std::clamp(pitch_deg_ + dy * 0.35f, -89.0f, 89.0f);
}

void Camera::pan(float dx, float dy, float viewport_h) {
    const V3 f = normalize(target_ - eye());
    const V3 right = normalize(cross(f, {0, 0, 1}));
    const V3 up = cross(right, f);
    const float mm_per_px = 2 * distance_ * std::tan(fov_deg_ * kDeg / 2) / std::max(viewport_h, 1.0f);
    target_ = target_ - right * (dx * mm_per_px) + up * (dy * mm_per_px);
}

void Camera::zoom(float steps) { distance_ = std::clamp(distance_ * std::pow(0.88f, steps), 5.0f, 3000.0f); }

void Camera::frame(V3 center, float radius) {
    target_ = center;
    // Room around the model, so the plate under it stays in view.
    distance_ = std::max(10.0f, radius / std::sin(fov_deg_ * kDeg / 2) * 1.6f);
}

void Camera::preset(ViewPreset p) {
    switch (p) {
        case ViewPreset::Front: yaw_deg_ = -90, pitch_deg_ = 8; break;
        case ViewPreset::Side: yaw_deg_ = 0, pitch_deg_ = 8; break;
        case ViewPreset::Angled: yaw_deg_ = -60, pitch_deg_ = 25; break;
        case ViewPreset::Below: yaw_deg_ = -90, pitch_deg_ = -60; break;
    }
}

V3 Camera::eye() const {
    const float cy = std::cos(yaw_deg_ * kDeg), sy = std::sin(yaw_deg_ * kDeg);
    const float cp = std::cos(pitch_deg_ * kDeg), sp = std::sin(pitch_deg_ * kDeg);
    return target_ + V3{cp * cy, cp * sy, sp} * distance_;
}

M4 Camera::view() const { return lookAt(eye(), target_, {0, 0, 1}); }

void Camera::ray(float nx, float ny, float aspect, V3* origin, V3* dir) const {
    const V3 e = eye();
    const V3 f = normalize(target_ - e);
    const V3 s = normalize(cross(f, {0, 0, 1}));
    const V3 u = cross(s, f);
    const float th = std::tan(fov_deg_ * kDeg / 2);
    *origin = e;
    *dir = normalize(f + s * (nx * th * aspect) + u * (ny * th));
}

M4 Camera::proj(float aspect) const {
    return perspective(fov_deg_ * kDeg, aspect, std::max(0.5f, distance_ * 0.01f), distance_ * 20 + 2000);
}

}  // namespace gui
