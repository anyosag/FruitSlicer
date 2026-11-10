// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The Label tool's letters: a string made into a closed solid of extruded
// letters (with the holes of o, a, e, ... right), and put on a model's surface
// raised or engraved.

#include <cstddef>
#include <string>

#include "geometry/Vec.h"
#include "mesh/TriangleMesh.h"
#include "ops/MeshOps.h"

namespace fruit::ops {

// A TrueType font held in memory (the bytes must outlive every call).
struct FontData {
    const unsigned char* data = nullptr;
    std::size_t size = 0;
};

// Inter Regular, built into FruitSlicer (SIL Open Font License 1.1).
const FontData& interRegular();

struct TextStyle {
    double height = 8.0;  // mm, height of a capital letter (not the font's em size)
    double depth = 1.0;   // mm, how far the letters are extruded
    double extra_spacing = 0.0;  // mm added after every letter
    double line_spacing = 1.5;   // between lines, in capital heights ('\n' starts a line)
    double curve_tolerance = 0.02;  // mm: how far the flattened outline may stray from the true one
    FontData font{};  // empty: Inter Regular
};

struct TextResult {
    TriangleMesh mesh;  // closed; x and y centred on 0, z from 0 to `depth`, letters facing +Z with the top of the letters toward +Y
    bool ok = false;
    std::string message;  // why not, or a note ("Left out: ...")
};

// UTF-8 text. Letters the font does not have are left out and listed in
// `message`. Spaces and line breaks only move the pen: if nothing is left to
// draw, `ok` is false.
TextResult makeTextSolid(const std::string& text, const TextStyle& style = {});

// Where letters go on a surface: at `point`, facing along `normal` (away from
// the material), with the top of the letters toward `up` (any vector not
// along the normal; it is projected onto the surface plane).
struct TextPlacement {
    Vec3 point{0, 0, 0};
    Vec3 normal{0, 0, 1};
    Vec3 up{0, 1, 0};
    bool engrave = false;  // false: raised (united), true: cut in (subtracted)
};

// The letters moved into place without combining them: z of `text` (0..depth)
// is taken along the normal, shifted by `z_shift` mm.
TriangleMesh placeText(const TriangleMesh& text, const TextPlacement& placement, double z_shift = 0);

// Raise the text from the surface, or engrave it into it, by style.depth. The
// letters are flat, not bent round the surface: on a curve they sink a little
// into it (raised) or are cut a little deeper or shallower (engraved) away
// from `point`. If the letters do not meet the surface the result is not ok.
MeshResult applyText(const TriangleMesh& base, const std::string& text, const TextStyle& style,
                     const TextPlacement& placement);

}  // namespace fruit::ops
