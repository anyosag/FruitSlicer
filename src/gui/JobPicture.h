// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// A small picture of a job as it sits: its parts and supports seen from the
// front right and a little above, for Home's cards. Drawn on the CPU (the 3D
// view's renderer has one target, and saving runs off the UI thread), so the
// same job always gives the same picture. No UI here.

#include <cstdint>
#include <vector>

#include "mesh/TriangleMesh.h"

namespace gui {

class Document;

struct JobPicture {
    int w = 0, h = 0;
    std::vector<std::uint8_t> rgba;  // top row first; empty: no picture
    bool empty() const { return rgba.empty(); }
};

// One mesh to draw, in printer coordinates, in one colour (0xRRGGBB).
struct PictureMesh {
    const fruit::TriangleMesh* mesh = nullptr;
    unsigned rgb = 0xAEB6C4;
};

// The meshes on the 3D view's dark ground, shaded by a light from the front
// right and above, fitted to the picture with a margin. Drawn at twice the
// size and averaged down. Nothing to draw: the ground alone.
JobPicture drawJob(const std::vector<PictureMesh>& meshes, int w, int h);

// The job as it is now: every part, and its supports when they are up to date.
JobPicture jobPicture(const Document& doc, int w = 400, int h = 300);

// As kept in a job file: "FSP1", width and height (16 bits each, little
// end first), then the pixels. A picture that doesn't read is empty.
std::vector<std::uint8_t> pictureBytes(const JobPicture& p);
JobPicture pictureFrom(const std::vector<std::uint8_t>& bytes);

}  // namespace gui
