// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// A plain-text log next to the user's data, so problems seen on a Mac can be
// reported without a debugger. Thread-safe.

#include <string>

namespace gui::log {

// Opens <dir>/fruitslicer.log (the previous run's log becomes .1).
void open(const std::string& dir);
const std::string& path();

void info(const std::string& msg);
void error(const std::string& msg);

}  // namespace gui::log
