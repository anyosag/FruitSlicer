// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "formats/ThreeMf.h"

#include <locale.h>
#if defined(__APPLE__)
#include <xlocale.h>
#endif

#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <pugixml.hpp>
#include <stdexcept>
#include <vector>

namespace fruit {

namespace {

constexpr std::uint64_t kMaxPartBytes = std::uint64_t(2) << 30;  // 2 GiB of XML per part
constexpr int kMaxDepth = 32;                                    // component nesting

// 3MF numbers always use '.', whatever the user's locale.
locale_t cLocale() {
    static const locale_t loc = newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(nullptr));
    return loc;
}

// "p:path" -> "path": element and attribute names compared without prefixes.
const char* localName(const char* n) {
    const char* c = std::strrchr(n, ':');
    return c ? c + 1 : n;
}

bool is(const pugi::xml_node& n, const char* local) { return std::strcmp(localName(n.name()), local) == 0; }

pugi::xml_node child(const pugi::xml_node& n, const char* local) {
    for (pugi::xml_node c : n.children())
        if (is(c, local)) return c;
    return {};
}

double number(const pugi::xml_node& n, const char* name) {
    const char* s = n.attribute(name).value();
    char* end = nullptr;
    const double v = strtod_l(s, &end, cLocale());
    if (end == s || !std::isfinite(v))
        throw std::runtime_error(std::string("3MF: <") + n.name() + "> has a bad " + name + " value \"" + s + "\"");
    return v;
}

int integer(const pugi::xml_node& n, const char* name) {
    const char* s = n.attribute(name).value();
    char* end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (end == s || v < 0 || v > std::numeric_limits<int32_t>::max())
        throw std::runtime_error(std::string("3MF: <") + n.name() + "> has a bad " + name + " value \"" + s + "\"");
    return static_cast<int>(v);
}

// "m00 m01 m02 m10 m11 m12 m20 m21 m22 m30 m31 m32", applied to row vectors:
// x' = x m00 + y m10 + z m20 + m30, and so on. Transform uses column vectors,
// so the 3x3 part is transposed.
Transform transformOf(const pugi::xml_node& n) {
    Transform t;
    const pugi::xml_attribute a = n.attribute("transform");
    if (!a) return t;
    const char* s = a.value();
    double m[12];
    for (double& v : m) {
        char* end = nullptr;
        v = strtod_l(s, &end, cLocale());
        if (end == s || !std::isfinite(v))
            throw std::runtime_error(std::string("3MF: <") + n.name() + "> has a bad transform \"" + a.value() + "\"");
        s = end;
    }
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) t.m[r][c] = m[c * 3 + r];
    t.t = {m[9], m[10], m[11]};
    return t;
}

// The Production extension's p:path, or "" for the same part.
std::string pathOf(const pugi::xml_node& n) {
    for (pugi::xml_attribute a : n.attributes())
        if (std::strchr(a.name(), ':') && std::strcmp(localName(a.name()), "path") == 0) return a.value();
    return {};
}

// Part names are case-insensitive and start at the package root.
std::string partKey(std::string name) {
    if (!name.empty() && name[0] == '/') name.erase(0, 1);
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return name;
}

double unitToMm(const std::string& unit) {
    if (unit.empty() || unit == "millimeter") return 1.0;
    if (unit == "micron") return 0.001;
    if (unit == "centimeter") return 10.0;
    if (unit == "inch") return 25.4;
    if (unit == "foot") return 304.8;
    if (unit == "meter") return 1000.0;
    throw std::runtime_error("3MF: unknown unit \"" + unit + "\"");
}

class Package {
public:
    explicit Package(const std::string& path) {
        if (!mz_zip_reader_init_file(&zip_, path.c_str(), 0))
            throw std::runtime_error("not a 3MF file (it does not open as a zip archive): " + path);
        open_ = true;
    }
    Package(const void* data, std::size_t size) {
        if (!mz_zip_reader_init_mem(&zip_, data, size, 0))
            throw std::runtime_error("not a 3MF file (it does not open as a zip archive)");
        open_ = true;
    }
    ~Package() {
        if (open_) mz_zip_reader_end(&zip_);
    }
    Package(const Package&) = delete;
    Package& operator=(const Package&) = delete;

