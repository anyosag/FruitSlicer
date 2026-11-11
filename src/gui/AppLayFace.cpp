// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// On plate: laying a flat face of the selected part on the plate.
#include "App.h"
#include "LayFace.h"
#include "Placing.h"

namespace gui {

bool App::layFace(int region) {
    if (!doc_) return false;
    const std::vector<RestingFace> faces = restingFaces(doc_->part());
    if (region < 0 || region >= static_cast<int>(faces.size())) return false;
    const RestingFace& f = faces[static_cast<std::size_t>(region)];
    const fruit::Placement now = doc_->placement();
    const fruit::Placement p = laidFaceDown(now, doc_->part().flatFaces().faces[static_cast<std::size_t>(region)].normal);
    hover_face_ = -1;
    if (p == now) return false;  // already on the plate
    doc_->edit(p);
    laid_note_ = {f.name, doc_->selectedId(), doc_->revision()};
    frameModel();
    return true;
}

const App::LaidNote* App::laidNote() const {
    // True only until the next edit or undo: either gives the job another revision.
    if (!doc_ || laid_note_.part != doc_->selectedId() || laid_note_.revision != doc_->revision()) return nullptr;
    return &laid_note_;
}

int App::pickRestingFace(const fruit::Vec3& origin, const fruit::Vec3& dir) {
    if (!doc_) return -1;
    const auto hit = pickTree().raycast(origin, dir);
    if (!hit) return -1;
    const std::size_t face = static_cast<std::size_t>(hit->face);
    const int i = doc_->partOfFace(face);
    if (i < 0 || doc_->parts()[static_cast<std::size_t>(i)].id() != doc_->selectedId()) return -1;
    return doc_->part().flatFaces().regionOf(face - doc_->faceStart(i));
}

}  // namespace gui
