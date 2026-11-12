// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "ViewCube.h"

#include <algorithm>

namespace gui {

namespace {

struct FaceDef {
    CubeSide side;
    V3 n;
    V3 c[4];  // corners, going round the face
};

const FaceDef kFaces[6] = {
    {CubeSide::Right, {1, 0, 0}, {{1, -1, -1}, {1, 1, -1}, {1, 1, 1}, {1, -1, 1}}},
    {CubeSide::Left, {-1, 0, 0}, {{-1, 1, -1}, {-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}}},
    {CubeSide::Back, {0, 1, 0}, {{1, 1, -1}, {-1, 1, -1}, {-1, 1, 1}, {1, 1, 1}}},
    {CubeSide::Front, {0, -1, 0}, {{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}}},
    {CubeSide::Top, {0, 0, 1}, {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}}},
    {CubeSide::Bottom, {0, 0, -1}, {{-1, 1, -1}, {1, 1, -1}, {1, -1, -1}, {-1, -1, -1}}},
};

V3 row(const M4& m, int r) { return {m.at(r, 0), m.at(r, 1), m.at(r, 2)}; }

}  // namespace

std::vector<CubeFace> cubeFaces(const M4& view, float half) {
    // The view's rotation: rows are the camera's right, up and back.
    const V3 right = row(view, 0), up = row(view, 1), back = row(view, 2);
    std::vector<CubeFace> out;
    for (const FaceDef& f : kFaces) {
        const float facing = dot(back, f.n);
        if (facing <= 0.02f) continue;
        CubeFace face{f.side, {}, {}, facing};
        for (int i = 0; i < 4; ++i) {
            face.x[i] = dot(right, f.c[i]) * half;
            face.y[i] = -dot(up, f.c[i]) * half;
        }
        out.push_back(face);
    }
    std::sort(out.begin(), out.end(), [](const CubeFace& a, const CubeFace& b) { return a.facing < b.facing; });
    return out;
}

int cubeFaceAt(const std::vector<CubeFace>& faces, float x, float y) {
    for (int i = static_cast<int>(faces.size()) - 1; i >= 0; --i) {
        const CubeFace& f = faces[static_cast<std::size_t>(i)];
        // Inside a convex quad: on the same side of every edge.
        int pos = 0, neg = 0;
        for (int k = 0; k < 4; ++k) {
            const int j = (k + 1) % 4;
            const float cr = (f.x[j] - f.x[k]) * (y - f.y[k]) - (f.y[j] - f.y[k]) * (x - f.x[k]);
            (cr >= 0 ? pos : neg)++;
        }
        if (pos == 4 || neg == 4) return i;
    }
    return -1;
}

CubeSpot cubeSpotAt(const std::vector<CubeFace>& faces, float x, float y) {
    CubeSpot spot;
    const int i = cubeFaceAt(faces, x, y);
    if (i < 0) return spot;
    const CubeFace& f = faces[static_cast<std::size_t>(i)];
    const FaceDef* def = nullptr;
    for (const FaceDef& d : kFaces)
        if (d.side == f.side) def = &d;
    if (!def) return spot;
    // The face on screen is a parallelogram: its middle, and half of each of
    // its two sides (corner 0 is at -1, -1; corner 1 at 1, -1; corner 3 at -1, 1).
    const float nx = (f.x[0] + f.x[2]) / 2, ny = (f.y[0] + f.y[2]) / 2;
    const float ax = (f.x[1] - f.x[0]) / 2, ay = (f.y[1] - f.y[0]) / 2;
    const float bx = (f.x[3] - f.x[0]) / 2, by = (f.y[3] - f.y[0]) / 2;
    const float det = ax * by - ay * bx;
    if (det == 0) return spot;
    // The point in the face's own coordinates, -1 to 1 along each side.
    const float px = x - nx, py = y - ny;
    const float u = (px * by - py * bx) / det, v = (ax * py - ay * px) / det;
    const int su = u > kCubeEdge ? 1 : u < -kCubeEdge ? -1 : 0;
    const int sv = v > kCubeEdge ? 1 : v < -kCubeEdge ? -1 : 0;
    // The same two sides in the cube's own axes.
    const V3 a = (def->c[1] - def->c[0]) * 0.5f, b = (def->c[3] - def->c[0]) * 0.5f;
    spot.face = i;
    spot.dir = def->n + a * static_cast<float>(su) + b * static_cast<float>(sv);
    // The spot's outline: the stretch of each side it takes up.
    const auto lo = [](int s) { return s > 0 ? kCubeEdge : s < 0 ? -1.0f : -kCubeEdge; };
    const auto hi = [](int s) { return s > 0 ? 1.0f : s < 0 ? -kCubeEdge : kCubeEdge; };
    const float us[4] = {lo(su), hi(su), hi(su), lo(su)}, vs[4] = {lo(sv), lo(sv), hi(sv), hi(sv)};
    for (int k = 0; k < 4; ++k) {
        spot.x[k] = nx + us[k] * ax + vs[k] * bx;
        spot.y[k] = ny + us[k] * ay + vs[k] * by;
    }
    return spot;
}

std::string cubeSpotName(V3 dir) {
    std::string name;
    const auto add = [&](const char* word) {
        if (!name.empty()) name += ' ';
        name += word;
    };
    if (dir.z > 0) add("top");
    else if (dir.z < 0) add("bottom");
    if (dir.y > 0) add("back");
    else if (dir.y < 0) add("front");
    if (dir.x > 0) add("right");
    else if (dir.x < 0) add("left");
    return name;
}

const char* cubeLabel(CubeSide s) {
    switch (s) {
        case CubeSide::Right: return "Right";
        case CubeSide::Left: return "Left";
        case CubeSide::Back: return "Back";
        case CubeSide::Front: return "Front";
        case CubeSide::Top: return "Top";
        case CubeSide::Bottom: return "Bottom";
    }
    return "";
}

V3 cubeDirection(CubeSide s) {
    for (const FaceDef& f : kFaces)
        if (f.side == s) return f.n;
    return {0, 0, 1};
}

}  // namespace gui
