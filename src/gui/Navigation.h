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
    Fit,            // the model doesn't fit the plate where it is (too big, or over the edge)
    Supports,       // no supports yet
    FreshSupports,  // the supports were made for an earlier placement or settings
    Layers,         // the layers aren't cut for these supports yet (the Layers place cuts them)
};
Need needFor(const Document& d, Step s);

// A place's mark in the top bar.
enum class Mark { None, Done, Problem };
Mark markFor(const Document& d, Step s);

}  // namespace gui
