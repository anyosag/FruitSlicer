// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Job · Supports · editing tips by hand (canvas board "TipEdit"): add, move,
// delete, resize and pin tips, and draw no-tip zones. Every edit is listed
// and can be undone on its own.
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "App.h"
#include "Format.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"

namespace gui::screens::parts {

namespace {

const char* const kSizes[] = {"0.2", "0.3", "0.4", "0.6"};

double classDiameter(int c) {
    static const double d[] = {0.2, 0.3, 0.4, 0.6};
    return d[std::clamp(c, 0, 3)];
}

int classOf(double diameter) {
    int best = 0;
    for (int c = 1; c < 4; ++c)
        if (std::abs(classDiameter(c) - diameter) < std::abs(classDiameter(best) - diameter)) best = c;
    return best;
}

}  // namespace

void tipEditPanel(App& app, Document& doc) {
    ImGui::Dummy(ImVec2(0, 6));
    groupLabel("Size for new tips");
    for (int c = 1; c < 4; ++c) {
        if (segmentButton((std::string(kSizes[c]) + " mm").c_str(), app.newTipClass() == c)) app.newTipClass() = c;
        if (c < 3) ImGui::SameLine(0, 4);
    }
    ImGui::Dummy(ImVec2(0, 6));
    if (radioCard("zones", "Show no-tip zones", "Where deleted tips were, and the zones you drew.", app.showZones()))
        app.showZones() = !app.showZones();
    ImGui::Dummy(ImVec2(0, 8));
    tipEditSelected(app, doc);
    ImGui::Dummy(ImVec2(0, 8));
    // Your edits, newest first.
    const auto& edits = doc.supports.edits;
    {
        const float y = ImGui::GetCursorPosY();
        groupLabel("Your edits");
        if (!edits.empty()) {
            ImGui::PushFont(fonts().semibold, size::Body);
            const float lw = ImGui::CalcTextSize("Undo all").x;
            ImGui::PopFont();
            ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + ImGui::GetContentRegionAvail().x - lw, y - 2));
            if (linkButton("Undo all")) app.undoAllTipEdits();
        }
    }
    if (edits.empty()) {
        text("None yet. Click the model to add a tip, or select one to change it.", fonts().regular, size::Body,
             col::TextMuted, true);
    } else {
        const fruit::Transform t = doc.transform();
        const float list_h = std::min(220.0f, 70.0f * static_cast<float>(edits.size()));
        ImGui::BeginChild("edits", ImVec2(0, list_h), ImGuiChildFlags_None);
        for (int i = static_cast<int>(edits.size()) - 1; i >= 0; --i) {
            const auto [title, detail] = describeEdit(edits[static_cast<std::size_t>(i)], t);
            ImGui::PushID(i);
            beginCard("e", ImVec2(0, 0), 10, col::Panel, col::Border);
            const float y = ImGui::GetCursorPosY();
            text(title.c_str(), fonts().semibold, size::Body, col::Text);
            text(detail.c_str(), fonts().regular, size::Help, col::TextMuted, true);
            ImGui::PushFont(fonts().semibold, size::Body);
            const float lw = ImGui::CalcTextSize("Undo").x;
            ImGui::PopFont();
            ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + ImGui::GetContentRegionAvail().x - lw, y));
            if (linkButton("Undo")) app.undoTipEdit(i);
            endCard();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    ImGui::Dummy(ImVec2(0, 4));
    text("Edits stay when you remake supports: the planner works around your tips and keeps its own out of the "
         "zones.",
         fonts().regular, size::Help, col::TextMuted, true);
    ImGui::Dummy(ImVec2(0, 4));
    if (primaryButton("Done", ImVec2(-1, 38))) app.editTips(false);
}

