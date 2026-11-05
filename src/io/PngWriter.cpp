#include "io/PngWriter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace fruit {

namespace {

uint32_t crcTable(int n) {
    uint32_t c = static_cast<uint32_t>(n);
    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    return c;
}

uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) table[i] = crcTable(i);
        init = true;
    }
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24));
    v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}

void chunk(std::FILE* f, const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> buf;
    put32(buf, static_cast<uint32_t>(data.size()));
    buf.insert(buf.end(), type, type + 4);
    buf.insert(buf.end(), data.begin(), data.end());
    const uint32_t crc = crc32(buf.data() + 4, buf.size() - 4) ^ 0xFFFFFFFFu;
    put32(buf, crc);
    std::fwrite(buf.data(), 1, buf.size(), f);
}

}  // namespace

namespace {

void writePng(const std::string& path, int w, int h, int channels, const std::vector<uint8_t>& px) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write " + path);
    const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    std::fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    put32(ihdr, static_cast<uint32_t>(w));
    put32(ihdr, static_cast<uint32_t>(h));
    // 8-bit, colour type 0 (gray) or 2 (RGB), deflate, no filter, no interlace
    ihdr.insert(ihdr.end(), {8, static_cast<uint8_t>(channels == 3 ? 2 : 0), 0, 0, 0});
    chunk(f, "IHDR", ihdr);

    // Raw scanlines with filter byte 0, wrapped in stored deflate blocks.
    const size_t row = static_cast<size_t>(w) * channels;
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(h) * (row + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), px.begin() + static_cast<long>(y * row), px.begin() + static_cast<long>((y + 1) * row));
    }
    std::vector<uint8_t> z{0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t pos = 0; pos < raw.size() || raw.empty();) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n));
        z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<long>(pos), raw.begin() + static_cast<long>(pos + n));
        pos += n;
        if (last) break;
    }
    put32(z, (b << 16) | a);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    std::fclose(f);
}

}  // namespace

void writePngGray(const std::string& path, int w, int h, const std::vector<uint8_t>& px) { writePng(path, w, h, 1, px); }

void writePngRgb(const std::string& path, int w, int h, const std::vector<uint8_t>& rgb) { writePng(path, w, h, 3, rgb); }

std::vector<uint8_t> rasterize(const Paths& polys, const RasterWindow& win, int& w, int& h, uint8_t value) {
    const Vec2 size = win.area.size();
    w = std::max(1, static_cast<int>(std::ceil(size.x / win.pixel)));
    h = std::max(1, static_cast<int>(std::ceil(size.y / win.pixel)));
    std::vector<uint8_t> img(static_cast<size_t>(w) * h, 0);
    std::vector<double> xs;
    for (int row = 0; row < h; ++row) {
        // Image row 0 is the top (max y).
        const double y = win.area.max.y - (row + 0.5) * win.pixel;
        xs.clear();
        for (const auto& path : polys)
            for (size_t i = 0, j = path.size() - 1; i < path.size(); j = i++) {
                const Vec2 p = toVec(path[i]), q = toVec(path[j]);
                if ((p.y > y) != (q.y > y)) xs.push_back(p.x + (y - p.y) * (q.x - p.x) / (q.y - p.y));
            }
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            const int c0 = std::max(0, static_cast<int>(std::ceil((xs[k] - win.area.min.x) / win.pixel - 0.5)));
            const int c1 = std::min(w - 1, static_cast<int>(std::floor((xs[k + 1] - win.area.min.x) / win.pixel - 0.5)));
            for (int c = c0; c <= c1; ++c) img[static_cast<size_t>(row) * w + c] = value;
        }
    }
    return img;
}

}  // namespace fruit
