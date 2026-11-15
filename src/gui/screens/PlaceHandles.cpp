// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The handles that place the model in the 3D view (Model, and Orient's
// settings): flat arrows on the plate that move it in X and Y, and three
// curved arrows that turn it about the plate's axes, the turn about the
// vertical lying on the plate too. Where the part hides a handle from the
// camera it shows faintly, as if through the part. The plate's edge turns
// red while it doesn't fit where it is.
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
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

ImU32 mix(ImU32 c, ImU32 to, float t) {
    const auto ch = [&](int shift) {
        const float a = static_cast<float>((c >> shift) & 0xFF), b = static_cast<float>((to >> shift) & 0xFF);
        return static_cast<ImU32>(std::lround(a + (b - a) * t)) << shift;
    };
    return ch(0) | ch(8) | ch(16) | (c & 0xFF000000u);
}
ImU32 withAlpha(ImU32 c, int a) { return (c & 0x00FFFFFFu) | (static_cast<ImU32>(a) << 24); }
ImU32 lighter(ImU32 c) { return mix(c | 0xFF000000u, IM_COL32(255, 255, 255, 255), 0.45f); }
constexpr double kPi = 3.14159265358979;

using Hidden = std::function<bool(const fruit::Vec3&)>;  // whether a part hides a point from the camera

// A flat handle (an arrow on the plate, a curved arrow round the part) as
// seen on screen: stations along it from one end to the other, each a point
// on its middle line and one on either side, and whether a part hides that
// station from the camera. It is drawn as one shape: one outline all round,
// one fill.
struct Ribbon {
    bool ok = false;
    std::vector<ImVec2> mid, left, right;
    std::vector<char> hidden;
    float reach = 7;  // how near the mouse must come to its middle line
    // Its outline all the way round: down the left side, back up the right.
    std::vector<ImVec2> outline() const {
        std::vector<ImVec2> o(left.begin(), left.end());
        o.insert(o.end(), right.rbegin(), right.rend());
        return o;
    }
    std::vector<char> outlineHidden() const {
        std::vector<char> o(hidden.begin(), hidden.end());
        o.insert(o.end(), hidden.rbegin(), hidden.rend());
        return o;
    }
    // Enough of it in sight to take hold of (not through a part).
    bool grabbable() const {
        std::size_t shown = 0;
        for (char h : hidden) shown += h ? 0 : 1;
        return ok && shown * 5 >= hidden.size() * 2;
    }
    // The mouse's distance to its middle line, where it is in sight.
    float distance(ImVec2 p) const {
        float best = 1e9f;
        for (std::size_t i = 0; i + 1 < mid.size(); ++i)
            if (!(hidden[i] && hidden[i + 1])) best = std::min(best, distToSegment(p, mid[i], mid[i + 1]));
        return best;
    }
};

// A station: the middle line's point and its half width there, square to the line, in the plate's frame (mm).
struct Station {
    fruit::Vec3 at, across;  // `across`: a unit vector square to the line, in the handle's plane
    double half = 0;
};

Ribbon ribbonOnScreen(const View& v, const std::vector<Station>& st, const Hidden& hidden) {
    Ribbon r;
    for (const Station& s : st) {
        ImVec2 m, l, q;
        if (!v.project(s.at, &m) || !v.project(s.at + s.across * s.half, &l) || !v.project(s.at - s.across * s.half, &q))
            return Ribbon{};
        r.mid.push_back(m);
        r.left.push_back(l);
        r.right.push_back(q);
        r.hidden.push_back(hidden(s.at) ? 1 : 0);
    }
    float across = 0, widest = 0;
    for (std::size_t i = 0; i < r.mid.size(); ++i) {
        const float dx = r.left[i].x - r.right[i].x, dy = r.left[i].y - r.right[i].y, d = std::sqrt(dx * dx + dy * dy);
        across += d;
        widest = std::max(widest, d);
    }
    r.reach = std::max(8.0f, std::max(0.5f * widest, 0.5f * across / static_cast<float>(std::max<std::size_t>(1, r.mid.size())) + 4));
    r.ok = r.mid.size() >= 2;
    return r;
}

