// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The job's places: Model, Orient, Supports, Layers and Print. They are not
// steps: each opens at any time, in any order, and says what it needs
// before it can do its work (supports can be made straight after placing
// the model; Orient is there to help, never required).

namespace gui {

class Document;

enum class Step { Model, Orient, Supports, Layers, Print };

// What a place needs before it can do its work.
enum class Need {
    Nothing,
    Fit,            // a part doesn't fit the plate where it is (too big, or over the edge)
    Supports,       // no supports yet
    FreshSupports,  // the supports were made for an earlier placement or settings
    Layers,         // the layers aren't cut for these supports yet (the Layers place cuts them)
};
Need needFor(const Document& d, Step s);

// A place's mark in the top bar.
enum class Mark { None, Done, Problem };
Mark markFor(const Document& d, Step s);

// The tools around the 3D view: On plate, Move, Rotate, Scale, Mirror and Measure on the rail
// at its left, Supports, Hollow and Repair along its foot. Each opens a
// floating panel. Rotate, Supports and Hollow open the place they belong to
// (Orient, Supports, Orient · Hollows); the others are parts of Model.
enum class Tool { None, OnPlate, Move, Rotate, Scale, Supports, Hollow, Repair, Mirror, Measure,
                Duplicate, Arrange, Merge, Split };
// Duplicate and Arrange (rail), Merge and Split (along the foot) are parts of Model too: they
// make copies, lay the parts out, join them and split one into its shells.
// Mirror and Measure are parts of Model too: Mirror turns it into its mirror image
// (each choice at once), Measure shows distances and changes nothing.
// The tool whose panel a place shows: its own, or on Model the one picked there.
Tool toolShown(Step s, bool hollow_view, Tool model_tool);
// The place a tool opens.
Step stepFor(Tool t);
// Two modes: Prepare (placing it, supports) and Layers (the layers, saving).
inline bool inLayersMode(Step s) { return s == Step::Layers || s == Step::Print; }

// The Job panel's main button: the next thing the job needs.
enum class MainAction {
    OpenModel,       // nothing on the plate yet
    MakeSupports,    // fits, no supports yet
    RemakeSupports,  // supports made for an earlier placement or settings
    Slice,           // supports are up to date: cut the layers
    ShowLayers,      // the layers are cut for these supports
};
struct MainButton {
    MainAction action = MainAction::OpenModel;
    bool enabled = true;  // false while it doesn't fit the plate
};
MainButton mainButton(const Document* d);
const char* mainLabel(MainAction a);

}  // namespace gui
