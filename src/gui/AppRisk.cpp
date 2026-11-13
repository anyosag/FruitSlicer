// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The app's side of "what may not print" (Risk.h): the replay of the print
// as a background check for the job's supports, the place picked in it, and
// the ways from a place to the view and to its layer.
#include <algorithm>
#include <exception>

#include "App.h"
#include "Log.h"
#include "Print.h"

namespace gui {

namespace {
const char* const kRiskJob = "Replaying the print";
}

const RiskReport* App::risk() const {
    if (!doc_ || !risk_report_) return nullptr;
    const SupportState& s = doc_->supports;
    if (!s.result || risk_for_ != s.result || !s.fresh(doc_->revision(), doc_->supportSettings())) return nullptr;
    return risk_report_.get();
}

bool App::checkingRisk() const { return jobs_.busy() && jobs_.progress().name == kRiskJob; }

const std::string* App::riskProblem() const {
    if (!doc_ || risk_problem_.empty() || risk_tried_ != doc_->supports.result) return nullptr;
    return &risk_problem_;
}

void App::finishRisk() {
    jobs_.wait();  // whatever is running, and any check set aside that is still stopping
    startRiskCheck();
    if (checkingRisk()) jobs_.wait();
}

void App::startRiskCheck() {
    // A layer asked for from a place, once the layers are cut.
    if (risk_goto_layer_ >= 0) {
        if (!doc_ || screen_ != Screen::Job || step_ != Step::Layers) risk_goto_layer_ = -1;
        else if (doc_->layersFresh()) {
            setLayer(layerOfRisk(risk_goto_layer_));
            risk_goto_layer_ = -1;
        }
    }
    const std::shared_ptr<const SupportResult> sup = doc_ && screen_ == Screen::Job ? doc_->supports.result : nullptr;
    // Due: supports that count, not being edited by hand (every edit makes them anew), and no replay of them yet.
    const bool due = sup && !tip_editing_ && doc_->supports.fresh(doc_->revision(), doc_->supportSettings()) &&
                     !(risk_report_ && risk_for_ == sup);
    if (checkingRisk()) {
        // One being made for supports that no longer count, or with the job's screen left: it stops.
        if (!due || risk_tried_ != sup) jobs_.cancel();
        return;
    }
    if (!due || risk_tried_ == sup) return;
    if (jobs_.busy() || jobs_.stopping()) return;  // the work running now comes first; one at a time in the background
    risk_tried_ = sup;
    risk_problem_.clear();
    const int generation = ++risk_generation_;
    auto snap = std::make_shared<const Document>(*doc_);
    auto out = std::make_shared<RiskReport>();
    std::weak_ptr<Document> target = doc_;
    // It gives way: other work started now doesn't wait for the step it is in.
    jobs_.startGivingWay(
        kRiskJob, [snap, sup, out](const fruit::JobControl& ctl) { *out = checkRisk(*snap, *sup, ctl); },
        [this, target, sup, out, generation](std::exception_ptr err) {
            const auto d = target.lock();
            if (!d) return;
            if (err) {
                try {
                    std::rethrow_exception(err);
                } catch (const fruit::Cancelled&) {
                    if (generation == risk_generation_) risk_tried_.reset();  // made again when wanted
                } catch (const std::exception& e) {
                    if (generation == risk_generation_) risk_problem_ = e.what();
                    log::info(std::string("risk: ") + e.what());
                }
                return;
            }
            risk_report_ = std::make_shared<const RiskReport>(std::move(*out));
            risk_for_ = sup;
            risk_place_ = -1;
            log::info("risk: " + std::to_string(risk_report_->places_found) + " places, " +
                      std::to_string(risk_report_->tips.size()) + " tips that may let go, in " +
                      std::to_string(risk_report_->seconds) + " s");
        });
}

int App::riskPlace() const {
    const RiskReport* r = risk();
    return r && risk_place_ >= 0 && risk_place_ < static_cast<int>(r->places.size()) ? risk_place_ : -1;
}

void App::selectRiskPlace(int index) {
    const RiskReport* r = risk();
    if (!r || index < 0 || index >= static_cast<int>(r->places.size())) {
        risk_place_ = -1;
        return;
    }
    risk_place_ = index;
    if (step_ != Step::Supports) goToStep(Step::Supports);
    tip_editing_ = false;
    supports_view_ = SupportsView::Risk;
    // The tips are all over the model: the whole of it. Any other place: that place.
    const RiskPlace& p = r->places[static_cast<std::size_t>(index)];
    if (p.kind == RiskPlace::Kind::Tips) frameModel();
    else focusOn(p.at);
}

void App::pickRiskPlace(int index) {
    const RiskReport* r = risk();
    risk_place_ = r && index >= 0 && index < static_cast<int>(r->places.size()) ? index : -1;
}

void App::showRisk() {
    if (!doc_) return;
    if (step_ != Step::Supports) goToStep(Step::Supports);
    tip_editing_ = false;
    supports_view_ = SupportsView::Risk;
    // Its row in the Checks, with the list of places, in sight.
    job_tab_ = 0;
    risk_scroll_ = 3;
}

void App::showRiskLayer(int place) {
    const RiskReport* r = risk();
    if (!r || place < 0 || place >= static_cast<int>(r->places.size())) return;
    const int layer = r->places[static_cast<std::size_t>(place)].layer;
    if (layer < 0) return;
    risk_goto_layer_ = layer;
    layers_risk_ = true;
    goToStep(Step::Layers);
}

int App::layerOfRisk(int risk_layer) const {
    if (!doc_) return risk_layer;
    const int base = doc_->layersFresh() ? static_cast<int>(doc_->layers.result->base_layers) : baseLayers(*doc_, kLayerHeight);
    return risk_layer + std::max(0, base);
}

}  // namespace gui
