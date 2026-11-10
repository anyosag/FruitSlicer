// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Placing.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace gui {

namespace {

constexpr double kDeg = 180.0 / std::numbers::pi;

fruit::Vec3 axisVector(int axis) {
    return axis == 0 ? fruit::Vec3{1, 0, 0} : axis == 1 ? fruit::Vec3{0, 1, 0} : fruit::Vec3{0, 0, 1};
}

double component(const fruit::Vec3& v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }

// How far apart two angles are, the short way round (degrees).
double apart(double a, double b) { return std::abs(tidyAngle(a - b)); }

fruit::Vec3 tidy(const fruit::Vec3& v) { return {tidyAngle(v.x), tidyAngle(v.y), tidyAngle(v.z)}; }

// The angles of `r` closest to `near`: a rotation has two sets (three
// numbers each) besides gimbal lock, and the fields should change as little
// as the turn does.
fruit::Vec3 eulerNear(const fruit::Transform& r, const fruit::Vec3& near) {
    const double s = std::clamp(-r.m[2][0], -1.0, 1.0);
    if (std::abs(s) > 1 - 1e-9) {
        // Y at ±90°: only X - Z (or X + Z) is fixed; keep X as it was.
        const double x = near.x;
        const double y = s > 0 ? 90.0 : -90.0;
        const double z = s > 0 ? x - std::atan2(r.m[0][1], r.m[1][1]) * kDeg
                               : std::atan2(-r.m[0][1], r.m[1][1]) * kDeg - x;
        return tidy({x, y, z});
    }
    const fruit::Vec3 a = tidy(eulerXYZ(r));
    const fruit::Vec3 b = tidy({a.x + 180, 180 - a.y, a.z + 180});
    const double da = apart(a.x, near.x) + apart(a.y, near.y) + apart(a.z, near.z);
    const double db = apart(b.x, near.x) + apart(b.y, near.y) + apart(b.z, near.z);
    return db < da ? b : a;
}

fruit::Transform rotationAbout(int axis, double deg) {
    return fruit::Transform::rotationXYZ(axis == 0 ? deg : 0, axis == 1 ? deg : 0, axis == 2 ? deg : 0);
}

}  // namespace

fruit::Placement sizeChoicePlacement(const fruit::TriangleMesh& model, const fruit::PrinterProfile& printer,
                                     SizeChoice choice, double custom_percent) {
    fruit::Placement p;
    const fruit::PlateFit f = fruit::checkFit(model, p, printer);
    switch (choice) {
        case SizeChoice::Full:
            p.scale = 1.0;
            if (!f.fits) {
                fruit::Placement turned = p;
                turned.rotation_deg.z = 90;
                if (fruit::checkFit(model, turned, printer).fits) p = turned;
            }
            break;
        case SizeChoice::Largest:
            p.scale = std::max(f.largest, f.largest_turned);
            if (f.largest_turned > f.largest) p.rotation_deg.z = 90;
            break;
        case SizeChoice::Custom: p.scale = custom_percent / 100.0; break;
        case SizeChoice::Inches: p.scale = 25.4; break;
    }
    return p;
}

double tidyAngle(double deg) {
    double a = std::fmod(deg, 360.0);
    if (a <= -180) a += 360;
    if (a > 180) a -= 360;
    a = std::round(a * 1e6) / 1e6;
    if (a == -180) a = 180;
    return a == 0 ? 0.0 : a;  // no -0
}

double snapTo(double v, double step) {
    if (step <= 0) return v;
    const double s = std::round(v / step) * step;
    return s == 0 ? 0.0 : s;
}

fruit::Vec3 eulerXYZ(const fruit::Transform& r) {
    const double s = std::clamp(-r.m[2][0], -1.0, 1.0);
    const double y = std::asin(s) * kDeg;
    if (std::abs(s) > 1 - 1e-9) {
        // Gimbal lock: take X as 0 and put the rest in Z.
        const double z = s > 0 ? -std::atan2(r.m[0][1], r.m[1][1]) * kDeg : std::atan2(-r.m[0][1], r.m[1][1]) * kDeg;
        return {0, y, z};
    }
    return {std::atan2(r.m[2][1], r.m[2][2]) * kDeg, y, std::atan2(r.m[1][0], r.m[0][0]) * kDeg};
}

fruit::Vec3 turnAbout(const fruit::Vec3& rotation_deg, int axis, double deg) {
    if (axis == 2) {
        // Z is turned last, about the plate's own vertical: just add.
        return {tidyAngle(rotation_deg.x), tidyAngle(rotation_deg.y), tidyAngle(rotation_deg.z + deg)};
    }
    const fruit::Transform r =
        fruit::Transform::rotationXYZ(rotation_deg.x, rotation_deg.y, rotation_deg.z).then(rotationAbout(axis, deg));
    return eulerNear(r, rotation_deg);
}

fruit::Placement turnedQuarter(const fruit::TriangleMesh& model, const fruit::Placement& p,
                               const fruit::PrinterProfile& printer) {
    fruit::Placement q = p;
    q.rotation_deg = turnAbout(p.rotation_deg, 2, 90);
    return slidOnto(model, q, printer);
}

