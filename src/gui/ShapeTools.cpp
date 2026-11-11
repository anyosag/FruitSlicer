// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "ShapeTools.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <map>

#include "Document.h"
#include "mesh/ModelCheck.h"
#include "mesh/ModelLoad.h"
#include "mesh/Placement.h"

namespace gui {

namespace {

// The reason a mesh was refused, with where to look: "Repair it first" is the Repair tool.
std::string withRepairHint(std::string why) {
    const std::string tail = "Repair it first.";
    const auto at = why.rfind(tail);
    if (at != std::string::npos) why.replace(at, tail.size(), "Repair it first: the Repair tool shows what was found.");
    return why;
}

std::string quoted(const std::string& name) { return "“" + name + "”"; }

// A mesh in plate coordinates as a part's own mesh: back through the
// transform, repaired and checked like any loaded model. Empty when it came
// to nothing.
std::shared_ptr<const PartModel> ownModel(fruit::TriangleMesh plate, const fruit::Transform& own_to_plate) {
    plate.transform(own_to_plate.inverse());
    fruit::LoadedModel m = fruit::prepareModel(std::move(plate));
    if (m.mesh.empty()) return nullptr;
    return makePartModel(std::move(m));
}

fruit::BBox2 footprintOf(const fruit::TriangleMesh& mesh, const fruit::Placement& p) {
    const fruit::Transform t = fruit::placementTransform(mesh, p);
    fruit::BBox2 b;
    for (const fruit::Vec3& v : mesh.vertices) {
        const fruit::Vec3 w = t.apply(v);
        b.extend(fruit::Vec2{w.x, w.y});
    }
    return b;
}

std::vector<std::string> namesExcept(PartsEdit& e, std::initializer_list<int> gone) {
    std::vector<std::string> names;
    for (const PartState& s : e.parts())
        if (std::find(gone.begin(), gone.end(), s.id) == gone.end()) names.push_back(s.name);
    return names;
}

std::vector<fruit::BBox2> boxesExcept(PartsEdit& e, std::initializer_list<int> gone) {
    std::vector<fruit::BBox2> boxes;
    for (const PartState& s : e.parts())
        if (std::find(gone.begin(), gone.end(), s.id) == gone.end() && s.model)
            boxes.push_back(footprintOf(s.model->model.mesh, s.placement));
    return boxes;
}

}  // namespace

std::string pieceName(const std::string& base, const std::string& suffix, const std::vector<std::string>& taken) {
    const std::string name = base + suffix;
    auto used = [&](const std::string& n) { return std::find(taken.begin(), taken.end(), n) != taken.end(); };
    if (!used(name)) return name;
    for (int i = 2;; ++i) {
        const std::string n = name + " " + std::to_string(i);
        if (!used(n)) return n;
    }
}

fruit::Placement placeInPlace(const ShapePiece& piece, fruit::Placement base) {
    const fruit::TriangleMesh& mesh = piece.model->model.mesh;
    if (mesh.vertices.empty()) return base;
    base.offset = {0, 0};
    const fruit::Transform t0 = fruit::placementTransform(mesh, base);
    // Any point of the mesh does: it must land where it stood.
    const fruit::Vec3 ref = mesh.vertices.front();
    const fruit::Vec3 want = piece.own_to_plate.apply(ref), got = t0.apply(ref);
    base.offset = {want.x - got.x, want.y - got.y};
    return base;
}

// ---- Cut ----

const char* cutSideWord(CutAxis a, bool above) {
    switch (a) {
        case CutAxis::Z: return above ? "top" : "bottom";
        case CutAxis::X: return above ? "right" : "left";
        case CutAxis::Y: return above ? "back" : "front";
    }
    return "";
}

CutRange cutRange(const Part& part, CutAxis axis) {
    const fruit::BBox3& b = part.box();
    switch (axis) {
        case CutAxis::Z: return {b.min.z - part.placement().lift(), b.max.z - part.placement().lift()};
        case CutAxis::X: return {b.min.x, b.max.x};
        case CutAxis::Y: return {b.min.y, b.max.y};
    }
    return {};
}

void cutFit(CutSettings& s, const Part& part) {
    const CutRange r = cutRange(part, s.axis);
    if (!s.placed || s.part != part.id() || s.placed_axis != s.axis) {
        s.position = std::round((r.lo + r.hi) / 2 * 10) / 10;
        s.part = part.id();
        s.placed = true;
        s.placed_axis = s.axis;
    }
    s.position = std::clamp(s.position, r.lo, r.hi);
}

fruit::ops::Plane cutPlane(const Part& part, const CutSettings& s) {
    fruit::ops::Plane p;
    switch (s.axis) {
        case CutAxis::Z:
            p.point = {0, 0, part.placement().lift() + s.position};
            p.normal = {0, 0, 1};
            break;
        case CutAxis::X:
            p.point = {s.position, 0, 0};
            p.normal = {1, 0, 0};
            break;
        case CutAxis::Y:
            p.point = {0, s.position, 0};
            p.normal = {0, 1, 0};
            break;
    }
    return p;
}

bool cutCrosses(const Part& part, const CutSettings& s) {
    const CutRange r = cutRange(part, s.axis);
    return s.position > r.lo + 0.01 && s.position < r.hi - 0.01;
}

CutJob cutJobFor(const Part& part, const CutSettings& s) {
    CutJob j;
    j.placed = part.placed();
    j.own_to_plate = part.transform();
    j.plane = cutPlane(part, s);
    j.axis = s.axis;
    j.keep = s.keep;
    j.name = part.name();
    return j;
}

ShapeOutcome cutPart(const CutJob& job, const fruit::JobControl& ctl) {
    ShapeOutcome out;
    try {
        ctl.step("cut", 0.1);
        const fruit::ops::CutResult r = fruit::ops::cutByPlane(job.placed, job.plane);
        ctl.step("pieces", 0.7);
        if (!r.ok) {
            out.message = "Couldn't cut " + quoted(job.name) + ". " + withRepairHint(r.message);
            return out;
        }
        const bool want_below = job.keep != CutKeep::Above, want_above = job.keep != CutKeep::Below;
        if (r.above.empty() || r.below.empty()) {
            out.message = "The cut plane doesn't pass through " + quoted(job.name) + ", so nothing was cut.";
            return out;
        }
        struct Side {
            const fruit::TriangleMesh* mesh;
            bool above;
        };
        std::vector<Side> sides;
        if (want_below) sides.push_back({&r.below, false});
        if (want_above) sides.push_back({&r.above, true});
        for (const Side& s : sides) {
            ShapePiece p;
            p.model = ownModel(*s.mesh, job.own_to_plate);
            if (!p.model) {
                out.message = "Nothing was left on the " + std::string(cutSideWord(job.axis, s.above)) + " side.";
                out.pieces.clear();
                return out;
            }
            p.own_to_plate = job.own_to_plate;
            p.suffix = sides.size() > 1 ? std::string(" (") + cutSideWord(job.axis, s.above) + ")" : "";
            out.pieces.push_back(std::move(p));
        }
        ctl.check();
        if (sides.size() == 2)
            out.message = std::string("Cut into two parts, ") + cutSideWord(job.axis, false) + " and " +
                          cutSideWord(job.axis, true) + ". The cut faces are filled.";
        else
            out.message = std::string("Kept the ") + cutSideWord(job.axis, want_above) + " side. The cut face is filled.";
        out.ok = true;
    } catch (const fruit::Cancelled&) {
        throw;
    } catch (const std::exception& e) {
        out.pieces.clear();
        out.message = "Couldn't cut " + quoted(job.name) + ": " + e.what();
    }
    return out;
}

std::vector<int> cutIntoParts(PartsEdit& e, const Part& old, const ShapeOutcome& out, const fruit::PrinterProfile& printer) {
    std::vector<int> ids;
    if (!out.ok || out.pieces.empty()) return ids;
    const int gone = old.id();
    std::vector<std::string> names = namesExcept(e, {gone});
    std::vector<fruit::BBox2> boxes = boxesExcept(e, {gone});
    e.remove(gone);
    for (std::size_t i = 0; i < out.pieces.size(); ++i) {
        const ShapePiece& piece = out.pieces[i];
        fruit::Placement p = old.placement();
        if (i == 0) {
            p = placeInPlace(piece, p);
        } else {
            const fruit::PlateFit f = fruit::checkFit(piece.model->model.mesh, p, printer);
            p.offset = freeSpot(boxes, {f.size.x, f.size.y}, printer, fruit::plateMargin(p));
        }
        boxes.push_back(footprintOf(piece.model->model.mesh, p));
        const std::string name = pieceName(old.name(), piece.suffix, names);
        names.push_back(name);
        ids.push_back(e.add(NewPart{name, "", piece.model, p}));  // made in the app: no file of its own
    }
    e.selected = ids.front();
    return ids;
}

// ---- Label ----

namespace {

using Edge = std::pair<fruit::Vec3, fruit::Vec3>;

std::string previewKey(const LabelSettings& s) {
    return s.text + "|" + std::to_string(s.height) + "|" + std::to_string(s.depth);
}

// The edges round the flat side of letters (the z = 0 side), once each.
std::vector<Edge> flatSideOutline(const fruit::TriangleMesh& m) {
    using Key = std::array<long long, 4>;
    std::map<Key, std::pair<int, Edge>> edges;
    auto q = [](double v) { return static_cast<long long>(std::llround(v * 1e4)); };
    for (std::size_t f = 0; f < m.faceCount(); ++f) {
        const fruit::Face& face = m.faces[f];
        const fruit::Vec3 v[3] = {m.vertices[static_cast<std::size_t>(face[0])], m.vertices[static_cast<std::size_t>(face[1])],
                                  m.vertices[static_cast<std::size_t>(face[2])]};
        if (std::abs(v[0].z) > 1e-6 || std::abs(v[1].z) > 1e-6 || std::abs(v[2].z) > 1e-6) continue;
        for (int k = 0; k < 3; ++k) {
            const fruit::Vec3 a = v[k], b = v[(k + 1) % 3];
            Key ka{q(a.x), q(a.y), q(b.x), q(b.y)}, kb{q(b.x), q(b.y), q(a.x), q(a.y)};
            const Key key = std::min(ka, kb);
            auto it = edges.find(key);
            if (it == edges.end()) edges.emplace(key, std::make_pair(1, Edge{a, b}));
            else ++it->second.first;
        }
    }
    std::vector<Edge> out;
    for (const auto& [key, e] : edges)
        if (e.first == 1) out.push_back(e.second);
    return out;
}

}  // namespace

const LabelPreview& labelPreview(LabelSettings& s) {
    LabelPreview& p = s.preview;
    const std::string key = previewKey(s);
    if (p.key == key && (p.ok || !p.message.empty())) return p;
    p = LabelPreview{};
    p.key = key;
    if (s.text.empty()) {
        p.message = "Type the text first.";
        return p;
    }
    fruit::ops::TextStyle style;
    style.height = s.height;
    style.depth = s.depth;
    const fruit::ops::TextResult t = fruit::ops::makeTextSolid(s.text, style);
    p.ok = t.ok;
    p.message = t.message.empty() && !t.ok ? "The letters could not be made." : t.message;
    if (t.ok) {
        p.text = t.mesh;
        p.outline = flatSideOutline(p.text);
    }
    return p;
}

bool labelSpotStillOn(const LabelSettings& s, const Document& doc) {
    if (!s.spot) return false;
    const Part* part = doc.findPart(s.spot->part);
    return part && part->geometryId() == s.spot->geometry_id;
}

fruit::ops::TextPlacement labelPlacement(const Part& part, const LabelSpot& spot, bool engrave) {
    fruit::ops::TextPlacement p;
    p.point = part.transform().apply(spot.point);
    p.normal = fruit::normalized(part.transform().applyLinear(spot.normal));
    // Upright on a side; on a face that lies flat, the tops of the letters point to the back.
    p.up = std::abs(p.normal.z) > 0.9 ? fruit::Vec3{0, 1, 0} : fruit::Vec3{0, 0, 1};
    p.engrave = engrave;
    return p;
}

LabelSpot labelSpotAt(const Part& part, const fruit::Vec3& on_plate, const fruit::Vec3& normal_on_plate) {
    const fruit::Transform inv = part.transform().inverse();
    LabelSpot s;
    s.part = part.id();
    s.geometry_id = part.geometryId();
    s.point = inv.apply(on_plate);
    s.normal = fruit::normalized(inv.applyLinear(normal_on_plate));
    return s;
}

std::vector<std::pair<fruit::Vec3, fruit::Vec3>> labelOutlineOnPlate(const LabelPreview& p,
                                                                      const fruit::ops::TextPlacement& at) {
    fruit::TriangleMesh pts;  // placeText moves vertices only
    for (const Edge& e : p.outline) {
        pts.vertices.push_back(e.first);
        pts.vertices.push_back(e.second);
    }
    const fruit::TriangleMesh moved = fruit::ops::placeText(pts, at, 0.05);
    std::vector<Edge> out;
    for (std::size_t i = 0; i + 1 < moved.vertices.size(); i += 2) out.push_back({moved.vertices[i], moved.vertices[i + 1]});
    return out;
}

LabelJob labelJobFor(const Part& part, const LabelSettings& s) {
    LabelJob j;
    j.placed = part.placed();
    j.own_to_plate = part.transform();
    j.place = labelPlacement(part, *s.spot, s.engrave);
    j.style.height = s.height;
    j.style.depth = s.depth;
    j.text = s.text;
    j.name = part.name();
    return j;
}

ShapeOutcome labelPart(const LabelJob& job, const fruit::JobControl& ctl) {
    ShapeOutcome out;
    try {
        ctl.step("letters", 0.1);
        const fruit::ops::MeshResult r = fruit::ops::applyText(job.placed, job.text, job.style, job.place);
        ctl.step("label", 0.8);
        if (!r.ok) {
            out.message = "Couldn't put the text on " + quoted(job.name) + ". " + withRepairHint(r.message);
            return out;
        }
        ShapePiece p;
        p.model = ownModel(r.mesh, job.own_to_plate);
        if (!p.model) {
            out.message = "Couldn't put the text on " + quoted(job.name) + ": nothing was left.";
            return out;
        }
        p.own_to_plate = job.own_to_plate;
        out.pieces.push_back(std::move(p));
        ctl.check();
        out.message = std::string(job.place.engrave ? "Engraved " : "Raised ") + quoted(job.text) + " on " +
                      quoted(job.name) + "." + (r.message.empty() ? "" : " " + r.message);
        out.ok = true;
    } catch (const fruit::Cancelled&) {
        throw;
    } catch (const std::exception& e) {
        out.pieces.clear();
        out.message = "Couldn't put the text on " + quoted(job.name) + ": " + e.what();
    }
    return out;
}

void relabelPart(PartsEdit& e, int id, const ShapeOutcome& out) {
    if (!out.ok || out.pieces.empty()) return;
    PartState* s = e.find(id);
    if (!s) return;
    s->placement = placeInPlace(out.pieces.front(), s->placement);
    s->model = out.pieces.front().model;
    s->path.clear();  // not the file's mesh any more: finding that file again must not bring it back
}

// ---- Combine ----

const char* combineVerb(CombineMode m) {
    switch (m) {
        case CombineMode::Join: return "Join";
        case CombineMode::Subtract: return "Subtract";
        case CombineMode::Overlap: return "Keep the overlap";
    }
    return "";
}

int combineSecond(const CombineSettings& s, const Document& doc) {
    if (s.b >= 0 && s.b != doc.selectedId() && doc.partIndex(s.b) >= 0) return s.b;
    if (doc.partCount() == 2)
        for (const Part& p : doc.parts())
            if (p.id() != doc.selectedId()) return p.id();
    return -1;
}

ShapeOutcome combineParts(const CombineJob& job, const fruit::JobControl& ctl) {
    ShapeOutcome out;
    try {
        ctl.step("check", 0.05);
        const fruit::ops::Solid a(job.a);
        if (!a.valid()) {
            out.message = "Couldn't combine. " + quoted(job.a_name) + ": " + withRepairHint(a.message());
            return out;
        }
        const fruit::ops::Solid b(job.b);
        if (!b.valid()) {
            out.message = "Couldn't combine. " + quoted(job.b_name) + ": " + withRepairHint(b.message());
            return out;
        }
        ctl.step("combine", 0.3);
        fruit::ops::Solid res;
        switch (job.mode) {
            case CombineMode::Join: res = fruit::ops::unite(a, b); break;
            case CombineMode::Subtract: res = fruit::ops::subtract(a, b); break;
            case CombineMode::Overlap: res = fruit::ops::intersect(a, b); break;
        }
        ctl.step("result", 0.8);
        if (!res.valid()) {
            out.message = "Couldn't combine " + quoted(job.a_name) + " and " + quoted(job.b_name) + ". " + res.message();
            return out;
        }
        if (res.empty()) {
            out.message = job.mode == CombineMode::Overlap
                              ? quoted(job.a_name) + " and " + quoted(job.b_name) + " don't overlap, so nothing would be left."
                              : "Nothing would be left of " + quoted(job.a_name) + ".";
            return out;
        }
        fruit::TriangleMesh mesh = res.toMesh();
        const fruit::BBox3 box = mesh.bbox();
        const fruit::Vec3 c = box.center();
        const fruit::Vec3 shift{c.x, c.y, box.min.z};  // own coordinates: its middle at the origin, standing on z = 0
        mesh.translate({-shift.x, -shift.y, -shift.z});
        ShapePiece p;
        p.own_to_plate = fruit::Transform::translation(shift);
        fruit::LoadedModel m = fruit::prepareModel(std::move(mesh));
        if (m.mesh.empty()) {
            out.message = "Nothing would be left.";
            return out;
        }
        p.model = makePartModel(std::move(m));
        out.pieces.push_back(std::move(p));
        ctl.check();
        const double va = a.volume(), vres = res.volume();
        std::string note;
        if (job.mode == CombineMode::Join && fruit::ops::countPieces(out.pieces.front().model->model.mesh) > 1)
            note = " They don't touch, so it is one part in separate pieces.";
        else if (job.mode == CombineMode::Subtract && std::abs(va - vres) < 1e-6)
            note = " " + quoted(job.b_name) + " doesn't reach " + quoted(job.a_name) + ", so it is unchanged.";
        if (job.mode == CombineMode::Join) out.message = "Joined " + quoted(job.a_name) + " and " + quoted(job.b_name) + " into one part." + note;
        else if (job.mode == CombineMode::Subtract)
            out.message = "Took " + quoted(job.b_name) + " away from " + quoted(job.a_name) + ": one part." + note;
        else out.message = "Kept only where " + quoted(job.a_name) + " and " + quoted(job.b_name) + " overlap: one part." + note;
        out.ok = true;
    } catch (const fruit::Cancelled&) {
        throw;
    } catch (const std::exception& e) {
        out.pieces.clear();
        out.message = std::string("Couldn't combine: ") + e.what();
    }
    return out;
}

int combineIntoPart(PartsEdit& e, const Part& a, const Part& b, const ShapeOutcome& out) {
    if (!out.ok || out.pieces.empty()) return -1;
    const std::vector<std::string> names = namesExcept(e, {a.id(), b.id()});
    // Its own coordinates are the plate's, so it is not turned, scaled or mirrored again.
    fruit::Placement base = a.placement();
    base.scale = 1.0;
    base.rotation_deg = {0, 0, 0};
    base.mirror_x = base.mirror_y = base.mirror_z = false;
    const ShapePiece& piece = out.pieces.front();
    const fruit::Placement p = placeInPlace(piece, base);
    e.remove(a.id());
    e.remove(b.id());
    const int id = e.add(NewPart{pieceName(a.name(), "", names), "", piece.model, p});  // made in the app
    e.selected = id;
    return id;
}

}  // namespace gui