// One shape: filled in thin pieces between its stations, without
// anti-aliasing so no seam shows between them, then one anti-aliased light
// outline all round. Where a part hides it, faint, as if seen through it.
void drawRibbon(ImDrawList* dl, const Ribbon& r, ImU32 c, bool hot) {
    if (!r.ok) return;
    const ImDrawFlags keep = dl->Flags;
    dl->Flags &= ~ImDrawFlags_AAFill;
    for (std::size_t i = 0; i + 1 < r.mid.size(); ++i) {
        const ImVec2 quad[4] = {r.left[i], r.left[i + 1], r.right[i + 1], r.right[i]};
        const bool hid = r.hidden[i] && r.hidden[i + 1];
        dl->AddConvexPolyFilled(quad, 4, withAlpha(c, hid ? 50 : hot ? 235 : 200));
    }
    dl->Flags = keep;
    // The outline, in runs of the same strength so its corners join cleanly.
    const std::vector<ImVec2> o = r.outline();
    const std::vector<char> h = r.outlineHidden();
    const ImU32 edge = lighter(c);
    const float t = hot ? 2.2f : 1.5f;
    const std::size_t n = o.size();
    const auto strength = [&](std::size_t i) { return h[i] && h[(i + 1) % n] ? 80 : 255; };
    std::size_t first = 0;  // start a run where the strength changes, so no run is cut in two
    while (first < n && strength(first) == strength((first + n - 1) % n)) ++first;
    if (first == n) {
        dl->AddPolyline(o.data(), static_cast<int>(n), withAlpha(edge, strength(0)), t, ImDrawFlags_Closed);
        return;
    }
    std::vector<ImVec2> run;
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t i = (first + k) % n;
        if (run.empty()) run.push_back(o[i]);
        run.push_back(o[(i + 1) % n]);
        const bool last = k + 1 == n || strength((i + 1) % n) != strength(i);
        if (last) {
            dl->AddPolyline(run.data(), static_cast<int>(run.size()), withAlpha(edge, strength(i)), t, ImDrawFlags_None);
            run.clear();
        }
    }
}

// The circle's point at angle `t`, `r` from `centre`, in the plane square to `axis`.
fruit::Vec3 onCircle(const fruit::Vec3& centre, int axis, double t, double r) {
    const double c = std::cos(t) * r, s = std::sin(t) * r;
    return centre + (axis == 0 ? fruit::Vec3{0, c, s} : axis == 1 ? fruit::Vec3{s, 0, c} : fruit::Vec3{c, s, 0});
}
// The angle on that circle of a direction lying in its plane.
double angleOf(int axis, const fruit::Vec3& d) {
    return axis == 0 ? std::atan2(d.z, d.y) : axis == 1 ? std::atan2(d.x, d.z) : std::atan2(d.y, d.x);
}

// The handle that turns the part about plate axis `axis`: a curved arrow,
// a head at each end, on the side of the part towards the camera. The turn
// about the vertical lies on the plate (where it is dragged), in the quarter
// between the two move arrows nearest the camera, short enough to stay clear
// of them; each tilt stands on its own side, 50° down from the top, so the
// two stay apart and above the plate. Not `ok` when its circle is seen edge
// on (it can't be dragged then) or a point of it is behind the camera.
Ribbon arcOnScreen(const View& v, const HandleFrame& f, int axis, const fruit::Vec3& eye_dir, const Hidden& hidden) {
    const fruit::Vec3 n = axis == 0 ? fruit::Vec3{1, 0, 0} : axis == 1 ? fruit::Vec3{0, 1, 0} : fruit::Vec3{0, 0, 1};
    if (std::abs(fruit::dot(eye_dir, n)) < 0.12) return Ribbon{};
    const fruit::Vec3 toward{-eye_dir.x, -eye_dir.y, -eye_dir.z};
    const fruit::Vec3 centre = turnCentre(f, axis);
    fruit::Vec3 d;
    double span = 0;
    if (axis == 2) {
        const double az = std::atan2(toward.y, toward.x), q = kPi / 2;
        const double snapped = std::round((az - q / 2) / q) * q + q / 2;
        d = {std::cos(snapped), std::sin(snapped), 0};
        span = 42 * kPi / 180;
    } else {
        const double down = 50 * kPi / 180, side = std::sin(down), up = std::cos(down);
        d = axis == 0 ? fruit::Vec3{0, toward.y < 0 ? -side : side, up} : fruit::Vec3{toward.x < 0 ? -side : side, 0, up};
        span = 54 * kPi / 180;
    }
    // In mm, like the arrows on the plate: a band, its heads wider, each head's sides curving
    // round the circle to its tip.
    const double R = f.radius, w = std::clamp(0.09 * R, 1.8, 3.5), hw = 1.15 * w;
    const double hl = std::clamp(0.2 * R, 3.0, 7.0) / R;  // a head's length, as an angle
    const double t0 = angleOf(axis, d) - span / 2, t1 = t0 + span;
    std::vector<Station> st;
    const auto station = [&](double t, double half) {
        const fruit::Vec3 at = onCircle(centre, axis, t, R);
        st.push_back({at, fruit::normalized(at - centre), half});
    };
    constexpr int kHead = 8, kBand = 24;
    for (int k = 0; k <= kHead; ++k) station(t0 - hl * (1 - double(k) / kHead), hw * k / kHead);  // the first head
    for (int k = 0; k <= kBand; ++k) station(t0 + span * k / kBand, w / 2);                       // the band
    for (int k = 0; k <= kHead; ++k) station(t1 + hl * k / kHead, hw * (1 - double(k) / kHead));  // the last head
    return ribbonOnScreen(v, st, hidden);
}

