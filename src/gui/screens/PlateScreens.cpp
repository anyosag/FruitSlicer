// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Resin test plates (canvas boards "PlatePreview" and "Results"): the plate
// from above with what each coupon tests, and scoring it after printing,
// blind to the settings.
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
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

constexpr float kSideW = 360;

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

std::string lowerFirst(std::string s) {
    if (!s.empty()) s[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[0])));
    return s;
}

// How a coupon's raft looks on the map.
struct CellLook {
    ImU32 bg, fg;
};

// A result's colour as a raft's fill: strong enough to tell apart at a glance.
ImU32 tint(ImU32 c) { return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(0.32f * 255) << IM_COL32_A_SHIFT); }

// The plate from above: the build plate with each coupon on its raft and
// the L marker in row 0, column 0, drawn as the plate is made (rows count
// up the plate, columns to the right). `mirrored` flips it left to right.
// Returns the coupon clicked (0 if none).
template <class Look>
int plateMap(const PlateLayout& l, int selected, bool mirrored, float width, const Look& look) {
    if (l.rows <= 0 || l.cols <= 0) return 0;
    // Every plate fills the same build plate, so all are drawn at one scale:
    // as wide as there is room for, and never taller than kMapHeight.
    constexpr float pad = 6, gap = 4, kMapHeight = 520;
    const float cell = std::floor(std::min((width - 2 * pad + gap) / static_cast<float>(l.cols),
                                           (kMapHeight - 2 * pad + gap) / static_cast<float>(l.rows)));
    const float raft = cell - gap;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const ImVec2 end(o.x + 2 * pad + l.cols * cell - gap, o.y + 2 * pad + l.rows * cell - gap);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, end, col::Viewport, 8);
    auto at = [&](int row, int c) {
        const int x = mirrored ? l.cols - 1 - c : c;
        return ImVec2(o.x + pad + x * cell, o.y + pad + (l.rows - 1 - row) * cell);
    };
    // The marker: an L on its raft, long arm along the row, short arm up the
    // column, from the cell's corner nearest the plate's origin.
    {
        const ImVec2 a = at(0, 0), b(a.x + raft, a.y + raft);
        dl->AddRectFilled(a, b, col::Active, 5);
        const double raft_mm = std::max(1.0, l.cell_mm - 2);
        const float s = raft / static_cast<float>(raft_mm);
        const float m = s, t = std::max(3.0f, 1.5f * s), arm = std::max(4 * t, static_cast<float>((l.cell_mm - 4) * s));
        const float base = b.y - m;
        if (!mirrored) {
            dl->AddRectFilled(ImVec2(a.x + m, base - t), ImVec2(a.x + m + arm, base), col::Text);
            dl->AddRectFilled(ImVec2(a.x + m, base - 0.6f * arm), ImVec2(a.x + m + t, base), col::Text);
        } else {
            dl->AddRectFilled(ImVec2(b.x - m - arm, base - t), ImVec2(b.x - m, base), col::Text);
            dl->AddRectFilled(ImVec2(b.x - m - t, base - 0.6f * arm), ImVec2(b.x - m, base), col::Text);
        }
    }
    int clicked = 0;
    ImGui::PushFont(fonts().semibold, cell < 40 ? size::Help - 1 : size::Help);
    for (const PlateLayout::Cell& cp : l.cells) {
        const ImVec2 a = at(cp.row, cp.col), b(a.x + raft, a.y + raft);
        const CellLook lk = look(cp.id);
        dl->AddRectFilled(a, b, lk.bg, 5);
        if (cp.id == selected) dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), col::Accent, 6, 2.5f);
        const std::string n = std::to_string(cp.id);
        const ImVec2 ts = ImGui::CalcTextSize(n.c_str());
        dl->AddText(ImVec2(std::floor((a.x + b.x - ts.x) / 2), std::floor((a.y + b.y - ts.y) / 2)), lk.fg, n.c_str());
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(cp.id);
        if (ImGui::InvisibleButton("c", ImVec2(raft, raft))) clicked = cp.id;
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::PopID();
    }
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(o.x, end.y + 8));
    ImGui::Dummy(ImVec2(end.x - o.x, 0));
    return clicked;
}

