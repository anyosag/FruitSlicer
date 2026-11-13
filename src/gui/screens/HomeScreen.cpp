// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Home (canvas boards "Home · redesign" and "Home · redesign, first run"):
// the open job and its next step, or a place to start one; the jobs saved
// lately, each with a picture of how it sat; and what has been checked
// before a print, and what hasn't.
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "App.h"
#include "Format.h"
#include "Home.h"
#include "Icons.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/Screens.h"

namespace gui::screens {

namespace {

constexpr float kGap = 20;      // between the columns, and between cards
constexpr float kAsideW = 340;  // "Before you print" and "Print files"
constexpr float kJobGap = 14;   // between recent jobs' cards
constexpr float kJobMinW = 200;
constexpr float kJobPictureH = 116;
constexpr float kJobCardH = 236;
constexpr ImU32 kDropBg = IM_COL32(0x14, 0x17, 0x1C, 255);
constexpr ImU32 kRowBg = IM_COL32(0x1A, 0x1D, 0x23, 255);
constexpr ImU32 kRing = IM_COL32(0x3A, 0x40, 0x4D, 255);
constexpr ImU32 kDoneRing = IM_COL32(0x1E, 0x5A, 0x3E, 255);
constexpr ImU32 kReviewDot = IM_COL32(0x3D, 0x33, 0x15, 255);
constexpr ImU32 kPlateRing = IM_COL32(0x4A, 0x51, 0x60, 255);

ImTextureID texId(unsigned tex) { return static_cast<ImTextureID>(static_cast<intptr_t>(tex)); }

float textWidth(const char* s, ImFont* f, float sz) {
    ImGui::PushFont(f, sz);
    const float w = ImGui::CalcTextSize(s).x;
    ImGui::PopFont();
    return w;
}

// `s` broken into lines no wider than `w`, each centred on `cx` from `y`
// down (drawn only when `dl` is given). Returns the height.
float centredLines(ImDrawList* dl, const std::string& s, ImFont* f, float sz, ImU32 c, float cx, float y, float w) {
    ImGui::PushFont(f, sz);
    std::vector<std::string> lines;
    std::string line;
    std::size_t i = 0;
    while (i <= s.size()) {
        std::size_t j = s.find(' ', i);
        if (j == std::string::npos) j = s.size();
        const std::string word = s.substr(i, j - i);
        const std::string longer = line.empty() ? word : line + " " + word;
        if (!line.empty() && ImGui::CalcTextSize(longer.c_str()).x > w) {
            lines.push_back(line);
            line = word;
        } else {
            line = longer;
        }
        i = j + 1;
    }
    if (!line.empty()) lines.push_back(line);
    const float lh = ImGui::GetTextLineHeight() + 3;
    if (dl)
        for (std::size_t k = 0; k < lines.size(); ++k) {
            const float lw = ImGui::CalcTextSize(lines[k].c_str()).x;
            dl->AddText(ImVec2(std::floor(cx - lw / 2), y + static_cast<float>(k) * lh), c, lines[k].c_str());
        }
    ImGui::PopFont();
    return static_cast<float>(lines.size()) * lh;
}

void hand() {
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
}

// The page title, and on its right the drop hint and Open… (the dialog takes
// models and saved jobs alike).
void header(App& app, float w) {
    const ImVec2 start = ImGui::GetCursorScreenPos();
    title("Home");
    const ImVec2 after = ImGui::GetCursorScreenPos();
    const char* open = "Open…";
    const char* hint = "Drop a model or a job file anywhere in this window";
    const float bw = textWidth(open, fonts().semibold, size::Body) + 34;
    const float hw = textWidth(hint, fonts().regular, size::Body);
    const float right = start.x + w;
    ImGui::SetCursorScreenPos(ImVec2(right - bw, start.y - 2));
    if (secondaryButton(open, ImVec2(bw, 38))) app.openDialog();
    if (w > hw + bw + 200) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float hx = right - bw - 18 - hw;
        icon::upload(dl, ImVec2(hx - 24, start.y + 9), 16, col::TextMuted);
        ImGui::PushFont(fonts().regular, size::Body);
        dl->AddText(ImVec2(hx, start.y + 9), col::TextMuted, hint);
        ImGui::PopFont();
    }
    ImGui::SetCursorScreenPos(after);
}

// The open job's places, as pills that wrap: done (a tick), the next one
// (the accent) and the rest (their number).
void stepRow(const std::vector<StepChip>& chips) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float right = o.x + ImGui::GetContentRegionAvail().x, h = 34;
    float x = o.x, y = o.y;
    for (std::size_t i = 0; i < chips.size(); ++i) {
        const StepChip& c = chips[i];
        const bool done = c.state == StepChip::State::Done, next = c.state == StepChip::State::Next;
        ImFont* lf = next ? fonts().semibold : fonts().medium;
        const float lw = textWidth(c.label.c_str(), lf, size::Body);
        const float dw = c.detail.empty() ? 0 : textWidth(c.detail.c_str(), fonts().regular, size::Body);
        const float cw = 5 + 24 + 8 + lw + dw + 12;
        if (x + cw > right && x > o.x) x = o.x, y += h + 6;
        if (next) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cw, y + h), col::Active, h / 2);
        const ImVec2 dot(x + 5 + 12, y + h / 2);
        if (done) {
            dl->AddCircleFilled(dot, 12, col::OkBg, 24);
            dl->AddCircle(dot, 11.5f, kDoneRing, 24);
            icon::check(dl, ImVec2(dot.x - 6, dot.y - 6), 12, col::Ok);
        } else {
            if (next) dl->AddCircleFilled(dot, 12, col::Accent, 24);
            else dl->AddCircle(dot, 11.5f, kRing, 24);
            const std::string n = std::to_string(i + 1);
            ImGui::PushFont(fonts().bold, size::Help);
            const ImVec2 ns = ImGui::CalcTextSize(n.c_str());
            dl->AddText(ImVec2(dot.x - ns.x / 2, dot.y - ns.y / 2), next ? col::OnAccent : col::TextMuted, n.c_str());
            ImGui::PopFont();
        }
        ImGui::PushFont(lf, size::Body);
        const float ty = y + (h - ImGui::GetTextLineHeight()) / 2;
        dl->AddText(ImVec2(x + 37, ty), next ? col::Text : done ? col::TextSecondary : col::TextMuted, c.label.c_str());
        ImGui::PopFont();
        if (!c.detail.empty()) {
            ImGui::PushFont(fonts().regular, size::Body);
            dl->AddText(ImVec2(x + 37 + lw, ty), col::TextMuted, c.detail.c_str());
            ImGui::PopFont();
        }
        x += cw + 6;
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x, y + h));
    ImGui::Dummy(ImVec2(0, 0));
}

