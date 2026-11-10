// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The app's commands for the shape tools: Cut, Label and Combine. Each one
// copies what it needs, works in the background (the job runner's progress
// and cancel), and when it is done puts the result into the job as one undo
// step, unless the job changed while it worked.
#include <exception>

#include "App.h"
#include "Log.h"

namespace gui {

void App::runShape(const std::string& job, std::function<ShapeOutcome(const fruit::JobControl&)> work,
                   std::function<bool(const ShapeOutcome&)> install, Tool tool) {
    shape();
    shape_.running = true;
    shape_.job = job;
    shape_.note.clear();
    shape_.note_bad = false;
    shape_.note_tool = tool;
    const int revision = doc_->revision();
    std::weak_ptr<Document> target = doc_;
    auto out = std::make_shared<ShapeOutcome>();
    jobs_.start(
        job, [work = std::move(work), out](const fruit::JobControl& ctl) { *out = work(ctl); },
        [this, target, revision, out, install = std::move(install), tool](std::exception_ptr err) {
            shape_.running = false;
            shape_.note_tool = tool;
            shape_.note_bad = true;
            if (err) {
                try {
                    std::rethrow_exception(err);
                } catch (const fruit::Cancelled&) {
                    shape_.note = "Cancelled. Nothing was changed.";
                } catch (const std::exception& e) {
                    shape_.note = std::string("It didn't work: ") + e.what();
                    log::error(shape_.note);
                }
                return;
            }
            const auto d = target.lock();
            if (!d || d != doc_) return;  // the job was closed
            if (!out->ok) {
                shape_.note = out->message;
                log::info("shape tool: " + out->message);
                return;
            }
            if (d->revision() != revision) {
                shape_.note = "The job changed while this was working, so nothing was applied. Try it again.";
                return;
            }
            if (!install(*out)) {
                shape_.note = "Nothing was changed.";
                return;
            }
            shape_.note = out->message + " Supports need making again.";
            shape_.note_bad = false;
            log::info("shape tool: " + out->message);
        });
}

ShapeState& App::shape() {
    if (shape_.doc != doc_.get()) {
        const bool running = shape_.running;  // work started for the job left is still to finish
        shape_ = ShapeState{};
        shape_.running = running;
        shape_.doc = doc_.get();
    }
    return shape_;
}

void App::shapeCancel() {
    if (shape_.running) jobs_.cancel();
}

void App::cutApply() {
    if (!doc_ || shape_.running || placing()) return;
    const Part& part = doc_->part();
    cutFit(shape_.cut, part);
    if (!cutCrosses(part, shape_.cut)) {
        shape_.note = "The plane has to pass through the part: pick a position between its two ends.";
        shape_.note_bad = true;
        shape_.note_tool = Tool::Cut;
        return;
    }
    auto job = std::make_shared<CutJob>(cutJobFor(part, shape_.cut));
    const int id = part.id();
    runShape(
        "Cutting " + part.name(), [job](const fruit::JobControl& ctl) { return cutPart(*job, ctl); },
        [this, id](const ShapeOutcome& out) {
            const Part* old = doc_->findPart(id);
            if (!old) return false;
            const std::vector<fruit::Vec3> sides = doc_->orientOf(id).visible;
            std::vector<int> ids;
            const bool done = editParts([&](PartsEdit& e) { ids = cutIntoParts(e, *old, out, doc_->printer); });
            // The sides people will see are the same ones on the pieces: they share its coordinates.
            if (done && !sides.empty())
                for (const int n : ids) {
                    OrientState& o = doc_->orientOf(n);
                    o.visible = sides;
                    ++o.settings;
                }
            return done;
        },
        Tool::Cut);
}

void App::labelPick(const fruit::Vec3& on_plate, const fruit::Vec3& normal) {
    if (!doc_ || shape_.running) return;
    shape_.label.spot = labelSpotAt(doc_->part(), on_plate, normal);
    shape_.note.clear();
}

void App::labelApply() {
    if (!doc_ || shape_.running || placing()) return;
    LabelSettings& ls = shape_.label;
    shape_.note_tool = Tool::Label;
    shape_.note_bad = true;
    if (ls.text.empty()) {
        shape_.note = "Type the text first.";
        return;
    }
    if (!labelSpotStillOn(ls, *doc_) || ls.spot->part != doc_->selectedId()) {
        shape_.note = "Click the part to say where the letters go.";
        return;
    }
    const Part& part = doc_->part();
    auto job = std::make_shared<LabelJob>(labelJobFor(part, ls));
    const int id = part.id();
    runShape(
        "Adding the label", [job](const fruit::JobControl& ctl) { return labelPart(*job, ctl); },
        [this, id](const ShapeOutcome& out) {
            if (!doc_->findPart(id)) return false;
            const bool done = editParts([&](PartsEdit& e) { relabelPart(e, id, out); });
            if (done) shape_.label.spot.reset();  // the surface is new: pick the next spot on it
            return done;
        },
        Tool::Label);
}

void App::combinePick(int part) {
    if (!doc_ || doc_->partIndex(part) < 0 || part == doc_->selectedId()) return;
    shape_.combine.b = part;
    shape_.note.clear();
}

void App::combineApply() {
    if (!doc_ || shape_.running || placing()) return;
    shape_.note_tool = Tool::Combine;
    shape_.note_bad = true;
    const int b = combineSecond(shape_.combine, *doc_);
    if (doc_->partCount() < 2 || b < 0) {
        shape_.note = doc_->partCount() < 2 ? "Combine needs two parts." : "Pick the second part.";
        return;
    }
    const Part& a = doc_->part();
    const Part& pb = *doc_->findPart(b);
    auto job = std::make_shared<CombineJob>();
    job->a = a.placed();
    job->b = pb.placed();
    job->mode = shape_.combine.mode;
    job->a_name = a.name();
    job->b_name = pb.name();
    const int ia = a.id();
    const char* verb = job->mode == CombineMode::Join ? "Joining" : job->mode == CombineMode::Subtract ? "Subtracting" : "Finding the overlap of";
    runShape(
        std::string(verb) + " " + a.name() + " and " + pb.name(),
        [job](const fruit::JobControl& ctl) { return combineParts(*job, ctl); },
        [this, ia, b](const ShapeOutcome& out) {
            const Part* pa = doc_->findPart(ia);
            const Part* pb2 = doc_->findPart(b);
            if (!pa || !pb2) return false;
            int id = -1;
            const bool done = editParts([&](PartsEdit& e) { id = combineIntoPart(e, *pa, *pb2, out); });
            if (done) shape_.combine.b = -1;
            return done;
        },
        Tool::Combine);
}

}  // namespace gui
