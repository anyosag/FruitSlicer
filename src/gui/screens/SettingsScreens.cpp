// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Settings and first-run setup (canvas boards "Settings" and "Setup").
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "App.h"
#include "Format.h"
#include "Icons.h"
#include "Log.h"
#include "Setup.h"
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

// Segmented choice: the labels side by side; returns the index clicked, or -1.
float segmentsWidth(const std::vector<std::string>& labels) {
    float w = 0;
    for (const auto& l : labels) w += textWidth(l.c_str(), fonts().semibold, size::Body) + 22;
    return w + 4 * static_cast<float>(labels.size() - 1);
}

int segments(const char* id, const std::vector<std::string>& labels, int active) {
    ImGui::PushID(id);
    int clicked = -1;
    for (std::size_t i = 0; i < labels.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (segmentButton(labels[i].c_str(), static_cast<int>(i) == active)) clicked = static_cast<int>(i);
        ImGui::PopID();
        if (i + 1 < labels.size()) ImGui::SameLine(0, 4);
    }
    ImGui::PopID();
    return clicked;
}

// A fixed value in a field's box ("Never sent").
void valuePill(const char* s) {
    ImGui::PushFont(fonts().medium, size::Body);
    const ImVec2 ts = ImGui::CalcTextSize(s);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + ts.x + 20, p.y + 32), col::Input, 6);
    dl->AddRect(p, ImVec2(p.x + ts.x + 20, p.y + 32), col::BorderStrong, 6);
    dl->AddText(ImVec2(p.x + 10, p.y + 16 - ts.y / 2), col::Text, s);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(ts.x + 20, 32));
}

float pillWidth(const char* s) { return textWidth(s, fonts().medium, size::Body) + 20; }

// One row of a settings card: label and help on the left, the control (of
// width `control_w`) on the right; a line above all but the first.
void settingRow(const char* label, const std::string& help, float control_w, const std::function<void()>& control,
                bool first) {
    const float w = ImGui::GetContentRegionAvail().x;
    if (!first) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), col::Border);
        ImGui::Dummy(ImVec2(w, 8));
    }
    const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
    ImGui::PushTextWrapPos(x0 + w - control_w - 18);
    text(label, fonts().semibold, size::Body, col::Text);
    ImGui::PushFont(fonts().regular, size::Body);
    ImGui::PushStyleColor(ImGuiCol_Text, col::TextMuted);
    ImGui::TextWrapped("%s", help.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    const float text_bottom = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(x0 + w - control_w, y0));
    ImGui::BeginGroup();
    control();
    ImGui::EndGroup();
    const float control_bottom = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(x0, std::max(text_bottom, control_bottom) + 8));
    ImGui::Dummy(ImVec2(0, 0));
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

}  // namespace

// ---- Settings ----

