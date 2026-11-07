// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Widgets.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <unordered_map>

#include <imgui_internal.h>

#include "Icons.h"
#include "Theme.h"

namespace gui {

void text(const char* s, ImFont* font, float size, ImU32 color, bool wrap) {
    ImGui::PushFont(font, size);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    if (wrap) ImGui::TextWrapped("%s", s);
    else ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void textMuted(const char* s, bool wrap) { text(s, fonts().regular, size::Body, col::TextMuted, wrap); }

void title(const char* s) { text(s, fonts().bold, size::PageTitle, col::Text); }

void sectionTitle(const char* s) { text(s, fonts().semibold, size::Section, col::Text); }

void groupLabel(const char* s) {
    std::string up(s);
    for (char& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    text(up.c_str(), fonts().semibold, size::Label, col::TextMuted);
}

namespace {

bool styledButton(const char* label, ImVec2 size, bool enabled, ImU32 bg, ImU32 hover, ImU32 fg, ImU32 border,
                  ImFont* font) {
    ImGui::PushFont(font, size::Body);
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, hover);
    ImGui::PushStyleColor(ImGuiCol_Text, fg);
    ImGui::PushStyleColor(ImGuiCol_Border, border);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16, 10));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, border ? 1.0f : 0.0f);
    if (!enabled) ImGui::BeginDisabled();
    const bool clicked = ImGui::Button(label, size);
    if (!enabled) ImGui::EndDisabled();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
    ImGui::PopFont();
    return clicked && enabled;
}

}  // namespace

bool primaryButton(const char* label, ImVec2 size, bool enabled) {
    return styledButton(label, size, enabled, col::Accent, col::AccentHover, col::OnAccent, 0, fonts().bold);
}

bool secondaryButton(const char* label, ImVec2 size, bool enabled) {
    return styledButton(label, size, enabled, col::Input, col::Active, col::Text, col::BorderStrong,
                        fonts().semibold);
}

bool linkButton(const char* label) {
    ImGui::PushFont(fonts().semibold, size::Body);
    const ImVec2 sz = ImGui::CalcTextSize(label, nullptr, true);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, sz);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 c = hovered ? IM_COL32(0xFF, 0xD2, 0xB0, 255) : col::Link;
    dl->AddText(p, c, label);
    dl->AddLine(ImVec2(p.x, p.y + sz.y + 1), ImVec2(p.x + sz.x, p.y + sz.y + 1), c, 1.0f);
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::PopFont();
    return clicked;
}

void beginCard(const char* id, ImVec2 size, float padding, ImU32 bg, ImU32 border) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg ? bg : col::Panel);
    ImGui::PushStyleColor(ImGuiCol_Border, border ? border : col::Border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
    ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding |
                                    (size.y == 0 ? ImGuiChildFlags_AutoResizeY : 0));
}

void endCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

bool railItem(const char* id, const char* label, IconFn icon, bool active) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 sz(64, 56);
    const bool clicked = ImGui::InvisibleButton(id, sz);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active || hovered)
        dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), active ? col::Card : col::Panel, 10);
    const ImU32 c = active ? col::Text : col::TextMuted;
    icon(dl, ImVec2(p.x + (sz.x - 20) / 2, p.y + 9), 20, c);
    ImFont* f = active ? fonts().semibold : fonts().medium;
    ImGui::PushFont(f, size::Label);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x + (sz.x - ts.x) / 2, p.y + 34), c, label);
    ImGui::PopFont();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}

void keyValue(const char* key, const std::string& value, ImU32 value_color) {
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::PushFont(fonts().regular, size::Body);
    ImGui::PushStyleColor(ImGuiCol_Text, col::TextMuted);
    ImGui::TextUnformatted(key);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::PushFont(fonts().semibold, size::Body);
    const float vw = ImGui::CalcTextSize(value.c_str()).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, w - vw - ImGui::GetItemRectSize().x -
                                                                       ImGui::GetStyle().ItemSpacing.x));
    ImGui::PushStyleColor(ImGuiCol_Text, value_color ? value_color : col::Text);
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

