// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Numbers as the interface writes them: "50.8 × 60.5 mm", "99,976", "78 %".

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>

#include "geometry/Vec.h"

namespace gui {

// The local time at `t` (localtime_r; Windows has localtime_s, its arguments the other way round).
inline std::tm localTime(std::time_t t) {
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

// printf into a string of any length (a fixed buffer once cut long sentences off mid-word).
inline std::string strf(const char* f, ...) {
    char b[256];
    va_list a, again;
    va_start(a, f);
    va_copy(again, a);
    const int n = std::vsnprintf(b, sizeof b, f, a);
    va_end(a);
    std::string out;
    if (n >= static_cast<int>(sizeof b)) {
        out.resize(static_cast<std::size_t>(n) + 1);
        std::vsnprintf(out.data(), out.size(), f, again);
        out.resize(static_cast<std::size_t>(n));
    } else if (n > 0) {
        out.assign(b, static_cast<std::size_t>(n));
    }
    va_end(again);
    return out;
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

// A shortcut as the platform writes it: "⌘O" on a Mac, "Ctrl+O" on Windows, "Ctrl O" elsewhere.
inline std::string cmd(const char* key, bool shift = false) {
#if defined(__APPLE__)
    return std::string(shift ? "⇧" : "") + "⌘" + key;
#elif defined(_WIN32)
    return std::string("Ctrl+") + (shift ? "Shift+" : "") + key;
#else
    return std::string("Ctrl ") + (shift ? "Shift " : "") + key;
#endif
}

// The button that shows a file where it is, as the platform names it.
inline const char* showFileLabel() {
#if defined(__APPLE__)
    return "Show in Finder";
#elif defined(_WIN32)
    return "Show in Explorer";
#else
    return "Show in folder";
#endif
}

// The computer, as a sentence starts with it: "The Mac doesn't list it as a drive".
inline const char* thisComputer() {
#if defined(__APPLE__)
    return "The Mac";
#elif defined(_WIN32)
    return "Windows";
#else
    return "This computer";
#endif
}

}  // namespace gui