void settings(App& app) {
    beginPage("settings");
    const float w = std::min(ImGui::GetContentRegionAvail().x, 1100.0f);
    {
        const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
        text("Settings", fonts().bold, size::PageTitle - 4, col::Text);
        const float after = ImGui::GetCursorPosY();
        const float l2 = textWidth("Keyboard shortcuts", fonts().semibold, size::Body);
        const float l1 = textWidth("About FruitSlicer", fonts().semibold, size::Body);
        ImGui::SetCursorPos(ImVec2(x0 + w - l2, y0 + 8));
        if (linkButton("Keyboard shortcuts")) app.showShortcuts(true);
        ImGui::SetCursorPos(ImVec2(x0 + w - l2 - 22 - l1, y0 + 8));
        if (linkButton("About FruitSlicer")) app.setScreen(Screen::About);
        ImGui::SetCursorPos(ImVec2(x0, after + 8));
        ImGui::Dummy(ImVec2(0, 0));
    }
    const float gap = 16, cw = std::floor((w - gap) / 2);
    const Preferences& p = app.preferences();

    beginCard("look", ImVec2(cw, 0), 16, col::Panel, col::Border);
    text("Look and speed", fonts().semibold, size::Section, col::Text);
    ImGui::Dummy(ImVec2(0, 2));
    {
        const std::vector<std::string> l = {"Smooth", "Faster"};
        settingRow("3D view", "Smooth edges cost a little speed; Faster helps on older graphics.", segmentsWidth(l),
                   [&] {
                       const int c = segments("view", l, p.smooth_view ? 0 : 1);
                       if (c >= 0) {
                           Preferences q = p;
                           q.smooth_view = c == 0;
                           app.setPreferences(q);
                       }
                   },
                   true);
    }
    {
        // On a scripted run the count is fixed, so pictures match on any computer.
        const unsigned all = app.options().deterministic ? 10 : std::max(1u, std::thread::hardware_concurrency());
        const std::vector<unsigned> counts = coreChoices(all);
        std::vector<std::string> l;
        int active = 0;
        for (std::size_t i = 0; i < counts.size(); ++i) {
            l.push_back(i == 0 ? strf("All %u", counts[i]) : std::to_string(counts[i]));
            if (p.cores != 0 && counts[i] == p.cores) active = static_cast<int>(i);
        }
        settingRow("Processor cores",
                   "For slicing, planning tips and saving. Fewer keeps the computer quicker to answer while it works.",
                   segmentsWidth(l),
                   [&] {
                       const int c = segments("cores", l, active);
                       if (c >= 0) {
                           Preferences q = p;
                           q.cores = c == 0 ? 0 : counts[static_cast<std::size_t>(c)];
                           app.setPreferences(q);
                       }
                   },
                   false);
    }
    endCard();
    ImGui::SameLine(0, gap);
    beginCard("files", ImVec2(cw, 0), 16, col::Panel, col::Border);
    text("Files", fonts().semibold, size::Section, col::Text);
    ImGui::Dummy(ImVec2(0, 2));
    {
        const std::vector<std::string> l = {"USB drive", "Ask each time"};
        settingRow("Save .goo files to",
                   "A USB drive when one is plugged in, or a folder you choose each time. You can still pick in Layers, "
                   "beside Save.",
                   segmentsWidth(l),
                   [&] {
                       const int c = segments("goo", l, p.goo_to_drive ? 0 : 1);
                       if (c >= 0) {
                           Preferences q = p;
                           q.goo_to_drive = c == 0;
                           app.setPreferences(q);
                       }
                   },
                   true);
    }
    {
        const float bw = textWidth("Change", fonts().semibold, size::Body) + 28;
        const Resin& r = app.newJobResin();
        const std::string values = resinState(r) == "Defaults" ? "FruitSlicer's support values" : resinState(r);
        settingRow("Resin for new jobs", r.name + " · " + printSummary(r) + " · " + values, bw,
                   [&] {
                       if (secondaryButton("Change", ImVec2(bw, 32))) {
                           app.resinSelected() = r.id;
                           app.setScreen(Screen::Resins);
                       }
                   },
                   false);
    }
    endCard();

    ImGui::Dummy(ImVec2(0, gap - 8));
    beginCard("privacy", ImVec2(cw, 0), 16, col::Panel, col::Border);
    text("Help and privacy", fonts().semibold, size::Section, col::Text);
    ImGui::Dummy(ImVec2(0, 2));
    settingRow("Usage data",
               "FruitSlicer sends nothing about your models or prints. It uses the network only to find your printer and "
               "send files to it, when you ask.",
               pillWidth("Never sent"), [] { valuePill("Never sent"); }, true);
    {
        const char* label = "Show in Finder";
        const float bw = textWidth(label, fonts().semibold, size::Body) + 28;
        const bool det = app.options().deterministic;
        settingRow("Log file",
                   "What FruitSlicer did, to attach to a problem report if you choose to. " +
                       (det ? std::string("(Its place is hidden in test captures.)") : log::path()),
                   bw,
                   [&] {
                       if (secondaryButton(label, ImVec2(bw, 32), !det)) app.revealLog();
                   },
                   false);
    }
    endCard();

    ImGui::Dummy(ImVec2(0, gap - 8));
    beginCard("start_over", ImVec2(w, 0), 16, col::Panel, col::Border);
    {
        const char* label = "Run setup again";
        const float bw = textWidth(label, fonts().semibold, size::Body) + 28;
        settingRow("Start over",
                   "Runs first-run setup again. Your jobs, printer checks and these settings are kept.", bw,
                   [&] {
                       if (secondaryButton(label, ImVec2(bw, 34))) app.startSetup();
                   },
                   true);
    }
    endCard();
    endPage();
}

// ---- First-run setup ----

