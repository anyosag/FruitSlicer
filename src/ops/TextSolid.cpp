// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "ops/TextSolid.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wpedantic"
#endif
// stb_truetype as shipped with Dear ImGui. Static, so it cannot clash with
// the copy inside the app's imgui_draw.cpp.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "geometry/Polygon.h"

extern const std::uint8_t kFruitTextFontInter[];
extern const std::size_t kFruitTextFontInter_size;

namespace fruit::ops {

const FontData& interRegular() {
    static const FontData f{kFruitTextFontInter, kFruitTextFontInter_size};
    return f;
}

namespace {

struct Pt {
    double x, y;
};

// Decodes one UTF-8 code point; bad bytes give U+FFFD.
char32_t nextCodepoint(const std::string& s, size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i++]);
    if (c < 0x80) return c;
    int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : -1;
    if (extra < 0) return 0xFFFD;
    char32_t cp = c & (0x3F >> extra);
    for (int k = 0; k < extra; ++k) {
        if (i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3F);
    }
    return cp;
}

void appendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Flattens the outline of one glyph into closed polygons, in font units.
std::vector<std::vector<Pt>> glyphContours(const stbtt_fontinfo& font, int glyph, double tol_units) {
    std::vector<std::vector<Pt>> out;
    stbtt_vertex* v = nullptr;
    const int n = stbtt_GetGlyphShape(&font, glyph, &v);
    std::vector<Pt> cur;
    Pt pen{0, 0};
    auto close = [&] {
        if (cur.size() >= 3) out.push_back(std::move(cur));
        cur.clear();
    };
    for (int i = 0; i < n; ++i) {
        const stbtt_vertex& s = v[i];
        const Pt p{static_cast<double>(s.x), static_cast<double>(s.y)};
        switch (s.type) {
            case STBTT_vmove:
                close();
                cur.push_back(p);
                break;
            case STBTT_vline:
                cur.push_back(p);
                break;
            case STBTT_vcurve: {
                const Pt c{static_cast<double>(s.cx), static_cast<double>(s.cy)};
                const double dev = std::hypot(pen.x - 2 * c.x + p.x, pen.y - 2 * c.y + p.y);
                const int k = std::clamp(static_cast<int>(std::ceil(std::sqrt(dev / (4 * tol_units)))), 1, 64);
                for (int j = 1; j <= k; ++j) {
                    const double t = static_cast<double>(j) / k, u = 1 - t;
                    cur.push_back({u * u * pen.x + 2 * u * t * c.x + t * t * p.x,
                                   u * u * pen.y + 2 * u * t * c.y + t * t * p.y});
                }
                break;
            }
            case STBTT_vcubic: {
                const Pt c1{static_cast<double>(s.cx), static_cast<double>(s.cy)};
                const Pt c2{static_cast<double>(s.cx1), static_cast<double>(s.cy1)};
                const double m = std::max(std::hypot(pen.x - 2 * c1.x + c2.x, pen.y - 2 * c1.y + c2.y),
                                          std::hypot(c1.x - 2 * c2.x + p.x, c1.y - 2 * c2.y + p.y));
                const int k = std::clamp(static_cast<int>(std::ceil(std::sqrt(0.75 * m / tol_units))), 1, 64);
                for (int j = 1; j <= k; ++j) {
                    const double t = static_cast<double>(j) / k, u = 1 - t;
                    cur.push_back({u * u * u * pen.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * p.x,
                                   u * u * u * pen.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * p.y});
                }
                break;
            }
            default:
                break;
        }
        pen = p;
    }
    close();
    if (v) stbtt_FreeShape(&font, v);
    return out;
}

}  // namespace

