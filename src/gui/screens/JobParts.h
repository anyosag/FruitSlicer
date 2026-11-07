// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Pieces the job's steps share: panel layout, the 3D view, and each step's
// panels (defined in that step's file).

#include <imgui.h>

#include <functional>

namespace gui {
class App;
class Document;
}  // namespace gui

namespace gui::screens::parts {

inline constexpr float kTopH = 60, kStatusH = 42, kLeftW = 316, kRightW = 297;

// A structural panel: square, one background; item spacing set inside only.
void beginPanel(const char* id, ImVec2 size, ImU32 bg, ImVec2 padding);
void endPanel();
void edge(bool right, bool bottom);  // a border on the current window's right or bottom
void tooltip(const char* s);         // for the item just drawn, also when disabled

// The 3D view. `hint`, when given, replaces the view buttons with a line of
// help; `overlay` draws a step's own controls on top (gets the view's
// column-major view-projection matrix), and `presets` shows the view buttons.
using Overlay = std::function<void(ImVec2 origin, ImVec2 size, const float* view_proj)>;
void viewport3d(App& app, ImVec2 size, const char* hint = nullptr, const Overlay& overlay = {}, bool presets = true);

// Supports.
void supportsLeft(App& app, Document& doc, float h);
void supportsRight(App& app, Document& doc, float h);
void supportsOverlay(App& app, Document& doc, ImVec2 origin, ImVec2 size, const float* view_proj);

// Layers: the whole body of the step.
void layersView(App& app, Document& doc, ImVec2 size);

// Orient.
void orientLeft(App& app, Document& doc, float h);
void orientRight(App& app, Document& doc, float h);
void orientResults(App& app, Document& doc, ImVec2 size);

}  // namespace gui::screens::parts
