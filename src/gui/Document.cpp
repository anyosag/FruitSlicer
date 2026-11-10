// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Document.h"

#include <atomic>
#include <filesystem>
#include <stdexcept>

#include "mesh/Primitives.h"

namespace gui {

void Document::place(const fruit::Placement& p) {
    placement_ = p;
    transform_ = fruit::placementTransform(model.mesh, p);
    placed_ = model.mesh;
    placed_.transform(transform_);
    fit_ = fruit::checkFit(model.mesh, p, printer);
    ++revision_;
}

void Document::edit(const fruit::Placement& p) {
    undo_.push_back(placement_);
    redo_.clear();
    place(p);
}

void Document::undo() {
    if (undo_.empty()) return;
    redo_.push_back(placement_);
    const fruit::Placement p = undo_.back();
    undo_.pop_back();
    place(p);
}

void Document::redo() {
    if (redo_.empty()) return;
    undo_.push_back(placement_);
    const fruit::Placement p = redo_.back();
    redo_.pop_back();
    place(p);
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
