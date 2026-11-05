#include "support/SupportGeometry.h"

#include "mesh/Primitives.h"

namespace fruit {

int GeometryQuality::segments(double radius) const { return segmentsFor(radius, chord_error); }

Vec3 appendPinhead(TriangleMesh& m, const Vec3& contact, const Vec3& dir_in, const PinheadSpec& s,
                   const GeometryQuality& q) {
    const Vec3 d = normalized(dir_in);
    const double r_tip = s.tip_diameter / 2;
    const double r_back = s.shaft_diameter / 2;
    // Front sphere sinks `penetration` into the part: its far side is at
    // contact - d * penetration.
    const Vec3 front = contact + d * (r_tip - s.penetration);
    const Vec3 junction = front + d * (r_tip + s.head_length + r_back);
    appendSphere(m, front, r_tip, q.segments(r_tip));
    appendFrustum(m, front, r_tip, junction, r_back, q.segments(r_back));
    appendSphere(m, junction, r_back, q.segments(r_back));
    return junction;
}

void appendShaft(TriangleMesh& m, const Vec3& top, const Vec3& bottom, double diameter,
                 const GeometryQuality& q, bool top_joint) {
    const double r = diameter / 2;
    if (distance(top, bottom) <= 1e-9) return;
    appendFrustum(m, bottom, r, top, r, q.segments(r));
    if (top_joint) appendSphere(m, top, r, q.segments(r));
}

void appendBaseCone(TriangleMesh& m, const Vec3& top, double shaft_diameter, double base_diameter,
                    double base_z, const GeometryQuality& q) {
    const Vec3 bottom{top.x, top.y, base_z};
    if (top.z <= base_z) return;
    appendFrustum(m, bottom, base_diameter / 2, top, shaft_diameter / 2, q.segments(base_diameter / 2));
}

Vec3 headDirection(const Vec3& outward_normal, double max_angle_deg) {
    const Vec3 down{0, 0, -1};
    Vec3 d = normalized(outward_normal);
    if (norm2(d) == 0) return down;
    const double cos_max = std::cos(deg2rad(max_angle_deg));
    if (dot(d, down) >= cos_max) return d;
    // Tilt within the vertical plane containing d: keep its horizontal
    // heading, set the angle from vertical to exactly max_angle_deg.
    Vec2 h = d.xy();
    if (norm(h) < 1e-12) return down;  // pointing straight up: no valid tilt
    h = normalized(h);
    const double s = std::sin(deg2rad(max_angle_deg));
    return normalized(Vec3{h.x * s, h.y * s, -cos_max});
}

void appendHexahedron(TriangleMesh& m, const Vec3 (&v)[8]) {
    const auto b = static_cast<int32_t>(m.vertices.size());
    for (const auto& p : v) m.vertices.push_back(p);
    const int quads[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    for (const auto& qd : quads) {
        m.faces.push_back({b + qd[0], b + qd[1], b + qd[2]});
        m.faces.push_back({b + qd[0], b + qd[2], b + qd[3]});
    }
}

void appendRaft(TriangleMesh& m, const Vec2& lo, const Vec2& hi, double z0, double z1, double chamfer) {
    if (chamfer <= 0 || chamfer * 2 >= std::min(hi.x - lo.x, hi.y - lo.y) || chamfer >= z1 - z0) {
        appendBox(m, {lo.x, lo.y, z0}, {hi.x, hi.y, z1});
        return;
    }
    // Frustum-shaped slab: full footprint at z0, inset by `chamfer` at z1.
    const double c = chamfer;
    const Vec3 v[8] = {{lo.x, lo.y, z0},         {hi.x, lo.y, z0},         {hi.x, hi.y, z0},
                       {lo.x, hi.y, z0},         {lo.x + c, lo.y + c, z1}, {hi.x - c, lo.y + c, z1},
                       {hi.x - c, hi.y - c, z1}, {lo.x + c, hi.y - c, z1}};
    appendHexahedron(m, v);
}

}  // namespace fruit
