// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Tools (canvas boards "GooInspect" and "GooCompare"): a print file's header
// in plain words with a check of every layer, and two files compared,
// header and layer images.
#include <imgui.h>

#include <algorithm>
#include <filesystem>
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

float textWidth(const char* s, ImFont* f, float sz) {
    ImGui::PushFont(f, sz);
    const float w = ImGui::CalcTextSize(s).x;
    ImGui::PopFont();
    return w;
}

void beginPage(const char* id) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28, 24));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::App);
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 8));
}

void endPage() {
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

std::string fileName(const std::string& p) { return std::filesystem::path(p).filename().string(); }

// A stat: a label over a big value.
void stat(const char* id, const char* label, const std::string& value, float w) {
    beginCard(id, ImVec2(w, 76), 14, col::Panel, col::Border);
    text(label, fonts().regular, size::Help, col::TextMuted);
    text(value.c_str(), fonts().bold, size::Section + 3, col::Text);
    endCard();
}

// A ✓ or ✗ and a line.
void checkLine(bool ok, const std::string& s) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (ok) icon::check(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + 1), 16, col::Ok);
    else icon::cross(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + 1), 16, col::Error);
    ImGui::Dummy(ImVec2(18, 18));
    ImGui::SameLine(0, 8);
    text(s.c_str(), fonts().regular, size::Body, ok ? col::TextSecondary : col::Error, true);
}

void inspect(App& app) {
    const App::GooInspected* g = app.gooInspected();
    const float w = ImGui::GetContentRegionAvail().x;
    if (!g) {
        beginCard("none", ImVec2(std::min(w, 640.0f), 0), 18, col::Panel, col::Border);
        text("Inspect a print file", fonts().semibold, size::Section, col::Text);
        text("Open a .goo to see its settings in plain words, read every layer back, and see the pictures the "
             "printer shows.",
             fonts().regular, size::Body, col::TextSecondary, true);
        ImGui::Dummy(ImVec2(0, 4));
        if (primaryButton("Open a .goo…", ImVec2(0, 36))) app.inspectGoo();
        endCard();
        return;
    }
    const GooSummary& s = g->summary;
    {
        const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
        text(fileName(g->path).c_str(), fonts().bold, size::Section + 4, col::Text);
        text(s.made_by.c_str(), fonts().regular, size::Body, col::TextMuted);
        const float after = ImGui::GetCursorPosY();
        const float bw = textWidth("Open another .goo…", fonts().semibold, size::Body) + 28;
        ImGui::SetCursorPos(ImVec2(x0 + w - bw, y0));
        if (secondaryButton("Open another .goo…", ImVec2(bw, 36))) app.inspectGoo();
        ImGui::SetCursorPos(ImVec2(x0, after + 6));
    }
    const float side = 380, main_w = w - side - 16;
    ImGui::BeginGroup();
    {
        const float sw = (main_w - 3 * 12) / 4;
        stat("layers", "Layers", thousands(s.layers), sw);
        ImGui::SameLine(0, 12);
        stat("height", "Height", strf("%.1f mm", s.height_mm), sw);
        ImGui::SameLine(0, 12);
        stat("time", "Print time", s.print_time_s ? durationText(s.print_time_s) : std::string("—"), sw);
        ImGui::SameLine(0, 12);
        stat("resin", "Resin", s.resin, sw);
    }
    ImGui::Dummy(ImVec2(0, 4));
    {
        const float gw = (main_w - 2 * 12) / 3;
        for (std::size_t i = 0; i < s.groups.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            beginCard("group", ImVec2(gw, 176), 16, col::Panel, col::Border);
            text(s.groups[i].title.c_str(), fonts().semibold, size::Body + 1, col::Text);
            ImGui::Dummy(ImVec2(0, 2));
            for (const GooRow& r : s.groups[i].rows) keyValue(r.label.c_str(), r.value);
            endCard();
            ImGui::PopID();
            if (i % 3 != 2 && i + 1 < s.groups.size()) ImGui::SameLine(0, 12);
            else ImGui::Dummy(ImVec2(0, 4));
        }
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, 16);
    ImGui::BeginGroup();
    beginCard("check", ImVec2(side, 0), 18, col::Panel, col::Border);
    {
        const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
        text("Check every layer", fonts().semibold, size::Section, col::Text);
        const char* label = g->check ? "Run again" : "Check";
        const float bw = textWidth(label, fonts().semibold, size::Body) + 28;
        ImGui::SetCursorPos(ImVec2(x0 + ImGui::GetContentRegionAvail().x - bw, y0 - 4));
        const bool busy = app.jobs().busy();
        if (secondaryButton(label, ImVec2(bw, 30), !busy)) app.checkGoo();
        if (busy) tooltip("Wait for the current task to finish.");
        ImGui::SetCursorPos(ImVec2(x0, y0 + 30));
        if (!g->check) {
            text("Reads each layer back as the printer would: checksums, pixel values, the volume they add up to, and "
                 "the end of the file.",
                 fonts().regular, size::Body, col::TextSecondary, true);
        } else {
            for (const GooCheckLine& l : checkLines(*g->check, g->header)) checkLine(l.ok, l.text);
        }
    }
    endCard();
    ImGui::Dummy(ImVec2(0, 4));
    beginCard("pictures", ImVec2(side, 0), 18, col::Panel, col::Border);
    text("Pictures stored in the file", fonts().semibold, size::Section, col::Text);
    ImGui::Dummy(ImVec2(0, 2));
    if (!g->small_tex && !g->big_tex) {
        text("None stored: the printer shows a blank picture.", fonts().regular, size::Body, col::TextSecondary, true);
    } else {
        const float y0 = ImGui::GetCursorPosY(), x0 = ImGui::GetCursorPosX();
        if (g->small_tex) {
            ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(g->small_tex)), ImVec2(116, 116));
            text("116 × 116", fonts().regular, size::Help, col::TextMuted);
        }
        if (g->big_tex) {
            ImGui::SetCursorPos(ImVec2(x0 + 136, y0));
            ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(g->big_tex)), ImVec2(190, 190));
            ImGui::SetCursorPosX(x0 + 136);
            text("290 × 290, shown smaller", fonts().regular, size::Help, col::TextMuted);
        }
        text("Shown on the printer's screen when you pick the file.", fonts().regular, size::Help, col::TextMuted,
             true);
    }
    endCard();
    ImGui::EndGroup();
}