fruit::Placement slidOnto(const fruit::TriangleMesh& model, const fruit::Placement& p,
                          const fruit::PrinterProfile& printer) {
    const fruit::PlateFit f = fruit::checkFit(model, p, printer);
    if (f.fits || !f.fits_centred) return p;
    fruit::Placement q = p;
    q.offset.x = std::clamp(p.offset.x, -f.room.x, f.room.x);
    q.offset.y = std::clamp(p.offset.y, -f.room.y, f.room.y);
    return q;
}

// ---- Handles ----

HandleFrame handleFrame(const fruit::BBox3& box) {
    HandleFrame f;
    const fruit::Vec3 s = box.size();
    f.centre = box.center();
    f.radius = 0.5 * std::sqrt(s.x * s.x + s.y * s.y + s.z * s.z) + 4.0;
    // Clear of the outline, and of a long thin model seen at an angle.
    const double longest = std::max(s.x, s.y);
    f.arrow_gap = {0.5 * s.x + 6.0 + 0.1 * longest, 0.5 * s.y + 6.0 + 0.1 * longest};
    f.arrow_len = std::clamp(0.12 * longest, 8.0, 18.0);
    return f;
}

std::optional<fruit::Vec3> hitPlane(const fruit::Vec3& origin, const fruit::Vec3& dir, const fruit::Vec3& point,
                                    const fruit::Vec3& normal) {
    const double d = fruit::dot(dir, normal);
    if (std::abs(d) < 1e-9) return std::nullopt;
    const double t = fruit::dot(point - origin, normal) / d;
    if (t < 0) return std::nullopt;
    return origin + dir * t;
}

double angleAbout(const fruit::Vec3& centre, int axis, const fruit::Vec3& p) {
    const fruit::Vec3 d = p - centre;
    // The right-handed turn about each axis takes the first of these
    // towards the second: Y to Z about X, Z to X about Y, X to Y about Z.
    if (axis == 0) return std::atan2(d.z, d.y) * kDeg;
    if (axis == 1) return std::atan2(d.x, d.z) * kDeg;
    return std::atan2(d.y, d.x) * kDeg;
}

namespace {

// The plane a handle is dragged in: the plate for arrows, the ring's own
// plane for rings. Nothing when the ray is too near to running along it.
std::optional<fruit::Vec3> handleHit(Handle h, const HandleFrame& f, const fruit::Vec3& origin, const fruit::Vec3& dir) {
    const fruit::Vec3 n = isTurn(h) ? axisVector(handleAxis(h)) : fruit::Vec3{0, 0, 1};
    const fruit::Vec3 at = isTurn(h) ? f.centre : fruit::Vec3{0, 0, 0};
    const double len = std::sqrt(fruit::dot(dir, dir));
    if (len <= 0 || std::abs(fruit::dot(dir, n)) / len < (isTurn(h) ? 0.08 : 0.02)) return std::nullopt;
    return hitPlane(origin, dir, at, n);
}

}  // namespace

bool startDrag(HandleDrag& d, Handle h, const fruit::Placement& p, const HandleFrame& frame, const fruit::Vec3& origin,
               const fruit::Vec3& dir) {
    const auto hit = handleHit(h, frame, origin, dir);
    if (h == Handle::None || !hit) return false;
    d = HandleDrag{};
    d.handle = h;
    d.start = p;
    d.frame = frame;
    d.grab = *hit;
    d.last_angle = isTurn(h) ? angleAbout(frame.centre, handleAxis(h), *hit) : 0;
    d.now = p;
    return true;
}

fruit::Placement dragTo(HandleDrag& d, const fruit::Vec3& origin, const fruit::Vec3& dir, bool snap) {
    if (!d.active()) return d.now;
    const auto hit = handleHit(d.handle, d.frame, origin, dir);
    if (!hit) return d.now;
    double amount = 0;
    if (isTurn(d.handle)) {
        const double a = angleAbout(d.frame.centre, handleAxis(d.handle), *hit);
        d.turned += tidyAngle(a - d.last_angle);  // the short way round since last frame
        d.last_angle = a;
        amount = d.turned;
    } else {
        amount = component(*hit - d.grab, handleAxis(d.handle));
    }
    d.now = draggedBy(d.start, d.handle, amount, snap);
    return d.now;
}

fruit::Placement draggedBy(const fruit::Placement& start, Handle h, double amount, bool snap) {
    fruit::Placement p = start;
    switch (h) {
        case Handle::None: break;
        case Handle::MoveX: p.offset.x = snapTo(start.offset.x + amount, snap ? 1.0 : 0.1); break;
        case Handle::MoveY: p.offset.y = snapTo(start.offset.y + amount, snap ? 1.0 : 0.1); break;
        case Handle::TurnX:
        case Handle::TurnY:
        case Handle::TurnZ:
            p.rotation_deg = turnAbout(start.rotation_deg, handleAxis(h), snapTo(amount, snap ? kTurnSnap : 0.1));
            break;
    }
    return p;
}

}  // namespace gui
