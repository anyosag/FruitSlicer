// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Slicing settings and Layer height (canvas "Slicing settings"): every
// setting the slice and its print file use, where each comes from, what has
// been checked; and what each layer height takes in light and time, and what
// it changes in the print.
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "App.h"
#include "Format.h"
#include "Orient.h"
#include "SlicingSettings.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"

namespace gui::screens::parts {

namespace {

constexpr float kChipW = 84, kChipH = 19;

struct ChipLook {
    ImU32 fg, bg, border;
};

ChipLook lookOf(ValueFrom f) {
    switch (f) {
        case ValueFrom::File: return {IM_COL32(0x8C, 0xB8, 0xF2, 255), IM_COL32(0x16, 0x20, 0x2C, 255), IM_COL32(0x2B, 0x45, 0x66, 255)};
        case ValueFrom::Yours: return {col::Accent, IM_COL32(0x2A, 0x1D, 0x12, 255), IM_COL32(0x5A, 0x3A, 0x1E, 255)};
        case ValueFrom::Printer: return {IM_COL32(0xC9, 0xCD, 0xD4, 255), IM_COL32(0x26, 0x2A, 0x31, 255), IM_COL32(0x30, 0x34, 0x3C, 255)};
        case ValueFrom::Checked: return {IM_COL32(0x7F, 0xD4, 0xA0, 255), IM_COL32(0x15, 0x29, 0x1E, 255), IM_COL32(0x2B, 0x5A, 0x3E, 255)};
        case ValueFrom::NotChecked: return {IM_COL32(0xE8, 0xB0, 0x6A, 255), IM_COL32(0x33, 0x27, 0x0F, 255), IM_COL32(0x6A, 0x4E, 0x1F, 255)};
        case ValueFrom::Missing:
        case ValueFrom::OutOfDate: return {IM_COL32(0xFF, 0x9B, 0x9B, 255), IM_COL32(0x33, 0x1A, 0x1C, 255), IM_COL32(0x6A, 0x2D, 0x31, 255)};
        default: return {col::TextMuted, col::Panel, col::BorderStrong};
    }
}

const ImU32 kTestedFg = IM_COL32(0x7F, 0xD4, 0xA0, 255);
const ImU32 kModeledFg = IM_COL32(0x8C, 0xB8, 0xF2, 255);
const ImU32 kAmber = IM_COL32(0xE8, 0xB0, 0x6A, 255);
const ImU32 kRed = IM_COL32(0xFF, 0x9B, 0x9B, 255);

// A rounded tag at the cursor, `w` wide (0: fits its words).
void tagAt(ImVec2 p, const char* s, const ChipLook& c, float w = 0) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushFont(fonts().semibold, size::Label - 0.5f);
    const ImVec2 ts = ImGui::CalcTextSize(s);
    if (w <= 0) w = ts.x + 14;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + kChipH), c.bg, kChipH / 2);
    dl->AddRect(p, ImVec2(p.x + w, p.y + kChipH), c.border, kChipH / 2);
    dl->AddText(ImVec2(p.x + std::round((w - ts.x) / 2), p.y + std::round((kChipH - ts.y) / 2)), c.fg, s);
    ImGui::PopFont();
}

float tagWidth(const char* s) {
    ImGui::PushFont(fonts().semibold, size::Label - 0.5f);
    const float w = ImGui::CalcTextSize(s).x + 14;
    ImGui::PopFont();
    return w;
}

void wrapped(const std::string& s, ImFont* f, float sz, ImU32 c, float w) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    text(s.c_str(), f, sz, c, true);
    ImGui::PopTextWrapPos();
}

void caps(const char* s, ImU32 c = col::TextMuted) { text(s, fonts().semibold, size::Label, c); }

// A tick, a cross or an open ring, then the words.
void mark(int state, ImVec2 c) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (state > 0) {
        dl->AddLine(ImVec2(c.x - 4, c.y), ImVec2(c.x - 1, c.y + 3), col::Ok, 2);
        dl->AddLine(ImVec2(c.x - 1, c.y + 3), ImVec2(c.x + 5, c.y - 4), col::Ok, 2);
    } else if (state < 0) {
        dl->AddLine(ImVec2(c.x - 4, c.y - 4), ImVec2(c.x + 4, c.y + 4), kRed, 2);
        dl->AddLine(ImVec2(c.x + 4, c.y - 4), ImVec2(c.x - 4, c.y + 4), kRed, 2);
    } else {
        dl->AddCircle(c, 4.6f, kAmber, 16, 1.6f);
    }
}

void markLine(int state, const std::string& s, float w, ImU32 color = col::TextSecondary) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    mark(state, ImVec2(p.x + 7, p.y + 8));
    ImGui::SetCursorScreenPos(ImVec2(p.x + 22, p.y));
    wrapped(s, fonts().regular, size::Help, state < 0 ? kRed : color, w - 22);
}

// ---- Slicing settings ----