// An arrow lying flat on the plate along `axis` (0 X, 1 Y), pointing `sign`
// (±1). Not `ok` when behind the camera or too short to see.
struct ArrowShape {
    Ribbon shape;
    ImVec2 a, b;  // its base and tip on screen
    bool ok() const { return shape.ok; }
};

ArrowShape arrowOnScreen(const View& v, const HandleFrame& f, int axis, int sign, const Hidden& hidden) {
    ArrowShape r;
    const fruit::Vec3 dir = axis == 0 ? fruit::Vec3{1.0 * sign, 0, 0} : fruit::Vec3{0, 1.0 * sign, 0};
    const fruit::Vec3 side = axis == 0 ? fruit::Vec3{0, 1.0 * sign, 0} : fruit::Vec3{-1.0 * sign, 0, 0};
    const double gap = axis == 0 ? f.arrow_gap.x : f.arrow_gap.y;
    const fruit::Vec3 base{f.centre.x, f.centre.y, 0};
    if (!v.project(base + dir * gap, &r.a) || !v.project(base + dir * (gap + f.arrow_len), &r.b)) return r;
    const float dx = r.b.x - r.a.x, dy = r.b.y - r.a.y;
    if (std::sqrt(dx * dx + dy * dy) < 6) return r;
    // In mm: its shaft a third as wide as it is long, its head twice as wide.
    const double len = f.arrow_len, w = std::clamp(0.3 * len, 3.0, 5.0), hw = 0.95 * w, hl = 0.36 * len;
    std::vector<Station> st;
    const auto station = [&](double u, double half) { st.push_back({base + dir * (gap + u), side, half}); };
    constexpr int kShaft = 6, kHead = 5;
    for (int k = 0; k <= kShaft; ++k) station((len - hl) * k / kShaft, w / 2);
    for (int k = 0; k <= kHead; ++k) station(len - hl + hl * k / kHead, hw * (1 - double(k) / kHead));
    r.shape = ribbonOnScreen(v, st, hidden);
    return r;
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
    const HandleFrame f = dragging ? drag.frame : handleFrame(doc.part().box());
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
        bool ring;
        Ribbon shape;
    };
    // While a handle is held the part moves under it: nothing is hidden then.
    const Hidden hidden = [&](const fruit::Vec3& p) { return !dragging && app.hiddenFromCamera(p); };
    std::vector<Shape> shapes;
    for (int axis = 0; axis < 2; ++axis)
        for (int sign : {1, -1})
            shapes.push_back({axis == 0 ? Handle::MoveX : Handle::MoveY, false, arrowOnScreen(v, f, axis, sign, hidden).shape});
    for (int axis = 0; axis < 3; ++axis)
        shapes.push_back({axis == 0 ? Handle::TurnX : axis == 1 ? Handle::TurnY : Handle::TurnZ, true,
                          arcOnScreen(v, f, axis, eye_dir, hidden)});

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
                if (!s.shape.grabbable()) continue;  // not through a part
                const float reach = s.shape.reach;
                const float d = s.shape.distance(m);
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
        if (!s.shape.ok || (held != Handle::None && s.h != held)) continue;
        const int axis = handleAxis(s.h);
        const bool hot = s.h == hover;
        drawRibbon(dl, s.shape, hot ? kAxisHot[axis] : kAxis[axis], hot);
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