void tipEditSelected(App& app, Document& doc) {
    const SupportState& s = doc.supports;
    groupLabel("Selected tip");
    if (!s.result || s.selected_tip < 0 || s.selected_tip >= static_cast<int>(s.result->points.size())) {
        text("Click a tip to change its size, pin it or delete it. Drag it to move it.", fonts().regular, size::Body,
             col::TextSecondary, true);
        return;
    }
    const SupportResult& r = *s.result;
    const fruit::SupportPoint& p = r.points[static_cast<std::size_t>(s.selected_tip)];
    const bool yours = p.category == fruit::PointCategory::Manual;
    beginCard("tip", ImVec2(0, 0), 13, col::Panel, col::Border);
    text(yours ? "Your tip" : tipKind(p).c_str(), fonts().semibold, size::Body, col::Text);
    text(yours ? "Added, moved, resized or pinned by you: kept when supports are remade." : tipWhy(p).c_str(),
         fonts().regular, size::Body, col::TextSecondary, true);
    ImGui::Dummy(ImVec2(0, 4));
    text("Size", fonts().semibold, size::Help, col::TextMuted);
    const int now = classOf(p.tip_diameter);
    for (int c = 0; c < 4; ++c) {
        if (segmentButton(kSizes[c], c == now) && c != now) app.resizeTip(s.selected_tip, classDiameter(c));
        if (c < 3) ImGui::SameLine(0, 4);
    }
    ImGui::Dummy(ImVec2(0, 2));
    keyValue("Holds", strf("%.1f mm² of layer", r.cfg.capacity.capacity(p.tip_diameter)));
    keyValue("Contact depth", strf("%.2f mm", fruit::tipPenetration(p.tip_diameter, r.tree_cfg)));
    keyValue("Position", strf("%.2f, %.2f, %.2f mm", p.pos.x, p.pos.y, p.pos.z));
    keyValue("Layer", thousands(static_cast<std::size_t>(std::max(0, p.layer)) + 1));
    endCard();
    ImGui::Dummy(ImVec2(0, 6));
    if (secondaryButton(yours ? "Pinned" : "Pin it", ImVec2(110, 36), !yours)) app.pinTip(s.selected_tip);
    ImGui::SameLine(0, 8);
    if (secondaryButton("Delete", ImVec2(0, 36))) app.deleteTip(s.selected_tip);
    ImGui::Dummy(ImVec2(0, 4));
    text("A pinned tip is never removed when supports are remade. Moving or resizing a tip pins it.",
         fonts().regular, size::Help, col::TextMuted, true);
}