// The open job: its picture, name, where it stands, and Continue (the
// screen's one accent button).
void openNow(App& app, const Document& doc, float w) {
    beginCard("open_now", ImVec2(w, 0), 20);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float top = ImGui::GetCursorPosY();
    const float pw = 200, ph = 156;
    const bool beside = w >= 560;
    dl->AddRectFilled(p, ImVec2(p.x + pw, p.y + ph), col::Viewport, 10);
    if (const unsigned tex = app.openJobPicture()) {
        const float iw = std::min(pw - 2, (ph - 2) * 4 / 3), ih = iw * 3 / 4;
        const ImVec2 a(p.x + (pw - iw) / 2, p.y + (ph - ih) / 2);
        dl->AddImageRounded(texId(tex), a, ImVec2(a.x + iw, a.y + ih), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 9);
    }
    dl->AddRect(p, ImVec2(p.x + pw, p.y + ph), col::Border, 10);
    if (ImGui::InvisibleButton("picture", ImVec2(pw, ph))) app.setScreen(Screen::Job);
    hand();
    const float indent = beside ? pw + 20 : 0;
    if (beside) ImGui::SetCursorPosY(top);
    else ImGui::Dummy(ImVec2(0, 6));
    if (indent > 0) ImGui::Indent(indent);
    groupLabel("Open now");
    text(doc.name.c_str(), fonts().bold, size::DialogTitle, col::Text);
    text(openJobLine(doc).c_str(), fonts().regular, size::Body, col::TextMuted, true);
    ImGui::Dummy(ImVec2(0, 4));
    stepRow(stepChips(doc));
    ImGui::Dummy(ImVec2(0, 4));
    const ContinueTo next = continueTo(doc);
    const float bh = 40;
    const float by_screen = ImGui::GetCursorScreenPos().y;
    if (primaryButton(next.label.c_str(), ImVec2(textWidth(next.label.c_str(), fonts().bold, size::Body) + 36, bh)))
        app.continueJob();
    ImGui::SameLine(0, 10);
    const bool never_saved = doc.job_path.empty();
    const char* save = never_saved ? "Save job…" : "Save job";
    if (secondaryButton(save, ImVec2(textWidth(save, fonts().semibold, size::Body) + 32, bh), never_saved || doc.unsaved()))
        app.saveJob();
    const std::string sup = openJobSupports(doc);
    if (!sup.empty()) {
        const float sw = textWidth(sup.c_str(), fonts().regular, size::Body);
        ImGui::SameLine(0, 14);
        if (ImGui::GetContentRegionAvail().x < sw) {
            ImGui::NewLine();
            text(sup.c_str(), fonts().regular, size::Body, col::TextMuted, true);
        } else {
            // Drawn by hand: text after a button would sit on the button's text line.
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::PushFont(fonts().regular, size::Body);
            const float lh = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddText(ImVec2(at.x, by_screen + (bh - lh) / 2), col::TextMuted, sup.c_str());
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(sw, bh));
        }
    }
    if (indent > 0) ImGui::Unindent(indent);
    if (beside && ImGui::GetCursorPosY() < top + ph) {
        ImGui::SetCursorPosY(top + ph);
        ImGui::Dummy(ImVec2(0, 0));
    }
    endCard();
}