bool numberField(const char* id, double* v, float width, const char* format, double lo, double hi) {
    // While the field is being edited its value lives here, so the commit
    // (which happens on the frame focus leaves) sees what was typed.
    static std::unordered_map<ImGuiID, double> pending;
    ImGui::PushID(id);
    const ImGuiID item = ImGui::GetID("##v");
    double& p = pending[item];
    if (ImGui::GetActiveID() != item) p = *v;
    ImGui::PushFont(fonts().regular, size::Body);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, col::Viewport);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, col::Viewport);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, col::Viewport);
    ImGui::PushStyleColor(ImGuiCol_Border, col::BorderStrong);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1);
    ImGui::SetNextItemWidth(width);
    ImGui::InputDouble("##v", &p, 0, 0, format);
    const bool commit = ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::IsItemActive()) {  // accent outline while typing
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), col::Accent, 6, 1.5f);
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);
    ImGui::PopFont();
    ImGui::PopID();
    if (!commit) return false;
    const double nv = std::clamp(p, lo, hi);
    if (nv == *v) return false;
    *v = nv;
    return true;
}

bool radioCard(const char* id, const char* title, const char* body, bool selected) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float pad = 12, mark = 16;
    ImGui::PushFont(fonts().regular, size::Body);
    const float text_w = w - pad * 2 - mark - 10;
    const float body_h = ImGui::CalcTextSize(body, nullptr, false, text_w).y;
    ImGui::PopFont();
    const float h = pad * 2 + 18 + body_h + 2;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::SetNextItemAllowOverlap();  // a field drawn on top (Custom size) stays usable
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + w, p.y + h);
    dl->AddRectFilled(p, q, selected ? IM_COL32(0x2A, 0x1E, 0x14, 255) : (hovered ? col::Card : col::Panel), 10);
    dl->AddRect(p, q, selected ? col::Accent : (hovered ? col::BorderStrong : col::Border), 10, selected ? 1.5f : 1.0f);
    const ImVec2 c(p.x + pad + mark / 2, p.y + pad + 9);
    dl->AddCircle(c, mark / 2 - 0.5f, selected ? col::Accent : col::TextMuted, 24, 1.5f);
    if (selected) dl->AddCircleFilled(c, mark / 2 - 4, col::Accent, 24);
    const float tx = p.x + pad + mark + 10;
    ImGui::PushFont(fonts().semibold, size::Body);
    dl->AddText(ImVec2(tx, p.y + pad + 1), col::Text, title);
    ImGui::PopFont();
    ImGui::PushFont(fonts().regular, size::Body);
    dl->AddText(nullptr, 0, ImVec2(tx, p.y + pad + 21), col::TextSecondary, body, nullptr, text_w);
    ImGui::PopFont();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}

bool segmentButton(const char* label, bool active) {
    ImGui::PushFont(active ? fonts().semibold : fonts().medium, size::Body);
    ImGui::PushStyleColor(ImGuiCol_Button, active ? col::Active : col::Panel);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col::Active);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, col::BorderStrong);
    ImGui::PushStyleColor(ImGuiCol_Text, active ? col::Text : col::TextSecondary);
    ImGui::PushStyleColor(ImGuiCol_Border, active ? col::BorderStrong : col::Border);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(11, 7));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1);
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
    ImGui::PopFont();
    return clicked;
}

bool chip(const char* id, const char* label, ImU32 dot, ImU32 fg, ImU32 bg, ImU32 border) {
    ImGui::PushFont(fonts().medium, size::Body);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const float dot_w = dot ? 14.0f : 0.0f;
    const ImVec2 sz(ts.x + 26 + dot_w, 30);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, sz);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), bg, sz.y / 2);
    if (border) dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), border, sz.y / 2);
    if (dot) dl->AddCircleFilled(ImVec2(p.x + 17, p.y + sz.y / 2), 3.5f, dot);
    dl->AddText(ImVec2(p.x + 13 + dot_w, p.y + (sz.y - ts.y) / 2), fg, label);
    ImGui::PopFont();
    return clicked;
}

