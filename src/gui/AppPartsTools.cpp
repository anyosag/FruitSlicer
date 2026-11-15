// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The app's commands for the parts tools (Duplicate, Arrange, Split, Merge)
// and the Collisions check that runs in the background.
#include <algorithm>
#include <exception>

#include "App.h"
#include "Log.h"

namespace gui {

namespace {

// Keeps what a tool reported, for its panel, until the job changes.
void keepReport(App& app, Document& doc, Tool which, const ToolReport& report) {
    PartsToolState& st = app.partsTools();
    st.last = report;
    st.last_tool = static_cast<int>(which);
    st.last_doc = app.document();
    st.last_revision = doc.revision();
    log::info(report.message);
}

// Runs a tool as one edit of the parts and keeps what it reported.
void runTool(App& app, Document& doc, Tool which, const std::function<ToolReport(const Document&, PartsEdit&)>& tool) {
    ToolReport report;
    // The tool reads the job as it is before the edit: a copy of the boxes is
    // not needed, editParts hands over a working copy of the parts only.
    const bool changed = app.editParts([&](PartsEdit& e) { report = tool(doc, e); });
    report.changed = changed;
    keepReport(app, doc, which, report);
}

}  // namespace

void App::duplicateParts() { duplicateParts(parts_tools_.copies); }

void App::duplicateParts(int n) {
    if (!doc_ || n < 1) return;
    runTool(*this, *doc_, Tool::Duplicate, [n](const Document& d, PartsEdit& e) { return duplicatePart(d, e, n, kPartGap); });
}

void App::openPartMenu(int id, float x, float y) {
    if (!doc_ || !doc_->findPart(id)) return;
    selectPart(id);  // what the menu does, it does to this part
    part_menu_request_ = id;
    part_menu_x_ = x;
    part_menu_y_ = y;
}

int App::takePartMenu() {
    const int id = part_menu_request_;
    part_menu_request_ = -1;
    if (id >= 0) part_menu_part_ = id;
    return id;
}

void App::duplicateOnce(int id) {
    part_menu_part_ = -1;  // done with the menu: it closes
    if (!doc_ || !doc_->findPart(id) || placing()) return;
    selectPart(id);
    duplicateParts(1);
    // No room beside it: the Duplicate tool says so, and offers more room through Arrange.
    if (parts_tools_.last.done < 1) openTool(Tool::Duplicate);
}

namespace {
const char* const kLayoutsJob = "Finding layouts";
const char* const kMoveSupportsJob = "Moving supports with the parts";
}  // namespace

bool App::findingLayouts() const { return layouts_wanted_ || (jobs_.busy() && jobs_.progress().name == kLayoutsJob); }
bool App::movingSupports() const { return jobs_.busy() && jobs_.progress().name == kMoveSupportsJob; }

bool App::layoutsOpen() const {
    return layouts_open_ && layouts_ && doc_ && layouts_doc_.lock() == doc_ && layouts_->revision == doc_->revision();
}

void App::findLayouts() {
    if (!doc_ || doc_->partCount() < 2) return;
    // Supports moving with the parts: let them finish, then search (starting
    // now would stop them, and they would need making again).
    if (movingSupports()) {
        layouts_wanted_ = true;
        return;
    }
    layouts_wanted_ = false;
    auto snap = std::make_shared<const Document>(*doc_);
    auto out = std::make_shared<LayoutSearch>();
    const double gap = parts_tools_.arrange_gap;
    const bool turn = parts_tools_.arrange_turn;
    std::weak_ptr<const Document> target = doc_;
    jobs_.start(
        kLayoutsJob, [snap, out, gap, turn](const fruit::JobControl& ctl) { *out = gui::findLayouts(*snap, gap, turn, ctl); },
        [this, target, out](std::exception_ptr err) {
            if (err) {
                try {
                    std::rethrow_exception(err);
                } catch (const fruit::Cancelled&) {
                } catch (const std::exception& e) {
                    showError("Couldn't find layouts", e.what());
                }
                return;
            }
            const auto d = target.lock();
            if (!d || d != doc_) return;
            layouts_ = std::make_shared<const LayoutSearch>(std::move(*out));
            layouts_doc_ = d;
            layouts_open_ = true;
            log::info("layouts: " + std::to_string(layouts_->candidates) + " found, " +
                      std::to_string(layouts_->choices.size()) + " shown, in " + std::to_string(layouts_->seconds) + " s");
        });
}

unsigned App::layoutPicture(int choice) {
    if (!layouts_) return 0;
    if (layout_pictures_for_ != layouts_) {
        for (unsigned t : layout_pictures_) Renderer::freeTexture(t);
        layout_pictures_.clear();
        for (const LayoutChoice& c : layouts_->choices)
            layout_pictures_.push_back(c.picture.empty() ? 0u : renderer_.uploadRgba(c.picture.w, c.picture.h, c.picture.rgba));
        layout_pictures_for_ = layouts_;
    }
    return choice >= 0 && choice < static_cast<int>(layout_pictures_.size()) ? layout_pictures_[static_cast<std::size_t>(choice)] : 0;
}

void App::useLayout(int choice) {
    if (!layoutsOpen()) return;
    auto before = std::make_shared<const Document>(*doc_);
    const std::shared_ptr<const LayoutSearch> s = layouts_;
    runTool(*this, *doc_, Tool::Arrange,
            [s, choice](const Document& d, PartsEdit& e) { return applyLayout(d, e, *s, choice); });
    layouts_open_ = false;
    if (parts_tools_.last.changed) moveSupportsWith(before);
}

void App::moveSupportsWith(const std::shared_ptr<const Document>& before) {
    const SupportState& was = before->supports;
    if (!doc_ || !was.result || !was.fresh(before->revision(), before->supportSettings())) return;
    MovedTips mt = moveTips(*before, *doc_, *was.result);
    if (!mt.ok) {
        parts_tools_.last.message += " The supports need making again: " + mt.why + ".";
        return;
    }
    parts_tools_.last.message += " Their supports move with them.";
    auto snap = std::make_shared<const Document>(*doc_);
    auto out = std::make_shared<SupportResult>();
    const SupportChoices choices = doc_->supports.choices;
    const std::vector<TipEdit> edits = doc_->supports.edits;
    const int rev = doc_->revision();
    const std::int64_t settings = doc_->supportSettings();
    std::weak_ptr<Document> target = doc_;
    jobs_.start(
        kMoveSupportsJob,
        [snap, out, choices, edits, plan = std::move(mt.plan)](const fruit::JobControl& ctl) mutable {
            fruit::JobControl rest = ctl;
            // Planned on the hollowed or drilled model: that is made again where the part now stands.
            if (snap->hollow.applied) {
                auto hp = std::make_shared<const HollowPlan>(
                    planHollow(*snap, snap->hollow.choices, snap->hollow.manual, kLayerHeight, ctl.sub(0.0, 0.3)));
                if (hp->changes()) plan.hollow = hp;
                rest = ctl.sub(0.3, 1.0);
            }
            *out = finishWithEdits(*snap, choices, std::move(plan), edits, rest);
        },
        [this, target, out, rev, settings](std::exception_ptr err) {
            const auto d = target.lock();
            if (!d) return;
            if (err) {
                try {
                    std::rethrow_exception(err);
                } catch (const fruit::Cancelled&) {
                } catch (const std::exception& e) {
                    showError("Couldn't move the supports", e.what());
                }
                if (layouts_wanted_) findLayouts();
                return;
            }
            installSupports(d, std::make_shared<const SupportResult>(std::move(*out)), rev, settings);
            if (layouts_wanted_) findLayouts();
        });
}

void App::arrangeAll() {
    if (!doc_ || doc_->partCount() < 2) return;
    const double gap = parts_tools_.arrange_gap;
    const bool turn = parts_tools_.arrange_turn;
    auto before = std::make_shared<const Document>(*doc_);
    runTool(*this, *doc_, Tool::Arrange,
            [gap, turn](const Document& d, PartsEdit& e) { return arrangeParts(d, e, gap, turn); });
    if (parts_tools_.last.changed) moveSupportsWith(before);
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
    if (!doc_ || doc_->partCount() < 2 || parts_tools_.fusing) return;
    const std::vector<int> ids = mergeChosen();
    if (!fuse) {
        runTool(*this, *doc_, Tool::Merge, [&ids](const Document& d, PartsEdit& e) { return gui::mergeParts(d, e, ids, false); });
        parts_tools_.merge_off.clear();
        return;
    }
    // Fusing big parts takes a while: Manifold works on copies in the
    // background (with the job runner's progress and Cancel), and the result
    // goes in as one edit when it's done, unless the job changed meanwhile.
    auto meshes = std::make_shared<std::vector<std::pair<std::string, fruit::TriangleMesh>>>();
    for (const Part& p : doc_->parts())
        if (std::find(ids.begin(), ids.end(), p.id()) != ids.end()) meshes->emplace_back(p.name(), p.placed());
    if (meshes->size() < 2) return;
    auto out = std::make_shared<FuseResult>();
    const int revision = doc_->revision();
    std::weak_ptr<Document> target = doc_;
    parts_tools_.fusing = true;
    jobs_.start(
        "Fusing parts", [meshes, out](const fruit::JobControl& ctl) { *out = fuseMeshes(*meshes, ctl); },
        [this, target, revision, out, ids](std::exception_ptr err) {
            parts_tools_.fusing = false;
            const auto d = target.lock();
            if (!d || d != doc_) return;  // the job was closed
            ToolReport r;
            r.wanted = static_cast<int>(ids.size());
            if (err) {
                try {
                    std::rethrow_exception(err);
                } catch (const fruit::Cancelled&) {
                    r.message = "Cancelled. Nothing was changed.";
                } catch (const std::exception& e) {
                    r.message = std::string("It didn't work: ") + e.what();
                }
            } else if (!out->ok) {
                r.message = out->message;
            } else if (d->revision() != revision) {
                r.message = "The job changed while this was working, so nothing was fused. Try it again.";
            } else {
                runTool(*this, *d, Tool::Merge, [&ids, out](const Document& doc, PartsEdit& e) {
                    return mergeFused(doc, e, ids, std::move(out->mesh));
                });
                parts_tools_.merge_off.clear();
                return;
            }
            keepReport(*this, *d, Tool::Merge, r);
        });
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

void App::settleChecks() {
    // Other work: a script waits for that itself.
    if (jobs_.busy() && !checkingCollisions() && !checkingRisk() && !countingTips() && !makingSupportsAhead()) return;
    jobs_.wait();  // the check running now, and any set aside for other work that is still stopping
    startCollisionCheck();
    if (checkingCollisions()) jobs_.wait();
    startSupportsAhead();
    if (makingSupportsAhead()) jobs_.wait();
    startTipCounts();
    if (countingTips()) jobs_.wait();
    startRiskCheck();
    if (checkingRisk()) jobs_.wait();
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
    tip_counts_for_.reset();
    tip_counts_tried_.reset();
    tip_plans_for_.reset();
    tip_plans_ = {};
    ahead_.reset();
    ahead_out_.reset();
    ahead_for_.reset();
    ahead_tried_.reset();
    ahead_seen_.reset();
    risk_report_.reset();
    risk_for_.reset();
    risk_tried_.reset();
    risk_place_ = -1;
    risk_goto_layer_ = -1;
}

void App::startCollisionCheck() {
    if (!doc_ || doc_->partCount() < 2 || screen_ != Screen::Job) return;
    const int rev = doc_->revision();
    if (collisions()) return;
    if (collision_tried_ == rev && sameJob(collision_tried_doc_, doc_) && collision_tried_gap_ == collision_gap_) return;
    // The work running now comes first; the replay of the print gives way to this, and starts again after.
    if (jobs_.busy() && !checkingCollisions() && !checkingRisk()) return;
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
