// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Icons.h"

#include <algorithm>
#include <cmath>

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

}  // namespace gui::icon
