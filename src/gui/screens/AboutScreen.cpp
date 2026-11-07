// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// About (canvas board "About"): version and licence, what has been checked
// and what not yet, the licences of everything built in, and diagnostics to
// paste into a problem report.
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "App.h"
#include "Icons.h"
#include "Licences.h"
#include "Log.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/Screens.h"

namespace gui::screens {

namespace {

float textWidth(const char* s, ImFont* f, float sz) {
    ImGui::PushFont(f, sz);
    const float w = ImGui::CalcTextSize(s).x;
    ImGui::PopFont();
    return w;
}

// A list item: a mark, then words wrapped beside it.
void item(bool ok, const std::string& s) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ok) {
        icon::check(dl, ImVec2(p.x, p.y + 2), 14, col::Ok);
    } else {
        ImGui::PushFont(fonts().bold, size::Body);
        dl->AddText(ImVec2(p.x + 5, p.y), col::Warn, "!");
        ImGui::PopFont();
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + 24, p.y));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    ImGui::PushFont(fonts().regular, size::Body);
    ImGui::TextWrapped("%s", s.c_str());
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 2));
}

void licenceText(App& app) {
    std::string& open = app.licenceOpen();
    const Licence* lic = nullptr;
    for (const Licence& l : licences())
        if (open == l.name) lic = &l;
    if (!lic) open.clear();
    if (lic && !ImGui::IsPopupOpen("licence")) ImGui::OpenPopup("licence");
    if (!ImGui::IsPopupOpen("licence")) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 size(std::min(760.0f, vp->Size.x - 80), vp->Size.y - 120);
    ImGui::SetNextWindowSize(size);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + vp->Size.y / 2), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, col::Panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22, 20));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14);
    if (ImGui::BeginPopupModal("licence", nullptr,
                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        if (!lic) {  // closed from elsewhere (a script)
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
            return;
        }
        const float w = ImGui::GetContentRegionAvail().x;
        text(lic->name, fonts().bold, size::DialogTitle, col::Text);
        text((std::string(lic->licence) + " · " + lic->what).c_str(), fonts().regular, size::Body, col::TextMuted);
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + w - 28, ImGui::GetCursorStartPos().y));
        if (closeButton("close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            open.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x, ImGui::GetCursorStartPos().y + 56));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, col::App);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
        ImGui::BeginChild("text", ImVec2(w, ImGui::GetContentRegionAvail().y),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushFont(fonts().regular, size::Help);
        ImGui::PushStyleColor(ImGuiCol_Text, col::TextSecondary);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(lic->text.data(), lic->text.data() + lic->text.size());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

}  // namespace

void about(App& app) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28, 24));
    ImGui::BeginChild("about", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 8));
    const float w = std::min(ImGui::GetContentRegionAvail().x, 1100.0f);
    {
        const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 lp = ImGui::GetCursorScreenPos();
        icon::logo(dl, ImVec2(lp.x, lp.y - 12), 44);  // the mark fills the logo's lower half
        ImGui::SetCursorPos(ImVec2(x0 + 58, y0));
        ImGui::BeginGroup();
        text("FruitSlicer " FRUIT_VERSION, fonts().bold, size::PageTitle - 4, col::Text);
        text("Free software under the GNU AGPL, version 3 or later ·", fonts().regular, size::Body,
             col::TextSecondary);
        ImGui::SameLine(0, 6);
        if (linkButton("Source code")) app.openSourcePage();
        ImGui::EndGroup();
        const float after = ImGui::GetCursorPosY();
        const float lw = textWidth("‹ Settings", fonts().semibold, size::Body);
        ImGui::SetCursorPos(ImVec2(x0 + w - lw, y0 + 8));
        if (linkButton("‹ Settings")) app.setScreen(Screen::Settings);
        ImGui::SetCursorPos(ImVec2(x0, after + 10));
        ImGui::Dummy(ImVec2(0, 0));
    }

    // What has been checked, and what not yet (on this printer, in your resin).
    const PrinterSetup& ps = app.printerSetup();
    std::vector<std::string> checked = {
        "Print files follow the layout, header and settings of a file SatelLite 1.0.2.43 saved for this printer, "
        "its exposure ramp included",
        "Every print file is read back after saving, with each layer's checksum checked",
        "Layer images use the printer's own pixels, 14 × 19 µm",
    };
    std::vector<std::string> unchecked;
    if (ps.orientation_confirmed)
        checked.push_back("Which way your printer flips the layer image, checked with " + ps.checked_with);
    else
        unchecked.push_back("Which way your printer flips the layer image: the check under Calibrate");
    for (const char* s :
         {"How much a tip holds, how far it reaches and the steepest face that prints, in your resin: starting "
          "values borrowed from PrusaSlicer until the resin test plates are printed",
          "How much extra hold the side away from the vat's hinge needs",
          "Whether tiny specks are carried by the layer above them, as assumed",
          "Grey edge pixels (anti-aliasing) on this printer: files are pure black and white, as SatelLite writes "
          "them",
          "Print time beyond one file: the 2.4 s of tilting per layer comes from one SatelLite file, where the "
          "estimate is within 1 % of SatelLite's",
          "Sending over Wi-Fi and ejecting USB drives with a real printer: tried against a stand-in printer only"})
        unchecked.push_back(s);
    const float gap = 16, cw = std::floor((w - gap) / 2);
    beginCard("checked", ImVec2(cw, 0), 16, col::Panel, col::Border);
    text("Checked", fonts().semibold, size::Section, col::Text);
    ImGui::Dummy(ImVec2(0, 2));
    for (const auto& s : checked) item(true, s);
    endCard();
    ImGui::SameLine(0, gap);
    beginCard("unchecked", ImVec2(cw, 0), 16, col::Panel, col::Border);
    text("Not checked yet", fonts().semibold, size::Section, col::Text);
    ImGui::Dummy(ImVec2(0, 2));
    for (const auto& s : unchecked) item(false, s);
    endCard();
    ImGui::Dummy(ImVec2(0, gap - 8));

    beginCard("licences", ImVec2(w, 0), 16, col::Panel, col::Border);
    text("Built with", fonts().semibold, size::Section, col::Text);
    ImGui::Dummy(ImVec2(0, 2));
    {
        const float iw = ImGui::GetContentRegionAvail().x;
        const float rw = textWidth("Read", fonts().semibold, size::Body);
        int i = 0;
        for (const Licence& l : licences()) {
            ImGui::PushID(i++);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + iw, p.y), col::Border);
            ImGui::Dummy(ImVec2(iw, 6));
            const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
            text(l.name, fonts().semibold, size::Body, col::Text);
            ImGui::SameLine(0, 6);
            text((std::string("· ") + l.what).c_str(), fonts().regular, size::Body, col::TextMuted);
            const float lw = textWidth(l.licence, fonts().regular, size::Body);
            ImGui::SetCursorPos(ImVec2(x0 + iw - rw - 24 - lw, y0));
            text(l.licence, fonts().regular, size::Body, col::TextSecondary);
            ImGui::SetCursorPos(ImVec2(x0 + iw - rw, y0));
            if (linkButton("Read")) app.licenceOpen() = l.name;
            ImGui::SetCursorPos(ImVec2(x0, y0 + 24));
            ImGui::Dummy(ImVec2(0, 0));
            ImGui::PopID();
        }
    }
    endCard();
    ImGui::Dummy(ImVec2(0, gap - 8));

    beginCard("diag", ImVec2(w, 0), 16, col::Panel, col::Border);
    text("Diagnostics", fonts().semibold, size::Section, col::Text);
    ImGui::Dummy(ImVec2(0, 2));
    const ImGuiIO& io = ImGui::GetIO();
    char scale[64];
    std::snprintf(scale, sizeof scale, "%.0f × %.0f points at %.1fx", io.DisplaySize.x, io.DisplaySize.y,
                  io.DisplayFramebufferScale.x);
    const bool det = app.options().deterministic;
    keyValue("Graphics", det ? std::string("(hidden in test captures)") : app.glInfo());
    keyValue("Display", scale);
    keyValue("Dear ImGui", ImGui::GetVersion());
    keyValue("Log file", det ? std::string("(hidden in test captures)") : log::path());
    endCard();
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    licenceText(app);
}

}  // namespace gui::screens
