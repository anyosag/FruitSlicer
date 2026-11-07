// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The A/B test (canvas boards "ABSetup" and "ABResults"): copies of the
// job's model on one plate, half with FruitSlicer's supports (A) and half
// bare for SatelLite's auto-supports (B), shuffled so the plate position
// favours neither; then each copy scored without its side showing, and the
// two sides compared.
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
        const bool a = c.at("arm").get<std::string>() == "A";
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

// A result's colour as a cell's fill.
ImU32 tint(ImU32 c) { return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(0.32f * 255) << IM_COL32_A_SHIFT); }

// The plate while scoring: each copy by number and result; its side shows
// only once revealed. Returns the copy clicked (0 if none).
int abScoreMap(const nlohmann::json& m, const std::vector<fruit::ABCopyResult>& scores, int selected, bool revealed,
               float width) {
    const auto& g = m.at("grid");
    const int rows = g.value("rows", 0), cols = g.value("cols", 0);
    if (rows <= 0 || cols <= 0) return 0;
    const double cx = g["cell_mm"][0].get<double>(), cy = g["cell_mm"][1].get<double>();
    constexpr float pad = 6, gap = 6, max_h = 340;
    const float s = std::min((width - 2 * pad) / static_cast<float>(cols * cx),
                             (max_h - 2 * pad) / static_cast<float>(rows * cy));
    const float cw = static_cast<float>(cx) * s, ch = static_cast<float>(cy) * s;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const ImVec2 end(o.x + 2 * pad + cols * cw - gap, o.y + 2 * pad + rows * ch - gap);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, end, col::Viewport, 8);
    auto at = [&](int row, int c) { return ImVec2(o.x + pad + c * cw, o.y + pad + (rows - 1 - row) * ch); };
    {
        const ImVec2 a = at(0, 0), b(a.x + cw - gap, a.y + ch - gap);
        dl->AddRectFilled(a, b, col::Text, 6);
        ImGui::PushFont(fonts().bold, size::Section + 3);
        const ImVec2 ts = ImGui::CalcTextSize("L");
        dl->AddText(ImVec2((a.x + b.x - ts.x) / 2, (a.y + b.y - ts.y) / 2), col::App, "L");
        ImGui::PopFont();
    }
    int clicked = 0;
    for (const auto& c : m.at("copies")) {
        const int id = c["id"].get<int>();
        const ImVec2 a = at(c["row"].get<int>(), c["col"].get<int>()), b(a.x + cw - gap, a.y + ch - gap);
        const fruit::ABCopyResult* r = nullptr;
        for (const auto& x : scores)
            if (x.id == id) r = &x;
        const bool scored = r && r->recorded;
        const ImU32 fg = !scored ? col::TextSecondary : r->ok ? col::Ok : r->partial ? col::Warn : col::Error;
        dl->AddRectFilled(a, b, scored ? tint(fg) : col::Active, 6);
        const bool is_a = c.at("arm").get<std::string>() == "A";
        if (revealed) dl->AddRect(a, b, is_a ? kA.border : kB.border, 6, 2.0f);
        if (id == selected) dl->AddRect(ImVec2(a.x - 2, a.y - 2), ImVec2(b.x + 2, b.y + 2), col::Accent, 7, 2.5f);
        const std::string big = revealed ? (is_a ? "A" : "B") : std::to_string(id);
        const std::string small = revealed ? strf("copy %d", id)
                                  : !scored ? std::string("not scored")
                                  : r->ok   ? std::string("whole")
                                  : r->partial ? std::string("partly")
                                               : std::string("failed");
        ImGui::PushFont(fonts().bold, size::Section + 3);
        const ImVec2 bs = ImGui::CalcTextSize(big.c_str());
        const float top = (a.y + b.y) / 2 - (bs.y + 16) / 2;
        dl->AddText(ImVec2((a.x + b.x - bs.x) / 2, top), col::Text, big.c_str());
        ImGui::PopFont();
        ImGui::PushFont(fonts().regular, size::Help);
        const ImVec2 ss = ImGui::CalcTextSize(small.c_str());
        dl->AddText(ImVec2((a.x + b.x - ss.x) / 2, top + bs.y + 2), col::TextSecondary, small.c_str());
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(id);
        if (ImGui::InvisibleButton("copy", ImVec2(cw - gap, ch - gap))) clicked = id;
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x, end.y + 8));
    ImGui::Dummy(ImVec2(end.x - o.x, 0));
    return clicked;
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
        ImGui::Dummy(ImVec2(0, 2));
        if (linkButton("Score the copies after printing")) app.openABResults();
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

