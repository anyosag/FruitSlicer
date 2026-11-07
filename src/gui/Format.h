// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Numbers as the interface writes them: "50.8 × 60.5 mm", "99,976", "78 %".

#include <cstdarg>
#include <cstdio>
#include <string>

#include "geometry/Vec.h"

namespace gui {

inline std::string strf(const char* f, ...) {
    char b[256];
    va_list a;
    va_start(a, f);
    std::vsnprintf(b, sizeof b, f, a);
    va_end(a);
    return b;
}

inline std::string dims(const fruit::Vec3& s) { return strf("%.1f × %.1f × %.1f mm", s.x, s.y, s.z); }
inline std::string dims2(double a, double b) { return strf("%.1f × %.1f mm", a, b); }

inline std::string thousands(std::size_t n) {
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ",");
    return s;
}

// 0.78 -> "78 %"; 1.255 -> "125.5 %".
inline std::string percent(double scale) {
    const double p = scale * 100;
    return p == static_cast<long long>(p) ? strf("%.0f %%", p) : strf("%.1f %%", p);
}

// A shortcut as the platform writes it: "⌘O" on a Mac, "Ctrl O" elsewhere.
inline std::string cmd(const char* key, bool shift = false) {
#if defined(__APPLE__)
    return std::string(shift ? "⇧" : "") + "⌘" + key;
#else
    return std::string("Ctrl ") + (shift ? "Shift " : "") + key;
#endif
}

}  // namespace gui
