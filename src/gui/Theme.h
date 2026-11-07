// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Design tokens from the UI kit (canvas page "UI kit"): one dark look, one
// accent for actions, colours that each mean one thing.

#include <imgui.h>

namespace gui {

namespace col {
// Surfaces, back to front.
inline constexpr ImU32 App = IM_COL32(0x11, 0x13, 0x17, 255);
inline constexpr ImU32 Viewport = IM_COL32(0x0E, 0x10, 0x14, 255);
inline constexpr ImU32 Rail = IM_COL32(0x0E, 0x10, 0x14, 255);
inline constexpr ImU32 Panel = IM_COL32(0x18, 0x1B, 0x21, 255);
inline constexpr ImU32 Card = IM_COL32(0x1F, 0x23, 0x2B, 255);
inline constexpr ImU32 Input = IM_COL32(0x23, 0x27, 0x31, 255);
inline constexpr ImU32 Active = IM_COL32(0x2B, 0x30, 0x3B, 255);
inline constexpr ImU32 Border = IM_COL32(0x2A, 0x2F, 0x39, 255);
inline constexpr ImU32 BorderStrong = IM_COL32(0x34, 0x3A, 0x46, 255);
// Text.
inline constexpr ImU32 Text = IM_COL32(0xE8, 0xEA, 0xED, 255);
inline constexpr ImU32 TextSecondary = IM_COL32(0xC3, 0xC8, 0xD2, 255);
inline constexpr ImU32 TextMuted = IM_COL32(0xA4, 0xAB, 0xB8, 255);
inline constexpr ImU32 Link = IM_COL32(0xFF, 0xB2, 0x7A, 255);
// Action: the next step and the selection, nothing else.
inline constexpr ImU32 Accent = IM_COL32(0xFF, 0x8F, 0x3F, 255);
inline constexpr ImU32 AccentHover = IM_COL32(0xFF, 0xA0, 0x5C, 255);
inline constexpr ImU32 OnAccent = IM_COL32(0x1A, 0x12, 0x08, 255);
// Status: only for problems, always with an icon.
inline constexpr ImU32 Ok = IM_COL32(0x5B, 0xD6, 0x9A, 255);
inline constexpr ImU32 Warn = IM_COL32(0xF2, 0xB8, 0x4B, 255);
inline constexpr ImU32 Error = IM_COL32(0xFF, 0x6B, 0x6B, 255);
inline constexpr ImU32 OkBg = IM_COL32(0x16, 0x3A, 0x2A, 255);
inline constexpr ImU32 WarnBg = IM_COL32(0x2A, 0x24, 0x12, 255);
inline constexpr ImU32 WarnBorder = IM_COL32(0x5A, 0x4C, 0x1E, 255);
inline constexpr ImU32 ErrorBg = IM_COL32(0x2A, 0x16, 0x16, 255);
// Structure in the 3D view.
inline constexpr ImU32 Model = IM_COL32(0xAE, 0xB6, 0xC4, 255);
}  // namespace col

// Type scale (pixels at 1x).
namespace size {
inline constexpr float PageTitle = 28;
inline constexpr float DialogTitle = 20;
inline constexpr float Section = 15;
inline constexpr float Body = 13;
inline constexpr float Help = 12;
inline constexpr float Label = 11;
}  // namespace size

struct Fonts {
    ImFont* regular = nullptr;
    ImFont* medium = nullptr;
    ImFont* semibold = nullptr;
    ImFont* bold = nullptr;
};

const Fonts& fonts();

// Loads the embedded Inter faces and applies the style. Call once after
// creating the ImGui context.
void applyTheme();

ImVec4 toVec4(ImU32 c);

}  // namespace gui
