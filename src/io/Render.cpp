// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "io/Render.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "io/PngWriter.h"

namespace fruit {

namespace {

struct Camera {
    Vec3 right, up, fwd;  // fwd points from the camera into the scene
    double scale = 1, cx = 0, cy = 0;
    Vec3 project(const Vec3& p) const { return {dot(p, right) * scale + cx, -dot(p, up) * scale + cy, dot(p, fwd)}; }
};

void categoryColour(PointCategory c, uint8_t rgb[3]) {
    switch (c) {
        case PointCategory::Island: rgb[0] = 255; rgb[1] = 120; rgb[2] = 0; return;
        case PointCategory::Overhang: rgb[0] = 240; rgb[1] = 210; rgb[2] = 0; return;
        case PointCategory::Capacity: rgb[0] = 190; rgb[1] = 40; rgb[2] = 220; return;
        case PointCategory::Stabilization: rgb[0] = 30; rgb[1] = 190; rgb[2] = 70; return;
        case PointCategory::Manual: rgb[0] = 30; rgb[1] = 120; rgb[2] = 255; return;
    }
}

}  // namespace

void renderPreview(const std::string& path, const TriangleMesh& mesh, const std::vector<SupportPoint>& points,
                   const RenderView& v, const TriangleMesh* supports) {
    const double az = deg2rad(v.azimuth_deg), el = deg2rad(v.elevation_deg);
    Camera cam;
    // Camera sits in direction (sin az cos el, cos az cos el, sin el) from the
    // model and looks back at it; elevation < 0 puts it below, looking up.
    cam.fwd = normalized(Vec3{-std::sin(az) * std::cos(el), -std::cos(az) * std::cos(el), -std::sin(el)});
    cam.right = normalized(cross(cam.fwd, Vec3{0, 0, 1}));
    cam.up = cross(cam.right, cam.fwd);

    BBox3 box = mesh.bbox();
    if (supports) box.extend(supports->bbox());
    double minx = 1e300, maxx = -1e300, miny = 1e300, maxy = -1e300;
    for (int i = 0; i < 8; ++i) {
        const Vec3 c{(i & 1) ? box.max.x : box.min.x, (i & 2) ? box.max.y : box.min.y, (i & 4) ? box.max.z : box.min.z};
        const double x = dot(c, cam.right), y = dot(c, cam.up);
        minx = std::min(minx, x); maxx = std::max(maxx, x);
        miny = std::min(miny, y); maxy = std::max(maxy, y);
    }
    const int N = v.size;
    cam.scale = 0.92 * N / std::max(maxx - minx, maxy - miny);
    cam.cx = N / 2.0 - (minx + maxx) / 2 * cam.scale;
    cam.cy = N / 2.0 + (miny + maxy) / 2 * cam.scale;

    std::vector<double> zbuf(static_cast<size_t>(N) * N, std::numeric_limits<double>::max());
    std::vector<uint8_t> img(static_cast<size_t>(N) * N * 3);
    for (int i = 0; i < N * N; ++i) { img[3 * i] = 250; img[3 * i + 1] = 250; img[3 * i + 2] = 247; }
    const Vec3 light = normalized(-cam.fwd + cam.up * 0.4 + cam.right * 0.3);

    auto drawMesh = [&](const TriangleMesh& m, const uint8_t base[3]) {
        for (size_t fi = 0; fi < m.faces.size(); ++fi) {
            const Face& f = m.faces[fi];
            const Vec3 a = cam.project(m.vertices[f[0]]), b = cam.project(m.vertices[f[1]]), c = cam.project(m.vertices[f[2]]);
            const Vec3 n = m.faceNormal(fi);
            const double shade = 0.35 + 0.65 * std::abs(dot(n, light));
            const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
            const int x1 = std::min(N - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
            const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
            const int y1 = std::min(N - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
            const double area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
            if (std::abs(area) < 1e-12) continue;
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const double px = x + 0.5, py = y + 0.5;
                    const double w0 = ((b.x - px) * (c.y - py) - (c.x - px) * (b.y - py)) / area;
                    const double w1 = ((c.x - px) * (a.y - py) - (a.x - px) * (c.y - py)) / area;
                    const double w2 = 1 - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    const double z = w0 * a.z + w1 * b.z + w2 * c.z;
                    const size_t k = static_cast<size_t>(y) * N + x;
                    if (z >= zbuf[k]) continue;
                    zbuf[k] = z;
                    for (int ch = 0; ch < 3; ++ch) img[3 * k + ch] = static_cast<uint8_t>(std::min(255.0, base[ch] * shade));
                }
        }
    };
    const uint8_t model_col[3] = {170, 180, 195};
    drawMesh(mesh, model_col);
    if (supports) {
        const uint8_t sup_col[3] = {225, 215, 190};
        drawMesh(*supports, sup_col);
    }
    // Points: discs, depth-tested with a bias so tips on visible surfaces show.
    const double bias = 0.3;
    for (const auto& p : points) {
        const Vec3 q = cam.project(p.pos);
        const double r = std::max(4.5, p.tip_diameter / 2 * cam.scale);
        uint8_t col[3] = {0, 0, 0};
        categoryColour(p.category, col);
        for (int y = static_cast<int>(q.y - r - 1); y <= static_cast<int>(q.y + r + 1); ++y)
            for (int x = static_cast<int>(q.x - r - 1); x <= static_cast<int>(q.x + r + 1); ++x) {
                if (x < 0 || y < 0 || x >= N || y >= N) continue;
                const double d = std::hypot(x + 0.5 - q.x, y + 0.5 - q.y);
                if (d > r) continue;
                const size_t k = static_cast<size_t>(y) * N + x;
                if (q.z > zbuf[k] + bias) continue;
                const bool rim = d > r - 1.2;
                for (int ch = 0; ch < 3; ++ch) img[3 * k + ch] = rim ? 40 : col[ch];
            }
    }
    writePngRgb(path, N, N, img);
}

}  // namespace fruit
