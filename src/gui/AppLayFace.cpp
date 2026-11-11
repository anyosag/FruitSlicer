// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// On plate: turning a surface of the selected part to face the plate.
#include "App.h"
#include "LayFace.h"
#include "Placing.h"

namespace gui {

bool App::laySurface(const SurfacePick& s) {
    if (!doc_ || s.kind == SurfacePick::Kind::None || placing()) return false;
    const fruit::Placement now = doc_->placement();
    const fruit::Placement p = laidFaceDown(now, s.normal);
    hover_face_ = -1;
    hover_point_.reset();
    if (p == now) return false;  // it faces the plate already
    doc_->edit(p);
    laid_note_ = {s.name, s.kind, doc_->selectedId(), doc_->revision()};
    frameModel();
    return true;
}

bool App::layFace(int region) {
    if (!doc_) return false;
    const std::vector<RestingFace> faces = restingFaces(doc_->part());
    if (region < 0 || region >= static_cast<int>(faces.size())) return false;
    const RestingFace& f = faces[static_cast<std::size_t>(region)];
    SurfacePick s;
    s.kind = SurfacePick::Kind::Resting;
    s.region = region;
    s.normal = doc_->part().flatFaces().faces[static_cast<std::size_t>(region)].normal;
    s.name = f.name;
    s.area = f.area;
    s.facing_down = f.on_plate;
    return laySurface(s);
}

const App::LaidNote* App::laidNote() const {
    // True only until the next edit or undo: either gives the job another revision.
    if (!doc_ || laid_note_.part != doc_->selectedId() || laid_note_.revision != doc_->revision()) return nullptr;
    return &laid_note_;
}

namespace {

// The selected part's own triangle for a face of the job's placed mesh, or -1 for another part's.
int ownTriangle(const Document& doc, std::size_t face) {
    const int i = doc.partOfFace(face);
    if (i < 0 || doc.parts()[static_cast<std::size_t>(i)].id() != doc.selectedId()) return -1;
    return static_cast<int>(face - doc.faceStart(i));
}

}  // namespace

SurfacePick App::surfaceUnder(const fruit::Vec3& origin, const fruit::Vec3& dir) {
    if (!doc_) return {};
    const auto hit = pickTree().raycast(origin, dir);
    if (!hit) return {};
    const int t = ownTriangle(*doc_, static_cast<std::size_t>(hit->face));
    return t < 0 ? SurfacePick{} : surfaceAt(doc_->part(), t, hit->point);
}

SurfacePick App::surfaceNear(const fruit::Vec3& at) {
    if (!doc_) return {};
    const fruit::ClosestPoint c = pickTree().closest(at);
    if (c.face < 0) return {};
    const int t = ownTriangle(*doc_, static_cast<std::size_t>(c.face));
    return t < 0 ? SurfacePick{} : surfaceAt(doc_->part(), t, c.point);
}

int App::pickRestingFace(const fruit::Vec3& origin, const fruit::Vec3& dir) {
    const SurfacePick s = surfaceUnder(origin, dir);
    return s.kind == SurfacePick::Kind::Resting ? s.region : -1;
}

}  // namespace gui