namespace {

constexpr float kAsideW = 340;

struct SetupStep {
    const char* title;
    const char* detail;
};
constexpr SetupStep kSteps[] = {{"Welcome", "Saturn 4 Ultra 16K"},
                                {"Print settings", "From SatelLite or by hand"},
                                {"Image orientation", "5-minute check, no resin"},
                                {"Calibration", "Now or later"}};

// `done`: which steps are really done (a step passed but skipped keeps its number).
void aside(int current, const bool done_steps[4]) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::Rail);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 28));
    ImGui::BeginChild("setup_steps", ImVec2(kAsideW, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    dl->AddLine(ImVec2(wp.x + ws.x - 1, wp.y), ImVec2(wp.x + ws.x - 1, wp.y + ws.y), col::Border);
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        icon::logo(dl, ImVec2(p.x, p.y - 8), 28);  // the mark fills the logo's lower half
        ImGui::SetCursorScreenPos(ImVec2(p.x + 38, p.y + 4));
        text("FruitSlicer", fonts().bold, 16, col::Text);
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 56));
        ImGui::Dummy(ImVec2(0, 0));
    }
    const float w = ImGui::GetContentRegionAvail().x;
    for (int i = 0; i < 4; ++i) {
        const bool now = i == current, done = !now && done_steps[i];
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (now) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 54), col::Card, 10);
        const ImVec2 c(p.x + 10 + 13, p.y + 10 + 13);
        if (done) {
            dl->AddCircleFilled(c, 13, col::OkBg, 28);
            icon::check(dl, ImVec2(c.x - 7, c.y - 7), 14, col::Ok);
        } else {
            if (now) dl->AddCircleFilled(c, 13, col::Accent, 28);
            else dl->AddCircle(c, 12.5f, col::BorderStrong, 28, 1.5f);
            ImGui::PushFont(fonts().bold, size::Help);
            const std::string n = std::to_string(i + 1);
            const ImVec2 ts = ImGui::CalcTextSize(n.c_str());
            dl->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), now ? col::OnAccent : col::TextMuted, n.c_str());
            ImGui::PopFont();
        }
        ImGui::PushFont(fonts().semibold, size::Body);
        dl->AddText(ImVec2(p.x + 48, p.y + 10), now ? col::Text : (done ? col::TextSecondary : col::TextMuted),
                    kSteps[i].title);
        ImGui::PopFont();
        ImGui::PushFont(fonts().regular, size::Help);
        dl->AddText(ImVec2(p.x + 48, p.y + 29), col::TextMuted, kSteps[i].detail);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(w, 54));
        ImGui::Dummy(ImVec2(0, 2));
    }
    const char* note =
        "FruitSlicer supports one printer so far: the ELEGOO Saturn 4 Ultra 16K. You can run setup again from Settings.";
    ImGui::PushFont(fonts().regular, size::Body);
    const float nh = ImGui::CalcTextSize(note, nullptr, false, w).y;
    ImGui::PopFont();
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - 28 - nh));
    text(note, fonts().regular, size::Body, col::TextMuted, true);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void stepHeading(int n, const char* title, const char* body) {
    groupLabel(strf("Step %d of 4", n).c_str());
    text(title, fonts().bold, size::PageTitle, col::Text);
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(ImGui::GetContentRegionAvail().x, 660.0f));
    ImGui::PushFont(fonts().regular, size::Body + 1);
    ImGui::PushStyleColor(ImGuiCol_Text, col::TextSecondary);
    ImGui::TextWrapped("%s", body);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 10));
}

// The buttons along the bottom: Back on the left; on the right a note, a
// link, and the main button. Returns which was clicked: 'b', 'l', 'm' or 0.
char footer(bool back, const char* note, const char* link, const char* main, bool main_enabled,
            const char* main_why_not = nullptr) {
    char out = 0;
    const float w = ImGui::GetContentRegionAvail().x;
    const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
    if (back && secondaryButton("Back", ImVec2(0, 38))) out = 'b';
    float right = x0 + w;
    if (main) {
        const float mw = textWidth(main, fonts().bold, size::Body) + 36;
        right -= mw;
        ImGui::SetCursorPos(ImVec2(right, y0));
        if (primaryButton(main, ImVec2(mw, 38), main_enabled)) out = 'm';
        if (!main_enabled && main_why_not) tooltip(main_why_not);
    }
    if (link) {
        const float lw = textWidth(link, fonts().semibold, size::Body);
        right -= lw + 18;
        ImGui::SetCursorPos(ImVec2(right, y0 + 10));
        if (linkButton(link)) out = 'l';
    }
    if (note) {
        const float nw = textWidth(note, fonts().regular, size::Body);
        right -= nw + 18;
        ImGui::SetCursorPos(ImVec2(right, y0 + 10));
        text(note, fonts().regular, size::Body, col::TextMuted);
    }
    ImGui::SetCursorPos(ImVec2(x0, y0 + 48));
    ImGui::Dummy(ImVec2(0, 0));
    return out;
}

