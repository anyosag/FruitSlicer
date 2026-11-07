// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The few things that differ per operating system.

#include <functional>
#include <string>
#include <vector>

struct SDL_Window;

namespace gui::platform {

// Where settings, jobs and the log live
// (macOS: ~/Library/Application Support/FruitSlicer).
std::string dataDir();

// Native open-file dialog. `done` runs on the UI thread with the chosen
// path, or an empty string when the user cancelled. Filters are extension
// lists such as "stl;obj".
struct FileFilter {
    const char* name;
    const char* pattern;
};
void openFileDialog(SDL_Window* window, const std::vector<FileFilter>& filters,
                    std::function<void(const std::string& path)> done);

// Delivers dialog results on the UI thread; call once per frame.
void pumpCallbacks();

// Keeps macOS from napping the app while a job runs (App Nap would slow
// background work down to a crawl).
void beginBusy(const std::string& reason);
void endBusy();

}  // namespace gui::platform
