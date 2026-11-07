// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Document.h"

#include <filesystem>

#include "mesh/Primitives.h"

namespace gui {

void Document::place() {
    placed = fruit::placeModel(model.mesh, placement);
    ++revision;
}

bool Document::fitsPlate(const fruit::PrinterProfile& printer) const {
    if (placed.empty()) return false;
    const fruit::BBox3 b = placed.bbox();
    return printer.fitsXY(b.min.x, b.min.y) && printer.fitsXY(b.max.x, b.max.y) && b.max.z <= printer.build_z;
}

std::shared_ptr<Document> loadDocument(const std::string& path, const fruit::JobControl& ctl) {
    ctl.report("load", 0);
    auto doc = std::make_shared<Document>();
    doc->path = path;
    doc->name = std::filesystem::path(path).stem().string();
    doc->model = fruit::loadModel(path);
    ctl.step("load", 0.8);
    doc->place();
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

}  // namespace gui