// An option card with a radio mark whose extra content (drawn by `inside`)
// sits under its words.
bool optionCard(const char* id, const char* title, const char* body, bool selected,
                const std::function<void()>& inside) {
    const float w = ImGui::GetContentRegionAvail().x;
    beginCard(id, ImVec2(w, 0), 16, selected ? col::Card : col::Panel, selected ? col::Accent : col::Border);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + 9, p.y + 10);
    dl->AddCircle(c, 8.5f, selected ? col::Accent : col::TextMuted, 24, 1.5f);
    if (selected) dl->AddCircleFilled(c, 4.5f, col::Accent, 24);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 30, p.y));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    text(title, fonts().semibold, size::Body + 1, col::Text);
    ImGui::PushFont(fonts().regular, size::Body);
    ImGui::PushStyleColor(ImGuiCol_Text, col::TextMuted);
    ImGui::TextWrapped("%s", body);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    if (selected && inside) {
        ImGui::Dummy(ImVec2(0, 4));
        inside();
    }
    ImGui::EndGroup();
    // The whole card picks the option (its own controls still work).
    const ImVec2 a = ImGui::GetWindowPos(), b(a.x + ImGui::GetWindowWidth(), a.y + ImGui::GetWindowHeight());
    const bool hovered = ImGui::IsMouseHoveringRect(a, b) && !ImGui::IsAnyItemHovered();
    const bool clicked = !selected && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    if (hovered && !selected) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    endCard();
    return clicked;
}

void welcome(App& app) {
    stepHeading(1, "Welcome to FruitSlicer",
                "FruitSlicer turns a 3D model into a print file for your printer. It checks the model, finds a good "
                "way to stand it, adds supports, cuts the layers and saves a .goo, to a USB drive or over Wi-Fi.");
    const fruit::PrinterProfile& pr = app.printer();
    beginCard("printer", ImVec2(std::min(ImGui::GetContentRegionAvail().x, 660.0f), 0), 18, col::Panel, col::Border);
    text("Your printer", fonts().semibold, size::Help, col::TextMuted);
    text(fruit::GooHeader{}.printer_name.c_str(), fonts().semibold, size::Section + 2, col::Text);
    text(strf("%.2f × %.2f × %.0f mm to print in · %d × %d pixels of %.0f × %.0f µm", pr.build_x, pr.build_y,
              pr.build_z, pr.res_x, pr.res_y, pr.pixelX() * 1000, pr.pixelY() * 1000)
             .c_str(),
         fonts().regular, size::Body, col::TextSecondary, true);
    endCard();
    ImGui::Dummy(ImVec2(0, 6));
    text("Three short steps follow: your print settings, a 5-minute check of the image orientation, and calibration, "
         "now or later.",
         fonts().regular, size::Body, col::TextSecondary, true);
    ImGui::Dummy(ImVec2(0, 14));
    switch (footer(false, nullptr, "Skip setup", "Get started", true)) {
        case 'l': app.finishSetup(Screen::Home); break;
        case 'm': app.setupStep() = 1; break;
        default: break;
    }
}

