// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The A/B test (canvas board "ABSetup"): copies of the job's model on one
// plate, half with FruitSlicer's supports (A) and half bare for SatelLite's
// auto-supports (B), shuffled so the plate position favours neither.
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

#include "App.h"
#include "Format.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"
#include "screens/Screens.h"

namespace gui::screens {

namespace {

using parts::tooltip;

constexpr float kSideW = 380;

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

// The arms' colours, from the design: A blue, B brown, each with its letter
// on it, so they never rest on colour alone.
struct ArmLook {
    ImU32 bg, border;
};
constexpr ArmLook kA{IM_COL32(0x1B, 0x28, 0x38, 255), IM_COL32(0x2E, 0x4A, 0x6B, 255)};
constexpr ArmLook kB{IM_COL32(0x2A, 0x21, 0x18, 255), IM_COL32(0x5C, 0x41, 0x27, 255)};

// The plate from above, as the copies will sit: rows count up the plate and
// columns to the right; cell 0 holds the L marker.
void abMap(const fruit::ABPlate& p, const fruit::ABPlateFit& fit, int tips, float width) {
    if (p.rows <= 0 || p.cols <= 0) return;
    constexpr float pad = 6, gap = 6, max_h = 340;
    const float s = std::min((width - 2 * pad) / static_cast<float>(p.cols * fit.cell_x),
                             (max_h - 2 * pad) / static_cast<float>(p.rows * fit.cell_y));
    const float cw = static_cast<float>(fit.cell_x) * s, ch = static_cast<float>(fit.cell_y) * s;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const ImVec2 end(o.x + 2 * pad + p.cols * cw - gap, o.y + 2 * pad + p.rows * ch - gap);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, end, col::Viewport, 8);
    auto at = [&](int row, int c) { return ImVec2(o.x + pad + c * cw, o.y + pad + (p.rows - 1 - row) * ch); };
    auto cell = [&](ImVec2 a, ImU32 bg, ImU32 border, ImU32 fg, const char* big, const std::string& small,
                    ImU32 sub = col::TextSecondary) {
        const ImVec2 b(a.x + cw - gap, a.y + ch - gap);
        dl->AddRectFilled(a, b, bg, 6);
        dl->AddRect(a, b, border, 6, 1.5f);
        ImGui::PushFont(fonts().bold, size::Section + 3);
        const ImVec2 bs = ImGui::CalcTextSize(big);
        const float top = (a.y + b.y) / 2 - (bs.y + 16) / 2;
        dl->AddText(ImVec2((a.x + b.x - bs.x) / 2, top), fg, big);
        ImGui::PopFont();
        ImGui::PushFont(fonts().regular, size::Help);
        const ImVec2 ss = ImGui::CalcTextSize(small.c_str());
        if (ss.x < b.x - a.x - 6)
            dl->AddText(ImVec2((a.x + b.x - ss.x) / 2, top + bs.y + 2), sub, small.c_str());
        ImGui::PopFont();
    };
    std::vector<bool> used(static_cast<std::size_t>(p.rows * p.cols), false);
    used[0] = true;
    cell(at(0, 0), col::Text, col::Text, col::App, "L", "marker", IM_COL32(0x3A, 0x40, 0x4D, 255));
    for (const auto& c : p.manifest["copies"]) {
        const int r = c["row"].get<int>(), k = c["col"].get<int>(), id = c["id"].get<int>();
        used[static_cast<std::size_t>(r * p.cols + k)] = true;
        const bool a = c["arm"] == "A";
        cell(at(r, k), a ? kA.bg : kB.bg, a ? kA.border : kB.border, col::Text, a ? "A" : "B",
             a ? (tips >= 0 ? strf("copy %d · %d tips", id, tips) : strf("copy %d", id)) : strf("copy %d · bare", id));
    }
    for (int r = 0; r < p.rows; ++r)
        for (int k = 0; k < p.cols; ++k)
            if (!used[static_cast<std::size_t>(r * p.cols + k)]) {
                const ImVec2 a = at(r, k), b(a.x + cw - gap, a.y + ch - gap);
                dl->AddRect(a, b, col::Border, 6, 1.0f);
            }
    ImGui::Dummy(ImVec2(end.x - o.x, end.y - o.y + 6));
}

// One of the numbered steps.
void howStep(int n, const char* title, const char* detail) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(ImVec2(p.x + 11, p.y + 11), 11, col::Active, 20);
    ImGui::PushFont(fonts().semibold, size::Help);
    const std::string num = std::to_string(n);
    const ImVec2 ns = ImGui::CalcTextSize(num.c_str());
    dl->AddText(ImVec2(p.x + 11 - ns.x / 2, p.y + 11 - ns.y / 2), col::Text, num.c_str());
    ImGui::PopFont();
    const float x0 = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(x0 + 34);
    ImGui::BeginGroup();
    text(title, fonts().semibold, size::Body, col::Text);
    text(detail, fonts().regular, size::Help, col::TextSecondary, true);
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, 4));
}

}  // namespace

