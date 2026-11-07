// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Resins (canvas board "Resins"): each resin's print settings and the
// support values measured for it, with where each value comes from.
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "App.h"
#include "Format.h"
#include "Icons.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"
#include "screens/Screens.h"

namespace gui::screens {

namespace {

using parts::tooltip;

constexpr float kListW = 300;

float textWidth(const char* s, ImFont* f, float sz) {
    ImGui::PushFont(f, sz);
    const float w = ImGui::CalcTextSize(s).x;
    ImGui::PopFont();
    return w;
}

// Where a value comes from, as a small badge.
void sourceBadge(ResinValue::Source s) {
    const char* label = s == ResinValue::Source::Measured ? "Measured" : s == ResinValue::Source::Borrowed ? "Borrowed"
                                                                                                          : "Default";
    const ImU32 fg = s == ResinValue::Source::Measured ? col::Ok : col::TextSecondary;
    const ImU32 bg = s == ResinValue::Source::Measured ? col::OkBg : col::Panel;
    const ImU32 bd = s == ResinValue::Source::Measured ? IM_COL32(0x1E, 0x5A, 0x3E, 255) : col::BorderStrong;
    ImGui::PushFont(fonts().medium, size::Label);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + ts.x + 14, p.y + 18);
    dl->AddRectFilled(p, q, bg, 9);
    dl->AddRect(p, q, bd, 9);
    dl->AddText(ImVec2(p.x + 7, p.y + (18 - ts.y) / 2), fg, label);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(ts.x + 14, 18));
}

float badgeWidth(ResinValue::Source s) {
    return textWidth(s == ResinValue::Source::Measured   ? "Measured"
                     : s == ResinValue::Source::Borrowed ? "Borrowed"
                                                         : "Default",
                     fonts().medium, size::Label) +
           14;
}

// A line: label on the left, value (and a badge) on the right.
void line(const std::string& label, const std::string& value, const ResinValue::Source* source, bool first) {
    const float w = ImGui::GetContentRegionAvail().x;
    if (!first) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + w, p.y), col::Border);
    }
    ImGui::Dummy(ImVec2(w, 7));
    const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
    const float bw = source ? badgeWidth(*source) + 10 : 0;
    const float vw = textWidth(value.c_str(), fonts().semibold, size::Body);
    ImGui::PushTextWrapPos(x0 + w - vw - bw - 16);
    text(label.c_str(), fonts().regular, size::Body, col::TextSecondary, true);
    ImGui::PopTextWrapPos();
    const float bottom = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(x0 + w - bw - vw, y0));
    text(value.c_str(), fonts().semibold, size::Body, col::Text);
    if (source) {
        ImGui::SetCursorPos(ImVec2(x0 + w - bw + 10, y0));
        sourceBadge(*source);
    }
    ImGui::SetCursorPos(ImVec2(x0, std::max(bottom, y0 + 18) + 2));
    ImGui::Dummy(ImVec2(0, 0));
}

