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
constexpr unsigned kGrid = 0x1E222A;    // its plate grid
constexpr unsigned kRaft = 0xAEB6C4, kFoot = 0x8A93A3, kNotch = 0x0E1014;
constexpr double kNotchAlpha = 0.55;
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

fruit::Vec2 PictureFrame::at(const fruit::Vec3& p) const {
    const double sx = dotp(p, r), sy = dotp(p, u);
    return {(sx - cx) * scale + w / 2.0, h / 2.0 - (sy - cy) * scale};
}

namespace {

// Even-odd fill at pixel centres: `fill(y, x0, x1)` for each run of pixels [x0, x1) a row.
template <class F>
void scanFill(const std::vector<std::vector<fruit::Vec2>>& polys, int W, int H, F fill) {
    struct Edge {
        double y0, y1, x0, dxdy;
    };
    std::vector<Edge> edges;
    for (const auto& p : polys)
        for (std::size_t i = 0; i < p.size(); ++i) {
            fruit::Vec2 a = p[i], b = p[(i + 1) % p.size()];
            if (a.y == b.y) continue;
            if (a.y > b.y) std::swap(a, b);
            edges.push_back({a.y, b.y, a.x, (b.x - a.x) / (b.y - a.y)});
        }
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.y0 < b.y0; });
    std::vector<const Edge*> active;
    std::vector<double> xs;
    std::size_t next = 0;
    for (int y = 0; y < H; ++y) {
        const double cy = y + 0.5;
        while (next < edges.size() && edges[next].y0 <= cy) active.push_back(&edges[next++]);
        active.erase(std::remove_if(active.begin(), active.end(), [&](const Edge* e) { return e->y1 <= cy; }),
                     active.end());
        xs.clear();
        for (const Edge* e : active) xs.push_back(e->x0 + (cy - e->y0) * e->dxdy);
        std::sort(xs.begin(), xs.end());
        for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {
            const int x0 = std::max(0, static_cast<int>(std::ceil(xs[k] - 0.5)));
            const int x1 = std::min(W, static_cast<int>(std::ceil(xs[k + 1] - 0.5)));
            if (x1 > x0) fill(y, x0, x1);
        }
    }
}

void downsample(const std::vector<float>& rgb, int w, int h, JobPicture& out) {
    const int W = 2 * w;
    out.rgba.assign(static_cast<std::size_t>(w) * h * 4, 255);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int k = 0; k < 3; ++k) {
                double s = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) s += rgb[3 * (static_cast<std::size_t>(2 * y + dy) * W + (2 * x + dx)) + k];
                out.rgba[4 * (static_cast<std::size_t>(y) * w + x) + k] =
                    static_cast<std::uint8_t>(std::clamp(std::lround(s / 4), 0L, 255L));
            }
}

}  // namespace

void blendPolygon(JobPicture& p, const std::vector<fruit::Vec2>& pts, unsigned rgb, double alpha) {
    if (p.empty() || pts.size() < 3) return;
    const double cr = (rgb >> 16) & 0xFF, cg = (rgb >> 8) & 0xFF, cb = rgb & 0xFF;
    // Four by four samples a pixel: its coverage smooths the edges.
    constexpr int kSub = 4;
    std::vector<fruit::Vec2> fine;
    for (const auto& q : pts) fine.push_back({q.x * kSub, q.y * kSub});
    std::vector<double> cover(static_cast<std::size_t>(p.w) * p.h, 0.0);
    scanFill({fine}, p.w * kSub, p.h * kSub, [&](int y, int x0, int x1) {
        for (int x = x0; x < x1; ++x) cover[static_cast<std::size_t>(y / kSub) * p.w + x / kSub] += 1.0 / (kSub * kSub);
    });
    for (std::size_t i = 0; i < cover.size(); ++i) {
        const double a = alpha * std::min(1.0, cover[i]);
        if (a <= 0) continue;
        std::uint8_t* px = &p.rgba[4 * i];
        px[0] = static_cast<std::uint8_t>(std::lround(px[0] * (1 - a) + cr * a));
        px[1] = static_cast<std::uint8_t>(std::lround(px[1] * (1 - a) + cg * a));
        px[2] = static_cast<std::uint8_t>(std::lround(px[2] * (1 - a) + cb * a));
    }
}

