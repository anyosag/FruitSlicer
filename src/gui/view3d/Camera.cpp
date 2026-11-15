// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "view3d/Camera.h"

#include <algorithm>
#include <cmath>

namespace gui {

namespace {
constexpr float kDeg = 3.14159265358979f / 180.0f;
}

void Camera::orbit(float dx, float dy) { turn(-dx * 0.35f, dy * 0.35f); }

Camera::State Camera::shown() const {
    if (!glide_.on) return dest();
    const double t = std::clamp((now_ - glide_.start) / std::max(glide_.seconds, 1e-6), 0.0, 1.0);
    // Gently in and out (cubic).
    const float e = static_cast<float>(t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2);
    const State& a = glide_.from;
    const float dyaw = std::remainder(yaw_deg_ - a.yaw, 360.0f);  // the shorter way round
    return {a.yaw + dyaw * e, a.pitch + (pitch_deg_ - a.pitch) * e,
            a.distance * std::pow(distance_ / a.distance, e),  // evenly, as zoom steps are
            a.target + (target_ - a.target) * e};
}

void Camera::settle() {
    if (!glide_.on) return;
    const State s = shown();
    yaw_deg_ = s.yaw, pitch_deg_ = s.pitch, distance_ = s.distance, target_ = s.target;
    glide_.on = false;
}

void Camera::glideFrom(const Camera& before, double seconds) {
    const State from = before.shown(), to = dest();
    const bool same = std::abs(std::remainder(to.yaw - from.yaw, 360.0f)) < 0.01f && std::abs(to.pitch - from.pitch) < 0.01f &&
                      std::abs(to.distance - from.distance) < 0.01f && length(to.target - from.target) < 0.001f;
    if (same || seconds <= 0 || from.distance <= 0) {
        glide_.on = false;
        return;
    }
    glide_ = {true, from, now_, seconds};
}

void Camera::tick(double now) {
    now_ = now;
    if (glide_.on && now_ - glide_.start >= glide_.seconds) glide_.on = false;
}

void Camera::turn(float yaw_deg, float pitch_deg) {
    settle();
    yaw_deg_ += yaw_deg;
    pitch_deg_ = std::clamp(pitch_deg_ + pitch_deg, -89.0f, 89.0f);
}

void Camera::pan(float dx, float dy, float viewport_h) {
    settle();
    const V3 f = normalize(target_ - eye());
    const V3 right = normalize(cross(f, {0, 0, 1}));
    const V3 up = cross(right, f);
    const float mm_per_px = 2 * distance_ * std::tan(fov_deg_ * kDeg / 2) / std::max(viewport_h, 1.0f);
    target_ = target_ - right * (dx * mm_per_px) + up * (dy * mm_per_px);
}

void Camera::zoom(float steps) {
    settle();
    distance_ = std::clamp(distance_ * std::pow(0.88f, steps), 5.0f, 3000.0f);
}

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

void Camera::lookFrom(V3 dir) {
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len <= 0) return;
    pitch_deg_ = std::clamp(std::asin(dir.z / len) / kDeg, -80.0f, 80.0f);
    if (dir.x != 0 || dir.y != 0) yaw_deg_ = std::atan2(dir.y, dir.x) / kDeg;
}

V3 Camera::eye() const {
    const State s = shown();
    const float cy = std::cos(s.yaw * kDeg), sy = std::sin(s.yaw * kDeg);
    const float cp = std::cos(s.pitch * kDeg), sp = std::sin(s.pitch * kDeg);
    return s.target + V3{cp * cy, cp * sy, sp} * s.distance;
}

M4 Camera::view() const { return lookAt(eye(), shown().target, {0, 0, 1}); }

void Camera::ray(float nx, float ny, float aspect, V3* origin, V3* dir) const {
    nx -= shift_;  // the picture was moved sideways by the shift
    const V3 e = eye();
    const V3 f = normalize(shown().target - e);
    const V3 s = normalize(cross(f, {0, 0, 1}));
    const V3 u = cross(s, f);
    const float th = std::tan(fov_deg_ * kDeg / 2);
    *origin = e;
    *dir = normalize(f + s * (nx * th * aspect) + u * (ny * th));
}

M4 Camera::proj(float aspect) const {
    const float d = shown().distance;
    M4 r = perspective(fov_deg_ * kDeg, aspect, std::max(0.5f, d * 0.01f), d * 20 + 2000);
    r.at(0, 2) -= shift_;  // x_ndc moves by the shift: x_clip gains shift · w_clip (w_clip = -z_eye)
    return r;
}

}  // namespace gui