TextResult makeTextSolid(const std::string& text, const TextStyle& style) {
    TextResult r;
    if (!(style.height > 0) || !(style.depth > 0) || !std::isfinite(style.height) || !std::isfinite(style.depth)) {
        r.message = "The height and depth of the text must be more than zero.";
        return r;
    }
    if (text.size() > 1000) {
        r.message = "The text is too long (most 1000 bytes).";
        return r;
    }
    const FontData fd = style.font.data ? style.font : interRegular();
    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, fd.data, stbtt_GetFontOffsetForIndex(fd.data, 0))) {
        r.message = "The font could not be read.";
        return r;
    }
    int x0, y0, x1, y1;
    if (!stbtt_GetCodepointBox(&font, 'H', &x0, &y0, &x1, &y1) || y1 <= 0) {
        r.message = "The font has no capital letters to size the text by.";
        return r;
    }
    const double cap_units = static_cast<double>(y1);
    const double scale = style.height / cap_units;  // mm per font unit
    const double tol_units = std::max(1e-4, style.curve_tolerance) / scale;

    Paths paths;
    double pen_x = 0, pen_y = 0;
    std::string left_out;
    size_t i = 0;
    int drawn = 0;
    while (i < text.size()) {
        const char32_t cp = nextCodepoint(text, i);
        if (cp == U'\n') {
            pen_x = 0;
            pen_y -= style.line_spacing * style.height;
            continue;
        }
        if (cp == U'\r') continue;
        const int glyph = stbtt_FindGlyphIndex(&font, static_cast<int>(cp));
        int adv = 0, lsb = 0;
        if (glyph == 0 && cp != U' ') {
            if (!left_out.empty()) left_out += ' ';
            appendUtf8(left_out, cp);
            continue;
        }
        stbtt_GetGlyphHMetrics(&font, glyph, &adv, &lsb);
        for (const auto& c : glyphContours(font, glyph, tol_units)) {
            Path p;
            p.reserve(c.size());
            for (const Pt& q : c) p.push_back(toPoint({pen_x + q.x * scale, pen_y + q.y * scale}));
            paths.push_back(std::move(p));
            ++drawn;
        }
        pen_x += adv * scale + style.extra_spacing;
    }
    if (!left_out.empty()) left_out = "Left out (not in the font): " + left_out + ".";
    if (drawn == 0) {
        r.message = left_out.empty() ? "There is nothing to draw." : left_out;
        return r;
    }
    // Overlapping contours of one glyph (or of neighbours) merge; holes are
    // the contours that wind the other way.
    Paths merged = Clipper2Lib::Union(paths, Clipper2Lib::FillRule::NonZero);
    merged = Clipper2Lib::SimplifyPaths(merged, 0.5 * kScale * std::max(1e-4, style.curve_tolerance) * 0.1);
    if (merged.empty()) {
        r.message = "There is nothing to draw.";
        return r;
    }
    BBox2 box;
    for (const Path& p : merged)
        for (const Point64& q : p) box.extend(toVec(q));
    const Vec2 c = box.center();
    std::vector<std::vector<Vec2>> polys;
    for (const Path& p : merged) {
        if (p.size() < 3) continue;
        std::vector<Vec2> sp;
        sp.reserve(p.size());
        for (const Point64& q : p) sp.push_back(toVec(q) - c);
        polys.push_back(std::move(sp));
    }
    const Solid extruded = extrude(polys, style.depth);
    if (!extruded.valid() || extruded.empty()) {
        r.message = "The letters could not be made into a solid.";
        return r;
    }
    r.mesh = extruded.toMesh();
    r.ok = true;
    r.message = left_out;
    return r;
}

TriangleMesh placeText(const TriangleMesh& text, const TextPlacement& pl, double z_shift) {
    const Vec3 z = normalized(pl.normal);
    Vec3 y = pl.up - z * dot(pl.up, z);
    if (norm2(y) < 1e-12) {  // up along the normal (or missing): any direction along the surface
        const Vec3 axis = std::abs(z.z) < 0.9 ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
        y = axis - z * dot(axis, z);
    }
    y = normalized(y);
    const Vec3 x = cross(y, z);
    TriangleMesh out = text;
    for (Vec3& v : out.vertices) v = pl.point + x * v.x + y * v.y + z * (v.z + z_shift);
    return out;
}

MeshResult applyText(const TriangleMesh& base, const std::string& text, const TextStyle& style,
                     const TextPlacement& pl) {
    MeshResult r;
    if (norm2(pl.normal) == 0) {
        r.message = "The surface has no direction to put the text on.";
        return r;
    }
    const Solid b(base);
    if (!b.valid()) {
        r.message = b.message();
        return r;
    }
    // Both ways the letters reach a little past the surface, so the result is
    // fused (raised) or cut through cleanly (engraved), not left touching.
    const double over = std::min(0.1, 0.5 * style.depth);
    TextStyle s = style;
    s.depth = style.depth + over;
    const TextResult t = makeTextSolid(text, s);
    if (!t.ok) {
        r.message = t.message;
        return r;
    }
    const TriangleMesh placed = placeText(t.mesh, pl, pl.engrave ? -style.depth : -over);
    const Solid ts(placed);
    if (!ts.valid()) {
        r.message = "The letters: " + ts.message();
        return r;
    }
    const Solid res = pl.engrave ? subtract(b, ts) : unite(b, ts);
    if (!res.valid()) {
        r.message = res.message();
        return r;
    }
    const double tiny = 1e-6;
    if (pl.engrave ? (b.volume() - res.volume() < tiny) : (ts.volume() + b.volume() - res.volume() < tiny)) {
        r.message = "The text did not meet the surface there, so nothing changed.";
        return r;
    }
    r.mesh = res.toMesh();
    r.ok = true;
    r.message = t.message;
    return r;
}

}  // namespace fruit::ops
