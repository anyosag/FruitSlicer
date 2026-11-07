// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Modal dialogs drawn each frame; their state lives in App.

namespace gui {
class App;
}

namespace gui::dialogs {

// After a model loads: what the check found, and the size to place it at.
void health(App& app);
// The keyboard shortcuts that work in this version.
void shortcuts(App& app, bool& open);

}  // namespace gui::dialogs