void swatch(ImU32 c, const char* label) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 3), ImVec2(p.x + 12, p.y + 15), c, 3);
    ImGui::Dummy(ImVec2(16, 18));
    ImGui::SameLine(0, 4);
    text(label, fonts().regular, size::Help, col::TextMuted);
    ImGui::SameLine(0, 16);
}

// The marker in the legend: a small L.
void markerSwatch(const char* label) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(p.x, p.y + 12), ImVec2(p.x + 14, p.y + 15), col::Text);
    dl->AddRectFilled(ImVec2(p.x, p.y + 6), ImVec2(p.x + 3, p.y + 15), col::Text);
    ImGui::Dummy(ImVec2(18, 18));
    ImGui::SameLine(0, 4);
    text(label, fonts().regular, size::Help, col::TextMuted);
    ImGui::SameLine(0, 16);
}

// Title row: back link, title and a line under it, buttons on the right
// (drawn by `right`, which gets the x where they must end and the title's y).
void header(App& app, const std::string& title, const std::string& sub, const std::function<void(float, float)>& right) {
    if (linkButton("‹ Calibrate")) app.setScreen(Screen::Calibrate);
    const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
    text(title.c_str(), fonts().bold, size::PageTitle - 4, col::Text);
    text(sub.c_str(), fonts().regular, size::Body, col::TextMuted);
    const float after = ImGui::GetCursorPosY();
    right(x0 + ImGui::GetContentRegionAvail().x, y0);
    ImGui::SetCursorPos(ImVec2(x0, after + 8));
    ImGui::Dummy(ImVec2(0, 0));
}

const PlateLayout::Cell* cellOf(const PlateLayout& l, int id) {
    for (const auto& c : l.cells)
        if (c.id == id) return &c;
    return nullptr;
}

}  // namespace

// ---- The plate, from above ----