// No job open: a drop zone with Open model… (the accent) and the demo.
void startCard(App& app, float w, bool first) {
    beginCard("start", ImVec2(w, 0), 20);
    sectionTitle("Start a print");
    ImGui::Dummy(ImVec2(0, 4));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float zw = ImGui::GetContentRegionAvail().x, zh = first ? 300 : 230;
    dl->AddRectFilled(p, ImVec2(p.x + zw, p.y + zh), kDropBg, 12);
    dashedRect(dl, p, ImVec2(p.x + zw, p.y + zh), kRing, 2);
    const std::string t1 = "Drop an STL, OBJ or 3MF file here, or anywhere in this window";
    const std::string t2 = "FruitSlicer checks it, repairs what it can, places it on the plate, then walks you through "
                           "orienting, supporting and saving it.";
    const float tw = std::min(zw - 56, 520.0f), cx = p.x + zw / 2;
    const float h1 = centredLines(nullptr, t1, fonts().semibold, size::Section, 0, cx, 0, tw);
    const float h2 = centredLines(nullptr, t2, fonts().regular, size::Body, 0, cx, 0, std::min(tw, 460.0f));
    const float bh = 40, icon_h = 40;
    const float content = icon_h + 12 + h1 + 8 + h2 + 18 + bh;
    float y = p.y + std::max(16.0f, (zh - content) / 2);
    icon::cube(dl, ImVec2(cx - icon_h / 2, y), icon_h, col::TextMuted);
    y += icon_h + 12;
    y += centredLines(dl, t1, fonts().semibold, size::Section, col::Text, cx, y, tw) + 8;
    y += centredLines(dl, t2, fonts().regular, size::Body, col::TextMuted, cx, y, std::min(tw, 460.0f)) + 18;
    const char* open = "Open model…";
    const char* demo = "Try the demo bracket";
    const float ow = textWidth(open, fonts().bold, size::Body) + 36, dw = textWidth(demo, fonts().semibold, size::Body) + 32;
    ImGui::SetCursorScreenPos(ImVec2(std::floor(cx - (ow + 10 + dw) / 2), y));
    if (primaryButton(open, ImVec2(ow, bh))) app.openDialog();
    ImGui::SameLine(0, 10);
    if (secondaryButton(demo, ImVec2(dw, bh))) app.openDemo();
    ImGui::SetCursorScreenPos(ImVec2(p.x, std::max(p.y + zh, y + bh + 16)));
    ImGui::Dummy(ImVec2(zw, 0));
    endCard();
}

