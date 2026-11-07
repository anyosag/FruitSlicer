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
void settings(App& app);  // canvas board "Settings"
void setup(App& app);     // first-run setup, canvas board "Setup"
void resins(App& app);    // canvas board "Resins"
void plate(App& app);     // canvas board "PlatePreview"
void score(App& app);     // canvas board "Results"
void fit(App& app);       // canvas board "Fit"
void abSetup(App& app);   // canvas board "ABSetup"
// Calibrate: the hub, the image orientation check, the Printer page.
void calibrate(App& app);
void orientationCheck(App& app);
void printer(App& app);

}  // namespace gui::screens