void tipEditOverlay(App& app, Document& doc, ImVec2 origin, ImVec2 size, const float* vp) {
    const SupportState& s = doc.supports;
    if (!s.result) return;
    const SupportResult& r = *s.result;
    auto project = [&](const fruit::Vec3& p, ImVec2* out) {
        const fruit::Vec3 w = p;
        const float x = vp[0] * w.x + vp[4] * w.y + vp[8] * w.z + vp[12];
        const float y = vp[1] * w.x + vp[5] * w.y + vp[9] * w.z + vp[13];
        const float c = vp[3] * w.x + vp[7] * w.y + vp[11] * w.z + vp[15];
        if (c <= 1e-4f) return false;
        *out = ImVec2(origin.x + (x / c * 0.5f + 0.5f) * size.x, origin.y + (0.5f - y / c * 0.5f) * size.y);
        return true;
    };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const fruit::Transform t = doc.transform();
    // No-tip zones: a ring the zone's size, at its centre.
    if (app.showZones()) {
        for (const TipEdit& e : s.edits) {
            if (e.kind != TipEdit::Kind::Delete && e.kind != TipEdit::Kind::Zone && e.kind != TipEdit::Kind::Move)
                continue;
            const fruit::Vec3 c = t.apply(e.at);
            const double rad = e.radius;
            ImVec2 a, b;
            if (!project(c, &a) || !project({c.x + rad, c.y, c.z}, &b)) continue;
            const float px = std::max(4.0f, std::hypot(b.x - a.x, b.y - a.y));
            dl->AddCircleFilled(a, px, IM_COL32(0xFF, 0x6B, 0x6B, 40), 32);
            dl->AddCircle(a, px, IM_COL32(0xFF, 0x6B, 0x6B, 200), 32, 1.5f);
        }
    }
    // Your tips, ringed in the accent colour; the selected one stronger.
    for (std::size_t i = 0; i < r.points.size(); ++i) {
        if (r.points[i].category != fruit::PointCategory::Manual) continue;
        ImVec2 q;
        if (project(r.points[i].pos, &q)) dl->AddCircle(q, 6, col::Accent, 16, 1.5f);
    }
    if (s.selected_tip >= 0 && s.selected_tip < static_cast<int>(r.points.size())) {
        ImVec2 q;
        if (project(r.points[static_cast<std::size_t>(s.selected_tip)].pos, &q)) {
            dl->AddCircle(q, 9, col::Accent, 24, 2.5f);
            dl->AddCircle(q, 12, IM_COL32(0xFF, 0x8F, 0x3F, 90), 24, 2.0f);
        }
    }
    // Legend.
    {
        const ImVec2 p(origin.x + kToolPanelRight + 16, origin.y + size.y - 34);  // clear of the floating panel
        dl->AddCircleFilled(ImVec2(p.x + 6, p.y + 8), 6, IM_COL32(0xFF, 0x6B, 0x6B, 60), 16);
        dl->AddCircle(ImVec2(p.x + 6, p.y + 8), 6, IM_COL32(0xFF, 0x6B, 0x6B, 200), 16, 1.5f);
        ImGui::PushFont(fonts().regular, size::Help);
        dl->AddText(ImVec2(p.x + 18, p.y + 1), col::TextSecondary, "no-tip zone");
        dl->AddCircle(ImVec2(p.x + 112, p.y + 8), 6, col::Accent, 16, 1.5f);
        dl->AddText(ImVec2(p.x + 124, p.y + 1), col::TextSecondary, "yours");
        ImGui::PopFont();
    }

    // The mouse: pick, add, draw zones, drag tips.
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    const bool inside = m.x >= origin.x && m.y >= origin.y + 44 && m.x < origin.x + size.x && m.y < origin.y + size.y &&
                        ImGui::IsWindowHovered();  // not over a panel floating on the view
    auto ray = [&](fruit::Vec3* pos, fruit::Vec3* normal) {
        const float nx = 2 * (m.x - origin.x) / size.x - 1, ny = 1 - 2 * (m.y - origin.y) / size.y;
        V3 o, d;
        app.camera().ray(nx, ny, size.x / std::max(1.0f, size.y), &o, &d);
        return app.pickModel({o.x, o.y, o.z}, {d.x, d.y, d.z}, pos, normal);
    };
    auto nearestTip = [&]() {
        int best = -1;
        float best_d = 12 * 12;
        for (std::size_t i = 0; i < r.points.size(); ++i) {
            ImVec2 q;
            if (!project(r.points[i].pos, &q)) continue;
            const float d = (q.x - m.x) * (q.x - m.x) + (q.y - m.y) * (q.y - m.y);
            if (d < best_d) {
                best_d = d;
                best = static_cast<int>(i);
            }
        }
        return best;
    };
    int& drag = app.draggingTip();
    if (app.tipTool() == App::TipTool::Select) {
        if (inside && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyShift && !ImGui::IsAnyItemHovered()) {
            const int at = nearestTip();
            if (at >= 0) {
                app.selectTip(at);
                drag = at;
            }
        }
        if (drag >= 0) {
            fruit::Vec3 pos, n;
            const bool over = ray(&pos, &n);
            if (over) {
                ImVec2 q;
                if (project(pos, &q)) dl->AddCircle(q, 7, col::Accent, 16, 2.0f);
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                if (over && io.MouseDragMaxDistanceSqr[0] >= 9) app.moveTip(drag, pos, n);
                drag = -1;
            }
        } else if (inside && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && io.MouseDragMaxDistanceSqr[0] < 9 &&
                   !ImGui::IsAnyItemHovered() && nearestTip() < 0) {
            app.selectTip(-1);
        }
    } else if (inside && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && io.MouseDragMaxDistanceSqr[0] < 9 &&
               !ImGui::IsAnyItemHovered()) {
        fruit::Vec3 pos, n;
        if (ray(&pos, &n)) {
            if (app.tipTool() == App::TipTool::Add) app.addTipAt(pos, n);
            else app.addZoneAt(pos);
        }
    }
}

}  // namespace gui::screens::parts
