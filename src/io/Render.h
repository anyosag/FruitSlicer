// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Tiny software renderer for previews: orthographic, z-buffered, flat-shaded
// mesh plus support points drawn as discs coloured by category. Meant for
// eyeballing results without a slicer, not for accuracy.

#include <string>
#include <vector>

#include "mesh/TriangleMesh.h"
#include "support/SupportPoint.h"

namespace fruit {

struct RenderView {
    double azimuth_deg = 30;     // around Z, from +Y towards +X
    double elevation_deg = -35;  // negative = looking up from below
    int size = 900;              // output is size × size pixels
};

void renderPreview(const std::string& png_path, const TriangleMesh& mesh, const std::vector<SupportPoint>& points,
                   const RenderView& view, const TriangleMesh* supports = nullptr);

// Same picture as RGB bytes (size × size × 3), e.g. for print-file thumbnails.
std::vector<uint8_t> renderPreviewRgb(const TriangleMesh& mesh, const std::vector<SupportPoint>& points,
                                      const RenderView& view, const TriangleMesh* supports = nullptr);

}  // namespace fruit
