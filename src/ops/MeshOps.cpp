// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "ops/MeshOps.h"

#include <manifold/manifold.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <unordered_map>

namespace fruit::ops {

struct Solid::Impl {
    manifold::Manifold m;
};

// Lets the free functions below reach into a Solid without befriending each.
struct SolidAccess {
    static const manifold::Manifold& manifold(const Solid& s) { return s.impl_->m; }
    static Solid make(manifold::Manifold m, std::string message) {
        Solid s;
        auto impl = std::make_shared<Solid::Impl>();
        impl->m = std::move(m);
        s.impl_ = std::move(impl);
        s.valid_ = true;
        s.message_ = std::move(message);
        return s;
    }
    static Solid invalid(std::string message) {
        Solid s;
        s.message_ = std::move(message);
        return s;
    }
};

namespace {

std::string errorText(manifold::Manifold::Error e) {
    using E = manifold::Manifold::Error;
    switch (e) {
        case E::NonFiniteVertex: return "a point is not a number";
        case E::NotManifold: return "edges that do not pair up";
        case E::VertexOutOfBounds: return "a face names a point that is not there";
        case E::ResultTooLarge: return "the result is too large";
        case E::Cancelled: return "cancelled";
        default: return "unknown problem";
    }
}

std::string plural(size_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

// Edge usage: directed edge (a -> b) counts.
uint64_t edgeKey(int32_t a, int32_t b) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(a)) << 32) | static_cast<uint32_t>(b);
}

std::string whyNotSolidWelded(const TriangleMesh& m) {
    if (m.faces.empty()) return "The mesh has no faces.";
    for (const Vec3& v : m.vertices)
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
            return "The mesh has points that are not numbers.";
    std::unordered_map<uint64_t, int> count;
    count.reserve(m.faces.size() * 3);
    for (const Face& f : m.faces)
        for (int k = 0; k < 3; ++k) ++count[edgeKey(f[k], f[(k + 1) % 3])];
    size_t open = 0, bad = 0;
    for (const auto& [key, c] : count) {
        const int32_t a = static_cast<int32_t>(key >> 32), b = static_cast<int32_t>(key & 0xffffffffu);
        const auto rev = count.find(edgeKey(b, a));
        const int back = rev == count.end() ? 0 : rev->second;
        if (c == 1 && back == 1) continue;
        if (back == 0) ++open;  // counted once per open edge: only the directed one exists
        else if (a < b) ++bad;  // shared by more than two faces, or flipped, once per edge
    }
    if (open == 0 && bad == 0) return {};
    std::string s = "The mesh is not a closed solid: ";
    if (open) s += plural(open, "edge is", "edges are") + " open (holes or loose faces)";
    if (open && bad) s += " and ";
    if (bad) s += plural(bad, "edge is", "edges are") + " shared by more than two faces or has faces turned the wrong way";
    return s + ". Repair it first.";
}

}  // namespace

std::string whyNotSolid(const TriangleMesh& mesh) {
    TriangleMesh m = mesh;
    m.weld();
    return whyNotSolidWelded(m);
}

Solid::Solid(const TriangleMesh& mesh) {
    TriangleMesh m = mesh;
    m.weld();
    if (std::string why = whyNotSolidWelded(m); !why.empty()) {
        message_ = std::move(why);
        return;
    }
    std::string note;
    if (m.volume() < 0) {
        for (Face& f : m.faces) std::swap(f[1], f[2]);
        note = "The faces pointed inwards, so they were turned round.";
    }
    manifold::MeshGL64 mg;
    mg.numProp = 3;
    mg.vertProperties.reserve(m.vertices.size() * 3);
    for (const Vec3& v : m.vertices) {
        mg.vertProperties.push_back(v.x);
        mg.vertProperties.push_back(v.y);
        mg.vertProperties.push_back(v.z);
    }
    mg.triVerts.reserve(m.faces.size() * 3);
    for (const Face& f : m.faces)
        for (int k = 0; k < 3; ++k) mg.triVerts.push_back(static_cast<uint64_t>(f[k]));
    manifold::Manifold man(mg);
    if (man.Status() != manifold::Manifold::Error::NoError) {
        message_ = "The mesh is not a closed solid that can be combined (" +
                   errorText(man.Status()) + "). Repair it first.";
        return;
    }
    if (man.IsEmpty() || man.Volume() <= 0) {
        message_ = "The mesh has no volume.";
        return;
    }
    *this = SolidAccess::make(std::move(man), std::move(note));
}

