// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Home: start a print, recent jobs.
#include <imgui.h>

#include <cctype>
#include <filesystem>

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
    {
        const float y = ImGui::GetCursorPosY();
        sectionTitle("Recent jobs");
        ImGui::PushFont(fonts().semibold, size::Body);
        const float lw = ImGui::CalcTextSize("Open a job file…").x;
        ImGui::PopFont();
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + w - lw, y + 2));
        if (linkButton("Open a job file…")) app.openDialog();
    }
    ImGui::Dummy(ImVec2(0, 4));
    // A row: the job's initial, its name, and how far it got.
    auto row = [&](const char* id, const std::string& name, const std::string& detail, bool there) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 sz(w, 56);
        const bool clicked = ImGui::InvisibleButton(id, sz) && there;
        const bool hovered = ImGui::IsItemHovered() && there;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), hovered ? col::Card : col::Panel, 10);
        dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), col::Border, 10);
        dl->AddCircleFilled(ImVec2(p.x + 28, p.y + 28), 15, col::Active, 24);
        const std::string initial = name.empty() ? "?" : std::string(1, static_cast<char>(std::toupper(
                                                                          static_cast<unsigned char>(name[0]))));
        ImGui::PushFont(fonts().bold, size::Body);
        const ImVec2 is = ImGui::CalcTextSize(initial.c_str());
        dl->AddText(ImVec2(p.x + 28 - is.x / 2, p.y + 28 - is.y / 2), col::TextSecondary, initial.c_str());
        ImGui::PopFont();
        ImGui::PushFont(fonts().semibold, size::Body);
        dl->AddText(ImVec2(p.x + 56, p.y + 11), there ? col::Text : col::TextMuted, name.c_str());
        ImGui::PopFont();
        ImGui::PushFont(fonts().regular, size::Help);
        dl->AddText(ImVec2(p.x + 56, p.y + 30), col::TextMuted, detail.c_str());
        ImGui::PopFont();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::Dummy(ImVec2(0, 2));
        return clicked;
    };
    const auto& doc = app.document();
    bool any = false;
    if (doc && doc->job_path.empty()) {
        if (row("open_now", doc->name, "Open now · not saved yet", true)) app.setScreen(Screen::Job);
        any = true;
    }
    int shown = 0;
    for (const RecentJob& j : app.recentJobs()) {
        if (shown == 6) break;
        const bool there = std::filesystem::exists(j.path);
        std::string detail = there ? j.state : std::string("moved or deleted");
        if (there && !j.when.empty()) detail = j.when + (detail.empty() ? "" : " · " + detail);
        ImGui::PushID(shown);
        if (row("job", j.name, detail, there)) {
            if (doc && doc->job_path == j.path) app.setScreen(Screen::Job);
            else app.openPath(j.path);
        }
        ImGui::PopID();
        ++shown;
        any = true;
    }
    if (!any) textMuted("Nothing yet. Jobs you save show up here.");
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

}  // namespace gui::screens