void abResults(App& app) {
    beginPage("ab_results");
    if (linkButton("‹ A/B test")) app.openABSetup();
    const ABRecord& rec = app.abRecord();
    const nlohmann::json& m = app.abManifest();
    const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
    text(("A/B results · " + (rec.model.empty() ? std::string("no test yet") : rec.model)).c_str(), fonts().bold,
         size::PageTitle - 4, col::Text);
    if (m.is_null()) {
        ImGui::Dummy(ImVec2(0, 6));
        beginCard("none", ImVec2(std::min(ImGui::GetContentRegionAvail().x, 640.0f), 0), 18, col::Panel, col::Border);
        text("Set up a test first", fonts().semibold, size::Section, col::Text);
        text("Save the two files, print them, and score the copies here.", fonts().regular, size::Body,
             col::TextSecondary, true);
        ImGui::Dummy(ImVec2(0, 4));
        if (secondaryButton("Set it up", ImVec2(0, 34))) app.openABSetup();
        endCard();
        endPage();
        return;
    }
    const auto& scores = app.abScores();
    int done = 0;
    for (const auto& c : scores) done += c.recorded ? 1 : 0;
    const int total = static_cast<int>(scores.size());
    text((done == total ? strf("All %d copies scored", total) : strf("%d of %d copies scored", done, total)).c_str(),
         fonts().regular, size::Body, col::TextMuted);
    const float after = ImGui::GetCursorPosY();
    bool& revealed = app.abRevealed();
    {
        const char* label = revealed ? "Hide which is which" : "Reveal and compare";
        const float bw = textWidth(label, fonts().bold, size::Body) + 34;
        ImGui::SetCursorPos(ImVec2(x0 + ImGui::GetContentRegionAvail().x - bw, y0 + 4));
        const bool primary = !revealed && done == total;
        if (primary ? primaryButton(label, ImVec2(bw, 38)) : secondaryButton(label, ImVec2(bw, 38)))
            revealed = !revealed;
        if (!revealed && done < total)
            tooltip("Score every copy first: once you know which is which, the rest aren't scored blind.");
    }
    ImGui::SetCursorPos(ImVec2(x0, after + 8));
    // Progress.
    {
        const float pw = ImGui::GetContentRegionAvail().x;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + pw, p.y + 6), col::Active, 3);
        if (total > 0 && done > 0)
            dl->AddRectFilled(p, ImVec2(p.x + pw * static_cast<float>(done) / static_cast<float>(total), p.y + 6),
                              col::Accent, 3);
        ImGui::Dummy(ImVec2(pw, 14));
    }
    const float w = ImGui::GetContentRegionAvail().x, left_w = w - kSideW - 16;
    int& sel = app.abSelected();
    const fruit::ABCopyResult* r = nullptr;
    for (const auto& c : scores)
        if (c.id == sel) r = &c;
    if (!r && !scores.empty()) {
        sel = scores.front().id;
        r = &scores.front();
    }
    beginCard("map", ImVec2(left_w, 0), 18, col::Panel, col::Border);
    text("The plate, from above", fonts().semibold, size::Section, col::Text);
    text(revealed ? "Blue edges: FruitSlicer's supports (A). Brown edges: SatelLite's (B)."
                  : "Turn the plate until its L marker matches the one here. Which side each copy is on stays hidden "
                    "until you reveal it.",
         fonts().regular, size::Help, col::TextMuted, true);
    ImGui::Dummy(ImVec2(0, 2));
    if (const int c = abScoreMap(m, scores, sel, revealed, ImGui::GetContentRegionAvail().x)) sel = c;
    endCard();
    ImGui::SameLine(0, 16);
    // The selected copy's score, a draft until saved.
    static int draft_for = -1, draft_rev = -1;
    static App::Outcome outcome = App::Outcome::None;
    static int scar = 0;
    static char secs[16];  // empty: not timed
    static char notes[160];
    if (draft_for != sel || draft_rev != app.abScoresRevision()) {
        draft_for = sel;
        draft_rev = app.abScoresRevision();
        outcome = !r || !r->recorded ? App::Outcome::None
                  : r->ok            ? App::Outcome::Held
                  : r->partial       ? App::Outcome::Partly
                                     : App::Outcome::FellOff;
        scar = r ? r->scar : 0;
        if (r && r->removal_s >= 0) std::snprintf(secs, sizeof secs, "%.0f", r->removal_s);
        else secs[0] = 0;
        std::snprintf(notes, sizeof notes, "%s", r ? r->notes.c_str() : "");
    }
    beginCard("copy", ImVec2(kSideW, 0), 18, col::Panel, col::Border);
    text(strf("Copy %d", sel).c_str(), fonts().semibold, size::Section + 1, col::Text);
    ImGui::Dummy(ImVec2(0, 4));
    text("Did it print whole?", fonts().semibold, size::Body, col::Text);
    const struct {
        App::Outcome o;
        const char* label;
    } outs[] = {{App::Outcome::Held, "Yes"}, {App::Outcome::Partly, "Partly"}, {App::Outcome::FellOff, "No"}};
    for (int i = 0; i < 3; ++i) {
        ImGui::PushID(i);
        if (segmentButton(outs[i].label, outcome == outs[i].o)) outcome = outs[i].o;
        ImGui::PopID();
        if (i < 2) ImGui::SameLine(0, 4);
    }
    text(outcome == App::Outcome::Held      ? "Yes: the whole part printed and stayed on."
         : outcome == App::Outcome::Partly  ? "Partly: some of it failed or came off."
         : outcome == App::Outcome::FellOff ? "No: it fell off or failed. Nothing to clean or mark."
                                            : "Yes: it printed whole. Partly: some of it failed. No: it fell off or "
                                              "failed.",
         fonts().regular, size::Help, col::TextMuted, true);
    const bool printed = outcome == App::Outcome::Held || outcome == App::Outcome::Partly;
    ImGui::Dummy(ImVec2(0, 6));
    text("Seconds to clean off its supports", fonts().semibold, size::Body, col::Text);
    ImGui::BeginDisabled(!printed);
    ImGui::SetNextItemWidth(120);
    ImGui::InputTextWithHint("##secs", "Not timed", secs, sizeof secs, ImGuiInputTextFlags_CharsDecimal);
    ImGui::EndDisabled();
    text("Time each copy the same way, start to finish.", fonts().regular, size::Help, col::TextMuted, true);
    ImGui::Dummy(ImVec2(0, 6));
    text("Mark left on the part", fonts().semibold, size::Body, col::Text);
    static const char* const kMarks[] = {"1: can’t see it", "2: faint, only under raking light", "3: a visible mark",
                                         "4: a clear dent or bump", "5: a pit or torn skin"};
    ImGui::BeginDisabled(!printed);
    for (int k = 1; k <= 5; ++k) {
        ImGui::PushID(100 + k);
        if (segmentButton(std::to_string(k).c_str(), scar == k && printed)) scar = scar == k ? 0 : k;
        ImGui::PopID();
        if (k < 5) ImGui::SameLine(0, 4);
    }
    ImGui::EndDisabled();
    text(printed && scar >= 1 && scar <= 5 ? kMarks[scar - 1]
                                           : "Judge it after cleaning, under a low, raking light.",
         fonts().regular, size::Help, col::TextMuted, true);
    ImGui::Dummy(ImVec2(0, 6));
    text("Notes", fonts().semibold, size::Body, col::Text);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    ImGui::InputTextWithHint("##notes", "Optional", notes, sizeof notes);
    ImGui::Dummy(ImVec2(0, 8));
    if (primaryButton("Save and go to the next copy", ImVec2(ImGui::GetContentRegionAvail().x, 38),
                      outcome != App::Outcome::None)) {
        const int id = sel;
        char* end = nullptr;
        const double t = std::strtod(secs, &end);
        const bool timed = printed && end != secs && t >= 0;
        app.setABScore(id, outcome, printed ? scar : 0, timed ? t : -1, notes);
        const auto& now = app.abScores();
        std::size_t at = 0;
        for (std::size_t i = 0; i < now.size(); ++i)
            if (now[i].id == id) at = i;
        sel = now.empty() ? 0 : now[(at + 1) % now.size()].id;
        for (std::size_t k = 1; k <= now.size(); ++k)  // the next one not scored yet, if any
            if (!now[(at + k) % now.size()].recorded) {
                sel = now[(at + k) % now.size()].id;
                break;
            }
    }
    if (outcome == App::Outcome::None) tooltip("Say whether it printed whole first.");
    endCard();
    if (revealed) {
        ImGui::Dummy(ImVec2(0, 4));
        const fruit::ABSummary& sum = app.abSummary();
        const ABVerdict v = abVerdict(sum);
        const int tips = m.contains("tips_per_copy") && m["tips_per_copy"].is_number() ? m["tips_per_copy"].get<int>()
                                                                                        : -1;
        beginCard("compare", ImVec2(w, 0), 18, col::Panel, col::Border);
        text("A (FruitSlicer) against B (SatelLite auto-supports)", fonts().semibold, size::Section, col::Text);
        ImGui::Dummy(ImVec2(0, 4));
        const std::vector<ABTile> tiles = abTiles(sum, tips);
        const float tw = (ImGui::GetContentRegionAvail().x - 12 * (tiles.size() - 1)) / static_cast<float>(tiles.size());
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            beginCard("tile", ImVec2(tw, 108), 14, col::Card, col::Border);
            text(tiles[i].label.c_str(), fonts().regular, size::Help, col::TextMuted);
            ImGui::Dummy(ImVec2(0, 2));
            const float tx = ImGui::GetCursorPosX(), ty = ImGui::GetCursorPosY(), half = ImGui::GetContentRegionAvail().x / 2;
            text("A", fonts().semibold, size::Help, col::TextSecondary);
            ImGui::SetCursorPos(ImVec2(tx, ty + 18));
            text(tiles[i].a.c_str(), fonts().bold, size::Section + 2, col::Text);
            ImGui::SetCursorPos(ImVec2(tx + half, ty));
            text("B", fonts().semibold, size::Help, col::TextSecondary);
            ImGui::SetCursorPos(ImVec2(tx + half, ty + 18));
            text(tiles[i].b.c_str(), tiles[i].b == "from SatelLite" ? fonts().regular : fonts().bold,
                 tiles[i].b == "from SatelLite" ? size::Body : size::Section + 2,
                 tiles[i].b == "from SatelLite" ? col::TextSecondary : col::Text);
            endCard();
            ImGui::PopID();
            if (i + 1 < tiles.size()) ImGui::SameLine(0, 12);
        }
        ImGui::Dummy(ImVec2(0, 6));
        const ImU32 head = v.winner == ABVerdict::Winner::A   ? col::Ok
                           : v.winner == ABVerdict::Winner::B ? col::Warn
                                                              : col::Text;
        text(v.headline.c_str(), fonts().semibold, size::Section + 1, head, true);
        if (!v.detail.empty()) text(v.detail.c_str(), fonts().regular, size::Body, col::TextSecondary, true);
        if (!v.caveat.empty()) text(v.caveat.c_str(), fonts().regular, size::Help, col::TextMuted, true);
        endCard();
    }
    endPage();
}

}  // namespace gui::screens
