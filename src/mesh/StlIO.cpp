#include "mesh/StlIO.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace fruit {

namespace {

float readF32(const unsigned char* p) {
    float f;
    std::memcpy(&f, p, 4);  // STL is little-endian; so are all supported hosts
    return f;
}

TriangleMesh fromSoup(std::vector<Vec3>&& soup) {
    TriangleMesh m;
    m.vertices = std::move(soup);
    m.faces.reserve(m.vertices.size() / 3);
    for (int32_t i = 0; i + 2 < static_cast<int32_t>(m.vertices.size()); i += 3)
        m.faces.push_back({i, i + 1, i + 2});
    // STL stores float32, so shared corners are bit-identical; a tiny
    // tolerance also absorbs exporters that round inconsistently.
    m.weld(1e-5);
    return m;
}

TriangleMesh parseBinary(const std::vector<unsigned char>& data) {
    uint32_t n;
    std::memcpy(&n, data.data() + 80, 4);
    std::vector<Vec3> soup;
    soup.reserve(static_cast<size_t>(n) * 3);
    const unsigned char* p = data.data() + 84;
    for (uint32_t i = 0; i < n; ++i, p += 50)
        for (int v = 0; v < 3; ++v) {
            const unsigned char* q = p + 12 + 12 * v;
            soup.emplace_back(readF32(q), readF32(q + 4), readF32(q + 8));
        }
    return fromSoup(std::move(soup));
}

TriangleMesh parseAscii(const std::vector<unsigned char>& data) {
    std::string text(data.begin(), data.end());
    std::istringstream in(text);
    std::string tok;
    std::vector<Vec3> soup;
    while (in >> tok) {
        if (tok == "vertex") {
            Vec3 v;
            if (!(in >> v.x >> v.y >> v.z)) throw std::runtime_error("STL: bad vertex line");
            soup.push_back(v);
        }
    }
    if (soup.size() % 3 != 0) throw std::runtime_error("STL: vertex count not a multiple of 3");
    return fromSoup(std::move(soup));
}

}  // namespace

TriangleMesh readStl(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(f)), {});
    if (data.size() >= 84) {
        uint32_t n;
        std::memcpy(&n, data.data() + 80, 4);
        if (data.size() == 84 + static_cast<size_t>(n) * 50) return parseBinary(data);
    }
    if (data.size() >= 5 && std::memcmp(data.data(), "solid", 5) == 0) return parseAscii(data);
    throw std::runtime_error("STL: unrecognised format in " + path);
}

StlWriter::StlWriter(const std::string& path) {
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) throw std::runtime_error("cannot write " + path);
    char header[80] = {};
    std::snprintf(header, sizeof header, "FruitSlicer binary STL");
    std::fwrite(header, 1, 80, file_);
    const uint32_t zero = 0;
    std::fwrite(&zero, 4, 1, file_);
}

StlWriter::~StlWriter() { close(); }

void StlWriter::add(const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 n = normalized(cross(b - a, c - a));
    float buf[12] = {float(n.x), float(n.y), float(n.z), float(a.x), float(a.y), float(a.z),
                     float(b.x), float(b.y), float(b.z), float(c.x), float(c.y), float(c.z)};
    std::fwrite(buf, 4, 12, file_);
    const uint16_t attr = 0;
    std::fwrite(&attr, 2, 1, file_);
    ++count_;
}

void StlWriter::add(const TriangleMesh& mesh) {
    for (const auto& f : mesh.faces) add(mesh.vertices[f[0]], mesh.vertices[f[1]], mesh.vertices[f[2]]);
}

void StlWriter::close() {
    if (!file_) return;
    std::fseek(file_, 80, SEEK_SET);
    std::fwrite(&count_, 4, 1, file_);
    std::fclose(file_);
    file_ = nullptr;
}

void writeStl(const std::string& path, const TriangleMesh& mesh) {
    StlWriter w(path);
    w.add(mesh);
}

}  // namespace fruit