bool Solid::empty() const { return valid_ && impl_->m.IsEmpty(); }
double Solid::volume() const { return valid_ ? impl_->m.Volume() : 0.0; }

BBox3 Solid::bbox() const {
    BBox3 b;
    if (!valid_ || impl_->m.IsEmpty()) return b;
    const auto box = impl_->m.BoundingBox();
    b.min = {box.min.x, box.min.y, box.min.z};
    b.max = {box.max.x, box.max.y, box.max.z};
    return b;
}

TriangleMesh Solid::toMesh() const {
    TriangleMesh out;
    if (!valid_ || impl_->m.IsEmpty()) return out;
    const manifold::MeshGL64 mg = impl_->m.GetMeshGL64();
    const size_t np = mg.numProp;
    out.vertices.reserve(mg.vertProperties.size() / np);
    for (size_t i = 0; i + np <= mg.vertProperties.size(); i += np)
        out.vertices.push_back({mg.vertProperties[i], mg.vertProperties[i + 1], mg.vertProperties[i + 2]});
    out.faces.reserve(mg.triVerts.size() / 3);
    for (size_t i = 0; i + 3 <= mg.triVerts.size(); i += 3)
        out.faces.push_back({static_cast<int32_t>(mg.triVerts[i]), static_cast<int32_t>(mg.triVerts[i + 1]),
                             static_cast<int32_t>(mg.triVerts[i + 2])});
    return out;
}

Solid extrude(const std::vector<std::vector<Vec2>>& polygons, double height) {
    if (!(height > 0) || !std::isfinite(height)) return SolidAccess::invalid("The height must be more than zero.");
    manifold::Polygons polys;
    for (const auto& p : polygons) {
        manifold::SimplePolygon sp;
        sp.reserve(p.size());
        for (const Vec2& v : p) sp.push_back({v.x, v.y});
        polys.push_back(std::move(sp));
    }
    manifold::Manifold m = manifold::Manifold::Extrude(polys, height);
    if (m.Status() != manifold::Manifold::Error::NoError)
        return SolidAccess::invalid("The outlines could not be made into a solid.");
    return SolidAccess::make(std::move(m), {});
}

MeshResult cleanMesh(const TriangleMesh& mesh) {
    MeshResult r;
    const Solid s(mesh);
    r.message = s.message();
    if (!s.valid()) return r;
    r.mesh = s.toMesh();
    r.ok = true;
    return r;
}

namespace {

Solid pairOp(const Solid& a, const Solid& b,
             manifold::Manifold (*op)(const manifold::Manifold&, const manifold::Manifold&)) {
    if (!a.valid()) return SolidAccess::invalid("First mesh: " + a.message());
    if (!b.valid()) return SolidAccess::invalid("Second mesh: " + b.message());
    manifold::Manifold r = op(SolidAccess::manifold(a), SolidAccess::manifold(b));
    if (r.Status() != manifold::Manifold::Error::NoError)
        return SolidAccess::invalid("The operation failed (" + errorText(r.Status()) + ").");
    return SolidAccess::make(std::move(r), {});
}

MeshResult finish(const Solid& s, const char* empty_note) {
    MeshResult r;
    r.message = s.message();
    if (!s.valid()) return r;
    r.mesh = s.toMesh();
    r.ok = true;
    if (s.empty()) r.message = empty_note;
    return r;
}

}  // namespace