void plate(App& app) {
    beginPage("plate");
    const fruit::CalibrationPlate& p = app.plate();
    const std::string kind = app.plateKind();
    const Resin& resin = app.newJobResin();
    const bool has_print = !resin.sample_path.empty() || (resin.exposure > 0 && resin.bottom_exposure > 0);
    const bool busy = app.jobs().busy();
    header(app, plateTitle(kind), plateSubtitle(p) + " · each on its own small raft", [&](float right, float y) {
        const float gw = textWidth("Save .goo to print", fonts().bold, size::Body) + 34;
        ImGui::SetCursorPos(ImVec2(right - gw, y + 4));
        if (primaryButton("Save .goo to print", ImVec2(gw, 38), has_print && !busy)) app.savePlate(true);
        if (!has_print)
            tooltip("The resin has no print settings yet: add them on the Resins page, or save the STL and slice it "
                    "in SatelLite.");
        else if (busy)
            tooltip("Wait for the current task to finish.");
        const float sw = textWidth("Save STL", fonts().semibold, size::Body) + 28;
        ImGui::SetCursorPos(ImVec2(right - gw - 8 - sw, y + 4));
        if (secondaryButton("Save STL", ImVec2(sw, 38))) app.savePlate(false);
    });
    if (!app.printerSetup().orientation_confirmed) {
        text("The printer's image orientation isn't checked yet, so the plate may print mirrored. Scoring still "
             "works: its L marker shows which way round it is.",
             fonts().regular, size::Help, col::Warn, true);
        ImGui::Dummy(ImVec2(0, 2));
    }
    const PlateLayout layout = layoutOf(p);
    const float w = ImGui::GetContentRegionAvail().x;
    const float left_w = w - kSideW - 16;
    int& sel = app.plateSelected();
    if (!cellOf(layout, sel)) sel = layout.cells.empty() ? 0 : layout.cells.front().id;

    beginCard("map", ImVec2(left_w, 0), 18, col::Panel, col::Border);
    {
        const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
        text("The plate, from above", fonts().semibold, size::Section, col::Text);
        const char* hint = "Click a coupon to see what it tests";
        ImGui::SetCursorPos(ImVec2(x0 + ImGui::GetContentRegionAvail().x - textWidth(hint, fonts().regular, size::Help),
                                   y0 + 2));
        text(hint, fonts().regular, size::Help, col::TextMuted);
        ImGui::SetCursorPos(ImVec2(x0, y0 + 30));
        const int c = plateMap(layout, sel, false, ImGui::GetContentRegionAvail().x, [&](int id) {
            return CellLook{id == sel ? col::Card : col::Active, col::Text};
        });
        if (c) sel = c;
        swatch(col::Active, "Coupon");
        markerSwatch("L marker: its long arm runs along the rows");
        ImGui::NewLine();
        text("Settings are shuffled across the coupons, so where one sits doesn't favour any setting.",
             fonts().regular, size::Help, col::TextMuted, true);
    }
    endCard();
    ImGui::SameLine(0, 16);
    ImGui::BeginGroup();
    const fruit::PlateCoupon* cp = nullptr;
    for (const auto& c : p.coupons)
        if (c.id == sel) cp = &c;
    beginCard("coupon", ImVec2(kSideW, 0), 18, col::Panel, col::Border);
    if (cp) {
        text(strf("Coupon %d", cp->id).c_str(), fonts().semibold, size::Section + 1, col::Text);
        text(strf("Row %d, column %d, counting from the L", cp->row + 1, cp->col + 1).c_str(), fonts().regular,
             size::Help, col::TextMuted);
        ImGui::Dummy(ImVec2(0, 4));
        for (const CouponFact& f : couponFacts(kind, cp->params)) keyValue(f.label.c_str(), f.value);
        ImGui::Dummy(ImVec2(0, 4));
        text(couponMeaning(kind, cp->params).c_str(), fonts().regular, size::Body, col::TextSecondary, true);
    }
    endCard();
    ImGui::Dummy(ImVec2(0, 4));
    beginCard("tests", ImVec2(kSideW, 0), 18, col::Panel, col::Border);
    text("What the plate tests", fonts().semibold, size::Section, col::Text);
    ImGui::Dummy(ImVec2(0, 2));
    for (const std::string& t : plateTests(p)) text(("· " + t).c_str(), fonts().regular, size::Body, col::TextSecondary, true);
    endCard();
    ImGui::Dummy(ImVec2(0, 4));
    beginCard("files", ImVec2(kSideW, 0), 18, col::Panel, col::Border);
    const auto saved = resin.plates.find(kind);
    if (saved != resin.plates.end()) {
        const std::filesystem::path f(saved->second.file);
        const std::string where = f.parent_path().filename().string();
        text("Saved", fonts().semibold, size::Section, col::Text);
        ImGui::Dummy(ImVec2(0, 2));
        text((f.filename().string() + (where.empty() ? "" : ", in " + where)).c_str(), fonts().regular, size::Body,
             col::TextSecondary, true);
        text(("FruitSlicer keeps which coupon is where with " + resin.name + ", and the scores as you enter them.")
                 .c_str(),
             fonts().regular, size::Body, col::TextSecondary, true);
        ImGui::Dummy(ImVec2(0, 4));
        if (linkButton("Score it after printing")) app.openScores(kind);
    } else {
        text("Saving it", fonts().semibold, size::Section, col::Text);
        ImGui::Dummy(ImVec2(0, 2));
        text(("Save it as a print file with " + resin.name +
              "'s settings, or as an STL to slice in SatelLite. FruitSlicer then keeps which coupon is where, so you "
              "can score the plate after printing without seeing the settings.")
                 .c_str(),
             fonts().regular, size::Body, col::TextSecondary, true);
    }
    endCard();
    ImGui::EndGroup();
    endPage();
}

