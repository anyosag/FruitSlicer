// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Job · Model: the model on the plate, its size and whether it fits.
#include <imgui.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "App.h"
#include "Icons.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/Screens.h"

namespace gui::screens {

namespace {

std::string fmt(const char* f, double a) {
    char b[64];
    std::snprintf(b, sizeof b, f, a);
    return b;
}

std::string sizeText(const fruit::BBox3& b) {
    char s[96];
    std::snprintf(s, sizeof s, "%.1f × %.1f × %.1f mm", b.max.x - b.min.x, b.max.y - b.min.y, b.max.z - b.min.z);
    return s;
}

std::string thousands(size_t n) {
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return s;
}

void leftPanel(App& app) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::Panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 18));
    ImGui::BeginChild("left", ImVec2(300, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    dl->AddLine(ImVec2(wp.x + ws.x - 1, wp.y), ImVec2(wp.x + ws.x - 1, wp.y + ws.y), col::Border);

    sectionTitle("Model");
    ImGui::Dummy(ImVec2(0, 4));
    const auto& doc = app.document();
    if (!doc) {
        text("No model on the plate yet. Open a file or drop it on the plate.", fonts().regular, size::Body,
             col::TextSecondary, true);
        ImGui::Dummy(ImVec2(0, 8));
        groupLabel("Files FruitSlicer reads");
        ImGui::Dummy(ImVec2(0, 2));
        for (auto [k, v] : {std::pair{"STL", "binary or text"}, std::pair{"OBJ", "shapes only, no colours"}}) {
            beginCard(k, ImVec2(0, 0), 12, col::Card);
            text(k, fonts().semibold, size::Body, col::Text);
            ImGui::SameLine();
            const float vw = ImGui::CalcTextSize(v).x;
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 12 - vw);
            text(v, fonts().regular, size::Body, col::TextMuted);
            endCard();
        }
        ImGui::Dummy(ImVec2(0, 8));
        if (primaryButton("Open model...", ImVec2(-1, 0))) app.openDialog();
    } else {
        beginCard("file", ImVec2(0, 0), 14, col::Card);
        text(std::filesystem::path(doc->path).filename().string().c_str(), fonts().semibold, size::Body, col::Text);
        const bool closed = doc->model.mesh.isClosedManifold();
        textMuted((thousands(doc->model.mesh.faceCount()) + " triangles · " + (closed ? "watertight" : "not watertight")).c_str());
        endCard();
        ImGui::Dummy(ImVec2(0, 8));
        groupLabel("Size");
        const fruit::BBox3 b = doc->placed.bbox();
        keyValue("At 100 %", sizeText(b));
        keyValue("Volume", fmt("%.1f ml", doc->model.mesh.volume() / 1000.0));
        ImGui::Dummy(ImVec2(0, 8));
        groupLabel("On the plate");
        keyValue("Lift", fmt("%.0f mm", doc->placement.lift()));
        keyValue("Shells", std::to_string(doc->model.shells));
        if (doc->model.shells_flipped > 0)
            keyValue("Turned outward", std::to_string(doc->model.shells_flipped) + " inside-out");
        ImGui::Dummy(ImVec2(0, 10));
        const bool fits = doc->fitsPlate(app.printer());
        beginCard("fit", ImVec2(0, 0), 12, fits ? col::Card : col::ErrorBg, fits ? col::Border : IM_COL32(0x6B, 0x2A, 0x2A, 255));
        text(fits ? "Fits the plate" : "Too big for the plate", fonts().semibold, size::Body, fits ? col::Ok : col::Error);
        text(fits ? "Scaling and rotating come with the next update."
                  : "The plate is 211.68 × 118.37 mm and 220 mm tall. Scaling comes with the next update.",
             fonts().regular, size::Help, col::TextSecondary, true);
        endCard();
        ImGui::Dummy(ImVec2(0, 10));
        if (secondaryButton("Open another...", ImVec2(-1, 0))) app.openDialog();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void taskTray(App& app, ImVec2 vp_min, ImVec2 vp_max) {
    const JobProgress p = app.jobs().progress();
    if (!p.running) return;
    const ImVec2 size(340, 96);
    ImGui::SetCursorScreenPos(ImVec2(vp_max.x - size.x - 16, vp_max.y - size.y - 16));
    beginCard("tray", size, 14, col::Panel, col::BorderStrong);
    text(p.name.c_str(), fonts().semibold, size::Body, col::Text);
    ImGui::Dummy(ImVec2(0, 2));
    const ImVec2 bp = ImGui::GetCursorScreenPos();
    const float bw = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + 6), col::Input, 3);
    dl->AddRectFilled(bp, ImVec2(bp.x + bw * static_cast<float>(p.fraction), bp.y + 6), col::Accent, 3);
    ImGui::Dummy(ImVec2(bw, 12));
    if (secondaryButton("Cancel")) app.jobs().cancel();
    endCard();
    (void)vp_min;
}

