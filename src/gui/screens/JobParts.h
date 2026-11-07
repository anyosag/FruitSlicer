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
// `scroll`: it scrolls, with a bar, when what is in it is taller.
void beginPanel(const char* id, ImVec2 size, ImU32 bg, ImVec2 padding, bool scroll = false);
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
// Editing tips by hand: the panels and the view's overlay.
void tipEditLeft(App& app, Document& doc, float h);
void tipEditRight(App& app, Document& doc, float h);
void tipEditOverlay(App& app, Document& doc, ImVec2 origin, ImVec2 size, const float* view_proj);
// The advanced settings page, in place of the whole step.
void supportsAdvanced(App& app, Document& doc, ImVec2 size);

// Layers: the whole body of the step.
void layersView(App& app, Document& doc, ImVec2 size);

// Print: the whole body of the step, and the "save before checking the
// image orientation?" question (drawn with the other dialogs).
void printView(App& app, Document& doc, ImVec2 size);
void orientationQuestion(App& app);
void sendDialog(App& app);  // "Send to the printer" over Wi-Fi
void unsavedDialog(App& app);  // "Save … before closing?"
void relinkDialog(App& app);   // "Where is … now?"
bool showingSaved(const Document& doc);  // the "saved" page, for the file just written
// In place of the Layers or Print step when the supports no longer match the model.
void staleSupports(App& app, ImVec2 size);

// Orient.
void orientLeft(App& app, Document& doc, float h);
void orientRight(App& app, Document& doc, float h);
void orientResults(App& app, Document& doc, ImVec2 size);
// Orient · Hollows: the panels and the view's overlay (hole marks, and
// placing holes by hand).
void hollowLeft(App& app, Document& doc, float h);
void hollowRight(App& app, Document& doc, float h);
void hollowOverlay(App& app, Document& doc, ImVec2 origin, ImVec2 size, const float* view_proj);

}  // namespace gui::screens::parts