// ---- Scoring, blind to the settings ----

void score(App& app) {
    beginPage("score");
    const std::string kind = app.plateKind();
    const PlateScores* s = app.scores();
    const int total = s ? static_cast<int>(s->coupons.size()) : 0, done = s ? s->scored() : 0;
    const std::string status = !s                            ? std::string("Not saved yet")
                               : total > 0 && done == total ? strf("All %d scored", total)
                                                            : strf("%d of %d scored", done, total);
    header(app, "Score the " + lowerFirst(plateTitle(kind)), status, [&](float right, float y) {
               const float iw = textWidth("Import a filled CSV…", fonts().semibold, size::Body) + 28;
               ImGui::SetCursorPos(ImVec2(right - iw, y + 4));
               if (secondaryButton("Import a filled CSV…", ImVec2(iw, 38))) app.importScores();
               tooltip("A score sheet filled in by hand (.results.csv), with the plate's manifest (.json) beside it.");
           });
    if (!s || s->coupons.empty()) {
        beginCard("none", ImVec2(std::min(ImGui::GetContentRegionAvail().x, 640.0f), 0), 18, col::Panel, col::Border);
        text("Save the plate first", fonts().semibold, size::Section, col::Text);
        text("FruitSlicer keeps its score sheet from then on, and the scores as you enter them.", fonts().regular,
             size::Body, col::TextSecondary, true);
        ImGui::Dummy(ImVec2(0, 4));
        if (secondaryButton("Prepare the plate", ImVec2(0, 34))) app.openPlate(kind);
        endCard();
        endPage();
        return;
    }
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
    int& sel = app.scoreSelected();
    if (!s->find(sel)) sel = s->coupons.front().id;
    const ImU32 held = tint(col::Ok), partly = tint(col::Warn), fell = tint(col::Error);
    beginCard("map", ImVec2(left_w, 0), 18, col::Panel, col::Border);
    {
        text("The plate, from above", fonts().semibold, size::Section, col::Text);
        text("Turn the plate until its L marker matches the one here. Settings stay hidden while you score, so they "
             "can't sway you.",
             fonts().regular, size::Help, col::TextMuted, true);
        bool& mirrored = app.scoreMirrored();
        ImGui::Checkbox("It printed mirrored: no turn makes the L match", &mirrored);
        ImGui::Dummy(ImVec2(0, 2));
        const int c = plateMap(s->layout, sel, mirrored, ImGui::GetContentRegionAvail().x, [&](int id) {
            const fruit::CouponResult* r = s->find(id);
            if (!r || !r->recorded) return CellLook{col::Active, col::TextSecondary};
            return CellLook{r->ok ? held : r->partial ? partly : fell, r->ok ? col::Ok : r->partial ? col::Warn : col::Error};
        });
        if (c) sel = c;
        swatch(held, "Held");
        swatch(partly, "Partly");
        swatch(fell, "Fell off");
        swatch(col::Active, "Not scored");
        ImGui::NewLine();
        if (mirrored)
            text("A mirrored print means the printer's image orientation is off: check it on the Calibrate page "
                 "before printing models.",
                 fonts().regular, size::Help, col::Warn, true);
    }
    endCard();
    ImGui::SameLine(0, 16);
    // The selected coupon's score, a draft until saved. It starts again from
    // what is kept whenever the coupon or the scores change.
    static std::string draft_kind;
    static int draft_for = -1, draft_rev = -1;
    static App::Outcome outcome = App::Outcome::None;
    static int scar = 0;
    static char notes[160];
    const fruit::CouponResult* r = s->find(sel);
    if (draft_for != sel || draft_rev != app.scoresRevision() || draft_kind != kind) {
        draft_for = sel;
        draft_rev = app.scoresRevision();
        draft_kind = kind;
        outcome = !r || !r->recorded ? App::Outcome::None
                  : r->ok            ? App::Outcome::Held
                  : r->partial       ? App::Outcome::Partly
                                     : App::Outcome::FellOff;
        scar = r ? r->scar : 0;
        std::snprintf(notes, sizeof notes, "%s", r ? r->notes.c_str() : "");
    }
    beginCard("coupon", ImVec2(kSideW, 0), 18, col::Panel, col::Border);
    text(strf("Coupon %d", sel).c_str(), fonts().semibold, size::Section + 1, col::Text);
    if (const PlateLayout::Cell* cell = cellOf(s->layout, sel))
        text(strf("Row %d, column %d, counting from the L", cell->row + 1, cell->col + 1).c_str(), fonts().regular,
             size::Help, col::TextMuted);
    ImGui::Dummy(ImVec2(0, 4));
    text("Did it stay on?", fonts().semibold, size::Body, col::Text);
    const struct {
        App::Outcome o;
        const char* label;
    } outs[] = {{App::Outcome::Held, "Held"}, {App::Outcome::Partly, "Partly"}, {App::Outcome::FellOff, "Fell off"}};
    for (int i = 0; i < 3; ++i) {
        ImGui::PushID(i);
        if (segmentButton(outs[i].label, outcome == outs[i].o)) outcome = outs[i].o;
        ImGui::PopID();
        if (i < 2) ImGui::SameLine(0, 4);
    }
    const char* hint = outcome == App::Outcome::Held     ? "Held: every tip is still attached."
                       : outcome == App::Outcome::Partly  ? "Partly: some tips let go."
                       : outcome == App::Outcome::FellOff ? "Fell off: detached or missing. Look on the film for "
                                                            "cured bits."
                                                          : "Held: every tip still on. Partly: some let go. Fell off: "
                                                            "gone, so look on the film for cured bits.";
    text(hint, fonts().regular, size::Help, col::TextMuted, true);
    ImGui::Dummy(ImVec2(0, 6));
    text("Mark left on the part", fonts().semibold, size::Body, col::Text);
    const bool can_mark = outcome != App::Outcome::FellOff;
    static const char* const kMarks[] = {"1: can’t see it", "2: faint, only under raking light", "3: a visible mark",
                                         "4: a clear dent or bump", "5: a pit or torn skin"};
    ImGui::BeginDisabled(!can_mark);
    for (int m = 1; m <= 5; ++m) {
        ImGui::PushID(100 + m);
        if (segmentButton(std::to_string(m).c_str(), scar == m && can_mark)) scar = scar == m ? 0 : m;
        ImGui::PopID();
        if (m < 5) ImGui::SameLine(0, 4);
    }
    ImGui::EndDisabled();
    text(!can_mark                ? "Nothing to mark: this coupon fell off."
         : scar >= 1 && scar <= 5 ? kMarks[scar - 1]
                                  : "Look under a low, raking light, always at the same magnification.",
         fonts().regular, size::Help, col::TextMuted, true);
    ImGui::Dummy(ImVec2(0, 6));
    text("Notes", fonts().semibold, size::Body, col::Text);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    ImGui::InputTextWithHint("##notes", "Optional", notes, sizeof notes);
    ImGui::Dummy(ImVec2(0, 8));
    if (primaryButton("Save and go to the next coupon", ImVec2(ImGui::GetContentRegionAvail().x, 38),
                      outcome != App::Outcome::None)) {
        const int id = sel;
        app.setScore(id, outcome, can_mark ? scar : 0, notes);
        if (const PlateScores* now = app.scores()) sel = nextToScore(*now, id);
    }
    if (outcome == App::Outcome::None) tooltip("Choose whether it held first.");
    endCard();
    endPage();
}

}  // namespace gui::screens