// A saved job's picture, fitted to its card's top; without one, its initial.
void jobPicture(App& app, ImDrawList* dl, const RecentJob& j, bool there, ImVec2 a, ImVec2 b) {
    const unsigned tex = there ? app.recentJobPicture(j) : 0;
    if (tex) {
        float ih = b.y - a.y - 4, iw = ih * 4 / 3;
        if (iw > b.x - a.x - 4) iw = b.x - a.x - 4, ih = iw * 3 / 4;
        const ImVec2 c((a.x + b.x) / 2, (a.y + b.y) / 2);
        dl->AddImage(texId(tex), ImVec2(c.x - iw / 2, c.y - ih / 2), ImVec2(c.x + iw / 2, c.y + ih / 2));
        return;
    }
    const ImVec2 c((a.x + b.x) / 2, (a.y + b.y) / 2);
    dl->AddCircleFilled(c, 20, col::Active, 32);
    const std::string initial = j.name.empty() ? "?" : std::string(1, static_cast<char>(std::toupper(
                                                                          static_cast<unsigned char>(j.name[0]))));
    ImGui::PushFont(fonts().bold, size::Section);
    const ImVec2 is = ImGui::CalcTextSize(initial.c_str());
    dl->AddText(ImVec2(c.x - is.x / 2, c.y - is.y / 2), col::TextSecondary, initial.c_str());
    ImGui::PopFont();
}

// The jobs saved lately (the open one is above), newest first, as cards.
void recentJobs(App& app, float w) {
    const auto& doc = app.document();
    std::vector<const RecentJob*> jobs;
    for (const RecentJob& j : app.recentJobs())
        if (!(doc && !doc->job_path.empty() && doc->job_path == j.path)) jobs.push_back(&j);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    {
        const ImVec2 t = ImGui::GetCursorScreenPos();
        sectionTitle("Recent jobs");
        if (jobs.size() > 1) {
            const char* order = "Newest first";
            ImGui::PushFont(fonts().regular, size::Body);
            dl->AddText(ImVec2(t.x + w - ImGui::CalcTextSize(order).x, t.y + 1), col::TextMuted, order);
            ImGui::PopFont();
        }
    }
    ImGui::Dummy(ImVec2(0, 4));
    if (jobs.empty()) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = 88;
        dashedRect(dl, p, ImVec2(p.x + w, p.y + h), col::BorderStrong, 1);
        dl->AddRectFilled(ImVec2(p.x + 20, p.y + 20), ImVec2(p.x + 84, p.y + 68), col::Viewport, 8);
        dl->AddRect(ImVec2(p.x + 20, p.y + 20), ImVec2(p.x + 84, p.y + 68), col::Border, 8);
        const char* s = "Nothing saved yet. Each job you save shows up here, with a picture of how it sits and how "
                        "far it got.";
        const float tw = w - 124;
        const ImVec2 ts = fonts().regular->CalcTextSizeA(size::Body, FLT_MAX, tw, s);
        dl->AddText(fonts().regular, size::Body, ImVec2(p.x + 104, p.y + (h - ts.y) / 2), col::TextMuted, s, nullptr,
                    tw);
        ImGui::Dummy(ImVec2(w, h));
        return;
    }
    const int cols = std::max(1, static_cast<int>((w + kJobGap) / (kJobMinW + kJobGap)));
    const float cw = (w - kJobGap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const std::time_t now = std::time(nullptr);
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const RecentJob& j = *jobs[i];
        const int r = static_cast<int>(i) / cols, c = static_cast<int>(i) % cols;
        const ImVec2 p(o.x + static_cast<float>(c) * (cw + kJobGap), o.y + static_cast<float>(r) * (kJobCardH + kJobGap));
        const ImVec2 q(p.x + cw, p.y + kJobCardH);
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(static_cast<int>(i));
        const bool there = std::filesystem::exists(j.path);
        const bool clicked = ImGui::InvisibleButton("job", ImVec2(cw, kJobCardH)) && there;
        const bool hovered = ImGui::IsItemHovered() && there;
        if (there) hand();
        dl->AddRectFilled(p, q, hovered ? col::Card : col::Panel, 12);
        dl->AddRectFilled(p, ImVec2(q.x, p.y + kJobPictureH), col::Viewport, 12, ImDrawFlags_RoundTop);
        dl->AddLine(ImVec2(p.x, p.y + kJobPictureH), ImVec2(q.x, p.y + kJobPictureH), col::Border);
        jobPicture(app, dl, j, there, p, ImVec2(q.x, p.y + kJobPictureH));
        dl->AddRect(p, q, col::Border, 12);
        dl->PushClipRect(p, ImVec2(q.x - 12, q.y), true);
        const float x = p.x + 12, tw = cw - 24;
        float y = p.y + kJobPictureH + 12;
        ImGui::PushFont(fonts().semibold, size::Body);
        dl->AddText(ImVec2(x, y), there ? col::Text : col::TextMuted, j.name.c_str());
        ImGui::PopFont();
        y += 22;
        const std::string state = there ? jobState(j) : std::string("Moved or deleted");
        const ImU32 dot = !there ? col::Warn : j.state == "placed" ? col::TextMuted : col::Ok;
        dl->AddCircleFilled(ImVec2(x + 4, y + 8), 4, dot, 12);
        ImGui::PushFont(fonts().regular, size::Body);
        dl->AddText(ImVec2(x + 14, y), col::TextSecondary, state.c_str());
        ImGui::PopFont();
        y += 22;
        const std::string facts = jobFacts(j);
        if (!facts.empty()) {
            dl->AddText(fonts().regular, size::Help, ImVec2(x, y), col::TextMuted, facts.c_str(), nullptr, tw);
            y += fonts().regular->CalcTextSizeA(size::Help, FLT_MAX, tw, facts.c_str()).y + 4;
        }
        if (!j.when.empty())
            dl->AddText(fonts().regular, size::Help, ImVec2(x, y), col::TextMuted, whenWords(j.when, now).c_str());
        dl->PopClipRect();
        if (clicked) {
            if (doc && doc->job_path == j.path) app.setScreen(Screen::Job);
            else app.openPath(j.path);
        }
        ImGui::PopID();
    }
    const int rows = (static_cast<int>(jobs.size()) + cols - 1) / cols;
    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + static_cast<float>(rows) * (kJobCardH + kJobGap) - kJobGap));
    ImGui::Dummy(ImVec2(w, 0));
}

