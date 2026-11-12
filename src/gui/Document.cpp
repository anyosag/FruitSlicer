// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Document.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <stdexcept>

#include "Placing.h"
#include "mesh/AabbTree.h"
#include "mesh/Primitives.h"

namespace gui {

namespace {

bool sameLift(const fruit::Placement& a, const fruit::Placement& b) {
    return a.elevation == b.elevation && a.on_plate == b.on_plate;
}

void takeLift(fruit::Placement& p, const fruit::Placement& from) {
    p.elevation = from.elevation;
    p.on_plate = from.on_plate;
}

}  // namespace

const Part* Document::findPart(int id) const {
    for (const Part& p : parts_)
        if (p.id() == id) return &p;
    return nullptr;
}

int Document::partIndex(int id) const {
    for (std::size_t i = 0; i < parts_.size(); ++i)
        if (parts_[i].id() == id) return static_cast<int>(i);
    return -1;
}

void Document::select(int id) {
    if (const int i = partIndex(id); i >= 0 && !dragging()) selected_ = i;
}

Document::Snapshot Document::snapshot() const {
    Snapshot s{parts_, selected_, revision_};
    for (Part& p : s.parts) {
        p.placed_.reset();  // an undo step keeps no meshes it can make again
        p.footprint_.reset();
    }
    return s;
}

void Document::restore(Snapshot s) {
    // A part as it is now keeps what it has made.
    for (Part& p : s.parts)
        if (const Part* now = findPart(p.id()); now && now->revision() == p.revision()) {
            p.placed_ = now->placed_;
            p.footprint_ = now->footprint_;
        }
    parts_ = std::move(s.parts);
    selected_ = std::clamp(s.selected, 0, static_cast<int>(parts_.size()) - 1);
    revision_ = s.revision;
    changed();
}

void Document::record(Snapshot before) {
    undo_.push_back(std::move(before));
    redo_.clear();
}

void Document::changed() {
    box_ = fruit::BBox3{};
    for (const Part& p : parts_) box_.extend(p.box());
    placed_.reset();
    face_start_.clear();
}

void Document::setPlacement(std::size_t i, const fruit::Placement& p) {
    Part& part = parts_[i];
    const bool lift = !sameLift(p, part.placement());
    part.s_.placement = p;
    part.update(printer);
    part.revision_ = ++last_revision_;
    if (lift)  // the lift is the job's
        for (std::size_t j = 0; j < parts_.size(); ++j) {
            if (j == i) continue;
            takeLift(parts_[j].s_.placement, p);
            parts_[j].update(printer);
            parts_[j].revision_ = ++last_revision_;
        }
    changed();
}

void Document::prepare() const {
    for (const Part& p : parts_) p.placed();
    placed();
}

const fruit::TriangleMesh& Document::placed() const {
    if (!placed_) {
        face_start_.clear();
        if (parts_.size() == 1) {
            parts_[0].placed();
            placed_ = parts_[0].placed_;  // one part: its own placed mesh, as it is
            face_start_.push_back(0);
        } else {
            auto m = std::make_shared<fruit::TriangleMesh>();
            for (const Part& p : parts_) {
                face_start_.push_back(m->faces.size());
                m->append(p.placed());
            }
            placed_ = std::move(m);
        }
    }
    return *placed_;
}

int Document::partOfFace(std::size_t face) const {
    placed();
    if (face_start_.empty()) return -1;
    const auto it = std::upper_bound(face_start_.begin(), face_start_.end(), face);
    return static_cast<int>(it - face_start_.begin()) - 1;
}

std::size_t Document::faceStart(int part_index) const {
    placed();
    return part_index >= 0 && static_cast<std::size_t>(part_index) < face_start_.size()
               ? face_start_[static_cast<std::size_t>(part_index)]
               : 0;
}

int Document::partNear(const fruit::Vec3& at) const {
    if (parts_.empty()) return -1;
    if (parts_.size() == 1) return parts_[0].id();
    std::vector<const Part*> near;
    for (const Part& p : parts_) {
        fruit::BBox3 b = p.box();
        b.min -= fruit::Vec3{1, 1, 1};
        b.max += fruit::Vec3{1, 1, 1};
        if (at.x >= b.min.x && at.y >= b.min.y && at.z >= b.min.z && at.x <= b.max.x && at.y <= b.max.y &&
            at.z <= b.max.z)
            near.push_back(&p);
    }
    if (near.size() == 1) return near[0]->id();
    if (near.empty())
        for (const Part& p : parts_) near.push_back(&p);
    int best = -1;
    double best_d = 0;
    for (const Part* p : near) {
        const fruit::AabbTree tree(p->placed());
        const double d = tree.closest(at).distance;
        if (best < 0 || d < best_d) {
            best = p->id();
            best_d = d;
        }
    }
    return best;
}

bool Document::fits() const {
    if (parts_.empty()) return false;
    for (const Part& p : parts_)
        if (!p.fit().fits) return false;
    return true;
}

bool Document::fitsCentred() const {
    if (parts_.empty()) return false;
    for (const Part& p : parts_)
        if (!p.fit().fits_centred) return false;
    return true;
}

const Part* Document::partOff() const {
    if (parts_.empty()) return nullptr;
    if (!part().fit().fits) return &part();
    for (const Part& p : parts_)
        if (!p.fit().fits) return &p;
    return nullptr;
}

PartFrames Document::frames() const {
    PartFrames f;
    for (const Part& p : parts_) f.add(p.id(), p.transform());
    return f;
}

const OrientState& Document::orientOf(int id) const {
    static const OrientState none;
    const auto it = orient_.find(id);
    return it != orient_.end() ? it->second : none;
}

std::vector<fruit::Vec3> Document::visibleInPrinter() const {
    std::vector<fruit::Vec3> out;
    for (const Part& p : parts_)
        for (const fruit::Vec3& v : gui::visibleInPrinter(orientOf(p.id()).visible, p.placement())) {
            bool seen = false;
            for (const fruit::Vec3& w : out) seen = seen || fruit::dot(v, w) > 0.9999;
            if (!seen) out.push_back(v);
        }
    return out;
}

bool Document::orientDecided() const {
    for (const Part& p : parts_)
        if (!orientOf(p.id()).decided) return false;
    return !parts_.empty();
}

std::int64_t Document::supportSettings() const {
    const std::int64_t picked = orientSettings() * 100003LL + (hollow.applied ? 1 + hollow.settings : 0);
    const int held = !parts_.empty() && placement().on_plate ? 1 + plateHeldLayers(modelInBottomLayers(*this)) : 0;
    return picked * kHeldSpan + std::min<std::int64_t>(held, kHeldSpan - 1);
}

bool Document::supportsOutdatedByBottomLayers() const {
    const SupportState& s = supports;
    const std::int64_t now = supportSettings();
    // They would count with the bottom layers they were made for.
    return !s.fresh(revision_, now) && s.fresh(revision_, now / kHeldSpan * kHeldSpan + s.result_settings % kHeldSpan);
}

int Document::orientSettings() const {
    int n = 0;
    for (const Part& p : parts_) n += orientOf(p.id()).settings;
    return n;
}

void Document::place(const fruit::Placement& p) {
    if (parts_.empty()) return;
    setPlacement(static_cast<std::size_t>(selected_), p);
    revision_ = ++last_revision_;
    prepare();
}

void Document::edit(const fruit::Placement& p) {
    if (parts_.empty() || p == placement()) return;
    Snapshot before = snapshot();
    place(p);
    record(std::move(before));
}

void Document::mirror(int axis) {
    if (!parts_.empty()) edit(mirroredAlong(placement(), axis));
}

void Document::undo() {
    if (undo_.empty() || dragging()) return;
    redo_.push_back(snapshot());
    Snapshot u = std::move(undo_.back());
    undo_.pop_back();
    restore(std::move(u));
    prepare();
}

void Document::redo() {
    if (redo_.empty() || dragging()) return;
    undo_.push_back(snapshot());
    Snapshot r = std::move(redo_.back());
    redo_.pop_back();
    restore(std::move(r));
    prepare();
}

void Document::beginDrag() {
    if (!dragging() && !parts_.empty()) drag_from_ = snapshot();
}

void Document::preview(const fruit::Placement& p) {
    if (!dragging() || p == placement()) return;
    setPlacement(static_cast<std::size_t>(selected_), p);
    revision_ = ++last_revision_;
}

namespace {

// The same parts, meshes and placements.
bool sameParts(const std::vector<Part>& a, const std::vector<Part>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].id() != b[i].id() || a[i].shared() != b[i].shared() || !(a[i].placement() == b[i].placement()))
            return false;
    return true;
}

}  // namespace

