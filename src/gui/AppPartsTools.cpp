// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The app's commands for the parts tools (Duplicate, Arrange, Split, Merge)
// and the Collisions check that runs in the background.
#include <algorithm>

#include "App.h"
#include "Log.h"

namespace gui {

namespace {

// Runs a tool as one edit of the parts and keeps what it reported.
void runTool(App& app, Document& doc, Tool which, const std::function<ToolReport(const Document&, PartsEdit&)>& tool) {
    ToolReport report;
    // The tool reads the job as it is before the edit: a copy of the boxes is
    // not needed, editParts hands over a working copy of the parts only.
    const bool changed = app.editParts([&](PartsEdit& e) { report = tool(doc, e); });
    report.changed = changed;
    PartsToolState& st = app.partsTools();
    st.last = report;
    st.last_tool = static_cast<int>(which);
    st.last_doc = app.document();
    st.last_revision = doc.revision();
    log::info(report.message);
}

}  // namespace

void App::duplicateParts() {
    if (!doc_) return;
    const int n = parts_tools_.copies;
    runTool(*this, *doc_, Tool::Duplicate, [n](const Document& d, PartsEdit& e) { return duplicatePart(d, e, n, kPartGap); });
}

void App::arrangeAll() {
    if (!doc_ || doc_->partCount() < 2) return;
    const double gap = parts_tools_.arrange_gap;
    runTool(*this, *doc_, Tool::Arrange, [gap](const Document& d, PartsEdit& e) { return arrangeParts(d, e, gap); });
}

void App::splitSelected() {
    if (!doc_) return;
    const int id = doc_->selectedId();
    runTool(*this, *doc_, Tool::Split, [id](const Document& d, PartsEdit& e) { return splitPart(d, e, id); });
}

std::vector<int> App::mergeChosen() const {
    std::vector<int> ids;
    if (!doc_) return ids;
    for (const Part& p : doc_->parts()) {
        const auto& off = parts_tools_.merge_off;
        if (std::find(off.begin(), off.end(), p.id()) == off.end()) ids.push_back(p.id());
    }
    return ids;
}

void App::mergePick(int id, bool on) {
    auto& off = parts_tools_.merge_off;
    off.erase(std::remove(off.begin(), off.end(), id), off.end());
    if (!on) off.push_back(id);
}

void App::mergeParts(bool fuse) {
    if (!doc_ || doc_->partCount() < 2) return;
    const std::vector<int> ids = mergeChosen();
    runTool(*this, *doc_, Tool::Merge, [&ids, fuse](const Document& d, PartsEdit& e) { return gui::mergeParts(d, e, ids, fuse); });
    parts_tools_.merge_off.clear();
}

const CollisionReport* App::collisions() const {
    if (!doc_ || doc_->partCount() < 2 || !collision_report_ || !sameJob(collision_doc_, doc_) ||
        collision_report_->revision != doc_->revision() || collision_report_->min_gap != collision_gap_)
        return nullptr;
    return collision_report_.get();
}

bool App::checkingCollisions() const { return jobs_.busy() && jobs_.progress().name == "Checking the parts"; }

void App::finishCollisions() {
    if (!jobs_.busy()) jobs_.wait();  // a check set aside for other work may still be stopping
    startCollisionCheck();
    jobs_.wait();
}

void App::settleCollisions() {
    if (jobs_.busy() && !checkingCollisions()) return;  // other work: a script waits for that itself
    if (!jobs_.busy()) jobs_.wait();  // a check set aside for other work may still be stopping
    startCollisionCheck();
    if (checkingCollisions()) jobs_.wait();
}

void App::selectCollision(int part) {
    selectPart(part);
    // Ask for it in view: nothing else to do, the selected part is drawn lit.
}

void App::forgetJobTools() {
    // Part ids start again at 0 in every job, and revisions repeat: nothing here may carry over.
    collision_report_.reset();
    collision_doc_.reset();
    collision_tried_ = -1;
    collision_tried_doc_.reset();
    parts_tools_.merge_off.clear();
    parts_tools_.last = ToolReport{};
    parts_tools_.last_doc.reset();
    shape();  // the shape tools' settings and note start again too
}

void App::startCollisionCheck() {
    if (!doc_ || doc_->partCount() < 2 || screen_ != Screen::Job) return;
    const int rev = doc_->revision();
    if (collisions()) return;
    if (collision_tried_ == rev && sameJob(collision_tried_doc_, doc_) && collision_tried_gap_ == collision_gap_) return;
    if (jobs_.busy() && !checkingCollisions()) return;  // the work running now comes first
    if (jobs_.stopping()) return;  // one set aside is still stopping: one at a time in the background
    collision_tried_ = rev;
    collision_tried_doc_ = doc_;
    collision_tried_gap_ = collision_gap_;
    const int generation = ++collision_generation_;
    auto snap = std::make_shared<const Document>(*doc_);
    auto out = std::make_shared<CollisionReport>();
    const double gap = collision_gap_;
    std::weak_ptr<Document> target = doc_;
    // It gives way: other work started now doesn't wait for the pair it is on (Manifold can't stop part way).
    jobs_.startGivingWay(
        "Checking the parts",
        [snap, out, gap](const fruit::JobControl& ctl) { *out = checkCollisions(*snap, gap, ctl); },
        [this, target, out, generation](std::exception_ptr err) {
            const auto d = target.lock();
            if (!d) return;
            if (err) {
                try {
                    std::rethrow_exception(err);
                } catch (const fruit::Cancelled&) {
                    if (generation == collision_generation_) collision_tried_ = -1;  // made again when wanted
                } catch (const std::exception& e) {
                    log::info(std::string("collisions: ") + e.what());
                }
                return;
            }
            collision_report_ = std::make_shared<const CollisionReport>(std::move(*out));
            collision_doc_ = d;
            log::info("collisions: " + std::to_string(collision_report_->problems.size()) + " pairs to look at");
        });
}

}  // namespace gui