// One row of "Before you print": its mark, what it says, and a link.
void checkRow(App& app, int i, const SetupCheck& c) {
    using S = SetupCheck::State;
    const bool review = c.state == S::Review;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, review ? col::WarnBg : kRowBg);
    ImGui::PushStyleColor(ImGuiCol_Border, review ? col::WarnBorder : col::Border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    ImGui::PushID(i);
    ImGui::BeginChild("check", ImVec2(-FLT_MIN, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 dot(p.x + 10, p.y + 10);
    if (c.state == S::Ok) {
        dl->AddCircleFilled(dot, 10, col::OkBg, 24);
        icon::check(dl, ImVec2(dot.x - 5, dot.y - 5), 10, col::Ok);
    } else {
        dl->AddCircleFilled(dot, 10, review ? kReviewDot : col::Input, 24);
        const char* mark = review ? "!" : "i";
        ImGui::PushFont(fonts().bold, size::Label);
        const ImVec2 ms = ImGui::CalcTextSize(mark);
        dl->AddText(ImVec2(dot.x - ms.x / 2, dot.y - ms.y / 2), review ? col::Warn : col::TextSecondary, mark);
        ImGui::PopFont();
    }
    ImGui::Indent(30);
    text(c.title.c_str(), fonts().semibold, size::Body, col::Text, true);
    text(c.detail.c_str(), fonts().regular, size::Body, col::TextMuted, true);
    if (!c.action.empty()) {
        ImGui::Dummy(ImVec2(0, 1));
        if (linkButton(c.action.c_str())) {
            switch (c.go) {
                case SetupCheck::Go::None: break;
                case SetupCheck::Go::Resins:
                    app.resinSelected() = app.newJobResin().id;
                    app.setScreen(Screen::Resins);
                    break;
                case SetupCheck::Go::OrientationCheck: app.setScreen(Screen::OrientationCheck); break;
                case SetupCheck::Go::Calibrate: app.setScreen(Screen::Calibrate); break;
            }
        }
    }
    ImGui::Unindent(30);
    ImGui::EndChild();
    ImGui::PopID();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// What has been checked before a print, and what hasn't: the printer, the
// print settings, the image orientation and the support values, then the
// resin's test plates.
void checksPanel(App& app, float w) {
    const Resin& resin = app.newJobResin();
    const auto sample = app.resinSample(resin);
    const SetupChecks sc = setupChecks(app.printer(), app.printerSetup(), resin, sample.get());
    beginCard("checks", ImVec2(w, 0), 20);
    {
        const ImVec2 t = ImGui::GetCursorScreenPos();
        sectionTitle("Before you print");
        const std::string n = strf("%d of %zu checked", sc.checked, sc.rows.size());
        ImGui::PushFont(fonts().regular, size::Body);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(t.x + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(n.c_str()).x, t.y + 1), col::TextMuted,
            n.c_str());
        ImGui::PopFont();
    }
    textMuted("What FruitSlicer has checked, and what it hasn't.");
    ImGui::Dummy(ImVec2(0, 4));
    for (std::size_t i = 0; i < sc.rows.size(); ++i) checkRow(app, static_cast<int>(i), sc.rows[i]);
    ImGui::Dummy(ImVec2(0, 2));
    // The test plates, measured on the resin or not.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float right = o.x + ImGui::GetContentRegionAvail().x, h = 26;
    float x = o.x, y = o.y;
    ImGui::PushFont(fonts().regular, size::Help);
    for (const SetupChecks::Plate& pl : sc.plates) {
        const std::string s = pl.label + (pl.measured ? " · measured" : " · not measured");
        const float pw = ImGui::CalcTextSize(s.c_str()).x + 22;
        if (x + pw > right && x > o.x) x = o.x, y += h + 6;
        if (pl.measured) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + pw, y + h), col::OkBg, h / 2);
        dl->AddRect(ImVec2(x, y), ImVec2(x + pw, y + h), pl.measured ? kDoneRing : kPlateRing, h / 2);
        dl->AddText(ImVec2(x + 11, y + (h - ImGui::GetTextLineHeight()) / 2), col::TextSecondary, s.c_str());
        x += pw + 6;
    }
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(o.x, y + h));
    ImGui::Dummy(ImVec2(0, 0));
    endCard();
}

