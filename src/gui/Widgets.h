// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The UI kit's parts on top of Dear ImGui: buttons, cards, rail items,
// labels and key/value rows.

#include <string>

#include <imgui.h>

namespace gui {

// Text in a given face, size and colour. `wrap` wraps at the region edge.
void text(const char* s, ImFont* font, float size, ImU32 color, bool wrap = false);
void textMuted(const char* s, bool wrap = true);  // 13 px, muted
void title(const char* s);                         // page title, 28/700
void sectionTitle(const char* s);                  // 15/600
void groupLabel(const char* s);                    // 11/600 uppercase, muted

// One accent button per screen: it names the next thing to do.
bool primaryButton(const char* label, ImVec2 size = ImVec2(0, 0), bool enabled = true);
bool secondaryButton(const char* label, ImVec2 size = ImVec2(0, 0), bool enabled = true);
bool linkButton(const char* label);

// A rounded panel with a border, like the canvas cards. Content gets
// `padding` on every side.
void beginCard(const char* id, ImVec2 size, float padding = 16, ImU32 bg = 0, ImU32 border = 0);
void endCard();

// Left-rail item: icon above its label; returns true when clicked.
using IconFn = void (*)(ImDrawList*, ImVec2, float, ImU32);
bool railItem(const char* id, const char* label, IconFn icon, bool active);

// "Key ........ value" row across the available width.
void keyValue(const char* key, const std::string& value, ImU32 value_color = 0);

// Dashed rounded rectangle (drop zones).
void dashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c, float thickness = 1.5f, float dash = 6,
                float gap = 5);

}  // namespace gui
