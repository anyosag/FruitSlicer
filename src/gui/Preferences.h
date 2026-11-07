// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// How FruitSlicer behaves on this computer (the Settings page). Printer and
// print settings live in PrinterSetup; these are about the app itself.

#include <string>
#include <vector>

namespace gui {

struct Preferences {
    bool smooth_view = true;   // the 3D view drawn with multisampling (off: faster on old graphics)
    unsigned cores = 0;        // at most this many processor cores for slicing and saving (0 = all)
    bool goo_to_drive = true;  // a USB drive plugged in is where .goo files go
    std::string goo_folder;    // the folder the last .goo was saved to, when not to a drive
    bool operator==(const Preferences&) const = default;
};

// Missing or unreadable files read as the defaults; writing creates the folder.
Preferences loadPreferences(const std::string& file);
void savePreferences(const std::string& file, const Preferences& p);

// The core counts the Settings page offers on a computer with `all` cores:
// all of them, all but one, half, and one; each once, largest first.
std::vector<unsigned> coreChoices(unsigned all);

}  // namespace gui