bool stepPill(int number, const char* label, StepState state, bool enabled) {
    ImGui::PushFont(state == StepState::Current ? fonts().semibold : fonts().medium, size::Body);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const ImVec2 sz(ts.x + 54, 38);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(number);
    const bool clicked = ImGui::InvisibleButton("step", sz) && enabled;
    ImGui::PopID();
    const bool hovered = ImGui::IsItemHovered() && enabled;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (state == StepState::Current || hovered)
        dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), state == StepState::Current ? col::Active : col::Card,
                          sz.y / 2);
    const ImVec2 c(p.x + 19, p.y + sz.y / 2);
    char num[4];
    std::snprintf(num, sizeof num, "%d", number);
    if (state == StepState::Current) {
        dl->AddCircleFilled(c, 13, col::Accent, 32);
    } else if (state == StepState::Done) {
        dl->AddCircleFilled(c, 13, col::OkBg, 32);
    } else {
        dl->AddCircle(c, 12.5f, col::BorderStrong, 32, 1.0f);
    }
    const ImU32 fg = state == StepState::Current ? col::Text : (enabled ? col::TextSecondary : col::TextMuted);
    if (state == StepState::Done) {
        icon::check(dl, ImVec2(c.x - 7, c.y - 7), 14, col::Ok);
    } else {
        ImGui::PushFont(fonts().semibold, size::Help);
        const ImVec2 ns = ImGui::CalcTextSize(num);
        dl->AddText(ImVec2(c.x - ns.x / 2, c.y - ns.y / 2), state == StepState::Current ? col::OnAccent : fg, num);
        ImGui::PopFont();
    }
    dl->AddText(ImVec2(p.x + 40, p.y + (sz.y - ts.y) / 2), fg, label);
    ImGui::PopFont();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}

float keyBadgeWidth(const char* keys) {
    ImGui::PushFont(fonts().medium, size::Help);
    const float w = ImGui::CalcTextSize(keys).x + 14;
    ImGui::PopFont();
    return w;
}

void keyBadge(const char* keys) {
    ImGui::PushFont(fonts().medium, size::Help);
    const ImVec2 ts = ImGui::CalcTextSize(keys);
    const ImVec2 sz(ts.x + 14, 22);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(sz);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), col::Viewport, 5);
    dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), col::BorderStrong, 5);
    dl->AddText(ImVec2(p.x + 7, p.y + (sz.y - ts.y) / 2), col::TextSecondary, keys);
    ImGui::PopFont();
}

bool closeButton(const char* id) {
    const ImVec2 sz(36, 36);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, sz);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), hovered ? col::Active : col::Input, 8);
    dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), col::BorderStrong, 8);
    icon::cross(dl, ImVec2(p.x + 11, p.y + 11), 14, col::TextSecondary);
    return clicked;
}

void verdictRow(const char* id, Verdict v, const char* title, const char* body) {
    const ImU32 bg = v == Verdict::Error ? col::ErrorBg : (v == Verdict::Warn ? col::WarnBg : col::Panel);
    const ImU32 border = v == Verdict::Error ? IM_COL32(0x6B, 0x2A, 0x2A, 255)
                                             : (v == Verdict::Warn ? col::WarnBorder : col::Border);
    beginCard(id, ImVec2(0, 0), 10, bg, border);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 c(p.x + 10, p.y + 9);
    const ImU32 fg = v == Verdict::Error ? col::Error : (v == Verdict::Warn ? col::Warn : col::Ok);
    dl->AddCircleFilled(c, 10, v == Verdict::Error ? IM_COL32(0x4A, 0x1C, 0x1C, 255)
                                                   : (v == Verdict::Warn ? IM_COL32(0x4A, 0x3A, 0x12, 255) : col::OkBg),
                        24);
    if (v == Verdict::Ok) icon::check(dl, ImVec2(c.x - 6, c.y - 6), 12, fg);
    else if (v == Verdict::Error) icon::cross(dl, ImVec2(c.x - 5, c.y - 5), 10, fg);
    else {
        ImGui::PushFont(fonts().bold, size::Help);
        dl->AddText(ImVec2(c.x - 2, c.y - 7), fg, "!");
        ImGui::PopFont();
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 32);
    ImGui::BeginGroup();
    text(title, fonts().semibold, size::Body, col::Text);
    text(body, fonts().regular, size::Body, col::TextSecondary, true);
    ImGui::EndGroup();
    endCard();
}

void dashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c, float thickness, float dash, float gap) {
    auto line = [&](ImVec2 p, ImVec2 q) {
        const float len = std::hypot(q.x - p.x, q.y - p.y);
        if (len <= 0) return;
        const ImVec2 d((q.x - p.x) / len, (q.y - p.y) / len);
        for (float t = 0; t < len; t += dash + gap) {
            const float e = std::min(len, t + dash);
            dl->AddLine(ImVec2(p.x + d.x * t, p.y + d.y * t), ImVec2(p.x + d.x * e, p.y + d.y * e), c, thickness);
        }
    };
    line(a, ImVec2(b.x, a.y));
    line(ImVec2(b.x, a.y), b);
    line(b, ImVec2(a.x, b.y));
    line(ImVec2(a.x, b.y), a);
}

}  // namespace gui