    bool has(const std::string& name) {
        return mz_zip_reader_locate_file(&zip_, partKey(name).c_str(), nullptr, 0) >= 0;
    }

    std::string read(const std::string& name) {
        const std::string key = partKey(name);
        const int idx = mz_zip_reader_locate_file(&zip_, key.c_str(), nullptr, 0);
        if (idx < 0) throw std::runtime_error("3MF: the package has no part /" + key);
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip_, static_cast<mz_uint>(idx), &st))
            throw std::runtime_error("3MF: cannot read /" + key);
        if (st.m_uncomp_size > kMaxPartBytes) throw std::runtime_error("3MF: /" + key + " is too large to read");
        std::string out(static_cast<std::size_t>(st.m_uncomp_size), '\0');
        if (!out.empty() && !mz_zip_reader_extract_to_mem(&zip_, static_cast<mz_uint>(idx), out.data(), out.size(), 0))
            throw std::runtime_error("3MF: cannot decompress /" + key + " (damaged file?)");
        return out;
    }

private:
    mz_zip_archive zip_{};
    bool open_ = false;
};

struct Component {
    std::string part;  // "" = same part
    int object = 0;
    Transform t;
};

struct Object {
    bool model = true;  // type "model" (the default); supports and surfaces are skipped
    std::vector<Vec3> vertices;
    std::vector<Face> faces;
    std::vector<Component> components;
};

struct Item {
    std::string part;
    int object = 0;
    Transform t;
    bool printable = true;
};

struct Part {
    std::string unit;
    std::map<int, Object> objects;
    std::vector<Item> build;
};

void readMesh(const pugi::xml_node& mesh, Object& o) {
    for (pugi::xml_node v : child(mesh, "vertices").children())
        if (is(v, "vertex")) o.vertices.push_back({number(v, "x"), number(v, "y"), number(v, "z")});
    const int n = static_cast<int>(o.vertices.size());
    for (pugi::xml_node t : child(mesh, "triangles").children()) {
        if (!is(t, "triangle")) continue;
        const Face f{integer(t, "v1"), integer(t, "v2"), integer(t, "v3")};
        if (f[0] >= n || f[1] >= n || f[2] >= n)
            throw std::runtime_error("3MF: a triangle uses a vertex that does not exist");
        o.faces.push_back(f);
    }
}

Part parsePart(std::string xml, const std::string& name) {
    pugi::xml_document doc;
    const pugi::xml_parse_result r = doc.load_buffer_inplace(xml.data(), xml.size(), pugi::parse_default);
    if (!r) throw std::runtime_error("3MF: /" + partKey(name) + " is not valid XML (" + r.description() + ")");
    const pugi::xml_node model = doc.document_element();
    if (!is(model, "model")) throw std::runtime_error("3MF: /" + partKey(name) + " holds no <model>");
    Part p;
    p.unit = model.attribute("unit").value();
    for (pugi::xml_node sec : model.children()) {
        if (is(sec, "resources")) {
            for (pugi::xml_node obj : sec.children()) {
                if (!is(obj, "object")) continue;
                Object o;
                const char* type = obj.attribute("type").value();
                o.model = !*type || std::strcmp(type, "model") == 0;
                for (pugi::xml_node c : obj.children()) {
                    if (is(c, "mesh")) {
                        readMesh(c, o);
                    } else if (is(c, "components")) {
                        for (pugi::xml_node comp : c.children())
                            if (is(comp, "component"))
                                o.components.push_back({pathOf(comp), integer(comp, "objectid"), transformOf(comp)});
                    }
                }
                p.objects[integer(obj, "id")] = std::move(o);
            }
        } else if (is(sec, "build")) {
            for (pugi::xml_node item : sec.children()) {
                if (!is(item, "item")) continue;
                // Not in the core spec, but slicers write printable="0" for
                // objects switched off on the plate.
                const bool printable = std::strcmp(item.attribute("printable").value(), "0") != 0;
                p.build.push_back({pathOf(item), integer(item, "objectid"), transformOf(item), printable});
            }
        }
    }
    return p;
}

