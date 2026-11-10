// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The app's commands for the parts on the plate: adding one beside the
// others, selecting, removing, and finding the part under the mouse.
#include <filesystem>

#include "App.h"
#include "Log.h"
#include "mesh/ModelCheck.h"
#include "platform/Platform.h"

namespace gui {

void App::addPartDialog() {
    if (!doc_) {
        openDialog();  // nothing to add to: a new job
        return;
    }
    platform::openFileDialog(window_, {{"3D models", "stl;obj;3mf"}}, [this](const std::string& path) {
        if (!path.empty()) addPartFrom(path);
    });
}

void App::addPartFrom(const std::string& path) {
    if (!doc_) {
        openPath(path);
        return;
    }
    openModel(path, true);
}

void App::placeAddedPart() {
    fruit::Placement p = healthPlacement();  // the size the health dialog chose
    const std::shared_ptr<Document> add = std::move(checking_);
    checking_.reset();
    health_.open = false;
    adding_part_ = false;
    if (!doc_ || !add || add->parts().empty()) return;
    if (placing()) endPlacing();
    // Beside the others, on the job's lift.
    p.elevation = doc_->placement().elevation;
    p.on_plate = doc_->placement().on_plate;
    const fruit::PlateFit f = fruit::checkFit(add->part().mesh(), p, doc_->printer);
    std::vector<fruit::BBox2> taken;
    for (const Part& part : doc_->parts()) taken.push_back(part.footprintBox());
    p.offset = freeSpot(taken, {f.size.x, f.size.y}, doc_->printer, fruit::plateMargin(p));
    std::vector<std::string> names;
    for (const Part& part : doc_->parts()) names.push_back(part.name());
    const Part& from = add->part();
    const int id = doc_->addPart(NewPart{partName(from.path(), names), from.path(), from.shared(), p});
    if (id < 0) return;
    screen_ = Screen::Job;
    if (inLayersMode(step_)) goToStep(prepare_step_);
    log::info("added " + doc_->part().name() + " at " + std::to_string(p.scale * 100) + " %");
    frameModel();
}

void App::selectPart(int id) {
    if (!doc_ || doc_->selectedId() == id || doc_->partIndex(id) < 0) return;
    if (placing()) endPlacing();
    doc_->select(id);
    // What the Rotate tool showed was for the part selected before.
    if (step_ == Step::Orient && orient_view_ == OrientView::Results &&
        !doc_->orient().resultsFresh(doc_->part().revision()))
        orient_view_ = OrientView::Settings;
}

void App::removePart(int id) {
    if (!doc_ || placing() || doc_->partCount() <= 1) return;
    if (doc_->removePart(id)) log::info("removed a part; " + std::to_string(doc_->partCount()) + " left");
}

const fruit::AabbTree& App::pickTree() {
    if (!pick_tree_ || pick_doc_ != doc_.get() || pick_revision_ != doc_->revision()) {
        pick_tree_ = std::make_shared<fruit::AabbTree>(doc_->placed());
        pick_doc_ = doc_.get();
        pick_revision_ = doc_->revision();
    }
    return *pick_tree_;
}

int App::pickPart(const fruit::Vec3& origin, const fruit::Vec3& dir) {
    if (!doc_) return -1;
    const auto hit = pickTree().raycast(origin, dir);
    if (!hit) return -1;
    const int i = doc_->partOfFace(static_cast<std::size_t>(hit->face));
    return i >= 0 ? doc_->parts()[static_cast<std::size_t>(i)].id() : -1;
}

int App::partAt(const fruit::Vec3& at) {
    if (!doc_ || doc_->parts().empty()) return -1;
    if (doc_->partCount() == 1) return doc_->part().id();
    const fruit::ClosestPoint c = pickTree().closest(at);
    const int i = c.face >= 0 ? doc_->partOfFace(static_cast<std::size_t>(c.face)) : -1;
    return i >= 0 ? doc_->parts()[static_cast<std::size_t>(i)].id() : doc_->partNear(at);
}

}  // namespace gui
