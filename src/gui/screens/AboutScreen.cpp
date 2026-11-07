// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// About: version, licences, and diagnostics to paste into a bug report.
#include <imgui.h>

#include <cstdio>
#include <string>

#include "App.h"
#include "Icons.h"
#include "Log.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/Screens.h"

namespace gui::screens {

void about(App& app) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(32, 28));
    ImGui::BeginChild("about", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        icon::logo(dl, ImVec2(p.x, p.y + 2), 44);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 58);
        ImGui::BeginGroup();
        text("FruitSlicer " FRUIT_VERSION, fonts().bold, size::PageTitle, col::Text);
        text("Free software under the GNU AGPL, version 3 or later.", fonts().regular, size::Body, col::TextSecondary);
        ImGui::EndGroup();
    }
    ImGui::Dummy(ImVec2(0, 14));
    const float w = std::min(ImGui::GetContentRegionAvail().x, 900.0f);

    beginCard("preview", ImVec2(w, 0), 18);
    sectionTitle("This is an early build");
    ImGui::Dummy(ImVec2(0, 2));
    text("It takes a model from file to print on the Saturn 4 Ultra 16K: it opens STL, OBJ and 3MF files, checks "
         "them, finds a good way to stand them, generates supports, cuts the layers, and saves a .goo it reads back "
         "to check, to a USB drive or over Wi-Fi. Under Calibrate, the image orientation check and the printer's "
         "hinge side. Resin calibration, saved jobs, editing tips by hand and hollowing arrive in later updates.",
         fonts().regular, size::Body, col::TextSecondary, true);
    endCard();
    ImGui::Dummy(ImVec2(0, 14));

    beginCard("diag", ImVec2(w, 0), 18);
    sectionTitle("Diagnostics");
    ImGui::Dummy(ImVec2(0, 4));
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
    ImGui::Dummy(ImVec2(0, 14));

    beginCard("licences", ImVec2(w, 0), 18);
    sectionTitle("Built with");
    ImGui::Dummy(ImVec2(0, 4));
    const std::pair<const char*, const char*> libs[] = {
        {"Clipper2 · polygon clipping", "Boost Software License 1.0"},
        {"nlohmann/json · JSON files", "MIT"},
        {"cpp-httplib · sending over Wi-Fi", "MIT"},
        {"SDL 3 · window and input", "zlib"},
        {"Dear ImGui · interface", "MIT"},
        {"glad · OpenGL loader", "CC0-1.0 / WTFPL, Apache-2.0"},
        {"miniz · compression, 3MF packages", "MIT"},
        {"pugixml · 3MF models", "MIT"},
        {"Inter · typeface", "SIL Open Font License 1.1"},
    };
    for (const auto& [name, lic] : libs) keyValue(name, lic, col::TextSecondary);
    endCard();
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

}  // namespace gui::screens