void settingRow(App& app, const SettingRow& r, float w, bool used) {
    ImGui::PushID(r.label.c_str());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddLine(p, ImVec2(p.x + w, p.y), IM_COL32(0x26, 0x2A, 0x31, 255));
    ImGui::Dummy(ImVec2(0, 3));
    const float y = ImGui::GetCursorPosY();
    text(r.label.c_str(), fonts().regular, size::Help, col::TextMuted);
    if (r.label == "Layer height" && !used) {
        ImGui::SameLine(0, 10);
        ImGui::SetCursorPosY(y);
        if (linkButton("Change ›")) app.showLayerHeight(true);
    }
    tagAt(ImVec2(p.x + w - kChipW, p.y + 4), valueFromWord(r.from), lookOf(r.from), kChipW);
    if (!r.was.empty()) {
        wrapped("Was " + r.was + " when the layers were cut", fonts().regular, size::Help, kAmber, w);
    }
    wrapped(r.value, fonts().semibold, size::Body, r.from == ValueFrom::Missing ? kRed : col::Text, w - 6);
    if (!r.help.empty()) wrapped(r.help, fonts().regular, size::Help, col::TextMuted, w - 6);
    ImGui::Dummy(ImVec2(0, 3));
    ImGui::PopID();
}

void group(App& app, const SettingGroup& g, float w, bool used) {
    ImGui::PushID(g.title.c_str());
    const float y = ImGui::GetCursorPosY();
    caps(g.title.c_str());
    if (!used) {
        const char* link = g.link == "Settings" ? "Printer ›" : g.link == "Resins" ? "Resins ›"
                           : g.link == "Supports"                              ? "Supports ›"
                                                                                : "Layer height ›";
        ImGui::PushFont(fonts().semibold, size::Body);
        const float lw = ImGui::CalcTextSize(link).x;
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + w - lw, y - 1));
        if (linkButton(link)) {
            if (g.link == "Settings") {
                app.showSlicing(false);
                app.setScreen(Screen::Printer);
            } else if (g.link == "Resins") {
                app.showSlicing(false);
                app.setScreen(Screen::Resins);
            } else if (g.link == "Supports") {
                app.showSlicing(false);
                app.goToStep(Step::Supports);
            } else {
                app.showLayerHeight(true);
            }
        }
    }
    ImGui::Dummy(ImVec2(0, 2));
    for (const SettingRow& r : g.rows) settingRow(app, r, w, used);
    ImGui::Dummy(ImVec2(0, 10));
    ImGui::PopID();
}

void legend(const std::vector<std::pair<ValueFrom, const char*>>& items) {
    const float y = ImGui::GetCursorPosY();
    text("Where each value comes from:", fonts().regular, size::Help, col::TextMuted);
    for (const auto& [f, words] : items) {
        ImGui::SameLine(0, 12);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        tagAt(ImVec2(p.x, p.y - 2), valueFromWord(f), lookOf(f), kChipW);
        ImGui::Dummy(ImVec2(kChipW, 1));
        ImGui::SameLine(0, 6);
        ImGui::SetCursorPosY(y);
        text(words, fonts().regular, size::Help, col::TextMuted);
    }
}

}  // namespace