void Document::endDrag() {
    if (!dragging()) return;
    Snapshot from = std::move(*drag_from_);
    drag_from_.reset();
    if (sameParts(parts_, from.parts)) restore(std::move(from));  // dragged back to where it started: nothing changed
    else record(std::move(from));
    prepare();
}

void Document::cancelDrag() {
    if (!dragging()) return;
    Snapshot from = std::move(*drag_from_);
    drag_from_.reset();
    restore(std::move(from));
    prepare();
}

void Document::clearHistory() {
    undo_.clear();
    redo_.clear();
    drag_from_.reset();
}

bool Document::editParts(const std::function<void(PartsEdit&)>& fn) {
    if (dragging()) return false;
    PartsEdit e;
    for (const Part& p : parts_) e.parts_.push_back(p.state());
    e.selected = selectedId();
    e.next_id_ = next_id_;
    fn(e);
    std::vector<PartState>& now = e.parts_;
    std::erase_if(now, [](const PartState& s) { return !s.model || s.model->model.mesh.empty(); });
    if (now.empty()) return false;  // a job keeps a part
    // Every part a new id of its own; ids are never used twice in a job.
    int next = std::max(next_id_, e.next_id_);
    for (std::size_t i = 0; i < now.size(); ++i) {
        bool taken = now[i].id < 0;
        for (std::size_t j = 0; j < i && !taken; ++j) taken = now[j].id == now[i].id;
        if (taken) now[i].id = next++;
        next = std::max(next, now[i].id + 1);
    }
    // The lift is the job's: a part that had one and changed it gives it to
    // all; new parts take the job's.
    std::optional<fruit::Placement> lift;
    if (!parts_.empty()) lift = placement();
    for (const PartState& s : now)
        if (const Part* old = findPart(s.id); old && !sameLift(s.placement, old->placement())) {
            lift = s.placement;
            break;
        }
    if (!lift) lift = now.front().placement;
    for (PartState& s : now) takeLift(s.placement, *lift);
    // Nothing changed: nothing to undo.
    bool same = now.size() == parts_.size(), shape = !same;
    for (std::size_t i = 0; i < now.size() && same; ++i) {
        const Part& p = parts_[i];
        const PartState& s = now[i];
        if (s.id != p.id() || s.model != p.shared() || !(s.placement == p.placement())) shape = true;
        if (shape || s.name != p.name() || s.path != p.path()) same = false;
    }
    next_id_ = next;
    if (same) {
        select(e.selected);
        return false;
    }
    Snapshot before = snapshot();
    std::vector<Part> parts;
    parts.reserve(now.size());
    for (PartState& s : now) {
        const Part* old = findPart(s.id);
        Part p = old ? *old : Part{};
        const bool moved = !old || s.model != old->shared() || !(s.placement == old->placement());
        p.s_ = std::move(s);
        if (moved) {
            p.update(printer);
            p.revision_ = ++last_revision_;
        }
        parts.push_back(std::move(p));
    }
    const int old_index = selected_;
    parts_ = std::move(parts);
    selected_ = std::clamp(old_index, 0, static_cast<int>(parts_.size()) - 1);
    if (const int i = partIndex(e.selected); i >= 0) selected_ = i;
    if (shape) revision_ = ++last_revision_;
    changed();
    record(std::move(before));
    prepare();
    return true;
}

