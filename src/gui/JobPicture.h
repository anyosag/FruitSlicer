// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// A small picture of a job as it sits: its parts and supports seen from the
// front right and a little above, for Home's cards. Drawn on the CPU (the 3D
// view's renderer has one target, and saving runs off the UI thread), so the
// same job always gives the same picture. No UI here.

#include <cstdint>
#include <vector>

#include "geometry/Polygon.h"
#include "geometry/Vec.h"
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

// Where a point in printer coordinates lands in a picture, in its pixels (top left 0, 0).
struct PictureFrame {
    fruit::Vec3 r{1, 0, 0}, u{0, 1, 0};  // the picture's right and up
    double cx = 0, cy = 0, scale = 0;    // scale: pixels per mm
    int w = 0, h = 0;
    bool valid() const { return scale > 0; }
    fruit::Vec2 at(const fruit::Vec3& p) const;
};

// Fills a polygon (picture pixels, any shape, even-odd) with `rgb` at `alpha`
// over what is drawn, its edges smoothed.
void blendPolygon(JobPicture& p, const std::vector<fruit::Vec2>& pts, unsigned rgb, double alpha);

// A trunk's foot, for drawRaftPlan: a cone from `r_base` at `z0` up `h` to `r_top`.
struct PlanFoot {
    fruit::Vec2 at;
    double r_base = 0, r_top = 0, z0 = 0, h = 0;
};

// A raft seen from straight above, on the 3D view's ground and the plate's
// 10 mm grid: its layers (each `dz` thick, from the plate up) as a relief lit
// from the top left, the trunks' feet on it, and `notches` (the windows,
// under its top) laid over it, dark and see-through. Fitted with a margin,
// drawn at twice the size and averaged down; `frame` gets where points land.
JobPicture drawRaftPlan(const std::vector<fruit::Paths>& layers, double dz, const std::vector<PlanFoot>& feet,
                        const fruit::Paths& notches, int w, int h, PictureFrame* frame = nullptr);

// The job as it is now: every part, and its supports when they are up to date.
JobPicture jobPicture(const Document& doc, int w = 400, int h = 300);

// As kept in a job file: "FSP1", width and height (16 bits each, little
// end first), then the pixels. A picture that doesn't read is empty.
std::vector<std::uint8_t> pictureBytes(const JobPicture& p);
JobPicture pictureFrom(const std::vector<std::uint8_t>& bytes);

}  // namespace gui