void fileCard(App& app, int side, float w) {
    ImGui::PushID(side);
    beginCard("file", ImVec2(w, 92), 16, col::Panel, col::Border);
    const std::string& path = app.gooToCompare(side);
    const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
    text(side == 0 ? "A" : "B", fonts().bold, size::Section + 2, col::Accent);
    ImGui::SetCursorPos(ImVec2(x0 + 26, y0));
    ImGui::BeginGroup();
    if (path.empty()) {
        text("No file yet", fonts().semibold, size::Body, col::TextMuted);
    } else {
        text(fileName(path).c_str(), fonts().semibold, size::Body, col::Text);
        try {
            static std::string seen[2], made[2];
            if (seen[side] != path) {
                const fruit::GooHeader h = fruit::readGooHeader(path);
                made[side] = h.software_name + " " + h.software_version;
                seen[side] = path;
            }
            text(made[side].c_str(), fonts().regular, size::Help, col::TextMuted);
        } catch (const std::exception&) {
            text("Not a .goo FruitSlicer can read", fonts().regular, size::Help, col::Error);
        }
    }
    ImGui::EndGroup();
    const char* label = path.empty() ? "Choose…" : "Change";
    const float bw = textWidth(label, fonts().semibold, size::Body) + 28;
    ImGui::SetCursorPos(ImVec2(x0 + ImGui::GetContentRegionAvail().x - bw, y0 + 6));
    if (secondaryButton(label, ImVec2(bw, 32), !app.jobs().busy())) app.chooseGooToCompare(side);
    endCard();
    ImGui::PopID();
}

