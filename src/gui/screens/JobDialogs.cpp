// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// "Save … before closing?" and "Where is … now?" (canvas boards "Unsaved"
// and "Relink").
#include <imgui.h>

#include <filesystem>
#include <string>

#include "App.h"
#include "Format.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"

namespace gui::screens::parts {

namespace {

bool beginDialog(const char* id, bool open, float width) {
    if (open && !ImGui::IsPopupOpen(id)) ImGui::OpenPopup(id);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + 140), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 22));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 8));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, col::Panel);
    ImGui::PushStyleColor(ImGuiCol_Border, col::BorderStrong);
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(5, 6, 8, 184));
    const bool shown = ImGui::BeginPopupModal(id, nullptr,
                                              ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    if (!shown) {
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(3);
    }
    return shown;
}

void endDialog() {
    ImGui::EndPopup();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);
}

float width(const char* label, bool bold) {
    ImGui::PushFont(bold ? fonts().bold : fonts().semibold, size::Body);
    const float w = ImGui::CalcTextSize(label).x + 32;
    ImGui::PopFont();
    return w;
}

std::string sentence(const std::vector<std::string>& parts) {
    std::string s;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) s += i + 1 == parts.size() ? " and " : ", ";
        s += parts[i];
    }
    return s;
}

}  // namespace

void unsavedDialog(App& app) {
    App::UnsavedDialog& u = app.unsavedDialog();
    if (!beginDialog("unsaved", u.open, 520)) return;
    if (!u.open) ImGui::CloseCurrentPopup();
    const auto& doc = app.document();
    const std::string name = doc ? doc->name : std::string("this job");
    text(("Save " + name + " before closing?").c_str(), fonts().bold, size::DialogTitle, col::Text);
    ImGui::Dummy(ImVec2(0, 2));
    const std::string body =
        u.changes.empty() ? std::string("This job hasn't been saved yet. If you don't save it, your work on it is lost.")
                          : "Since the last save you " + sentence(u.changes) +
                                ". If you don't save, those changes are lost.";
    text(body.c_str(), fonts().regular, size::Body, col::TextSecondary, true);
    ImGui::Dummy(ImVec2(0, 10));
    const float dw = width("Don't save", false), cw = width("Cancel", false), sw = width("Save", true);
    if (secondaryButton("Don't save", ImVec2(dw, 40))) app.answerUnsaved(App::UnsavedAnswer::DontSave);
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 24 - sw - 10 - cw);
    if (secondaryButton("Cancel", ImVec2(cw, 40)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        app.answerUnsaved(App::UnsavedAnswer::Cancel);
    ImGui::SameLine(0, 10);
    if (primaryButton("Save", ImVec2(sw, 40)) || ImGui::IsKeyPressed(ImGuiKey_Enter))
        app.answerUnsaved(App::UnsavedAnswer::Save);
    endDialog();
}

void relinkDialog(App& app) {
    App::RelinkDialog& r = app.relinkDialog();
    if (!beginDialog("relink", r.open, 560)) return;
    if (!r.open) ImGui::CloseCurrentPopup();
    const std::string file = std::filesystem::path(r.model_path).filename().string();
    text(("Where is " + (file.empty() ? std::string("the model") : file) + " now?").c_str(), fonts().bold,
         size::DialogTitle, col::Text);
    ImGui::Dummy(ImVec2(0, 2));
    text("This job was made from a model that is no longer where it was. The job keeps its own copy, so nothing is "
         "lost.",
         fonts().regular, size::Body, col::TextSecondary, true);
    if (!r.model_path.empty())
        text(("Last seen: " + r.model_path).c_str(), fonts().regular, size::Help, col::TextMuted, true);
    ImGui::Dummy(ImVec2(0, 6));
    groupLabel("How to continue");
    if (radioCard("copy", "Use the copy in the job", "Everything stays as it was.", !r.find)) r.find = false;
    ImGui::Dummy(ImVec2(0, 2));
    if (radioCard("find", "Find the file", "Pick it where it is now. If it changed, supports go out of date.", r.find))
        r.find = true;
    ImGui::Dummy(ImVec2(0, 10));
    const char* go = r.find ? "Find and open…" : "Open the job";
    const float cw = width("Cancel", false), gw = width(go, true);
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 24 - gw - 10 - cw);
    if (secondaryButton("Cancel", ImVec2(cw, 40)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) app.relinkCancel();
    ImGui::SameLine(0, 10);
    if (primaryButton(go, ImVec2(gw, 40))) app.relinkOpen();
    endDialog();
}

}  // namespace gui::screens::parts
