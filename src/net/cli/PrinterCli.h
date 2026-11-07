// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The fruitslicer-printer command line, as a function so tests can drive it
// against the mock printer with their own streams.

#include <atomic>
#include <iosfwd>

namespace fruit::net {

int runPrinterCli(int argc, const char* const* argv, std::ostream& out, std::ostream& err,
                  std::istream& in, const std::atomic<bool>* cancel = nullptr);

}  // namespace fruit::net
