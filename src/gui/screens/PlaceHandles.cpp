// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The handles that place the model in the 3D view (Model, and Orient's
// settings), after Formlabs PreForm's: arrows on the plate that move it in X
// and Y, and three rings round it that turn it about the plate's axes. The
// plate's edge turns red while it doesn't fit where it is.
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "App.h"
#include "Format.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"
#include "view3d/Math.h"

namespace gui::screens::parts {

namespace {

constexpr float kTopBar = 44;  // the view's own buttons or hint across the top

const ImU32 kAxis[3] = {IM_COL32(0xE0, 0x6C, 0x6C, 255), IM_COL32(0x6C, 0xC8, 0x8A, 255),
                        IM_COL32(0x6C, 0x9C, 0xE8, 255)};
const ImU32 kAxisHot[3] = {IM_COL32(0xFF, 0x9A, 0x9A, 255), IM_COL32(0x9C, 0xF0, 0xB4, 255),
                           IM_COL32(0x9C, 0xC4, 0xFF, 255)};

struct View {
    M4 vp;
    ImVec2 origin, size;
    bool project(const fruit::Vec3& w, ImVec2* out) const {
        const float* m = vp.m;
        const float x = static_cast<float>(w.x), y = static_cast<float>(w.y), z = static_cast<float>(w.z);
        const float px = m[0] * x + m[4] * y + m[8] * z + m[12];
        const float py = m[1] * x + m[5] * y + m[9] * z + m[13];
        const float pw = m[3] * x + m[7] * y + m[11] * z + m[15];
        if (pw <= 1e-4f) return false;
        *out = ImVec2(origin.x + (px / pw * 0.5f + 0.5f) * size.x, origin.y + (0.5f - py / pw * 0.5f) * size.y);
        return true;
    }
};

float distToSegment(ImVec2 p, ImVec2 a, ImVec2 b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2 : 0;
    t = std::clamp(t, 0.0f, 1.0f);
    const float qx = a.x + t * dx - p.x, qy = a.y + t * dy - p.y;
    return std::sqrt(qx * qx + qy * qy);
}

float distToPolyline(ImVec2 p, const std::vector<ImVec2>& pts, bool closed) {
    float best = 1e9f;
    const std::size_t n = pts.size();
    for (std::size_t i = 0; i + (closed ? 0 : 1) < n; ++i) best = std::min(best, distToSegment(p, pts[i], pts[(i + 1) % n]));
    return best;
}

// A ring's points on screen; empty when it is seen edge on (it can't be
// dragged then) or any part is behind the camera.
std::vector<ImVec2> ringOnScreen(const View& v, const HandleFrame& f, int axis, const fruit::Vec3& eye_dir) {
    const fruit::Vec3 n = axis == 0 ? fruit::Vec3{1, 0, 0} : axis == 1 ? fruit::Vec3{0, 1, 0} : fruit::Vec3{0, 0, 1};
    if (std::abs(fruit::dot(eye_dir, n)) < 0.12) return {};
    std::vector<ImVec2> pts;
    constexpr int kN = 72;
    for (int i = 0; i < kN; ++i) {
        const double t = 2 * 3.14159265358979 * i / kN, c = std::cos(t) * f.radius, s = std::sin(t) * f.radius;
        const fruit::Vec3 off = axis == 0 ? fruit::Vec3{0, c, s} : axis == 1 ? fruit::Vec3{s, 0, c} : fruit::Vec3{c, s, 0};
        ImVec2 q;
        if (!v.project(f.centre + off, &q)) return {};
        pts.push_back(q);
    }
    return pts;
}

// An arrow on the plate along `axis` (0 X, 1 Y), pointing `sign` (±1):
// shaft and head on screen. Empty when behind the camera or too short to see.
std::vector<ImVec2> arrowOnScreen(const View& v, const HandleFrame& f, int axis, int sign) {
    const fruit::Vec3 dir = axis == 0 ? fruit::Vec3{1.0 * sign, 0, 0} : fruit::Vec3{0, 1.0 * sign, 0};
    const double gap = axis == 0 ? f.arrow_gap.x : f.arrow_gap.y;
    const fruit::Vec3 base{f.centre.x, f.centre.y, 0};
    ImVec2 a, b;
    if (!v.project(base + dir * gap, &a) || !v.project(base + dir * (gap + f.arrow_len), &b)) return {};
    const float dx = b.x - a.x, dy = b.y - a.y, l = std::sqrt(dx * dx + dy * dy);
    if (l < 6) return {};
    const float ux = dx / l, uy = dy / l, head = std::min(14.0f, l * 0.6f), half = head * 0.55f;
    const ImVec2 neck(b.x - ux * head, b.y - uy * head);
    return {a, neck, ImVec2(neck.x - uy * half, neck.y + ux * half), b, ImVec2(neck.x + uy * half, neck.y - ux * half),
            neck};
}

void drawArrow(ImDrawList* dl, const std::vector<ImVec2>& a, ImU32 c, bool hot) {
    if (a.size() < 6) return;
    dl->AddLine(a[0], a[1], c, hot ? 4.0f : 3.0f);
    dl->AddTriangleFilled(a[2], a[3], a[4], c);
}

void label(ImDrawList* dl, ImVec2 at, const std::string& s) {
    ImGui::PushFont(fonts().semibold, size::Help);
    const ImVec2 ts = ImGui::CalcTextSize(s.c_str());
    const ImVec2 p(at.x + 16, at.y + 14);
    dl->AddRectFilled(ImVec2(p.x - 6, p.y - 3), ImVec2(p.x + ts.x + 6, p.y + ts.y + 3), col::Panel, 5);
    dl->AddRect(ImVec2(p.x - 6, p.y - 3), ImVec2(p.x + ts.x + 6, p.y + ts.y + 3), col::BorderStrong, 5);
    dl->AddText(p, col::Text, s.c_str());
    ImGui::PopFont();
}

}  // namespace

void placeOverlay(App& app, Document& doc, ImVec2 origin, ImVec2 size, const float* view_proj) {
    View v;
    std::memcpy(v.vp.m, view_proj, sizeof v.vp.m);
    v.origin = origin;
    v.size = size;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const fruit::PrinterProfile& pr = doc.printer;

    // The plate's edge, red while the model doesn't fit where it is.
    if (!doc.fit().fits) {
        const double hx = pr.build_x / 2, hy = pr.build_y / 2;
        const fruit::Vec3 corners[4] = {{-hx, -hy, 0}, {hx, -hy, 0}, {hx, hy, 0}, {-hx, hy, 0}};
        ImVec2 q[4];
        bool ok = true;
        for (int i = 0; i < 4; ++i) ok = ok && v.project(corners[i], &q[i]);
        if (ok) dl->AddPolyline(q, 4, col::Error, 2.0f, ImDrawFlags_Closed);
    }

    HandleDrag& drag = app.handleDrag();
    const bool dragging = app.placing() && drag.active();
    const HandleFrame f = dragging ? drag.frame : handleFrame(doc.placedBox());
    const float aspect = size.x / std::max(1.0f, size.y);
    auto mouseRay = [&](fruit::Vec3* o, fruit::Vec3* d) {
        const float nx = 2 * (io.MousePos.x - origin.x) / size.x - 1, ny = 1 - 2 * (io.MousePos.y - origin.y) / size.y;
        V3 ro, rd;
        app.camera().ray(nx, ny, aspect, &ro, &rd);
        *o = {ro.x, ro.y, ro.z};
        *d = {rd.x, rd.y, rd.z};
    };
    // Which way the camera looks at the handles: decides which rings are seen edge on.
    fruit::Vec3 eye_dir;
    {
        ImVec2 c;
        if (v.project(f.centre, &c)) {
            const float nx = 2 * (c.x - origin.x) / size.x - 1, ny = 1 - 2 * (c.y - origin.y) / size.y;
            V3 ro, rd;
            app.camera().ray(nx, ny, aspect, &ro, &rd);
            eye_dir = fruit::normalized({rd.x, rd.y, rd.z});
        }
    }

    // The handles on screen.
    struct Shape {
        Handle h;
        std::vector<ImVec2> pts;
        bool ring;
    };
    std::vector<Shape> shapes;
    for (int axis = 0; axis < 2; ++axis)
        for (int sign : {1, -1})
            shapes.push_back({axis == 0 ? Handle::MoveX : Handle::MoveY, arrowOnScreen(v, f, axis, sign), false});
    for (int axis = 0; axis < 3; ++axis)
        shapes.push_back({axis == 0 ? Handle::TurnX : axis == 1 ? Handle::TurnY : Handle::TurnZ,
                          ringOnScreen(v, f, axis, eye_dir), true});

    // What the mouse is over: arrows before rings, the nearest within reach.
    Handle& hover = app.hoverHandle();
    const ImVec2 m = io.MousePos;
    const bool inside = m.x >= origin.x && m.y >= origin.y + kTopBar && m.x < origin.x + size.x &&
                        m.y < origin.y + size.y && ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered();
    if (!dragging) {
        hover = Handle::None;
        if (inside) {
            float best = 1e9f;
            for (const Shape& s : shapes) {
                if (s.pts.empty()) continue;
                const float reach = s.ring ? 6.0f : 10.0f;
                const float d = s.ring ? distToPolyline(m, s.pts, true) : distToPolyline(m, {s.pts[0], s.pts[3]}, false);
                const float score = d + (s.ring ? 4.0f : 0.0f);  // arrows win near a crossing
                if (d <= reach && score < best) {
                    best = score;
                    hover = s.h;
                }
            }
        }
        if (hover != Handle::None) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                fruit::Vec3 o, d;
                mouseRay(&o, &d);
                HandleDrag nd;
                if (startDrag(nd, hover, doc.placement(), f, o, d)) app.beginPlacing(nd);
            }
        }
    } else {
        hover = drag.handle;
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            app.endPlacing();
        } else {
            fruit::Vec3 o, d;
            mouseRay(&o, &d);
            app.previewPlacement(dragTo(drag, o, d, io.KeyShift));
        }
    }

    // Draw: while dragging, only the handle held.
    const Handle held = app.placing() ? app.handleDrag().handle : Handle::None;
    for (const Shape& s : shapes) {
        if (s.pts.empty() || (held != Handle::None && s.h != held)) continue;
        const int axis = handleAxis(s.h);
        const bool hot = s.h == hover;
        const ImU32 c = hot ? kAxisHot[axis] : (kAxis[axis] & 0x00FFFFFF) | (static_cast<ImU32>(s.ring ? 170 : 230) << 24);
        if (s.ring) dl->AddPolyline(s.pts.data(), static_cast<int>(s.pts.size()), c, hot ? 3.0f : 1.6f, ImDrawFlags_Closed);
        else drawArrow(dl, s.pts, c, hot);
    }

    // While dragging: how far, in numbers.
    if (app.placing() && app.handleDrag().active()) {
        const HandleDrag& d = app.handleDrag();
        const fruit::Placement& now = doc.placement();
        std::string s;
        if (isTurn(d.handle)) {
            const double turned = snapTo(d.turned, io.KeyShift ? kTurnSnap : 0.1);
            s = strf("%+g°", turned == 0 ? 0.0 : turned) + (io.KeyShift ? "" : "  ·  ⇧ for 15° steps");
        } else {
            s = d.handle == Handle::MoveX ? strf("X %g mm", now.offset.x) : strf("Y %g mm", now.offset.y);
        }
        if (!doc.fit().fits) s += doc.fit().overEdge() ? "  ·  over the edge" : "  ·  too big";
        label(dl, m, s);
    }
}

}  // namespace gui::screens::parts
