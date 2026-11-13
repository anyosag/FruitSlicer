// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "JobPicture.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "Document.h"

namespace gui {

namespace {

using fruit::Vec3;

// The view: from the front right (35° round from the front) and 28° above.
constexpr double kAzimuthDeg = 35, kElevationDeg = 28;
constexpr unsigned kGround = 0x0E1014;  // the 3D view's own
constexpr double kMargin = 0.08;        // of each side, left clear

Vec3 unit(const Vec3& v) {
    const double n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return n > 0 ? Vec3{v.x / n, v.y / n, v.z / n} : Vec3{0, 0, 1};
}

double dotp(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 crossp(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

}  // namespace

JobPicture drawJob(const std::vector<PictureMesh>& meshes, int w, int h) {
    JobPicture out;
    if (w <= 0 || h <= 0) return out;
    out.w = w;
    out.h = h;
    // Drawn at twice the size, then each 2 × 2 block averaged.
    const int W = 2 * w, H = 2 * h;
    std::vector<float> rgb(static_cast<std::size_t>(W) * H * 3);
    std::vector<double> depth(static_cast<std::size_t>(W) * H, -std::numeric_limits<double>::infinity());
    for (std::size_t i = 0; i < static_cast<std::size_t>(W) * H; ++i) {
        rgb[3 * i] = static_cast<float>((kGround >> 16) & 0xFF);
        rgb[3 * i + 1] = static_cast<float>((kGround >> 8) & 0xFF);
        rgb[3 * i + 2] = static_cast<float>(kGround & 0xFF);
    }
    // The camera: `v` from what is seen towards the eye, `r` to the right, `u` up the picture.
    const double az = kAzimuthDeg * 3.14159265358979323846 / 180, el = kElevationDeg * 3.14159265358979323846 / 180;
    const Vec3 v{std::sin(az) * std::cos(el), -std::cos(az) * std::cos(el), std::sin(el)};
    const Vec3 f{-v.x, -v.y, -v.z};
    const Vec3 r = unit(crossp(f, Vec3{0, 0, 1}));
    const Vec3 u = crossp(r, f);
    const Vec3 light = unit(Vec3{0.45, -0.55, 0.70});
    // What there is to fit.
    double x0 = std::numeric_limits<double>::infinity(), x1 = -x0, y0 = x0, y1 = -x0;
    for (const PictureMesh& m : meshes) {
        if (!m.mesh) continue;
        for (const Vec3& p : m.mesh->vertices) {
            const double sx = dotp(p, r), sy = dotp(p, u);
            x0 = std::min(x0, sx), x1 = std::max(x1, sx), y0 = std::min(y0, sy), y1 = std::max(y1, sy);
        }
    }
    if (x1 >= x0 && y1 >= y0) {
        const double sx = (1 - 2 * kMargin) * W / std::max(x1 - x0, 1e-9), sy = (1 - 2 * kMargin) * H / std::max(y1 - y0, 1e-9);
        const double scale = std::min(sx, sy);
        const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
        for (const PictureMesh& m : meshes) {
            if (!m.mesh) continue;
            const double cr = (m.rgb >> 16) & 0xFF, cg = (m.rgb >> 8) & 0xFF, cb = m.rgb & 0xFF;
            const auto& vs = m.mesh->vertices;
            for (const auto& face : m.mesh->faces) {
                const Vec3& a = vs[face[0]];
                const Vec3& b = vs[face[1]];
                const Vec3& c = vs[face[2]];
                Vec3 n = unit(crossp(Vec3{b.x - a.x, b.y - a.y, b.z - a.z}, Vec3{c.x - a.x, c.y - a.y, c.z - a.z}));
                if (dotp(n, v) < 0) n = Vec3{-n.x, -n.y, -n.z};  // both sides lit: meshes may be open
                const double shade =
                    std::min(1.0, 0.30 + 0.60 * std::max(0.0, dotp(n, light)) + 0.12 * std::max(0.0, dotp(n, v)));
                // Screen positions (y down) and depth (larger: nearer).
                double px[3], py[3], pz[3];
                const Vec3* q[3] = {&a, &b, &c};
                for (int k = 0; k < 3; ++k) {
                    px[k] = (dotp(*q[k], r) - cx) * scale + W / 2.0;
                    py[k] = H / 2.0 - (dotp(*q[k], u) - cy) * scale;
                    pz[k] = dotp(*q[k], v);
                }
                const double area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
                if (std::abs(area) < 1e-12) continue;
                const int bx0 = std::max(0, static_cast<int>(std::floor(std::min({px[0], px[1], px[2]}))));
                const int bx1 = std::min(W - 1, static_cast<int>(std::ceil(std::max({px[0], px[1], px[2]}))));
                const int by0 = std::max(0, static_cast<int>(std::floor(std::min({py[0], py[1], py[2]}))));
                const int by1 = std::min(H - 1, static_cast<int>(std::ceil(std::max({py[0], py[1], py[2]}))));
                for (int y = by0; y <= by1; ++y)
                    for (int x = bx0; x <= bx1; ++x) {
                        const double X = x + 0.5, Y = y + 0.5;
                        const double w0 = ((px[1] - X) * (py[2] - Y) - (px[2] - X) * (py[1] - Y)) / area;
                        const double w1 = ((px[2] - X) * (py[0] - Y) - (px[0] - X) * (py[2] - Y)) / area;
                        const double w2 = 1 - w0 - w1;
                        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                        const double z = w0 * pz[0] + w1 * pz[1] + w2 * pz[2];
                        const std::size_t i = static_cast<std::size_t>(y) * W + x;
                        if (z <= depth[i]) continue;
                        depth[i] = z;
                        rgb[3 * i] = static_cast<float>(cr * shade);
                        rgb[3 * i + 1] = static_cast<float>(cg * shade);
                        rgb[3 * i + 2] = static_cast<float>(cb * shade);
                    }
            }
        }
    }
    out.rgba.assign(static_cast<std::size_t>(w) * h * 4, 255);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int k = 0; k < 3; ++k) {
                double s = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx)
                        s += rgb[3 * (static_cast<std::size_t>(2 * y + dy) * W + (2 * x + dx)) + k];
                out.rgba[4 * (static_cast<std::size_t>(y) * w + x) + k] =
                    static_cast<std::uint8_t>(std::clamp(std::lround(s / 4), 0L, 255L));
            }
    return out;
}

JobPicture jobPicture(const Document& doc, int w, int h) {
    std::vector<PictureMesh> meshes;
    if (doc.partCount() > 0) meshes.push_back({&doc.placed(), 0xAEB6C4});
    const SupportState& s = doc.supports;
    if (s.result && s.fresh(doc.revision(), doc.supportSettings())) meshes.push_back({&s.result->mesh, 0x7D8696});
    return drawJob(meshes, w, h);
}

std::vector<std::uint8_t> pictureBytes(const JobPicture& p) {
    if (p.empty() || p.w <= 0 || p.h <= 0 || p.w > 0xFFFF || p.h > 0xFFFF) return {};
    std::vector<std::uint8_t> out{'F', 'S', 'P', '1', static_cast<std::uint8_t>(p.w & 0xFF),
                                  static_cast<std::uint8_t>(p.w >> 8), static_cast<std::uint8_t>(p.h & 0xFF),
                                  static_cast<std::uint8_t>(p.h >> 8)};
    out.insert(out.end(), p.rgba.begin(), p.rgba.end());
    return out;
}

JobPicture pictureFrom(const std::vector<std::uint8_t>& b) {
    JobPicture p;
    if (b.size() < 8 || b[0] != 'F' || b[1] != 'S' || b[2] != 'P' || b[3] != '1') return p;
    const int w = b[4] | (b[5] << 8), h = b[6] | (b[7] << 8);
    if (w <= 0 || h <= 0 || b.size() != 8 + static_cast<std::size_t>(w) * h * 4) return p;
    p.w = w;
    p.h = h;
    p.rgba.assign(b.begin() + 8, b.end());
    return p;
}

}  // namespace gui
