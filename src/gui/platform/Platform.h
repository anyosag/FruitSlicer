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

// Native save dialog, suggesting `name`; `done` as for openFileDialog.
void saveFileDialog(SDL_Window* window, const std::vector<FileFilter>& filters, const std::string& name,
                    std::function<void(const std::string& path)> done);

// Drives you can take to the printer: USB sticks and other removable or
// ejectable volumes that are mounted now, local and writable (so mounted
// disk images and network shares are left out).
struct Volume {
    std::string name;    // as Finder shows it, e.g. "ELEGOO"
    std::string path;    // where it is mounted
    std::string format;  // as Finder describes it, e.g. "MS-DOS (FAT32)"; may be empty
    unsigned long long free_bytes = 0;
};
std::vector<Volume> removableVolumes();
// Unmounts and ejects the drive at `path` (macOS). False with a message if
// it could not, e.g. a file on it is still open. Safe on any thread.
bool ejectVolume(const std::string& path, std::string* error);

// Shows the file in Finder, selected (elsewhere: opens its folder).
void revealFile(const std::string& path);

// Delivers dialog results on the UI thread; call once per frame.
void pumpCallbacks();

// Keeps macOS from napping the app while a job runs (App Nap would slow
// background work down to a crawl).
void beginBusy(const std::string& reason);
void endBusy();

}  // namespace gui::platform
