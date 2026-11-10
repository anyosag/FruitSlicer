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
