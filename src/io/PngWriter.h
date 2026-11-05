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

struct RasterWindow {
    BBox2 area;       // mm region to render
    double pixel = 0.05;  // mm per pixel (square, debug only)
};

// Even-odd scanline fill of already-unioned polygons. Returns width*height bytes.
std::vector<uint8_t> rasterize(const Paths& polys, const RasterWindow& win, int& width, int& height,
                               uint8_t value = 255);

}  // namespace fruit
