#include "mesh/ObjIO.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "mesh/StlIO.h"

namespace fruit {

TriangleMesh readObj(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    TriangleMesh m;
    std::string line;
    std::vector<int32_t> poly;
    size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        if (line.size() < 2) continue;
        if (line[0] == 'v' && line[1] == ' ') {
            std::istringstream ss(line.substr(2));
            Vec3 v;
            if (!(ss >> v.x >> v.y >> v.z)) throw std::runtime_error("OBJ: bad vertex at line " + std::to_string(line_no));
            m.vertices.push_back(v);
        } else if (line[0] == 'f' && line[1] == ' ') {
            std::istringstream ss(line.substr(2));
            std::string tok;
            poly.clear();
            while (ss >> tok) {
                const long idx = std::stol(tok.substr(0, tok.find('/')));
                const long n = static_cast<long>(m.vertices.size());
                const long i = idx > 0 ? idx - 1 : n + idx;
                if (i < 0 || i >= n) throw std::runtime_error("OBJ: bad face index at line " + std::to_string(line_no));
                poly.push_back(static_cast<int32_t>(i));
            }
            for (size_t k = 1; k + 1 < poly.size(); ++k) m.faces.push_back({poly[0], poly[k], poly[k + 1]});
        }
    }
    m.weld(1e-9);
    return m;
}

TriangleMesh readMesh(const std::string& path) {
    std::string ext = path.size() >= 4 ? path.substr(path.size() - 4) : "";
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    if (ext == ".obj") return readObj(path);
    return readStl(path);
}

}  // namespace fruit
