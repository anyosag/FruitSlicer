// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once

namespace gui {
class App;
}

namespace gui::screens {

void home(App& app);
void job(App& app);  // the job: step bar, the current step, status bar
void about(App& app);

}  // namespace gui::screens