void abSetup(App& app) {
    beginPage("ab_setup");
    if (linkButton("‹ Calibrate")) app.setScreen(Screen::Calibrate);
    text("A/B test: FruitSlicer vs SatelLite supports", fonts().bold, size::PageTitle - 4, col::Text);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(ImGui::GetContentRegionAvail().x, 860.0f));
    text("Copies of one model share a plate: half with FruitSlicer's supports (A), half bare, for SatelLite's "
         "auto-supports (B). Their places are shuffled so the plate position doesn't favour either.",
         fonts().regular, size::Body, col::TextMuted, true);
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 6));
    if (const std::string why = app.abBlocker(); !why.empty()) {
        beginCard("blocked", ImVec2(std::min(ImGui::GetContentRegionAvail().x, 640.0f), 0), 18, col::Panel,
                  col::Border);
        text("A job with supports comes first", fonts().semibold, size::Section, col::Text);
        text(why.c_str(), fonts().regular, size::Body, col::TextSecondary, true);
        ImGui::Dummy(ImVec2(0, 4));
        if (secondaryButton(app.document() ? "Go to the job" : "Back to Calibrate", ImVec2(0, 34)))
            app.setScreen(app.document() ? Screen::Job : Screen::Calibrate);
        endCard();
        endPage();
        return;
    }
    const fruit::ABPlateFit fit = app.abFit();
    const float w = ImGui::GetContentRegionAvail().x, left_w = w - kSideW - 16;
    beginCard("plan", ImVec2(left_w, 0), 18, col::Panel, col::Border);
    {
        keyValue("Model", app.document()->name + " · the current job");
        ImGui::Dummy(ImVec2(0, 2));
        const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
        text("Copies", fonts().regular, size::Body, col::TextSecondary);
        if (fit.capacity < 2) {
            text(strf("Two copies don't fit on the plate: each needs %.1f × %.1f mm with its supports.", fit.cell_x,
                      fit.cell_y)
                     .c_str(),
                 fonts().regular, size::Body, col::Warn, true);
            endCard();
            endPage();
            return;
        }
        const int n = app.abCopies();
        const float right = x0 + ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPos(ImVec2(right - 34 * 2 - 44, y0 - 6));
        if (secondaryButton("−", ImVec2(34, 30), n > 2)) app.setABCopies(n - 1);
        ImGui::SameLine(0, 0);
        const std::string ns = std::to_string(n);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (44 - textWidth(ns.c_str(), fonts().semibold, size::Body)) / 2);
        ImGui::SetCursorPosY(y0);
        text(ns.c_str(), fonts().semibold, size::Body, col::Text);
        ImGui::SetCursorPos(ImVec2(right - 34, y0 - 6));
        if (secondaryButton("+", ImVec2(34, 30), n < fit.capacity)) app.setABCopies(n + 1);
        ImGui::SetCursorPos(ImVec2(x0, y0 + 28));
        text(strf("Up to %d fit: each copy needs %.1f × %.1f mm.%s", fit.capacity, fit.cell_x, fit.cell_y,
                  n % 2 ? " An even number keeps the two sides the same size." : "")
                 .c_str(),
             fonts().regular, size::Help, col::TextMuted, true);
        ImGui::Dummy(ImVec2(0, 8));
        text("The plate, from above", fonts().semibold, size::Section, col::Text);
        ImGui::Dummy(ImVec2(0, 2));
        if (const fruit::ABPlate* p = app.abPlate()) {
            const int tips =
                p->manifest.contains("tips_per_copy") && p->manifest["tips_per_copy"].is_number()
                    ? p->manifest["tips_per_copy"].get<int>()
                    : -1;
            abMap(*p, fit, tips, ImGui::GetContentRegionAvail().x);
        }
        text("Shuffled with seed 1. The L marker sits straight on the plate, so you can tell the copies apart later.",
             fonts().regular, size::Help, col::TextMuted, true);
    }
    endCard();
    ImGui::SameLine(0, 16);
    ImGui::BeginGroup();
    beginCard("how", ImVec2(kSideW, 0), 18, col::Panel, col::Border);
    text("How to run it", fonts().semibold, size::Section, col::Text);
    ImGui::Dummy(ImVec2(0, 4));
    howStep(1, "Save the two STL files", "The A copies with their supports and the L marker, and the B copies, bare.");
    howStep(2, "Load both in SatelLite", "Leave them where they are. Add auto-supports to the B copies only.");
    howStep(3, "Print with your usual settings", "All the copies print together, under the same conditions.");
    howStep(4, "Score each copy",
            "Time how long its supports take to clean off, then score its marks. The two kinds of supports look "
            "different, so the scores aren't fully blind: judge the marks as if you didn't know.");
    endCard();
    ImGui::Dummy(ImVec2(0, 4));
    beginCard("files", ImVec2(kSideW, 0), 18, col::Panel, col::Border);
    const ABRecord& rec = app.abRecord();
    const bool saved = rec.copies > 0 && rec.model == app.document()->name;
    text(saved ? "Saved" : "Files", fonts().semibold, size::Section, col::Text);
    ImGui::Dummy(ImVec2(0, 2));
    if (saved) {
        const std::filesystem::path a(rec.file_a), b(rec.file_b);
        const std::string where = a.parent_path().filename().string();
        text(("· " + a.filename().string() + ": the A copies, with supports").c_str(), fonts().regular, size::Body,
             col::TextSecondary, true);
        text(("· " + b.filename().string() + ": the B copies, bare").c_str(), fonts().regular, size::Body,
             col::TextSecondary, true);
        if (!where.empty())
            text(("In " + where + ". FruitSlicer keeps which copy is where, for scoring.").c_str(), fonts().regular,
                 size::Help, col::TextMuted, true);
        text("Scoring the copies arrives in a later update.", fonts().regular, size::Help, col::TextMuted, true);
    } else {
        text("· <name>_A.stl: the A copies with their supports, plus the L marker", fonts().regular, size::Body,
             col::TextSecondary, true);
        text("· <name>_B.stl: the B copies, bare", fonts().regular, size::Body, col::TextSecondary, true);
        text("FruitSlicer keeps which copy is where, for scoring later.", fonts().regular, size::Help,
             col::TextMuted, true);
    }
    ImGui::Dummy(ImVec2(0, 6));
    const char* label = saved ? "Save them again…" : "Save the two STL files…";
    if (primaryButton(label, ImVec2(ImGui::GetContentRegionAvail().x, 38), app.abPlate() != nullptr))
        app.saveABFiles();
    if (saved) tooltip("Saving again starts a new test: scores kept for the last one are set aside, not lost.");
    endCard();
    ImGui::EndGroup();
    endPage();
}

}  // namespace gui::screens