void list(App& app) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::Rail);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 24));
    ImGui::BeginChild("resin_list", ImVec2(kListW, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    dl->AddLine(ImVec2(wp.x + ws.x - 1, wp.y), ImVec2(wp.x + ws.x - 1, wp.y + ws.y), col::Border);
    const float w = ImGui::GetContentRegionAvail().x;
    {
        const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
        text("Resins", fonts().bold, size::PageTitle - 6, col::Text);
        const float lw = textWidth("Printer", fonts().semibold, size::Body);
        ImGui::SetCursorPos(ImVec2(x0 + w - lw, y0 + 6));
        if (linkButton("Printer")) app.setScreen(Screen::Printer);
        ImGui::SetCursorPos(ImVec2(x0, y0 + 40));
        ImGui::Dummy(ImVec2(0, 0));
    }
    const std::string for_new = app.newJobResin().id;
    for (const Resin& r : app.resins()) {
        ImGui::PushID(r.id.c_str());
        const bool on = app.resinSelected() == r.id;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("r", ImVec2(w, 52))) app.resinSelected() = r.id;
        if (on || ImGui::IsItemHovered()) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 52), on ? col::Card : col::Panel, 10);
        if (on) dl->AddRect(p, ImVec2(p.x + w, p.y + 52), col::BorderStrong, 10);
        ImGui::PushFont(fonts().semibold, size::Body);
        dl->AddText(ImVec2(p.x + 12, p.y + 9), col::Text, r.name.c_str());
        ImGui::PopFont();
        const auto sample = app.resinSample(r);
        const std::string sub = resinSummary(r, sample.get()) + (r.id == for_new ? " · new jobs" : "");
        ImGui::PushFont(fonts().regular, size::Help);
        dl->AddText(ImVec2(p.x + 12, p.y + 29), col::TextMuted, sub.c_str());
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 2));
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 8));
    if (secondaryButton("New from a SatelLite .goo…", ImVec2(w, 34))) app.newResinFromGoo();
    if (secondaryButton("Import a profile (.json)…", ImVec2(w, 34))) app.importResin();
    tooltip("A profile written by `fruitslicer fit` from scored calibration plates.");
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void deleteDialog(App& app) {
    std::string& id = app.resinToDelete();
    const Resin* r = app.resinById(id);
    if (!r) id.clear();
    if (r && !ImGui::IsPopupOpen("delete_resin")) ImGui::OpenPopup("delete_resin");
    if (!ImGui::IsPopupOpen("delete_resin")) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + vp->Size.y / 2), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460, 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, col::Panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22, 20));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14);
    if (ImGui::BeginPopupModal("delete_resin", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                                            ImGuiWindowFlags_NoMove)) {
        if (!r) {  // answered elsewhere (a script)
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
            return;
        }
        text(("Delete " + r->name + "?").c_str(), fonts().bold, size::DialogTitle, col::Text);
        ImGui::Dummy(ImVec2(0, 4));
        text("Jobs printed in it keep their own copy of its values. Its file is removed; this can't be undone.",
             fonts().regular, size::Body, col::TextSecondary, true);
        ImGui::Dummy(ImVec2(0, 12));
        const float w = ImGui::GetContentRegionAvail().x;
        const float x0 = ImGui::GetCursorPosX();
        ImGui::SetCursorPosX(x0 + w - 100 - 10 - 90);
        if (secondaryButton("Cancel", ImVec2(90, 36)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            id.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(0, 10);
        if (primaryButton("Delete", ImVec2(100, 36))) {
            app.deleteResin(r->id);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

}  // namespace

void resins(App& app) {
    list(app);
    ImGui::SameLine(0, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28, 24));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::App);
    ImGui::BeginChild("resin_page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 8));
    const Resin* found = app.resinById(app.resinSelected());
    const Resin r = found ? *found : app.newJobResin();
    if (!found) app.resinSelected() = r.id;
    const float w = std::min(ImGui::GetContentRegionAvail().x, 1000.0f);
    const bool for_new = app.newJobResin().id == r.id;

    // ---- Name, state and what to do ----
    static std::string renaming;  // the id being renamed
    static char name_buf[96];
    {
        const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
        if (renaming == r.id) {
            ImGui::SetNextItemWidth(320);
            ImGui::PushFont(fonts().semibold, size::Section);
            ImGui::SetKeyboardFocusHere();
            const bool done = ImGui::InputText("##name", name_buf, sizeof name_buf, ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::PopFont();
            if (done) {
                app.renameResin(r.id, name_buf);
                renaming.clear();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) renaming.clear();
        } else {
            text(r.name.c_str(), fonts().bold, size::PageTitle - 6, col::Text);
            ImGui::SameLine(0, 12);
            ImGui::SetCursorPosY(y0 + 8);
            if (linkButton("Rename")) {
                renaming = r.id;
                std::snprintf(name_buf, sizeof name_buf, "%s", r.name.c_str());
            }
        }
        ImGui::SetCursorPos(ImVec2(x0, y0 + 36));
        const std::string state = resinState(r) == "Defaults" ? "Defaults · not calibrated" : resinState(r);
        text((state + (for_new ? " · used for new jobs" : "")).c_str(), fonts().regular, size::Body, col::TextMuted);
        const float after = ImGui::GetCursorPosY();
        // Right: Calibrate, Use for new jobs, Duplicate, Delete…
        float right = x0 + w;
        const float dw = textWidth("Delete…", fonts().semibold, size::Body) + 28;
        right -= dw;
        ImGui::SetCursorPos(ImVec2(right, y0 + 2));
        if (secondaryButton("Delete…", ImVec2(dw, 34), app.resins().size() > 1)) app.resinToDelete() = r.id;
        if (app.resins().size() <= 1) tooltip("One resin always stays.");
        const float pw = textWidth("Duplicate", fonts().semibold, size::Body) + 28;
        right -= pw + 8;
        ImGui::SetCursorPos(ImVec2(right, y0 + 2));
        if (secondaryButton("Duplicate", ImVec2(pw, 34))) app.duplicateResin(r.id);
        if (!for_new) {
            const float uw = textWidth("Use for new jobs", fonts().semibold, size::Body) + 28;
            right -= uw + 8;
            ImGui::SetCursorPos(ImVec2(right, y0 + 2));
            if (secondaryButton("Use for new jobs", ImVec2(uw, 34))) app.setNewJobResin(r.id);
        }
        const float cw = textWidth("Calibrate", fonts().semibold, size::Body);
        right -= cw + 18;
        ImGui::SetCursorPos(ImVec2(right, y0 + 10));
        if (linkButton("Calibrate")) {
            app.setNewJobResin(r.id);  // the plates calibrate the resin new jobs use
            app.setScreen(Screen::Calibrate);
        }
        tooltip("Calibration measures the resin new jobs use; this makes it that one.");
        ImGui::SetCursorPos(ImVec2(x0, after + 12));
        ImGui::Dummy(ImVec2(0, 0));
    }

    const float gap = 16, cw = std::floor((w - gap) / 2);
    // ---- Print settings ----
    const auto sample = app.resinSample(r);
    beginCard("print", ImVec2(cw, 0), 16, col::Panel, col::Border);
    {
        const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
        const float iw = ImGui::GetContentRegionAvail().x;
        text("Print settings", fonts().semibold, size::Section, col::Text);
        const char* label = r.sample_path.empty() ? "Choose a SatelLite .goo…" : "Replace from another .goo…";
        const float bw = textWidth(label, fonts().semibold, size::Body) + 28;
        ImGui::SetCursorPos(ImVec2(x0 + iw - bw, y0 - 4));
        if (secondaryButton(label, ImVec2(bw, 30))) app.replaceResinSample(r.id);
        ImGui::SetCursorPos(ImVec2(x0, y0 + 30));
        std::string from = printSummary(r);
        if (sample) from += ", " + sample->software_name + " " + sample->software_version;
        if (r.sample_path.empty() && r.exposure <= 0)
            from = "None yet. Each job asks for a SatelLite file or exposure times on its Print step.";
        text(from.c_str(), fonts().regular, size::Body, col::TextMuted, true);
        ImGui::Dummy(ImVec2(0, 2));
        const auto rows = printRows(r, sample.get(), app.printer());
        for (std::size_t i = 0; i < rows.size(); ++i) line(rows[i].first, rows[i].second, nullptr, i == 0);
    }
    endCard();
    ImGui::SameLine(0, gap);
    // ---- Support values ----
    beginCard("support", ImVec2(cw, 0), 16, col::Panel, col::Border);
    {
        text("Support values for this resin", fonts().semibold, size::Section, col::Text);
        text(resinState(r) == "Defaults" ? "FruitSlicer's starting values until the resin test plates are scored."
                                         : "Measured on the resin test plates; the rest are FruitSlicer's own.",
             fonts().regular, size::Body, col::TextMuted, true);
        ImGui::Dummy(ImVec2(0, 2));
        const auto values = resinSupportValues(r);
        for (std::size_t i = 0; i < values.size(); ++i) line(values[i].label, values[i].value, &values[i].source, i == 0);
        // The job's advanced page shows these, when the open job is in this resin.
        const auto& doc = app.document();
        if (doc && doc->supports.choices.resin == r.name && doc->supports.choices.profile == r.profile) {
            ImGui::Dummy(ImVec2(0, 4));
            if (linkButton("All support settings")) {
                app.setScreen(Screen::Job);
                app.goToStep(Step::Supports);
                app.showAdvanced(true);
            }
        }
    }
    endCard();
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    deleteDialog(app);
}

}  // namespace gui::screens
