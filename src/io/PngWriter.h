// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Minimal dependency-free PNG writer (8-bit grayscale, uncompressed deflate)
// and a debug rasterizer for layer polygons. Debug output only — the printer
// file rasterizer (anisotropic pixels, analytic anti-aliasing) is milestone M6.

#include <cstdint>
#include <string>
#include <vector>

#include "geometry/Polygon.h"

namespace fruit {

void writePngGray(const std::string& path, int width, int height, const std::vector<uint8_t>& pixels);
// 8-bit RGB, pixels row-major (3 bytes per pixel).
void writePngRgb(const std::string& path, int width, int height, const std::vector<uint8_t>& rgb);

struct RasterWindow {
    BBox2 area;       // mm region to render
    double pixel = 0.05;  // mm per pixel (square, debug only)
};

// Even-odd scanline fill of already-unioned polygons. Returns width*height bytes.
std::vector<uint8_t> rasterize(const Paths& polys, const RasterWindow& win, int& width, int& height,
                               uint8_t value = 255);

}  // namespace fruit