JobPicture drawRaftPlan(const std::vector<fruit::Paths>& layers, double dz, const std::vector<PlanFoot>& feet,
                        const fruit::Paths& notches, int w, int h, PictureFrame* frame) {
    JobPicture out;
    if (w <= 0 || h <= 0) return out;
    out.w = w;
    out.h = h;
    const int W = 2 * w, H = 2 * h;
    fruit::BBox2 b;
    for (const fruit::Paths& l : layers) b.extend(fruit::bbox(l));
    for (const PlanFoot& f : feet) b.extend(fruit::BBox2{{f.at.x - f.r_base, f.at.y - f.r_base}, {f.at.x + f.r_base, f.at.y + f.r_base}});
    std::vector<float> rgb(static_cast<std::size_t>(W) * H * 3);
    auto put = [&](std::size_t i, unsigned c, double shade) {
        rgb[3 * i] = static_cast<float>(std::min(255.0, ((c >> 16) & 0xFF) * shade));
        rgb[3 * i + 1] = static_cast<float>(std::min(255.0, ((c >> 8) & 0xFF) * shade));
        rgb[3 * i + 2] = static_cast<float>(std::min(255.0, (c & 0xFF) * shade));
    };
    if (b.empty()) {
        for (std::size_t i = 0; i < static_cast<std::size_t>(W) * H; ++i) put(i, kGround, 1.0);
        downsample(rgb, w, h, out);
        return out;
    }
    const double sc = std::min((1 - 2 * kMargin) * W / std::max(b.size().x, 1e-6), (1 - 2 * kMargin) * H / std::max(b.size().y, 1e-6));
    const double cx = b.center().x, cy = b.center().y;
    auto px = [&](const fruit::Vec2& v) { return fruit::Vec2{(v.x - cx) * sc + W / 2.0, H / 2.0 - (v.y - cy) * sc}; };
    // How high the raft is at each pixel (−1: the plate), layer on layer, then the feet.
    std::vector<float> hm(static_cast<std::size_t>(W) * H, -1.0f);
    std::vector<std::uint8_t> on_foot(hm.size(), 0);
    for (std::size_t k = 0; k < layers.size(); ++k) {
        std::vector<std::vector<fruit::Vec2>> polys;
        for (const fruit::Path& path : layers[k]) {
            std::vector<fruit::Vec2> poly;
            for (const auto& q : path) poly.push_back(px(fruit::toVec(q)));
            polys.push_back(std::move(poly));
        }
        const float z = static_cast<float>((k + 1) * dz);
        scanFill(polys, W, H, [&](int y, int x0, int x1) {
            for (int x = x0; x < x1; ++x) hm[static_cast<std::size_t>(y) * W + x] = z;
        });
    }
    for (const PlanFoot& f : feet) {
        const fruit::Vec2 c = px(f.at);
        const int r = static_cast<int>(std::ceil(f.r_base * sc)) + 1;
        for (int y = std::max(0, static_cast<int>(c.y) - r); y <= std::min(H - 1, static_cast<int>(c.y) + r); ++y)
            for (int x = std::max(0, static_cast<int>(c.x) - r); x <= std::min(W - 1, static_cast<int>(c.x) + r); ++x) {
                const double d = std::hypot(x + 0.5 - c.x, y + 0.5 - c.y) / sc;
                if (d > f.r_base) continue;
                const double t = d <= f.r_top ? 1.0 : (f.r_base - d) / std::max(1e-9, f.r_base - f.r_top);
                const float z = static_cast<float>(f.z0 + f.h * t);
                const std::size_t i = static_cast<std::size_t>(y) * W + x;
                if (z > hm[i]) hm[i] = z, on_foot[i] = 1;
            }
    }
    // Lit from the top left: its slopes, and each drop to the plate, shaded.
    const double l0 = -0.45, l1 = 0.55, l2 = 0.70, ln = std::sqrt(l0 * l0 + l1 * l1 + l2 * l2);
    const double mm = 1.0 / sc;
    float top = 0;
    for (float v : hm) top = std::max(top, v);
    auto at = [&](int x, int y) {
        x = std::clamp(x, 0, W - 1), y = std::clamp(y, 0, H - 1);
        const float v = hm[static_cast<std::size_t>(y) * W + x];
        return v < 0 ? 0.0 : static_cast<double>(v);
    };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * W + x;
            if (hm[i] < 0) {
                // The plate: its 10 mm grid.
                const double wx = (x + 0.5 - W / 2.0) / sc + cx, wy = cy - (y + 0.5 - H / 2.0) / sc;
                auto onLine = [&](double v) {
                    const double m = std::fmod(std::abs(v), 10.0);
                    return m < 0.9 * mm || 10.0 - m < 0.9 * mm;
                };
                put(i, onLine(wx) || onLine(wy) ? kGrid : kGround, 1.0);
                continue;
            }
            const double gx = (at(x + 1, y) - at(x - 1, y)) / (2 * mm), gy = (at(x, y - 1) - at(x, y + 1)) / (2 * mm);
            const double n0 = -gx, n1 = -gy, n2 = 1, nn = std::sqrt(n0 * n0 + n1 * n1 + n2 * n2);
            const double lit = std::max(0.0, (n0 * l0 + n1 * l1 + n2 * l2) / (nn * ln));
            const double shade = (0.42 + 0.58 * lit) * (0.92 + 0.08 * hm[i] / std::max(top, 1e-6f));
            put(i, on_foot[i] ? kFoot : kRaft, shade / 0.826);  // a flat top shows as the colour itself
        }
    downsample(rgb, w, h, out);
    PictureFrame f;
    f.cx = cx, f.cy = cy, f.scale = sc / 2, f.w = w, f.h = h;
    for (const fruit::Path& path : notches) {
        std::vector<fruit::Vec2> pts;
        for (const auto& q : path) pts.push_back(f.at({fruit::toMm(q.x), fruit::toMm(q.y), 0}));
        blendPolygon(out, pts, kNotch, kNotchAlpha);
    }
    if (frame) *frame = f;
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