void printFiles(App& app, float w) {
    beginCard("print_files", ImVec2(w, 0), 18);
    groupLabel("Print files");
    if (linkButton("Look inside a .goo file")) {
        app.toolsTab() = 0;
        app.setScreen(Screen::Tools);
    }
    if (linkButton("Compare two .goo files")) {
        app.toolsTab() = 1;
        app.setScreen(Screen::Tools);
    }
    endCard();
}

}  // namespace

void home(App& app) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(32, 28));
    ImGui::BeginChild("home", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    const float w = ImGui::GetContentRegionAvail().x;
    header(app, w);
    ImGui::Dummy(ImVec2(0, 12));
    // The checks beside the job and the recent ones, or below them when narrow.
    const bool beside = w >= 900;
    const float lw = beside ? w - kGap - kAsideW : w;
    const ImVec2 top = ImGui::GetCursorScreenPos();
    const auto& doc = app.document();
    if (doc && doc->partCount() > 0) openNow(app, *doc, lw);
    else startCard(app, lw, app.recentJobs().empty());
    ImGui::Dummy(ImVec2(0, 14));
    recentJobs(app, lw);
    float bottom = ImGui::GetCursorScreenPos().y;
    if (beside) ImGui::SetCursorScreenPos(ImVec2(top.x + lw + kGap, top.y));
    else ImGui::Dummy(ImVec2(0, 14));
    const float aw = beside ? kAsideW : w;
    ImGui::BeginGroup();
    checksPanel(app, aw);
    ImGui::Dummy(ImVec2(0, 10));
    printFiles(app, aw);
    ImGui::EndGroup();
    if (beside) {
        bottom = std::max(bottom, ImGui::GetCursorScreenPos().y);
        ImGui::SetCursorScreenPos(ImVec2(top.x, bottom));
        ImGui::Dummy(ImVec2(0, 0));
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

}  // namespace gui::screens