void slicingSettings(App& app, Document& doc, ImVec2 size) {
    beginPanel("slicing_page", ImVec2(0, size.y), col::App, ImVec2(28, 18));
    const bool used = app.slicingUsed();
    const SliceSettingsView v = sliceSettingsView(doc, app.printerSetup(), app.jobResin(), used ? app.cutWith() : nullptr);
    const float full = ImGui::GetContentRegionAvail().x;

    // ---- Header ----
    if (linkButton(used ? "‹ Back to the layers" : "‹ Back to the job")) app.showSlicing(false);
    ImGui::Dummy(ImVec2(0, 2));
    const float ty = ImGui::GetCursorPosY();
    text(used ? "Settings these layers used" : "Slicing settings", fonts().bold, size::DialogTitle + 2, col::Text);
    {
        const std::size_t n = doc.partCount();
        const std::string sub =
            used ? strf("%s · the layers were cut with these. A value changed since says what it was.", doc.name.c_str())
                 : strf("%s · %zu part%s. Everything the layers and the print file are made from.", doc.name.c_str(), n,
                        n == 1 ? "" : "s");
        text(sub.c_str(), fonts().regular, size::Body, col::TextMuted);
    }
    if (!used) {
        const float right = ImGui::GetCursorStartPos().x + full;
        ImGui::PushFont(fonts().bold, size::Body);
        const float sw = ImGui::CalcTextSize("Slice").x + 56;
        const float fw = v.fix.empty() ? 0 : ImGui::CalcTextSize(v.fix.c_str()).x + 36;
        ImGui::PopFont();
        float x = right - sw - 4;
        if (!v.fix.empty()) x -= fw + 10;
        // The switch: whether Slice shows this page first.
        bool ask = app.preferences().settings_before_slice;
        ImGui::PushFont(fonts().regular, size::Body);
        const float cw = ImGui::CalcTextSize("Show these before each slice").x + 34;
        ImGui::PopFont();
        ImGui::SetCursorPos(ImVec2(x - cw - 18, ty + 10));
        ImGui::PushFont(fonts().regular, size::Body);
        if (ImGui::Checkbox("Show these before each slice", &ask)) app.setSettingsBeforeSlice(ask);
        ImGui::PopFont();
        ImGui::SetCursorPos(ImVec2(x, ty + 2));
        if (!v.fix.empty()) {
            secondaryButton("Slice", ImVec2(sw, 38), false);
            ImGui::SameLine(0, 10);
            if (primaryButton(v.fix.c_str(), ImVec2(fw, 38))) {
                app.showSlicing(false);
                app.supportsNow();
            }
        } else if (primaryButton("Slice", ImVec2(sw, 38), v.waits_for.empty())) {
            app.sliceNow();
        }
        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ty + 52));
    }
    ImGui::Dummy(ImVec2(0, 4));

    // ---- What slicing waits for ----
    if (!v.waits_for.empty() || !v.missing.empty()) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool blocks = !v.waits_for.empty();
        const std::string line = blocks ? v.waits_for + (v.missing.empty() ? "" : " " + v.missing) : v.missing;
        ImGui::PushFont(fonts().regular, size::Body);
        const float th = ImGui::CalcTextSize(line.c_str(), nullptr, false, full - 260).y;
        ImGui::PopFont();
        const float h = std::max(36.0f, th + 18);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + full, p.y + h), blocks ? IM_COL32(0x2A, 0x17, 0x19, 255) : col::WarnBg, 10);
        dl->AddRect(p, ImVec2(p.x + full, p.y + h), blocks ? IM_COL32(0x5C, 0x2A, 0x2F, 255) : col::WarnBorder, 10);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 14, p.y + 9));
        text(blocks ? "Slicing waits." : "Saving waits.", fonts().semibold, size::Body, col::Text);
        ImGui::SameLine(0, 10);
        wrapped(line, fonts().regular, size::Body, col::TextSecondary, full - 200);
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 8));
        ImGui::Dummy(ImVec2(0, 0));
    }

    legend({{ValueFrom::File, "as the file has it"},
            {ValueFrom::Yours, "set by you"},
            {ValueFrom::Default, "FruitSlicer's own"},
            {ValueFrom::Printer, "fixed by the printer"}});
    ImGui::Dummy(ImVec2(0, 8));

    // ---- Three columns ----
    const float gap = 26;
    const float cw = std::floor((full - 2 * gap) / 3);
    const float checks_h = 26 + 18 * static_cast<float>(std::max(v.checked.size(), v.not_checked.size())) + 22;
    const float ch = std::max(160.0f, ImGui::GetContentRegionAvail().y - checks_h - 12);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::App);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, col::App);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 3));
    ImGui::BeginChild("slicing_col_a", ImVec2(cw, ch));
    group(app, v.groups[0], cw - 14, used);
    group(app, v.groups[1], cw - 14, used);
    ImGui::EndChild();
    ImGui::SameLine(0, gap);
    ImGui::BeginChild("slicing_col_b", ImVec2(cw, ch));
    group(app, v.groups[2], cw - 14, used);
    ImGui::EndChild();
    ImGui::SameLine(0, gap);
    ImGui::BeginChild("slicing_col_c", ImVec2(cw, ch));
    group(app, v.groups[3], cw - 14, used);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    // ---- Checked, and not checked yet ----
    ImGui::Dummy(ImVec2(0, 4));
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float h = checks_h;
        dl->AddRectFilled(p, ImVec2(p.x + full, p.y + h), col::Panel, 12);
        dl->AddRect(p, ImVec2(p.x + full, p.y + h), col::Border, 12);
        const float half = std::floor((full - 32) / 2);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 16, p.y + 10));
        ImGui::BeginGroup();
        caps("CHECKED", kTestedFg);
        for (const SliceCheck& c : v.checked) markLine(c.state, c.text, half - 20);
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(ImVec2(p.x + 16 + half + 16, p.y + 10));
        ImGui::BeginGroup();
        caps("NOT CHECKED YET", kAmber);
        for (const SliceCheck& c : v.not_checked) markLine(0, c.text, half - 20);
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
        ImGui::Dummy(ImVec2(0, 0));
    }
    endPanel();
}

// ---- Layer height ----

namespace {

// The stair steps a face at `nz` (|n·z|) gets from `h` mm layers, 600 × real
// size, with the face dashed over them.
void stairPicture(ImVec2 p, ImVec2 sz, double h, double nz, ImU32 border) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), col::Panel, 10);
    dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), border, 10);
    const float scale = 600;  // px per mm
    const float x0 = p.x + 14, y0 = p.y + sz.y - 14, top = p.y + 12;
    // The face: rises `dz` over `dx`, with its normal at |n·z| = nz from up, so it leans
    // atan(√(1 − nz²)/nz)... from the plate: its slope from horizontal is acos(nz).
    const double slope = std::acos(std::clamp(nz, 0.05, 0.9995));
    const double run_per_rise = 1.0 / std::tan(slope);
    const float rise = y0 - top;
    const float step = static_cast<float>(h * scale);
    // Layers sampled at their middles: each layer's edge is where the face is half way up it.
    ImU32 fill = IM_COL32(0xAE, 0xB6, 0xC4, 220);
    for (float yb = y0; yb > top + 0.5f; yb -= step) {
        const float yt = std::max(top, yb - step);
        const float mid = (yb + yt) / 2;
        const float edge = x0 + static_cast<float>((y0 - mid) * run_per_rise);
        dl->AddRectFilled(ImVec2(std::min(edge, p.x + sz.x - 10), yt), ImVec2(p.x + sz.x - 10, yb), fill);
    }
    // The face, dashed.
    const ImVec2 a(x0, y0), b(x0 + static_cast<float>(rise * run_per_rise), top);
    const float len = std::hypot(b.x - a.x, b.y - a.y);
    for (float t = 0; t < len; t += 7) {
        const float t1 = std::min(len, t + 4);
        dl->AddLine(ImVec2(a.x + (b.x - a.x) * t / len, a.y + (b.y - a.y) * t / len),
                    ImVec2(a.x + (b.x - a.x) * t1 / len, a.y + (b.y - a.y) * t1 / len), col::Accent, 1.5f);
    }
}