void viewport(App& app) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::Viewport);
    ImGui::BeginChild("viewport", ImVec2(0, -36), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImGuiIO& io = ImGui::GetIO();
    const int pw = static_cast<int>(avail.x * io.DisplayFramebufferScale.x);
    const int ph = static_cast<int>(avail.y * io.DisplayFramebufferScale.y);
    const auto& doc = app.document();
    if (doc) app.renderer().setModel(doc->placed, doc->revision);
    else app.renderer().clearModel();
    const unsigned tex = app.renderer().render(pw, ph, app.camera(), app.options().deterministic ? 0 : app.options().msaa_samples);
    if (tex) {
        ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(tex)), avail, ImVec2(0, 1), ImVec2(1, 0));
    } else {
        ImGui::Dummy(avail);
    }
    // Mouse: left drag turns, right/middle (or Shift + left) drag moves, wheel zooms.
    if (ImGui::IsItemHovered()) {
        if (io.MouseWheel != 0) app.camera().zoom(io.MouseWheel);
        const bool pan = ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                         (io.KeyShift && ImGui::IsMouseDragging(ImGuiMouseButton_Left));
        if (pan) app.camera().pan(io.MouseDelta.x, io.MouseDelta.y, avail.y);
        else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) app.camera().orbit(io.MouseDelta.x, io.MouseDelta.y);
    }
    // View buttons.
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 14, origin.y + 12));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 7));
    ImGui::PushFont(fonts().medium, size::Help);
    for (auto [label, v] : {std::pair{"Front", ViewPreset::Front}, std::pair{"Side", ViewPreset::Side},
                            std::pair{"Angled", ViewPreset::Angled}, std::pair{"From below", ViewPreset::Below}}) {
        if (ImGui::Button(label)) app.setView(v);
        ImGui::SameLine(0, 6);
    }
    ImGui::PopFont();
    ImGui::PopStyleVar();
    if (!doc && !app.jobs().busy()) {
        // Empty plate: say what to do, right where the model will appear.
        const ImVec2 cs(440, 190);
        ImGui::SetCursorScreenPos(ImVec2(origin.x + (avail.x - cs.x) / 2, origin.y + (avail.y - cs.y) / 2));
        beginCard("drop", cs, 22, IM_COL32(0x18, 0x1B, 0x21, 235), col::BorderStrong);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        icon::upload(dl, ImVec2(p.x + (cs.x - 44) / 2 - 16, p.y), 32, col::TextSecondary);
        ImGui::Dummy(ImVec2(0, 40));
        ImGui::PushFont(fonts().semibold, size::Section);
        const char* t = "Drop an STL or OBJ file";
        ImGui::SetCursorPosX((cs.x - ImGui::CalcTextSize(t).x) / 2);
        ImGui::TextUnformatted(t);
        ImGui::PopFont();
        ImGui::PushFont(fonts().regular, size::Body);
        const char* t2 = "The plate is 211.68 × 118.37 mm and 220 mm tall.";
        ImGui::SetCursorPosX((cs.x - ImGui::CalcTextSize(t2).x) / 2);
        ImGui::PushStyleColor(ImGuiCol_Text, col::TextSecondary);
        ImGui::TextUnformatted(t2);
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::SetCursorPosX((cs.x - 140) / 2);
        if (primaryButton("Open model...", ImVec2(140, 0))) app.openDialog();
        endCard();
    }
    taskTray(app, origin, ImVec2(origin.x + avail.x, origin.y + avail.y));
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void statusBar(App& app) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::Panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 9));
    ImGui::BeginChild("status", ImVec2(0, 36), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    dl->AddLine(wp, ImVec2(wp.x + ImGui::GetWindowWidth(), wp.y), col::Border);
    const auto& doc = app.document();
    ImU32 dot = col::TextMuted;
    std::string msg = "No model";
    if (doc) {
        const bool fits = doc->fitsPlate(app.printer());
        dot = fits ? col::Ok : col::Error;
        msg = (fits ? "Fits the plate · " : "Too big for the plate · ") + sizeText(doc->placed.bbox());
    }
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddCircleFilled(ImVec2(p.x + 4, p.y + 9), 3.5f, dot);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14);
    text(msg.c_str(), fonts().regular, size::Help, col::TextSecondary);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

}  // namespace

void model(App& app) {
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);  // structural panels are square
    leftPanel(app);
    ImGui::SameLine(0, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::BeginGroup();
    viewport(app);
    statusBar(app);
    ImGui::EndGroup();
    ImGui::PopStyleVar(2);
}

}  // namespace gui::screens