void printSettings(App& app) {
    stepHeading(2, "Bring in your print settings",
                "FruitSlicer reads the exposure and motion settings from a file SatelLite already made for this "
                "printer, so your prints behave the way they do today. Each new job starts with them.");
    const Resin& resin = app.newJobResin();  // setup sets the print settings of the resin new jobs use
    const auto sample = app.resinSample(resin);
    bool& typed = app.setupTyped();
    if (optionCard("from_file", "From a SatelLite .goo file (recommended)",
                   "Any small file you sliced in SatelLite for this printer with your usual resin settings.", !typed,
                   [&] {
                       if (!sample) {
                           if (secondaryButton("Choose a file…", ImVec2(0, 34))) app.chooseDefaultSample();
                           return;
                       }
                       const float w = ImGui::GetContentRegionAvail().x;
                       beginCard("chosen", ImVec2(w, 0), 10, col::App, col::Border);
                       const float x0 = ImGui::GetCursorPosX(), avail = ImGui::GetContentRegionAvail().x;
                       const ImVec2 p = ImGui::GetCursorScreenPos();
                       icon::check(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + 1), 16, col::Ok);
                       ImGui::SetCursorScreenPos(ImVec2(p.x + 24, p.y));
                       text(resin.sample_name.c_str(), fonts().semibold, size::Body, col::Text);
                       ImGui::SameLine(0, 10);
                       const std::string about = sample->software_name + " " + sample->software_version + " · " +
                                                 sample->printer_name + " · " +
                                                 std::to_string(sample->layer_count) + " layers";
                       text(about.c_str(), fonts().regular, size::Body, col::TextMuted);
                       const float lw = textWidth("Choose another…", fonts().semibold, size::Body);
                       ImGui::SameLine(0, 0);
                       ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 10, x0 + avail - lw));
                       if (linkButton("Choose another…")) app.chooseDefaultSample();
                       endCard();
                   }))
        typed = false;
    ImGui::Dummy(ImVec2(0, 2));
    App::TypedDefaults& t = app.typedDefaults();
    if (optionCard("typed", "Type them in",
                   "The exposures, and the bottom and transition layers, from your resin's settings sheet.", typed,
                   [&] {
                       // Four fields on one row, each placed where it goes (after SameLine
                       // ImGui would lower each label to the previous field's text).
                       const float row_y = ImGui::GetCursorPosY();
                       float x = ImGui::GetCursorPosX();
                       auto field = [&](const char* id, const char* label, double* v, const char* unit, double lo,
                                        double hi, const char* format) {
                           ImGui::SetCursorPos(ImVec2(x, row_y));
                           ImGui::BeginGroup();
                           text(label, fonts().regular, size::Help, col::TextMuted);
                           numberField(id, v, 90, format, lo, hi);
                           const float fh = ImGui::GetItemRectSize().y;
                           ImGui::SameLine(0, 6);
                           // The unit beside the field, centred on it.
                           const ImVec2 up = ImGui::GetCursorScreenPos();
                           ImGui::PushFont(fonts().regular, size::Body);
                           const ImVec2 us = ImGui::CalcTextSize(unit);
                           ImGui::GetWindowDrawList()->AddText(ImVec2(up.x, up.y + (fh - us.y) / 2), col::TextMuted,
                                                              unit);
                           ImGui::PopFont();
                           ImGui::Dummy(ImVec2(us.x, 1));
                           ImGui::EndGroup();
                           x += ImGui::GetItemRectSize().x + 24;
                       };
                       double bl = t.bottom_layers, tl = t.transition_layers;
                       field("exp", "Normal exposure", &t.exposure, "s", 0, 60, "%g");
                       field("bexp", "Bottom exposure", &t.bottom_exposure, "s", 0, 200, "%g");
                       field("bl", "Bottom layers", &bl, "layers", 0, 20, "%.0f");
                       field("tl", "Transition layers", &tl, "layers", 0, 20, "%.0f");
                       t.bottom_layers = static_cast<int>(std::lround(bl));
                       t.transition_layers = static_cast<int>(std::lround(tl));
                   }))
        typed = true;
    ImGui::Dummy(ImVec2(0, 6));
    if (!typed && sample) {
        const float w = ImGui::GetContentRegionAvail().x;
        beginCard("found", ImVec2(w, 0), 18, col::Panel, col::Border);
        text("Found in the file", fonts().semibold, size::Section, col::Text);
        ImGui::Dummy(ImVec2(0, 4));
        const auto facts = sampleFacts(*sample);
        const float gap = 12, fw = std::floor((ImGui::GetContentRegionAvail().x - 3 * gap) / 4);
        // Each row as tall as its tallest card, so no note scrolls.
        auto noteHeight = [&](const std::string& n) {
            ImGui::PushFont(fonts().regular, size::Help);
            const float h = ImGui::CalcTextSize(n.c_str(), nullptr, false, fw - 24).y;
            ImGui::PopFont();
            return h;
        };
        std::vector<float> row_h((facts.size() + 3) / 4, 0);
        for (std::size_t i = 0; i < facts.size(); ++i)
            row_h[i / 4] = std::max(row_h[i / 4], 12 + 17 + 8 + 19 + 8 + noteHeight(facts[i].note) + 12);
        for (std::size_t i = 0; i < facts.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            beginCard("fact", ImVec2(fw, std::ceil(row_h[i / 4])), 12, col::Card, col::Border);
            text(facts[i].label.c_str(), fonts().regular, size::Body, col::TextMuted);
            text(facts[i].value.c_str(), fonts().semibold, size::Section - 1, col::Text);
            text(facts[i].note.c_str(), fonts().regular, size::Help, col::TextMuted, true);
            endCard();
            ImGui::PopID();
            if (i % 4 != 3 && i + 1 < facts.size()) ImGui::SameLine(0, gap);
        }
        endCard();
        ImGui::Dummy(ImVec2(0, 6));
    }
    const bool ready = typed ? t.exposure > 0 && t.bottom_exposure > 0 : sample != nullptr;
    switch (footer(true, typed ? nullptr : "A file made for another printer is refused, with the reason.",
                   "Skip for now", "Next: image orientation", ready,
                   typed ? "Type both exposures first." : "Choose a SatelLite file first.")) {
        case 'b': app.setupStep() = 0; break;
        case 'l': app.setupStep() = 2; break;
        case 'm':
            if (typed) app.useTypedDefaults();
            app.setupStep() = 2;
            break;
        default: break;
    }
}

