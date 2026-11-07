// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Dear ImGui build configuration (IMGUI_USER_CONFIG). ImGui's own checks go
// to FruitSlicer's log instead of assert(), which release builds compile out;
// scripted runs then fail, so a misuse of ImGui shows up in CI.

void fruitImGuiCheckFailed(const char* expr, const char* file, int line);
#define IM_ASSERT(_EXPR)                                                   \
    do {                                                                   \
        if (!(_EXPR)) fruitImGuiCheckFailed(#_EXPR, __FILE__, __LINE__); \
    } while (0)