int Document::addPart(NewPart p) {
    int id = -1;
    editParts([&](PartsEdit& e) {
        id = e.add(std::move(p));
        e.selected = id;
    });
    return partIndex(id) >= 0 ? id : -1;
}

bool Document::removePart(int id) {
    if (parts_.size() <= 1 || partIndex(id) < 0) return false;
    return editParts([&](PartsEdit& e) { e.remove(id); });
}

bool Document::replaceMesh(int id, std::shared_ptr<const PartModel> m) {
    if (!m || partIndex(id) < 0) return false;
    return editParts([&](PartsEdit& e) {
        if (PartState* s = e.find(id)) s->model = std::move(m);
    });
}

bool Document::replaceMesh(int id, fruit::LoadedModel m) { return replaceMesh(id, makePartModel(std::move(m))); }

void Document::replaceParts(std::vector<NewPart> parts) {
    editParts([&](PartsEdit& e) {
        e.parts().clear();
        int first = -1;
        for (NewPart& p : parts) {
            const int id = e.add(std::move(p));
            if (first < 0) first = id;
        }
        e.selected = first;
    });
}

int newGeometryId() {
    static std::atomic<int> next_id{1};
    return next_id++;
}

bool Document::layersFresh() const {
    return layers.fresh(supports.result, static_cast<std::size_t>(baseLayers(*this, kLayerHeight)));
}