Solid unite(const Solid& a, const Solid& b) {
    return pairOp(a, b, [](const manifold::Manifold& x, const manifold::Manifold& y) { return x + y; });
}
Solid subtract(const Solid& a, const Solid& b) {
    return pairOp(a, b, [](const manifold::Manifold& x, const manifold::Manifold& y) { return x - y; });
}
Solid intersect(const Solid& a, const Solid& b) {
    return pairOp(a, b, [](const manifold::Manifold& x, const manifold::Manifold& y) { return x ^ y; });
}

MeshResult unionMeshes(const TriangleMesh& a, const TriangleMesh& b) {
    return finish(unite(Solid(a), Solid(b)), "Nothing is left.");
}
MeshResult subtractMeshes(const TriangleMesh& a, const TriangleMesh& b) {
    return finish(subtract(Solid(a), Solid(b)), "The second mesh covers the whole first one, so nothing is left.");
}
MeshResult intersectMeshes(const TriangleMesh& a, const TriangleMesh& b) {
    return finish(intersect(Solid(a), Solid(b)), "The two meshes do not overlap, so nothing is left.");
}

std::pair<Solid, Solid> cutByPlane(const Solid& solid, const Plane& plane) {
    if (!solid.valid()) return {SolidAccess::invalid(solid.message()), SolidAccess::invalid(solid.message())};
    const Vec3 n = normalized(plane.normal);
    if (norm2(n) == 0)
        return {SolidAccess::invalid("The cutting plane has no direction."),
                SolidAccess::invalid("The cutting plane has no direction.")};
    auto [up, down] = SolidAccess::manifold(solid).SplitByPlane({n.x, n.y, n.z}, dot(n, plane.point));
    if (up.Status() != manifold::Manifold::Error::NoError)
        return {SolidAccess::invalid("The cut failed."), SolidAccess::invalid("The cut failed.")};
    return {SolidAccess::make(std::move(up), {}), SolidAccess::make(std::move(down), {})};
}

CutResult cutByPlane(const TriangleMesh& mesh, const Plane& plane) {
    CutResult r;
    const Solid s(mesh);
    auto [up, down] = cutByPlane(s, plane);
    if (!up.valid()) {
        r.message = up.message();
        return r;
    }
    r.above = up.toMesh();
    r.below = down.toMesh();
    r.ok = true;
    if (up.empty() || down.empty())
        r.message = "The plane does not cut through the mesh: everything is on the " +
                    std::string(up.empty() ? "lower" : "upper") + " side.";
    return r;
}

namespace {

struct ShellInfo {
    double volume = 0;
    BBox3 box;
    int32_t first_vertex = 0;
};

bool contains(const BBox3& outer, const BBox3& in) {
    return in.min.x >= outer.min.x && in.max.x <= outer.max.x && in.min.y >= outer.min.y &&
           in.max.y <= outer.max.y && in.min.z >= outer.min.z && in.max.z <= outer.max.z;
}

}  // namespace

