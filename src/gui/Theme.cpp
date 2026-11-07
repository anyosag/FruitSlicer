// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Theme.h"

#include <cstddef>
#include <cstdint>

extern const std::uint8_t kFontInter_regular[];
extern const std::size_t kFontInter_regular_size;
extern const std::uint8_t kFontInter_medium[];
extern const std::size_t kFontInter_medium_size;
extern const std::uint8_t kFontInter_semibold[];
extern const std::size_t kFontInter_semibold_size;
extern const std::uint8_t kFontInter_bold[];
extern const std::size_t kFontInter_bold_size;

namespace gui {

namespace {

Fonts g_fonts;

ImFont* addFont(const std::uint8_t* data, std::size_t size) {
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;  // static data, embedded in the binary
    cfg.OversampleH = 2;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<std::uint8_t*>(data), static_cast<int>(size),
                                                      size::Body, &cfg);
}

}  // namespace

const Fonts& fonts() { return g_fonts; }

ImVec4 toVec4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

void applyTheme() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    g_fonts.regular = addFont(kFontInter_regular, kFontInter_regular_size);
    g_fonts.medium = addFont(kFontInter_medium, kFontInter_medium_size);
    g_fonts.semibold = addFont(kFontInter_semibold, kFontInter_semibold_size);
    g_fonts.bold = addFont(kFontInter_bold, kFontInter_bold_size);
    io.FontDefault = g_fonts.regular;

    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.FontSizeBase = size::Body;
    s.WindowPadding = ImVec2(0, 0);
    s.WindowRounding = 0;
    s.WindowBorderSize = 0;
    s.ChildRounding = 10;
    s.ChildBorderSize = 1;
    s.FrameRounding = 8;
    s.FramePadding = ImVec2(12, 8);
    s.FrameBorderSize = 1;
    s.ItemSpacing = ImVec2(10, 8);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.PopupRounding = 10;
    s.PopupBorderSize = 1;
    s.ScrollbarSize = 10;
    s.ScrollbarRounding = 6;
    s.GrabRounding = 6;
    s.TabRounding = 6;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = toVec4(col::Text);
    c[ImGuiCol_TextDisabled] = toVec4(col::TextMuted);
    c[ImGuiCol_WindowBg] = toVec4(col::App);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = toVec4(col::Panel);
    c[ImGuiCol_Border] = toVec4(col::Border);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = toVec4(col::Input);
    c[ImGuiCol_FrameBgHovered] = toVec4(col::Active);
    c[ImGuiCol_FrameBgActive] = toVec4(col::Active);
    c[ImGuiCol_Button] = toVec4(col::Input);
    c[ImGuiCol_ButtonHovered] = toVec4(col::Active);
    c[ImGuiCol_ButtonActive] = toVec4(col::BorderStrong);
    c[ImGuiCol_Header] = toVec4(col::Active);
    c[ImGuiCol_HeaderHovered] = toVec4(col::Active);
    c[ImGuiCol_HeaderActive] = toVec4(col::BorderStrong);
    c[ImGuiCol_CheckMark] = toVec4(col::Accent);
    c[ImGuiCol_SliderGrab] = toVec4(col::Accent);
    c[ImGuiCol_SliderGrabActive] = toVec4(col::AccentHover);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = toVec4(col::BorderStrong);
    c[ImGuiCol_ScrollbarGrabHovered] = toVec4(col::TextMuted);
    c[ImGuiCol_Separator] = toVec4(col::Border);
    c[ImGuiCol_NavCursor] = toVec4(col::Accent);
    c[ImGuiCol_TextSelectedBg] = ImVec4(1.0f, 0.56f, 0.25f, 0.35f);
}

}  // namespace gui