class Reader {
public:
    explicit Reader(Package& pkg) : pkg_(pkg) {}

    TriangleMesh read(ThreeMfInfo* info) {
        std::string root = "3D/3dmodel.model";
        if (pkg_.has("_rels/.rels")) {
            std::string rels = pkg_.read("_rels/.rels");
            pugi::xml_document doc;
            if (doc.load_buffer_inplace(rels.data(), rels.size())) {
                for (pugi::xml_node rel : doc.document_element().children()) {
                    const std::string type = rel.attribute("Type").value();
                    if (is(rel, "Relationship") && type.size() >= 8 &&
                        type.compare(type.size() - 8, 8, "/3dmodel") == 0) {
                        root = rel.attribute("Target").value();
                        break;
                    }
                }
            }
        }
        const Part& rootPart = part(root);
        info_.unit = rootPart.unit.empty() ? "millimeter" : rootPart.unit;
        const double scale = unitToMm(rootPart.unit);
        for (const Item& item : rootPart.build) {
            if (!item.printable) {
                ++info_.skipped;
                continue;
            }
            emit(item.part.empty() ? root : item.part, item.object, item.t, 0);
        }
        if (out_.empty()) throw std::runtime_error("3MF: the file has no printable model in its build");
        if (scale != 1.0)
            for (Vec3& v : out_.vertices) v = v * scale;
        if (info) *info = info_;
        return std::move(out_);
    }

private:
    const Part& part(const std::string& name) {
        const std::string key = partKey(name);
        auto it = parts_.find(key);
        if (it == parts_.end()) it = parts_.emplace(key, parsePart(pkg_.read(key), key)).first;
        return it->second;
    }

    // Appends object `id` of part `name`, mapped to the build by `toBuild`.
    void emit(const std::string& name, int id, const Transform& toBuild, int depth) {
        if (depth > kMaxDepth) throw std::runtime_error("3MF: components nest too deeply (a loop?)");
        const Part& p = part(name);
        const auto it = p.objects.find(id);
        if (it == p.objects.end())
            throw std::runtime_error("3MF: object " + std::to_string(id) + " is missing from /" + partKey(name));
        const Object& o = it->second;
        if (!o.model) {
            ++info_.skipped;
            return;
        }
        if (!o.faces.empty()) {
            if (out_.vertices.size() + o.vertices.size() > std::size_t(std::numeric_limits<int32_t>::max()))
                throw std::runtime_error("3MF: too many vertices");
            const auto base = static_cast<int32_t>(out_.vertices.size());
            for (const Vec3& v : o.vertices) out_.vertices.push_back(toBuild.apply(v));
            // A mirroring transform turns the triangles inside out; undo that.
            const bool mirrored = toBuild.determinant() < 0;
            for (const Face& f : o.faces)
                out_.faces.push_back(mirrored ? Face{f[0] + base, f[2] + base, f[1] + base}
                                              : Face{f[0] + base, f[1] + base, f[2] + base});
            ++info_.meshes;
        }
        for (const Component& c : o.components)
            emit(c.part.empty() ? name : c.part, c.object, c.t.then(toBuild), depth + 1);
    }

    Package& pkg_;
    std::map<std::string, Part> parts_;
    ThreeMfInfo info_;
    TriangleMesh out_;
};

}  // namespace

TriangleMesh read3mf(const std::string& path, ThreeMfInfo* info) {
    Package pkg(path);
    return Reader(pkg).read(info);
}

TriangleMesh read3mfFromMemory(const void* data, std::size_t size, ThreeMfInfo* info) {
    Package pkg(data, size);
    return Reader(pkg).read(info);
}

}  // namespace fruit