std::vector<TriangleMesh> splitShells(const TriangleMesh& input) {
    TriangleMesh mesh = input;
    mesh.weld();
    int n = 0;
    const std::vector<int> ids = mesh.shellIds(&n);
    if (n == 0) return {};
    std::vector<ShellInfo> info(static_cast<size_t>(n));
    const Vec3 o = mesh.bbox().center();
    for (size_t i = 0; i < mesh.faces.size(); ++i) {
        const Face& f = mesh.faces[i];
        ShellInfo& s = info[static_cast<size_t>(ids[i])];
        const Vec3 a = mesh.vertices[static_cast<size_t>(f[0])] - o, b = mesh.vertices[static_cast<size_t>(f[1])] - o,
                   c = mesh.vertices[static_cast<size_t>(f[2])] - o;
        s.volume += dot(a, cross(b, c)) / 6.0;
        s.box.extend(a + o);
        s.box.extend(b + o);
        s.box.extend(c + o);
    }
    // A shell with a negative volume is a cavity if a larger positive shell
    // holds it (bounding box test), else an inside-out piece to turn round.
    std::vector<int> owner(static_cast<size_t>(n));
    std::iota(owner.begin(), owner.end(), 0);
    std::vector<char> flip(static_cast<size_t>(n), 0);
    for (int i = 0; i < n; ++i) {
        if (info[static_cast<size_t>(i)].volume >= 0) continue;
        int best = -1;
        for (int j = 0; j < n; ++j) {
            if (j == i || info[static_cast<size_t>(j)].volume <= 0) continue;
            if (!contains(info[static_cast<size_t>(j)].box, info[static_cast<size_t>(i)].box)) continue;
            if (best < 0 || info[static_cast<size_t>(j)].volume < info[static_cast<size_t>(best)].volume) best = j;
        }
        if (best >= 0) owner[static_cast<size_t>(i)] = best;
        else flip[static_cast<size_t>(i)] = 1;
    }
    std::vector<int> pieces;  // owner ids that are pieces
    for (int i = 0; i < n; ++i)
        if (owner[static_cast<size_t>(i)] == i) pieces.push_back(i);
    auto vol = [&](int s) { return std::abs(info[static_cast<size_t>(s)].volume); };
    std::stable_sort(pieces.begin(), pieces.end(), [&](int a, int b) { return vol(a) > vol(b); });
    std::vector<int> slot(static_cast<size_t>(n), -1);
    for (size_t k = 0; k < pieces.size(); ++k) slot[static_cast<size_t>(pieces[k])] = static_cast<int>(k);

    std::vector<TriangleMesh> out(pieces.size());
    std::vector<std::vector<int32_t>> remap(pieces.size(), std::vector<int32_t>(mesh.vertices.size(), -1));
    for (size_t i = 0; i < mesh.faces.size(); ++i) {
        const int shell = ids[i];
        const size_t k = static_cast<size_t>(slot[static_cast<size_t>(owner[static_cast<size_t>(shell)])]);
        Face nf{};
        for (int c = 0; c < 3; ++c) {
            const int32_t v = mesh.faces[i][c];
            int32_t& m = remap[k][static_cast<size_t>(v)];
            if (m < 0) {
                m = static_cast<int32_t>(out[k].vertices.size());
                out[k].vertices.push_back(mesh.vertices[static_cast<size_t>(v)]);
            }
            nf[static_cast<size_t>(c)] = m;
        }
        if (flip[static_cast<size_t>(shell)]) std::swap(nf[1], nf[2]);
        out[k].faces.push_back(nf);
    }
    return out;
}

int countPieces(const TriangleMesh& mesh) { return static_cast<int>(splitShells(mesh).size()); }

CollisionResult checkCollision(const Solid& a, const Solid& b, double volume_threshold, double gap_search) {
    CollisionResult r;
    if (!a.valid()) { r.message = "First mesh: " + a.message(); return r; }
    if (!b.valid()) { r.message = "Second mesh: " + b.message(); return r; }
    r.ok = true;
    const double margin = std::max(0.0, gap_search);
    BBox3 ba = a.bbox(), bb = b.bbox();
    if (ba.empty() || bb.empty()) return r;
    // Bounding boxes farther apart than the search length: nothing to measure.
    const Vec3 pad{margin, margin, margin};
    ba.min -= pad;
    ba.max += pad;
    if (!ba.overlaps(bb)) return r;
    const manifold::Manifold& ma = SolidAccess::manifold(a);
    const manifold::Manifold& mb = SolidAccess::manifold(b);
    const manifold::Manifold both = ma ^ mb;
    r.overlap_volume = both.IsEmpty() ? 0.0 : both.Volume();
    r.overlaps = r.overlap_volume > volume_threshold;
    if (r.overlaps) { r.gap = 0; return r; }
    if (gap_search > 0) {
        const double g = ma.MinGap(mb, gap_search);
        r.gap = g < gap_search ? g : -1;
    }
    return r;
}

CollisionResult checkCollision(const TriangleMesh& a, const TriangleMesh& b, double volume_threshold,
                               double gap_search) {
    return checkCollision(Solid(a), Solid(b), volume_threshold, gap_search);
}

}  // namespace fruit::ops
