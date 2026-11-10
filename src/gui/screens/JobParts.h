// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Pieces the job screen shares: panel layout, the 3D view with the tools
// around it (canvas "FruitSlicer: tools around the 3D view"), the Job panel,
// and each place's panels (defined in that place's file).

#include <imgui.h>

#include <functional>

#include "Navigation.h"

namespace gui {
class App;
class Document;
}  // namespace gui

namespace gui::screens::parts {

// Layout. The view fills the window between the app rail and the Job panel;
// the tools float over it: the rail at its left, a tool's panel beside the
// rail, the model tools along its foot, the view cube at its top right.
inline constexpr float kTopH = 56, kStatusH = 42, kJobW = 340;
inline constexpr float kRailX = 8, kToolPanelX = 104, kToolPanelW = 300;
inline constexpr float kToolPanelRight = kToolPanelX + kToolPanelW;  // from the view's left edge
inline constexpr float kToolsBarH = 96;                              // the model tools, with their margin
inline constexpr float kCubeW = 160;

// A structural panel: square, one background; item spacing set inside only.
// `scroll`: it scrolls, with a bar, when what is in it is taller.
void beginPanel(const char* id, ImVec2 size, ImU32 bg, ImVec2 padding, bool scroll = false);
void endPanel();
void edge(bool right, bool bottom);  // a border on the current window's right or bottom
void tooltip(const char* s);         // for the item just drawn, also when disabled

// A card floating over the 3D view (the rail, a tool's panel, the model
// tools): rounded, bordered, as tall as what is in it up to `max_h`, then it
// scrolls.
void beginFloat(const char* id, ImVec2 pos, float w, float max_h = 0, ImVec2 padding = ImVec2(16, 16));
void endFloat();

// The 3D view. `hint`, when given, is a line of help in a pill along its
// top; `overlay` draws on top (gets the view's column-major
// view-projection matrix).
using Overlay = std::function<void(ImVec2 origin, ImVec2 size, const float* view_proj)>;
void viewport3d(App& app, ImVec2 size, const char* hint = nullptr, const Overlay& overlay = {});

// Everything floating over the view in Prepare: the tool rail (or, while
// editing tips, the tip tools), the open tool's panel, the view cube, the
// model tools, the pills along the top, and Orient's results.
void workspaceOverlay(App& app, Document* doc, ImVec2 origin, ImVec2 size, const float* view_proj);
// The Job panel at the right in Prepare: printer and resin, the parts, the
// checks and hollows, and the main button (Make supports, Slice…).
void jobPanel(App& app, float h);
// Its Parts section: each part on and off the plate (a click selects one),
// removing one, and adding another (PartsList.cpp).
void partsList(App& app, Document* doc);
// The 3D view's part of it: a click on a part selects it, when there are
// several and no tool takes the click (PartsList.cpp).
void partsPick(App& app, Document& doc, ImVec2 origin, ImVec2 size);

// Supports: the tool's panel, the checks and problems (in the Job panel),
// the view's marks, and the pill choosing what the view shows.
void supportsPanel(App& app, Document& doc);
void supportChecks(App& app, Document& doc);
void supportsOverlay(App& app, Document& doc, ImVec2 origin, ImVec2 size, const float* view_proj);
void supportsViewPill(App& app, Document& doc);
// Where Supports' banner (out of date, doesn't fit) sits over the view; false
// when none shows.
bool supportsBanner(App& app, const Document& doc, ImVec2 origin, ImVec2 size, ImVec2* min, ImVec2* max);
// Editing tips by hand: the panel (with the selected tip) and the view's marks.
void tipEditPanel(App& app, Document& doc);
void tipEditSelected(App& app, Document& doc);
void tipEditOverlay(App& app, Document& doc, ImVec2 origin, ImVec2 size, const float* view_proj);
// The handles that place the model (Move, and Rotate): arrows that move it,
// rings that turn it; the plate's edge red while it doesn't fit.
void placeOverlay(App& app, Document& doc, ImVec2 origin, ImVec2 size, const float* view_proj);
// What a place needs before it can do its work, and the way there.
void needCard(App& app, Document& doc, Need need, ImVec2 size);
// The advanced settings page.
void supportsAdvanced(App& app, Document& doc, ImVec2 size);

// Layers mode: the layers with this print's side panel; the side panel
// alone; the "saved" page, for the file just written.
void layersMode(App& app, Document& doc, ImVec2 size);
void printSide(App& app, Document& doc, float h);
void savedView(App& app, Document& doc, ImVec2 size);
bool showingSaved(const Document& doc);
// Dialogs, drawn with the other dialogs.
void orientationQuestion(App& app);  // "save before checking the image orientation?"
void sendDialog(App& app);           // "Send to the printer" over Wi-Fi
void unsavedDialog(App& app);        // "Save … before closing?"
void relinkDialog(App& app);         // "Where is … now?"

// Rotate: the search, how it sits now, the sides people see; the results.
void orientPanel(App& app, Document& doc);
void orientResults(App& app, Document& doc, ImVec2 size);
// Hollow: the tool's panel, what prints after it (in the Job panel), and the
// view's marks (holes, and placing holes by hand).
void hollowPanel(App& app, Document& doc);
void hollowApplyArea(App& app, Document& doc);  // the button that applies it, pinned under the panel
void hollowOutcomesList(App& app, Document& doc);
void hollowOverlay(App& app, Document& doc, ImVec2 origin, ImVec2 size, const float* view_proj);

}  // namespace gui::screens::parts