void orientation(App& app) {
    stepHeading(3, "Check which way the printer flips the image",
                "Printers differ in whether they mirror the picture of each layer, and a wrong guess prints text and "
                "left-handed parts backwards. The check takes 5 minutes and no resin: you slice a small F in "
                "SatelLite, and FruitSlicer compares that file with its own.");
    const PrinterSetup& ps = app.printerSetup();
    const float w = std::min(ImGui::GetContentRegionAvail().x, 660.0f);
    beginCard("status", ImVec2(w, 0), 16, col::Panel, col::Border);
    if (ps.orientation_confirmed) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        icon::check(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + 1), 16, col::Ok);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 24, p.y));
        text("Done", fonts().semibold, size::Body, col::Ok);
        text(("Checked with " + ps.checked_with + ". Every print file gets that fix.").c_str(), fonts().regular,
             size::Body, col::TextSecondary, true);
    } else {
        text("Not checked yet", fonts().semibold, size::Body, col::Warn);
        text("Until then, print files take their image direction from SatelLite's header for this printer, and "
             "FruitSlicer asks before saving one.",
             fonts().regular, size::Body, col::TextSecondary, true);
    }
    endCard();
    ImGui::Dummy(ImVec2(0, 14));
    const bool done = ps.orientation_confirmed;
    switch (footer(true, nullptr, done ? nullptr : "Later", done ? "Next: calibration" : "Do the check now", true)) {
        case 'b': app.setupStep() = 1; break;
        case 'l': app.setupStep() = 3; break;
        case 'm':
            if (done) app.setupStep() = 3;
            else app.finishSetup(Screen::OrientationCheck);
            break;
        default: break;
    }
}

void calibration(App& app) {
    stepHeading(4, "Calibration: now or later",
                "Under Calibrate you can set which side your vat hinges on, so tips hold harder where peeling pulls "
                "most. Test plates that measure your own resin arrive in a later update; until then FruitSlicer "
                "starts from values borrowed from PrusaSlicer, listed under Settings, About.");
    switch (footer(true, nullptr, "Open Calibrate", "Finish", true)) {
        case 'b': app.setupStep() = 2; break;
        case 'l': app.finishSetup(Screen::Calibrate); break;
        case 'm': app.finishSetup(Screen::Home); break;
        default: break;
    }
}

}  // namespace

void setup(App& app) {
    int& step = app.setupStep();
    step = std::clamp(step, 0, 3);
    const PrinterSetup& ps = app.printerSetup();
    const Resin& resin = app.newJobResin();
    const bool done[4] = {step > 0, !resin.sample_path.empty() || (resin.exposure > 0 && resin.bottom_exposure > 0),
                          ps.orientation_confirmed, false};
    aside(step, done);
    ImGui::SameLine(0, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(64, 40));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::App);
    ImGui::BeginChild("setup_main", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 8));
    const float w = std::min(ImGui::GetContentRegionAvail().x, 980.0f);
    ImGui::BeginChild("setup_body", ImVec2(w, 0), ImGuiChildFlags_None);
    switch (step) {
        case 0: welcome(app); break;
        case 1: printSettings(app); break;
        case 2: orientation(app); break;
        default: calibration(app); break;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

}  // namespace gui::screens
