// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Icons.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

#include "Theme.h"

namespace gui::icon {

namespace {

constexpr float kPi = 3.14159265358979f;

// Icons are designed on a 20-unit grid (like the canvas SVGs).
struct Grid {
    ImVec2 o;
    float u;
    ImVec2 at(float x, float y) const { return ImVec2(o.x + x * u, o.y + y * u); }
    float w(float units) const { return std::max(1.0f, units * u); }
};

}  // namespace

void home(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const Grid g{p, size / 20.0f};
    const ImVec2 pts[] = {g.at(3.5f, 9), g.at(10, 3.5f), g.at(16.5f, 9), g.at(16.5f, 16.5f), g.at(3.5f, 16.5f)};
    dl->AddPolyline(pts, 5, c, g.w(1.5f), ImDrawFlags_Closed | ImDrawFlags_StrokeCenter);
    dl->AddLine(g.at(8, 16.5f), g.at(8, 12), c, g.w(1.5f));
    dl->AddLine(g.at(8, 12), g.at(12, 12), c, g.w(1.5f));
    dl->AddLine(g.at(12, 12), g.at(12, 16.5f), c, g.w(1.5f));
}

void cube(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const Grid g{p, size / 20.0f};
    const ImVec2 outline[] = {g.at(10, 2.5f), g.at(16.5f, 6), g.at(16.5f, 14), g.at(10, 17.5f), g.at(3.5f, 14),
                              g.at(3.5f, 6)};
    dl->AddPolyline(outline, 6, c, g.w(1.5f), ImDrawFlags_Closed | ImDrawFlags_StrokeCenter);
    dl->AddLine(g.at(3.5f, 6), g.at(10, 9.5f), c, g.w(1.5f));
    dl->AddLine(g.at(16.5f, 6), g.at(10, 9.5f), c, g.w(1.5f));
    dl->AddLine(g.at(10, 9.5f), g.at(10, 17.5f), c, g.w(1.5f));
}

void target(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const Grid g{p, size / 20.0f};
    dl->AddCircle(g.at(10, 10), 6.5f * g.u, c, 0, g.w(1.5f));
    dl->AddCircle(g.at(10, 10), 2.5f * g.u, c, 0, g.w(1.5f));
    dl->AddLine(g.at(10, 1.5f), g.at(10, 3.5f), c, g.w(1.5f));
    dl->AddLine(g.at(10, 16.5f), g.at(10, 18.5f), c, g.w(1.5f));
    dl->AddLine(g.at(1.5f, 10), g.at(3.5f, 10), c, g.w(1.5f));
    dl->AddLine(g.at(16.5f, 10), g.at(18.5f, 10), c, g.w(1.5f));
}

void droplet(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const Grid g{p, size / 20.0f};
    dl->PathClear();
    dl->PathLineTo(g.at(10, 2.5f));
    dl->PathBezierCubicCurveTo(g.at(13, 6.5f), g.at(15.5f, 9.5f), g.at(15.5f, 12.5f));
    dl->PathBezierCubicCurveTo(g.at(15.5f, 15.6f), g.at(13, 17.5f), g.at(10, 17.5f));
    dl->PathBezierCubicCurveTo(g.at(7, 17.5f), g.at(4.5f, 15.6f), g.at(4.5f, 12.5f));
    dl->PathBezierCubicCurveTo(g.at(4.5f, 9.5f), g.at(7, 6.5f), g.at(10, 2.5f));
    dl->PathStroke(c, g.w(1.5f), ImDrawFlags_Closed | ImDrawFlags_StrokeCenter);
}

void wrench(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const Grid g{p, size / 20.0f};
    dl->AddLine(g.at(4, 16), g.at(10.5f, 9.5f), c, g.w(2.0f));
    dl->PathClear();
    dl->PathArcTo(g.at(13, 7), 3.6f * g.u, kPi * 0.95f, kPi * 2.55f);
    dl->PathStroke(c, g.w(1.5f), ImDrawFlags_None | ImDrawFlags_StrokeCenter);
}

void gear(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const Grid g{p, size / 20.0f};
    const ImVec2 ctr = g.at(10, 10);
    dl->AddCircle(ctr, 2.6f * g.u, c, 0, g.w(1.5f));
    for (int i = 0; i < 8; ++i) {
        const float a = i * kPi / 4;
        const ImVec2 d(std::cos(a), std::sin(a));
        dl->AddLine(ImVec2(ctr.x + d.x * 5.2f * g.u, ctr.y + d.y * 5.2f * g.u),
                    ImVec2(ctr.x + d.x * 7.4f * g.u, ctr.y + d.y * 7.4f * g.u), c, g.w(1.6f));
    }
    dl->AddCircle(ctr, 5.2f * g.u, c, 0, g.w(1.5f));
}

void upload(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const Grid g{p, size / 20.0f};
    dl->AddLine(g.at(10, 3), g.at(10, 13), c, g.w(1.6f));
    dl->AddLine(g.at(6, 7), g.at(10, 3), c, g.w(1.6f));
    dl->AddLine(g.at(14, 7), g.at(10, 3), c, g.w(1.6f));
    const ImVec2 tray[] = {g.at(3.5f, 13), g.at(3.5f, 17), g.at(16.5f, 17), g.at(16.5f, 13)};
    dl->AddPolyline(tray, 4, c, g.w(1.6f), ImDrawFlags_None | ImDrawFlags_StrokeCenter);
}

void check(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const Grid g{p, size / 20.0f};
    const ImVec2 pts[] = {g.at(5, 10.5f), g.at(8.5f, 14), g.at(15, 6.5f)};
    dl->AddPolyline(pts, 3, c, g.w(2.0f), ImDrawFlags_None | ImDrawFlags_StrokeCenter);
}

void cross(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const Grid g{p, size / 20.0f};
    dl->AddLine(g.at(6, 6), g.at(14, 14), c, g.w(2.0f));
    dl->AddLine(g.at(14, 6), g.at(6, 14), c, g.w(2.0f));
}

void logo(ImDrawList* dl, ImVec2 p, float size) {
    // Rail logo from the canvas (26-unit grid): half-disc below y = 15.
    const float u = size / 26.0f;
    const ImVec2 ctr(p.x + 13 * u, p.y + 15 * u);
    dl->PathClear();
    dl->PathArcTo(ctr, 10 * u, 0, kPi, 32);
    dl->PathFillConvex(col::Accent);
    dl->PathClear();
    dl->PathArcTo(ctr, 7 * u, 0, kPi, 32);
    dl->PathStroke(col::Viewport, std::max(1.0f, 1.5f * u), ImDrawFlags_None | ImDrawFlags_StrokeCenter);
    dl->AddCircleFilled(ImVec2(p.x + 10 * u, p.y + 18 * u), 1.0f * u, col::Viewport);
    dl->AddCircleFilled(ImVec2(p.x + 16 * u, p.y + 18 * u), 1.0f * u, col::Viewport);
}


// ---- The tools around the 3D view: a 24-unit grid, as the canvas's SVGs ----

namespace {

struct G24 {
    ImVec2 o;
    float u;
    ImVec2 at(float x, float y) const { return ImVec2(o.x + x * u, o.y + y * u); }
    float w() const { return std::max(1.0f, 1.6f * u); }
};

void line(ImDrawList* dl, const G24& g, float x0, float y0, float x1, float y1, ImU32 c) {
    dl->AddLine(g.at(x0, y0), g.at(x1, y1), c, g.w());
}

void poly(ImDrawList* dl, const G24& g, std::initializer_list<ImVec2> pts, ImU32 c, bool closed) {
    ImVec2 v[16];
    int n = 0;
    for (const ImVec2& q : pts) v[n++] = g.at(q.x, q.y);
    dl->AddPolyline(v, n, c, g.w(), (closed ? ImDrawFlags_Closed : ImDrawFlags_None));
}

// A dashed straight line, for the dashed boxes.
void dashed(ImDrawList* dl, const G24& g, float x0, float y0, float x1, float y1, ImU32 c) {
    const float len = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    const int n = std::max(1, static_cast<int>(len / 4.0f));
    for (int i = 0; i < n; ++i) {
        const float a = static_cast<float>(i) / n, b = (i + 0.55f) / n;
        line(dl, g, x0 + (x1 - x0) * a, y0 + (y1 - y0) * a, x0 + (x1 - x0) * b, y0 + (y1 - y0) * b, c);
    }
}

}  // namespace

void folder(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const G24 g{p, size / 24};
    poly(dl, g, {{3, 6}, {9.5f, 6}, {11.5f, 8.5f}, {21, 8.5f}, {21, 19.5f}, {3, 19.5f}}, c, true);
    line(dl, g, 12, 17, 12, 11, c);
    poly(dl, g, {{9.5f, 13.5f}, {12, 11}, {14.5f, 13.5f}}, c, false);
}

void onPlate(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const G24 g{p, size / 24};
    line(dl, g, 3, 20.5f, 21, 20.5f, c);
    poly(dl, g, {{7, 11}, {17, 11}, {17, 17.5f}, {7, 17.5f}}, c, true);
    line(dl, g, 12, 3, 12, 8, c);
    poly(dl, g, {{9.5f, 5.5f}, {12, 8}, {14.5f, 5.5f}}, c, false);
}

void move(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const G24 g{p, size / 24};
    line(dl, g, 12, 3, 12, 21, c);
    line(dl, g, 3, 12, 21, 12, c);
    poly(dl, g, {{9, 6}, {12, 3}, {15, 6}}, c, false);
    poly(dl, g, {{9, 18}, {12, 21}, {15, 18}}, c, false);
    poly(dl, g, {{6, 9}, {3, 12}, {6, 15}}, c, false);
    poly(dl, g, {{18, 9}, {21, 12}, {18, 15}}, c, false);
}

void rotate(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const G24 g{p, size / 24};
    dl->PathClear();
    dl->PathArcTo(g.at(12, 12), 7 * g.u, 0.0f, 3.14159265f * 1.75f, 28);
    dl->PathStroke(c, g.w());
    poly(dl, g, {{19, 4}, {19, 8}, {15, 8}}, c, false);
    dl->AddCircleFilled(g.at(12, 12), 1.3f * g.u, c);
}

void scale(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const G24 g{p, size / 24};
    poly(dl, g, {{3.5f, 10.5f}, {13.5f, 10.5f}, {13.5f, 20.5f}, {3.5f, 20.5f}}, c, true);
    line(dl, g, 13, 11, 20.5f, 3.5f, c);
    poly(dl, g, {{15, 3.5f}, {20.5f, 3.5f}, {20.5f, 9}}, c, false);
}

void mirror(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const G24 g{p, size / 24};
    dashed(dl, g, 12, 2.5f, 12, 21.5f, c);
    poly(dl, g, {{9.5f, 6}, {9.5f, 18}, {3, 18}}, c, true);
    poly(dl, g, {{14.5f, 6}, {14.5f, 18}, {21, 18}}, c, true);
}

void measure(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const G24 g{p, size / 24};
    line(dl, g, 5, 19, 19, 5, c);
    line(dl, g, 3, 15, 9, 21, c);
    line(dl, g, 15, 3, 21, 9, c);
    dl->AddCircleFilled(g.at(12, 12), 1.3f * g.u, c);
}

void supports(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const G24 g{p, size / 24};
    poly(dl, g, {{5, 3.5f}, {19, 3.5f}, {19, 9.5f}, {5, 9.5f}}, c, true);
    line(dl, g, 8, 9.5f, 7, 18, c);
    line(dl, g, 12, 9.5f, 12, 18, c);
    line(dl, g, 16, 9.5f, 17, 18, c);
    line(dl, g, 4, 20.5f, 20, 20.5f, c);
}

void hollow(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const G24 g{p, size / 24};
    dl->AddRect(g.at(3.5f, 3.5f), g.at(20.5f, 20.5f), c, 2.5f * g.u, g.w());
    dashed(dl, g, 8, 8, 16, 8, c);
    dashed(dl, g, 16, 8, 16, 16, c);
    dashed(dl, g, 16, 16, 8, 16, c);
    dashed(dl, g, 8, 16, 8, 8, c);
}

void drainHole(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const G24 g{p, size / 24};
    dl->AddCircle(g.at(12, 9), 5.5f * g.u, c, 0, g.w());
    dl->AddCircle(g.at(12, 9), 1.8f * g.u, c, 0, g.w());
    poly(dl, g, {{12, 16.2f}, {13.8f, 19}, {13.6f, 20.6f}, {12, 21.6f}, {10.4f, 20.6f}, {10.2f, 19}}, c, true);
}

void repair(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const G24 g{p, size / 24};
    poly(dl, g, {{2.81f, 16.24f}, {16.24f, 2.81f}, {21.19f, 7.76f}, {7.76f, 21.19f}}, c, true);
    poly(dl, g, {{12, 10}, {14, 12}, {12, 14}, {10, 12}}, c, true);
}

void cursor(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const G24 g{p, size / 24};
    poly(dl, g, {{6, 3.5f}, {18, 12}, {12.5f, 13.3f}, {9.8f, 19}}, c, true);
}

void addTip(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const G24 g{p, size / 24};
    dl->AddCircle(g.at(12, 15), 2.5f * g.u, c, 0, g.w());
    line(dl, g, 12, 3.5f, 12, 9.5f, c);
    line(dl, g, 9, 6.5f, 15, 6.5f, c);
    line(dl, g, 5, 20.5f, 19, 20.5f, c);
}

void zone(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const G24 g{p, size / 24};
    dashed(dl, g, 4, 4, 20, 4, c);
    dashed(dl, g, 20, 4, 20, 20, c);
    dashed(dl, g, 20, 20, 4, 20, c);
    dashed(dl, g, 4, 20, 4, 4, c);
    line(dl, g, 8, 16, 16, 8, c);
}

void back(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const G24 g{p, size / 24};
    poly(dl, g, {{15, 6}, {9, 12}, {15, 18}}, c, false);
}

void undo(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const G24 g{p, size / 24};
    poly(dl, g, {{9, 7}, {5, 11}, {9, 15}}, c, false);
    dl->PathClear();
    dl->PathLineTo(g.at(5, 11));
    dl->PathLineTo(g.at(14, 11));
    dl->PathArcTo(g.at(14, 16), 5 * g.u, -1.5707963f, 1.5707963f, 14);
    dl->PathLineTo(g.at(11, 21));
    dl->PathStroke(c, g.w());
}

void redo(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const G24 g{p, size / 24};
    poly(dl, g, {{15, 7}, {19, 11}, {15, 15}}, c, false);
    dl->PathClear();
    dl->PathLineTo(g.at(19, 11));
    dl->PathLineTo(g.at(10, 11));
    dl->PathArcTo(g.at(10, 16), 5 * g.u, -1.5707963f, -4.712389f, 14);
    dl->PathLineTo(g.at(13, 21));
    dl->PathStroke(c, g.w());
}

void lookUp(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const G24 g{p, size / 24};
    line(dl, g, 4, 5, 20, 5, c);
    line(dl, g, 12, 20, 12, 9, c);
    poly(dl, g, {{8.5f, 12.5f}, {12, 9}, {15.5f, 12.5f}}, c, false);
}

void dots(ImDrawList* dl, ImVec2 p, float size, ImU32 c) {
    const G24 g{p, size / 24};
    for (float y : {5.5f, 12.0f, 18.5f}) dl->AddCircleFilled(g.at(12, y), 1.7f * g.u, c);
}

}  // namespace gui::icon
