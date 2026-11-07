// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Widgets.h"

#include <algorithm>
#include <cctype>
#include <cmath>

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