std::string mmText(double h) { return strf("%.2f mm", h); }

std::string againstNow(double s, double now) {
    const double d = s - now;
    if (std::abs(d) < 30) return "—";
    return (d < 0 ? "−" : "+") + hoursMinutes(std::abs(d));
}

// Light against layer height: the tested points, filled; the modeled ones, rings; the curve, dashed.
void lightChart(const std::vector<TestedLight>& tested, const std::vector<HeightChoice>& rows, ImVec2 p, ImVec2 sz) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const LightCurve curve = lightCurve(tested);
    double top_s = 1;
    for (const TestedLight& t : tested) top_s = std::max(top_s, t.exposure);
    for (const HeightChoice& c : rows) top_s = std::max(top_s, c.light.exposure);
    top_s = std::ceil(top_s * 1.2);
    const float x0 = p.x + 34, x1 = p.x + sz.x - 8, y0 = p.y + sz.y - 18, y1 = p.y + 8;
    const double h_max = 0.10;
    auto X = [&](double h) { return x0 + static_cast<float>(h / h_max) * (x1 - x0); };
    auto Y = [&](double s) { return y0 - static_cast<float>(s / top_s) * (y0 - y1); };
    ImGui::PushFont(fonts().regular, size::Label);
    for (int s = 0; s <= static_cast<int>(top_s); ++s) {
        const float y = Y(s);
        dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), IM_COL32(0x26, 0x2A, 0x31, 255));
        const std::string l = s == 0 ? "0 s" : std::to_string(s);
        dl->AddText(ImVec2(x0 - 8 - ImGui::CalcTextSize(l.c_str()).x, y - 7), col::TextMuted, l.c_str());
    }
    for (double h : {0.02, 0.04, 0.06, 0.08, 0.10}) {
        const std::string l = h >= 0.1 ? "0.10 mm" : strf("%.2f", h);
        const float w = ImGui::CalcTextSize(l.c_str()).x;
        dl->AddText(ImVec2(std::min(X(h) - w / 2, x1 - w), y0 + 3), col::TextMuted, l.c_str());
    }
    ImGui::PopFont();
    if (curve.ok) {
        ImVec2 last(X(0.005), Y(curve.at(0.005)));
        int i = 0;
        for (double h = 0.0075; h <= h_max + 1e-9; h += 0.0025, ++i) {
            const ImVec2 now(X(h), Y(curve.at(h)));
            if (i % 2 == 0) dl->AddLine(last, now, kModeledFg, 1.6f);
            last = now;
        }
    }
    for (const HeightChoice& c : rows)
        if (c.light.kind == LightAt::Kind::Modeled) {
            dl->AddCircleFilled(ImVec2(X(c.h), Y(c.light.exposure)), 4, col::Panel);
            dl->AddCircle(ImVec2(X(c.h), Y(c.light.exposure)), 4, kModeledFg, 16, 1.6f);
        }
    ImGui::PushFont(fonts().semibold, size::Label);
    for (const TestedLight& t : tested) {
        if (t.layer_height > h_max + 1e-9) continue;
        const ImVec2 c(X(t.layer_height), Y(t.exposure));
        dl->AddCircleFilled(c, 5, kTestedFg);
        const std::string l = strf("%g s", t.exposure);
        dl->AddText(ImVec2(c.x - ImGui::CalcTextSize(l.c_str()).x / 2, c.y - 21), kTestedFg, l.c_str());
    }
    ImGui::PopFont();
}

}  // namespace

