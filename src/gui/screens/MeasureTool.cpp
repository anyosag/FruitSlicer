// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The Measure tool: click a point on the model, then a second; the panel gives
// the distance and its X, Y and Z parts, the view draws the line. It changes
// nothing in the job.
#include <imgui.h>

#include <algorithm>
#include <cmath>

#include "App.h"
#include "Format.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"

namespace gui::screens::parts {

namespace {

constexpr double kShown = 0.005;  // below this, a distance reads as 0

std::string mm(double v) { return strf("%.2f mm", v < kShown ? 0.0 : v); }

}  // namespace

void measurePanel(App& app, Document& doc) {
    const MeasureState& ms = app.measured();
    const bool mine = ms.geometry_id == doc.geometry_id;
    const auto m = mine ? measurementOf(ms, doc.transform()) : std::nullopt;
    groupLabel("Distance");
    if (m) {
        keyValue("Between the points", mm(m->distance));
        ImGui::Dummy(ImVec2(0, 4));
        keyValue("Along X", mm(m->dx));
        ImGui::Dummy(ImVec2(0, 4));
        keyValue("Along Y", mm(m->dy));
        ImGui::Dummy(ImVec2(0, 4));
        keyValue("Along Z", mm(m->dz));
        ImGui::Dummy(ImVec2(0, 4));
        text("Click again to start a new one.", fonts().regular, size::Help, col::TextMuted, true);
    } else if (mine && !ms.empty()) {
        text("Now click a second point on the model.", fonts().regular, size::Body, col::TextSecondary, true);
    } else {
        text("Click a point on the model, then a second one.", fonts().regular, size::Body, col::TextSecondary, true);
    }
    ImGui::Dummy(ImVec2(0, 6));
    if (secondaryButton("Clear", ImVec2(0, 32), mine && !ms.empty())) app.clearMeasure();
    text("Esc clears it too.", fonts().regular, size::Help, col::TextMuted);
    ImGui::Dummy(ImVec2(0, 10));
    groupLabel("The whole model");
    const fruit::Vec3 s = doc.placedBox().size();
    keyValue("Width (X)", mm(s.x));
    ImGui::Dummy(ImVec2(0, 4));
    keyValue("Depth (Y)", mm(s.y));
    ImGui::Dummy(ImVec2(0, 4));
    keyValue("Height (Z)", mm(s.z));
    ImGui::Dummy(ImVec2(0, 8));
    text("Measuring changes nothing in the job. Distances are on the surface points you click, at the model's current size.",
         fonts().regular, size::Help, col::TextMuted, true);
}

void measureOverlay(App& app, Document& doc, ImVec2 origin, ImVec2 size, const float* vp) {
    auto project = [&](const fruit::Vec3& w, ImVec2* out) {
        const float x = vp[0] * w.x + vp[4] * w.y + vp[8] * w.z + vp[12];
        const float y = vp[1] * w.x + vp[5] * w.y + vp[9] * w.z + vp[13];
        const float c = vp[3] * w.x + vp[7] * w.y + vp[11] * w.z + vp[15];
        if (c <= 1e-4f) return false;
        *out = ImVec2(origin.x + (x / c * 0.5f + 0.5f) * size.x, origin.y + (0.5f - y / c * 0.5f) * size.y);
        return true;
    };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const MeasureState& ms = app.measured();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool inside = mouse.x >= origin.x && mouse.y >= origin.y + 44 && mouse.x < origin.x + size.x &&
                        mouse.y < origin.y + size.y && ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered();

    // The spot under the mouse, as a ring, before it is clicked.
    fruit::Vec3 under, normal;
    bool over_model = false;
    if (inside && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Right) &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        fruit::Vec3 o, d;
        rayThroughPixel(app.camera(), mouse.x, mouse.y, origin.x, origin.y, size.x, size.y, &o, &d);
        over_model = app.pickModel(o, d, &under, &normal);
    }
    if (over_model) {
        ImVec2 a;
        if (project(under, &a)) {
            dl->AddCircle(a, 7, col::Accent, 24, 1.5f);
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
    }

    // The points picked and the line between them.
    if (ms.geometry_id == doc.geometry_id && !ms.empty()) {
        const std::vector<fruit::Vec3> pts = pointsOnPlate(ms, doc.transform());
        ImVec2 a[2];
        bool ok[2] = {false, false};
        for (std::size_t i = 0; i < pts.size(); ++i) ok[i] = project(pts[i], &a[i]);
        const ImU32 line = IM_COL32(0xFF, 0xC8, 0x5A, 255);
        if (pts.size() == 2 && ok[0] && ok[1]) {
            dl->AddLine(a[0], a[1], IM_COL32(0x0E, 0x10, 0x14, 200), 4.5f);
            dl->AddLine(a[0], a[1], line, 2.0f);
        }
        for (std::size_t i = 0; i < pts.size(); ++i) {
            if (!ok[i]) continue;
            dl->AddCircleFilled(a[i], 6, IM_COL32(0x0E, 0x10, 0x14, 230), 20);
            dl->AddCircleFilled(a[i], 4.2f, line, 20);
        }
        if (const auto m = measurementOf(ms, doc.transform()); m && ok[0] && ok[1]) {
            const std::string label = strf("%.2f mm", m->distance);
            ImGui::PushFont(fonts().semibold, size::Help);
            const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
            const ImVec2 mid((a[0].x + a[1].x) / 2, (a[0].y + a[1].y) / 2);
            const ImVec2 at(mid.x - ts.x / 2, mid.y - ts.y / 2 - 14);
            dl->AddRectFilled(ImVec2(at.x - 7, at.y - 3), ImVec2(at.x + ts.x + 7, at.y + ts.y + 3), IM_COL32(0x0E, 0x10, 0x14, 225), 7);
            dl->AddText(at, col::Text, label.c_str());
            ImGui::PopFont();
        }
    }

    // A click (not a drag of the view) on the model picks a point.
    if (inside && over_model && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && io.MouseDragMaxDistanceSqr[0] < 9)
        app.measureClick(under);
}

}  // namespace gui::screens::parts