JobStamp Document::stamp() const {
    JobStamp s;
    for (const Part& p : parts_) {
        s.parts.push_back(p.id());
        s.model_paths.push_back(p.path());
        s.orient_decided.push_back(orientOf(p.id()).decided);
    }
    s.revision = revision_;
    s.orient_settings = orientSettings();
    s.supports = supports.result.get();
    s.choices = supports.choices;
    s.edits = supports.edits;
    s.hollow_applied = hollow.applied;
    s.hollow = hollow.choices;
    s.holes = hollow.manual;
    s.sample_path = print.settings.sample_path;
    s.print = print.settings;
    return s;
}

std::shared_ptr<Document> loadDocument(const std::string& path, const fruit::PrinterProfile& printer,
                                       const fruit::JobControl& ctl) {
    ctl.report("load", 0);
    auto doc = std::make_shared<Document>();
    doc->name = std::filesystem::path(path).stem().string();
    doc->printer = printer;
    fruit::LoadedModel m = fruit::loadModel(path);
    if (m.mesh.empty()) throw std::runtime_error("the file holds no triangles");
    ctl.step("load", 0.5);
    std::shared_ptr<const PartModel> pm = makePartModel(std::move(m));
    ctl.step("load", 0.7);
    doc->replaceParts({NewPart{doc->name, path, std::move(pm), fruit::Placement{}}});
    doc->clearHistory();
    doc->low_points = fruit::countLowPoints(doc->placed(), false);
    ctl.report("load", 1);
    return doc;
}

fruit::TriangleMesh demoModel() {
    using fruit::makeBox;
    using fruit::makeCylinder;
    // Parts overlap instead of touching, so every shell stays closed after
    // an STL round trip (welding would join touching corners).
    fruit::TriangleMesh m = makeBox({0, 0, 0}, {30, 20, 4});        // base
    m.append(makeBox({0.5, 0.5, 3}, {6, 19.5, 24}));                 // upright
    m.append(makeBox({5, 4, 18}, {22, 16, 21}));                     // shelf (overhang)
    m.append(makeCylinder({20, 10, 3}, 3.0, 9.0, 48));               // post
    return m;
}

fruit::TriangleMesh demoFaultyModel(const std::string& kind) {
    if (kind == "loose") {
        const fruit::TriangleMesh box = fruit::makeBox({0, 0, 0}, {20, 20, 20});
        fruit::TriangleMesh m;
        for (std::size_t q = 0; q < 6; ++q) {
            const fruit::Face& a = box.faces[2 * q];
            const int32_t quad[4] = {a[0], a[1], a[2], box.faces[2 * q + 1][2]};
            fruit::Vec3 c;
            for (int32_t i : quad) c += box.vertices[static_cast<std::size_t>(i)] / 4.0;
            const auto base = static_cast<int32_t>(m.vertices.size());
            for (int32_t i : quad) m.vertices.push_back(c + (box.vertices[static_cast<std::size_t>(i)] - c) * 0.95);
            m.faces.push_back({base, base + 1, base + 2});
            m.faces.push_back({base, base + 2, base + 3});
        }
        return m;
    }
    // The bracket's faces: base 0-11, upright 12-23, shelf 24-35 (its top:
    // 26, 27), then the post: 96 round its side, 48 underneath, 48 on top.
    fruit::TriangleMesh m = demoModel();
    std::vector<char> drop(m.faces.size(), 0);
    if (kind == "open") {
        drop[26] = drop[27] = 1;
        for (std::size_t i = m.faces.size() - 48; i < m.faces.size(); ++i) drop[i] = 1;
    } else {
        drop[36 + 2 * 30] = drop[36 + 2 * 30 + 1] = 1;  // one strip of the post's side, facing the front
        for (std::size_t i : {16u, 19u, 22u}) std::swap(m.faces[i][1], m.faces[i][2]);
        m.faces.push_back(m.faces[2]);
        drop.push_back(0);
    }
    std::size_t i = 0;
    std::erase_if(m.faces, [&](const fruit::Face&) { return drop[i++] != 0; });
    return m;
}

}  // namespace gui