void layerHeight(App& app, Document& doc, ImVec2 size) {
    beginPanel("layer_height_page", ImVec2(0, size.y), col::App, ImVec2(28, 18));
    const PrintShape& shape = app.printShapeNow();
    const std::vector<TestedLight> tested = testedLight(doc.print.settings, app.jobResin());
    const double now_h = printLayerHeight(doc);
    std::vector<HeightChoice> rows;
    for (double h : layerHeightChoices()) rows.push_back(heightChoice(doc, shape, tested, h));
    if (std::none_of(rows.begin(), rows.end(), [&](const HeightChoice& c) { return std::abs(c.h - now_h) < 1e-6; })) {
        rows.push_back(heightChoice(doc, shape, tested, now_h));
        std::sort(rows.begin(), rows.end(), [](const HeightChoice& a, const HeightChoice& b) { return a.h < b.h; });
    }
    double& pick = app.layerHeightPick();
    if (!(pick > 0)) pick = now_h;
    const HeightChoice* picked = nullptr;
    const HeightChoice* current = nullptr;
    for (const HeightChoice& c : rows) {
        if (std::abs(c.h - pick) < 1e-6) picked = &c;
        if (std::abs(c.h - now_h) < 1e-6) current = &c;
    }
    if (!picked) picked = current;
    const bool same = std::abs(picked->h - now_h) < 1e-6;
    const float full = ImGui::GetContentRegionAvail().x;
    const double pixel_um = std::max(doc.printer.pixelX(), doc.printer.pixelY()) * 1000;

    // ---- Header ----
    if (linkButton("‹ Back to slicing settings")) app.showLayerHeight(false);
    ImGui::Dummy(ImVec2(0, 2));
    const float ty = ImGui::GetCursorPosY();
    text("Layer height", fonts().bold, size::DialogTitle + 2, col::Text);
    text(strf("%s · %.1f mm tall%s. What each height takes in light and time, and what it changes in the print.",
              doc.name.c_str(), shape.top, doc.allOnPlate() ? "" : " with the raft")
             .c_str(),
         fonts().regular, size::Body, col::TextMuted);
    {
        const std::string keep = "Keep " + mmText(now_h);
        const std::string use = "Use " + mmText(picked->h) + (picked->supports_change ? " and remake supports" : "");
        ImGui::PushFont(fonts().bold, size::Body);
        const float uw = ImGui::CalcTextSize(use.c_str()).x + 36, kw = ImGui::CalcTextSize(keep.c_str()).x + 32;
        ImGui::PopFont();
        const float right = ImGui::GetCursorStartPos().x + full;
        ImGui::SetCursorPos(ImVec2(right - uw - kw - 14, ty + 2));
        if (secondaryButton(keep.c_str(), ImVec2(kw, 38))) app.showLayerHeight(false);
        ImGui::SameLine(0, 10);
        if (primaryButton(use.c_str(), ImVec2(uw, 38), !same)) app.useLayerHeight(picked->h, picked->supports_change);
        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ty + 52));
    }
    ImGui::Dummy(ImVec2(0, 2));

    // ---- One height, or (not yet) finer only where it shows ----
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGui::PushFont(fonts().semibold, size::Help);
        const char* a = "One height for the whole print";
        const char* b = "Finer only where it shows";
        const float aw = ImGui::CalcTextSize(a).x + 36, bw = ImGui::CalcTextSize(b).x + 36 + tagWidth("Not yet") + 6;
        const float h = 32;
        dl->AddRectFilled(p, ImVec2(p.x + aw + bw + 10, p.y + h), col::Panel, 9);
        dl->AddRect(p, ImVec2(p.x + aw + bw + 10, p.y + h), col::Border, 9);
        dl->AddRectFilled(ImVec2(p.x + 3, p.y + 3), ImVec2(p.x + 3 + aw, p.y + h - 3), col::Active, 7);
        dl->AddText(ImVec2(p.x + 3 + 18, p.y + 9), col::Text, a);
        dl->AddText(ImVec2(p.x + 7 + aw + 18, p.y + 9), IM_COL32(0x5F, 0x66, 0x72, 255), b);
        ImGui::PopFont();
        tagAt(ImVec2(p.x + 7 + aw + bw - tagWidth("Not yet") - 10, p.y + 6.5f), "Not yet",
              {IM_COL32(0x7C, 0x82, 0x8D, 255), col::Panel, IM_COL32(0x30, 0x34, 0x3C, 255)});
        ImGui::SetCursorScreenPos(ImVec2(p.x + 7 + aw, p.y));
        ImGui::InvisibleButton("finer_tab", ImVec2(bw, h));
        tooltip("Not available yet: a file whose layers changed height printed as a thin sheet on your printer.");
        ImGui::SetCursorScreenPos(ImVec2(p.x + aw + bw + 26, p.y + 2));
        wrapped("Off for now: a file whose layers changed height printed as a thin sheet on your printer. It comes "
                "back once it is built and a test print works.",
                fonts().regular, size::Help, col::TextMuted, std::min(560.0f, full - aw - bw - 30));
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 12));
        ImGui::Dummy(ImVec2(0, 0));
    }

    // ---- Left: the heights side by side, and the light ----
    const float lw = std::floor(full * 0.62f), rw = full - lw - 28;
    const float body_h = ImGui::GetContentRegionAvail().y;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::App);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, col::App);
    ImGui::BeginChild("lh_left", ImVec2(lw, body_h));
    {
        const float w = lw - 14;
        const float cols[] = {0, 120, 166, 280, 392, 472, 516};
        const char* heads[] = {"HEIGHT", "LAYERS", "LIGHT", "PRINT TIME", "VS NOW", "STEPS", "FLAT FACES OFF"};
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float row_h = 38, head_h = 30;
        const float table_h = head_h + row_h * static_cast<float>(rows.size());
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + head_h), col::Panel, 12, ImDrawFlags_RoundTop);
        dl->AddRect(p, ImVec2(p.x + w, p.y + table_h), col::Border, 12);
        ImGui::PushFont(fonts().semibold, size::Label);
        for (int i = 0; i < 7; ++i) dl->AddText(ImVec2(p.x + 14 + cols[i] * (w - 28) / 600, p.y + 9), col::TextMuted, heads[i]);
        ImGui::PopFont();
        double longest = 1;
        for (const HeightChoice& c : rows) longest = std::max(longest, c.seconds);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const HeightChoice& c = rows[i];
            const float y = p.y + head_h + row_h * static_cast<float>(i);
            const bool sel = &c == picked;
            const bool is_now = &c == current;
            ImGui::SetCursorScreenPos(ImVec2(p.x, y));
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::InvisibleButton("row", ImVec2(w, row_h))) pick = c.h;
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            if (sel) {
                dl->AddRectFilled(ImVec2(p.x + 1, y), ImVec2(p.x + w - 1, y + row_h), IM_COL32(0x2A, 0x21, 0x15, 255),
                                  i + 1 == rows.size() ? 12.0f : 0.0f, ImDrawFlags_RoundBottom);
                dl->AddRectFilled(ImVec2(p.x + 1, y), ImVec2(p.x + 4, y + row_h), col::Accent);
            } else if (hov) {
                dl->AddRectFilled(ImVec2(p.x + 1, y), ImVec2(p.x + w - 1, y + row_h), IM_COL32(0x1C, 0x1F, 0x26, 255));
            }
            dl->AddLine(ImVec2(p.x, y), ImVec2(p.x + w, y), col::Border);
            auto at = [&](int k) { return p.x + 14 + cols[k] * (w - 28) / 600; };
            const float ty2 = y + 11;
            // Height, with its radio mark.
            const ImVec2 rc(at(0) + 6, y + row_h / 2);
            dl->AddCircle(rc, 6, sel ? col::Accent : col::TextMuted, 16, 1.5f);
            if (sel) dl->AddCircleFilled(rc, 3, col::Accent);
            ImGui::PushFont(sel ? fonts().bold : fonts().semibold, size::Body);
            const std::string hs = mmText(c.h);
            dl->AddText(ImVec2(at(0) + 18, ty2), col::Text, hs.c_str());
            const float hw = ImGui::CalcTextSize(hs.c_str()).x;
            ImGui::PopFont();
            if (is_now) tagAt(ImVec2(at(0) + 22 + hw, y + 9.5f), "Now", {col::TextSecondary, col::Active, col::Active});
            ImGui::PushFont(fonts().regular, size::Help);
            dl->AddText(ImVec2(at(1), ty2), col::TextSecondary, thousands(c.layers).c_str());
            // Light, tested or modeled.
            if (c.light.kind == LightAt::Kind::Unknown) {
                dl->AddText(ImVec2(at(2), ty2), col::TextMuted, "not known");
            } else {
                const std::string ls = strf("%g s", c.light.exposure);
                dl->AddText(ImVec2(at(2), ty2), col::TextSecondary, ls.c_str());
                const float lsw = ImGui::CalcTextSize(ls.c_str()).x;
                ImGui::PopFont();
                const bool t = c.light.kind == LightAt::Kind::Tested;
                tagAt(ImVec2(at(2) + lsw + 6, y + 9.5f), t ? "Tested" : "Modeled",
                      t ? lookOf(ValueFrom::Checked) : lookOf(ValueFrom::File));
                ImGui::PushFont(fonts().regular, size::Help);
            }
            // Time, with a bar.
            const float bar = std::max(3.0f, static_cast<float>(c.seconds / longest) * 34);
            dl->AddRectFilled(ImVec2(at(3), y + row_h / 2 - 3), ImVec2(at(3) + bar, y + row_h / 2 + 3),
                              sel ? col::Accent : IM_COL32(0x5A, 0x61, 0x70, 255), 3);
            dl->AddText(ImVec2(at(3) + 42, ty2), sel ? col::Text : col::TextSecondary, hoursMinutes(c.seconds).c_str());
            const std::string an = againstNow(c.seconds, current ? current->seconds : c.seconds);
            dl->AddText(ImVec2(at(4), ty2), an[0] == '-' || an.rfind("−", 0) == 0 ? col::Ok : an == "—" ? col::TextMuted : kAmber,
                        an.c_str());
            dl->AddText(ImVec2(at(5), ty2), c.step_um > pixel_um ? kAmber : col::TextSecondary,
                        shape.sloped > 0 ? strf("%.0f µm", c.step_um).c_str() : "none");
            dl->AddText(ImVec2(at(6), ty2), c.flat_off > 0 ? kAmber : col::TextSecondary,
                        c.flat_off == 0 ? "none"
                                        : strf("%d of %zu, ≤ %.0f µm", c.flat_off, shape.flat_levels.size(), c.flat_off_um).c_str());
            ImGui::PopFont();
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + table_h + 12));

        // The light by layer height.
        const ImVec2 q = ImGui::GetCursorScreenPos();
        const float card_h = 178;
        dl->AddRectFilled(q, ImVec2(q.x + w, q.y + card_h), col::Panel, 12);
        dl->AddRect(q, ImVec2(q.x + w, q.y + card_h), col::Border, 12);
        ImGui::SetCursorScreenPos(ImVec2(q.x + 14, q.y + 10));
        caps("LIGHT BY LAYER HEIGHT");
        const float chart_w = std::min(420.0f, w * 0.6f);
        lightChart(tested, rows, ImVec2(q.x + 14, q.y + 30), ImVec2(chart_w, card_h - 40));
        ImGui::SetCursorScreenPos(ImVec2(q.x + 28 + chart_w, q.y + 28));
        ImGui::BeginGroup();
        const float tw = w - chart_w - 42;
        std::string notes;
        for (const TestedLight& t : tested) {
            if (!notes.empty()) notes += ", ";
            notes += mmText(t.layer_height) + " " + t.note;
        }
        {
            const ImVec2 d = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(d.x + 4, d.y + 8), 4.5f, kTestedFg);
            ImGui::SetCursorScreenPos(ImVec2(d.x + 14, d.y));
            wrapped("Tested with your resin" + (notes.empty() ? std::string(": none yet.") : ": " + notes + "."),
                    fonts().regular, size::Help, col::TextSecondary, tw - 14);
        }
        {
            const ImVec2 d = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddCircle(ImVec2(d.x + 4, d.y + 8), 3.8f, kModeledFg, 16, 1.6f);
            ImGui::SetCursorScreenPos(ImVec2(d.x + 14, d.y));
            const LightCurve curve = lightCurve(tested);
            wrapped(curve.ok ? strf("Modeled on the curve through your %s and %s results. Not printed yet.",
                                    mmText(tested.front().layer_height).c_str(), mmText(tested.back().layer_height).c_str())
                             : std::string("Modeled: needs two tested heights, the thicker needing more light."),
                    fonts().regular, size::Help, col::TextSecondary, tw - 14);
        }
        // Add a time you've printed with, at the picked height.
        if (app.jobResin()) {
            static double add_s = 0;
            static double add_for = -1;
            if (add_for != picked->h) {
                add_s = picked->light.exposure;
                add_for = picked->h;
            }
            ImGui::Dummy(ImVec2(0, 2));
            text(strf("Printed well at %s? Its time:", mmText(picked->h).c_str()).c_str(), fonts().regular, size::Help,
                 col::TextMuted);
            numberField("##tested_s", &add_s, 64, "%g", 0.1, 120);
            ImGui::SameLine(0, 6);
            text("s", fonts().regular, size::Help, col::TextMuted);
            ImGui::SameLine(0, 10);
            const bool is_tested = picked->light.kind == LightAt::Kind::Tested;
            if (secondaryButton(is_tested ? "Save as tested##t" : "Mark as tested##t", ImVec2(0, 28), add_s > 0))
                app.addTestedLight(picked->h, add_s, "your print");
        }
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + card_h + 10));
        ImGui::Dummy(ImVec2(0, 0));
        wrapped(strf("Steps: the deepest stair step on a sloped face; amber when deeper than one pixel across (%.0f µm), "
                     "so it can show.",
                     pixel_um),
                fonts().regular, size::Help, col::TextMuted, w - 40);
        wrapped("Flat faces off: how many flat faces fall between two layers and print a little higher or lower.",
                fonts().regular, size::Help, col::TextMuted, w - 40);
        wrapped("Print time: each height's light, your waits and 2.4 s of movement per layer.", fonts().regular, size::Help,
                col::TextMuted, w - 40);
    }
    ImGui::EndChild();
    ImGui::SameLine(0, 28);

    // ---- Right: the picked height ----
    ImGui::BeginChild("lh_right", ImVec2(rw, body_h));
    {
        const float w = rw - 14;
        const float y = ImGui::GetCursorPosY();
        text(hoursMinutes(picked->seconds).c_str(), fonts().bold, 28, col::Text);
        if (current) {
            const double d = picked->seconds - current->seconds;
            ImGui::SameLine(0, 12);
            ImGui::SetCursorPosY(y + 12);
            if (std::abs(d) >= 30)
                text((hoursMinutes(std::abs(d)) + (d < 0 ? " less" : " more")).c_str(), fonts().semibold, size::Body,
                     d < 0 ? col::Ok : kAmber);
            ImGui::SameLine(0, 10);
            ImGui::SetCursorPosY(y + 12);
            const std::string after = same ? strf("now · %s layers", thousands(picked->layers).c_str())
                                           : strf("than %s now · %s layers", hoursMinutes(current->seconds).c_str(),
                                                  thousands(picked->layers).c_str());
            text(after.c_str(), fonts().regular, size::Body, col::TextMuted);
        }
        ImGui::Dummy(ImVec2(0, 4));
        // The stair steps, now and picked.
        if (shape.sloped > 0) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float pw = std::floor((w - 10) / 2), ph = 112;
            stairPicture(p, ImVec2(pw, ph), now_h, shape.steepest, col::Border);
            stairPicture(ImVec2(p.x + pw + 10, p.y), ImVec2(pw, ph), picked->h, shape.steepest,
                         same ? col::Border : IM_COL32(0x3A, 0x2E, 0x1E, 255));
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + ph + 4));
            text(strf("%s now: steps %.0f µm", mmText(now_h).c_str(), current ? current->step_um : 0).c_str(), fonts().regular,
                 size::Help, col::TextSecondary);
            ImGui::SameLine(0, 0);
            ImGui::SetCursorScreenPos(ImVec2(p.x + pw + 10, p.y + ph + 4));
            text(strf("%s: steps %.0f µm", mmText(picked->h).c_str(), picked->step_um).c_str(), fonts().regular, size::Help,
                 col::TextSecondary);
            text(strf("The steepest sloped face, %.0f° from flat, from the side at 600 × real size. Dashed: the model's face.",
                      std::acos(std::clamp(shape.steepest, 0.0, 1.0)) * 180 / 3.14159265)
                     .c_str(),
                 fonts().regular, size::Label, col::TextMuted);
        } else {
            wrapped("No sloped faces: walls and flat faces only, so no stair steps at any height.", fonts().regular, size::Help,
                    col::TextSecondary, w);
        }
        ImGui::Dummy(ImVec2(0, 4));

        // What the picked height changes.
        auto detail = [&](const char* label, int state, const std::string& s, ImU32 c = col::Text) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddLine(p, ImVec2(p.x + w, p.y), IM_COL32(0x26, 0x2A, 0x31, 255));
            ImGui::Dummy(ImVec2(0, 3));
            const float yy = ImGui::GetCursorPosY();
            text(label, fonts().regular, size::Help, col::TextMuted);
            ImGui::SetCursorPosY(yy);
            ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x + 86);
            if (state != 2) {
                const ImVec2 m = ImGui::GetCursorScreenPos();
                mark(state, ImVec2(m.x + 6, m.y + 8));
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 20);
            }
            wrapped(s, fonts().regular, size::Help, c, w - 110);
            ImGui::Dummy(ImVec2(0, 2));
        };
        {
            const LightAt& l = picked->light;
            if (l.kind == LightAt::Kind::Tested)
                detail("Light", 1, strf("%g s at %s, tested with your resin (%s).", l.exposure, mmText(picked->h).c_str(), l.note.c_str()));
            else if (l.kind == LightAt::Kind::Modeled)
                detail("Light", 0, strf("%g s, modeled from your tested times. Not printed at %s yet.", l.exposure,
                                        mmText(picked->h).c_str()));
            else
                detail("Light", -1, strf("Not known at %s. The time keeps today's exposure.", mmText(picked->h).c_str()));
        }
        if (!doc.supports.result) detail("Supports", 0, "None yet: they're made before slicing.");
        else if (picked->supports_change)
            detail("Supports", -1,
                   strf("Out of date at %s. The plate holds what the long exposures print, so no tips go there: %.2f mm of "
                        "the part now, %.2f mm at %s. Using it remakes them.",
                        mmText(picked->h).c_str(), current ? current->held_mm : 0.0, picked->held_mm, mmText(picked->h).c_str()));
        else
            detail("Supports", 1,
                   doc.anyOnPlate() ? std::string("Stay as they are, up to date: no tip moves.")
                                    : std::string("Stay as they are, up to date. Every part stands on a raft."));
        if (picked->raft > 0) {
            if (picked->raft_thin) {
                detail("Raft", -1,
                       strf("Too thin: the %d long-exposure layers reach %.2f mm, past the %.1f mm raft, so the long light "
                            "would reach the trunks' feet.",
                            picked->long_layers, picked->long_mm, picked->raft));
                ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x + 86);
                const double need = std::ceil(picked->long_mm * 10 - 1e-6) / 10;
                if (primaryButton(strf("Make the raft %.1f mm thick…", need).c_str(), ImVec2(0, 32))) app.thickenRaftFor(picked->h);
                const HeightChoice* fits = nullptr;
                for (const HeightChoice& c : rows)
                    if (!c.raft_thin && c.h > now_h + 1e-6) fits = &c;
                if (fits) {
                    ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x + 86);
                    if (secondaryButton(strf("Or %s, where they reach %.2f mm", mmText(fits->h).c_str(), fits->long_mm).c_str(),
                                        ImVec2(0, 32)))
                        pick = fits->h;
                }
                ImGui::Dummy(ImVec2(0, 2));
            } else {
                detail("Raft", 1,
                       strf("The %d long-exposure layers reach %.2f mm, inside the %.1f mm raft.", picked->long_layers,
                            picked->long_mm, picked->raft));
            }
        } else if (doc.allOnPlate()) {
            detail("Raft", 2, "None: the parts stand on the plate.");
        }
        if (std::abs(picked->h - kLayerHeight) < 1e-6)
            detail("Checks", 1, strf("Mid-air spots and the replay run on %g mm layers: the same as this print's.", kLayerHeight));
        else
            detail("Checks", 0,
                   strf("Mid-air spots and the replay run on %g mm layers, not on the %s ones this print would have.",
                        kLayerHeight, mmText(picked->h).c_str()));
        if (shape.area > 0) {
            if (shape.sloped > 0)
                detail("Slopes", 2,
                       strf("Steps %.0f µm deep%s, on %.1f %% of the surface, %.1f to %.1f mm up.", picked->step_um,
                            same || !current ? "" : strf(" instead of %.0f µm", current->step_um).c_str(),
                            shape.sloped / shape.area * 100, shape.sloped_lo, shape.sloped_hi));
            detail("Flat faces", 2,
                   shape.flat_levels.empty()
                       ? std::string("None.")
                       : picked->flat_off == 0
                             ? strf("All %zu land on a layer.", shape.flat_levels.size())
                             : strf("%d of %zu fall between layers and print up to %.0f µm off.", picked->flat_off,
                                    shape.flat_levels.size(), picked->flat_off_um));
            detail("Walls", 2, strf("%.0f %% of the surface: no steps at any height.", shape.walls / shape.area * 100));
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    endPanel();
}

}  // namespace gui::screens::parts