void compare(App& app) {
    const float w = ImGui::GetContentRegionAvail().x;
    fileCard(app, 0, (w - 12) / 2);
    ImGui::SameLine(0, 12);
    fileCard(app, 1, (w - 12) / 2);
    ImGui::Dummy(ImVec2(0, 4));
    const auto c = app.gooComparison();
    if (!c) {
        text(app.gooToCompare(0).empty() || app.gooToCompare(1).empty()
                 ? "Choose two print files: their headers are compared setting by setting, and a layer from each is "
                   "overlaid to see whether one is the other flipped."
                 : "Comparing…",
             fonts().regular, size::Body, col::TextSecondary, true);
        return;
    }
    const std::vector<GooDiffRow> rows = diffRows(c->header);
    const int cures = static_cast<int>(std::count_if(rows.begin(), rows.end(), [](const GooDiffRow& r) { return r.cures; }));
    const float half = (w - 16) / 2;
    beginCard("header", ImVec2(half, 0), 18, col::Panel, col::Border);
    {
        const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
        text("Settings in the header", fonts().semibold, size::Section, col::Text);
        const std::string n = rows.empty() ? "no differences"
                                           : strf("%zu difference%s", rows.size(), rows.size() == 1 ? "" : "s");
        ImGui::SetCursorPos(ImVec2(x0 + ImGui::GetContentRegionAvail().x - textWidth(n.c_str(), fonts().semibold, size::Body),
                                   y0 + 1));
        text(n.c_str(), fonts().semibold, size::Body, rows.empty() ? col::Ok : col::Text);
        ImGui::SetCursorPos(ImVec2(x0, y0 + 26));
        text("About 45 settings compared: printer, image, exposures, waits, movement and light power. Dates, pictures "
             "and totals are skipped.",
             fonts().regular, size::Help, col::TextMuted, true);
        ImGui::Dummy(ImVec2(0, 2));
        if (!rows.empty()) {
            const float tw = ImGui::GetContentRegionAvail().x, c1 = tw * 0.42f, c2 = tw * 0.71f;
            auto row = [&](const std::string& a, const std::string& b, const std::string& cc, bool head, bool dim) {
                const float y = ImGui::GetCursorPosY();
                ImFont* f = head ? fonts().semibold : fonts().regular;
                const ImU32 fg = head ? col::TextMuted : dim ? col::TextMuted : col::TextSecondary;
                ImGui::SetCursorPos(ImVec2(x0, y));
                text(a.c_str(), f, size::Body, head ? col::TextMuted : dim ? col::TextMuted : col::Text);
                ImGui::SetCursorPos(ImVec2(x0 + c1, y));
                text(b.c_str(), f, size::Body, fg);
                ImGui::SetCursorPos(ImVec2(x0 + c2, y));
                text(cc.c_str(), f, size::Body, fg);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 2), ImVec2(p.x + tw, p.y + 2), col::Border);
                ImGui::Dummy(ImVec2(0, 6));
            };
            row("Setting", "A", "B", true, false);
            for (const GooDiffRow& r : rows) row(r.label, r.a, r.b, false, !r.cures);
        }
        ImGui::Dummy(ImVec2(0, 2));
        const std::string verdict =
            cures == 0              ? std::string("Everything that changes how the print cures is the same.")
            : rows.size() == 1      ? std::string("It changes how the print cures.")
            : cures == static_cast<int>(rows.size()) ? strf("All %d change how the print cures.", cures)
                                                      : strf("%d of the %zu change how the print cures.", cures, rows.size());
        text(verdict.c_str(), fonts().semibold, size::Body, cures == 0 ? col::Ok : col::Warn, true);
    }
    endCard();
    ImGui::SameLine(0, 16);
    beginCard("images", ImVec2(half, 0), 18, col::Panel, col::Border);
    {
        text("Layer images", fonts().semibold, size::Section, col::Text);
        text((c->layer_a == c->layer_b ? strf("Layer %u in both, at %.2f mm", c->layer_a + 1, c->z_a)
                                       : strf("Layer %u of A at %.2f mm, layer %u of B at %.2f mm", c->layer_a + 1,
                                              c->z_a, c->layer_b + 1, c->z_b))
                 .c_str(),
             fonts().regular, size::Help, col::TextMuted);
        ImGui::Dummy(ImVec2(0, 4));
        const ImageVerdict v = imageVerdict(*c);
        text(v.headline.c_str(), fonts().semibold, size::Body + 1, v.clear ? col::Text : col::Warn, true);
        text(v.detail.c_str(), fonts().regular, size::Body, col::TextSecondary, true);
        ImGui::Dummy(ImVec2(0, 4));
        const float x0 = ImGui::GetCursorPosX(), tw = ImGui::GetContentRegionAvail().x;
        for (const auto& sc : c->scores) {
            const float y = ImGui::GetCursorPosY();
            const bool best = sc.transform == c->best && v.clear;
            text(transformText(sc.transform).c_str(), best ? fonts().semibold : fonts().regular, size::Body,
                 best ? col::Text : col::TextSecondary);
            const std::string pct = strf("%.1f %%", 100 * sc.iou);
            ImGui::SetCursorPos(ImVec2(x0 + tw - textWidth(pct.c_str(), fonts().semibold, size::Body), y));
            text(pct.c_str(), fonts().semibold, size::Body, best ? col::Ok : col::TextSecondary);
            ImGui::SetCursorPosX(x0);
        }
        ImGui::Dummy(ImVec2(0, 2));
        text(strf("Lit area: %s cells in A against %s in B, at 0.12 mm per cell.", thousands(c->cells_a).c_str(),
                  thousands(c->cells_b).c_str())
                 .c_str(),
             fonts().regular, size::Help, col::TextMuted, true);
        ImGui::Dummy(ImVec2(0, 2));
        if (linkButton("Run the orientation check instead")) app.setScreen(Screen::OrientationCheck);
    }
    endCard();
}

}  // namespace

void tools(App& app) {
    beginPage("tools");
    text("Tools", fonts().bold, size::PageTitle, col::Text);
    ImGui::Dummy(ImVec2(0, 2));
    int& tab = app.toolsTab();
    if (segmentButton("Inspect a .goo", tab == 0)) tab = 0;
    ImGui::SameLine(0, 4);
    if (segmentButton("Compare two files", tab == 1)) tab = 1;
    ImGui::Dummy(ImVec2(0, 8));
    if (tab == 0) inspect(app);
    else compare(app);
    endPage();
}

}  // namespace gui::screens
