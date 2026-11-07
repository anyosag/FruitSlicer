// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Home: start a print, recent jobs.
#include <imgui.h>

#include "App.h"
#include "Icons.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/Screens.h"

namespace gui::screens {

void home(App& app) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(32, 28));
    ImGui::BeginChild("home", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    title("Home");
    ImGui::Dummy(ImVec2(0, 12));

    const float w = std::min(ImGui::GetContentRegionAvail().x, 760.0f);
    beginCard("start", ImVec2(w, 0), 20);
    sectionTitle("Start a print");
    ImGui::Dummy(ImVec2(0, 6));
    {
        // Drop zone: the whole app accepts dropped files; this says so.
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float zw = ImGui::GetContentRegionAvail().x, zh = 250;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dashedRect(dl, p, ImVec2(p.x + zw, p.y + zh), col::BorderStrong);
        icon::cube(dl, ImVec2(p.x + zw / 2 - 18, p.y + 34), 36, col::TextMuted);
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 88));
        ImGui::PushFont(fonts().semibold, size::Section);
        const char* t1 = "Drop an STL, OBJ or 3MF file here";
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (zw - ImGui::CalcTextSize(t1).x) / 2);
        ImGui::PushStyleColor(ImGuiCol_Text, col::Text);
        ImGui::TextUnformatted(t1);
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::PushFont(fonts().regular, size::Body);
        const char* t2 = "FruitSlicer checks it and places it on the plate.";
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (zw - ImGui::CalcTextSize(t2).x) / 2);
        ImGui::PushStyleColor(ImGuiCol_Text, col::TextSecondary);
        ImGui::TextUnformatted(t2);
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 10));
        const float bw = 150;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (zw - bw) / 2);
        if (primaryButton("Open model...", ImVec2(bw, 0))) app.openDialog();
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + zh));
        ImGui::Dummy(ImVec2(zw, 1));
    }
    endCard();

    ImGui::Dummy(ImVec2(0, 20));
    sectionTitle("Recent jobs");
    ImGui::Dummy(ImVec2(0, 4));
    if (const auto& doc = app.document()) {
        beginCard("recent", ImVec2(w, 0), 14, col::Panel);
        text(doc->name.c_str(), fonts().semibold, size::Body, col::Text);
        textMuted("Open now. Saving jobs comes with the next update.");
        if (linkButton("Show it")) app.setScreen(Screen::Job);
        endCard();
    } else {
        textMuted("Nothing yet. Jobs you open show up here.");
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

}  // namespace gui::screens
