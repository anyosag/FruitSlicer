// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Document.h"

#include <atomic>
#include <filesystem>
#include <stdexcept>

#include "Placing.h"
#include "mesh/Primitives.h"

namespace gui {

void Document::set(const fruit::Placement& p) {
    placement_ = p;
    transform_ = fruit::placementTransform(model.mesh, p);
    placed_box_ = fruit::BBox3{};
    for (const fruit::Vec3& v : model.mesh.vertices) placed_box_.extend(transform_.apply(v));
    fit_ = fruit::checkFit(model.mesh, p, printer);
    placed_stale_ = true;
}

const fruit::TriangleMesh& Document::placed() const {
    if (placed_stale_) {
        placed_ = model.mesh;
        placed_.transform(transform_);
        placed_stale_ = false;
    }
    return placed_;
}

void Document::place(const fruit::Placement& p) {
    set(p);
    revision_ = ++last_revision_;
    placed();
}

void Document::edit(const fruit::Placement& p) {
    if (p == placement_) return;
    undo_.push_back({placement_, revision_});
    redo_.clear();
    place(p);
}

void Document::mirror(int axis) { edit(mirroredAlong(placement_, axis)); }

void Document::undo() {
    if (undo_.empty() || dragging()) return;
    redo_.push_back({placement_, revision_});
    const Placed u = undo_.back();
    undo_.pop_back();
    set(u.placement);
    revision_ = u.revision;
    placed();
}

void Document::redo() {
    if (redo_.empty() || dragging()) return;
    undo_.push_back({placement_, revision_});
    const Placed r = redo_.back();
    redo_.pop_back();
    set(r.placement);
    revision_ = r.revision;
    placed();
}

void Document::beginDrag() {
    if (!dragging()) drag_from_ = Placed{placement_, revision_};
}

void Document::preview(const fruit::Placement& p) {
    if (!dragging() || p == placement_) return;
    set(p);
    revision_ = ++last_revision_;
}

void Document::endDrag() {
    if (!dragging()) return;
    const Placed from = *drag_from_;
    drag_from_.reset();
    if (placement_ == from.placement) {
        revision_ = from.revision;  // dragged back to where it started: nothing changed
    } else {
        undo_.push_back(from);
        redo_.clear();
    }
    placed();
}

void Document::cancelDrag() {
    if (!dragging()) return;
    const Placed from = *drag_from_;
    drag_from_.reset();
    set(from.placement);
    revision_ = from.revision;
    placed();
}

void Document::modelChanged() {
    undo_.clear();
    redo_.clear();
    drag_from_.reset();
    place(placement_);
}

int newGeometryId() {
    static std::atomic<int> next_id{1};
    return next_id++;
}

JobStamp Document::stamp() const {
    JobStamp s;
    s.model_path = path;
    s.revision = revision_;
    s.orient_settings = orient.settings;
    s.orient_decided = orient.decided;
    s.supports = supports.result.get();
    s.choices = supports.choices;
    s.edits = supports.edits;
    s.hollow_applied = hollow.applied;
    s.hollow = hollow.choices;
    s.holes = hollow.manual;
    s.sample_path = print.settings.sample_path;
    s.exposure = print.settings.exposure;
    s.bottom_exposure = print.settings.bottom_exposure;
    s.bottom_layers = print.settings.bottom_layers;
    s.transition_layers = print.settings.transition_layers;
    return s;
}

std::shared_ptr<Document> loadDocument(const std::string& path, const fruit::PrinterProfile& printer,
                                       const fruit::JobControl& ctl) {
    ctl.report("load", 0);
    auto doc = std::make_shared<Document>();
    doc->path = path;
    doc->name = std::filesystem::path(path).stem().string();
    doc->printer = printer;
    doc->geometry_id = newGeometryId();
    doc->model = fruit::loadModel(path);
    if (doc->model.mesh.empty()) throw std::runtime_error("the file holds no triangles");
    ctl.step("load", 0.5);
    doc->check = fruit::checkMesh(doc->model);
    ctl.step("load", 0.7);
    doc->place(fruit::Placement{});
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
