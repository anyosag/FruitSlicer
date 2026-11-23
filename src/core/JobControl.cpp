// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "core/JobControl.h"

#include <algorithm>
#include <mutex>

namespace fruit {

struct JobControl::Shared {
    std::atomic<bool> cancelled{false};
    Sink sink;
    std::mutex mu;
    double last = 0;
};

JobControl::JobControl() : s_(std::make_shared<Shared>()) {}

JobControl::JobControl(Sink sink) : s_(std::make_shared<Shared>()) { s_->sink = std::move(sink); }

const JobControl& JobControl::none() {
    static const JobControl inert(nullptr, 0, 1, true);
    return inert;
}

void JobControl::cancel() const noexcept {
    if (s_) s_->cancelled.store(true, std::memory_order_relaxed);
}

bool JobControl::cancelled() const noexcept { return s_ && s_->cancelled.load(std::memory_order_relaxed); }

const std::atomic<bool>* JobControl::cancelFlag() const noexcept { return s_ ? &s_->cancelled : nullptr; }

void JobControl::check() const {
    if (cancelled()) throw Cancelled();
}

void JobControl::report(std::string_view stage, double f) const {
    if (!s_ || silent_ || !s_->sink) return;
    const double g = begin_ + std::clamp(f, 0.0, 1.0) * (end_ - begin_);
    const std::string labelled = labels_.empty() ? std::string() : labels_ + std::string(stage);
    std::lock_guard<std::mutex> lock(s_->mu);
    s_->last = std::max(s_->last, std::min(g, 1.0));
    s_->sink(labels_.empty() ? stage : std::string_view(labelled), s_->last);
}

JobControl JobControl::sub(double begin, double end) const {
    const double b = begin_ + std::clamp(begin, 0.0, 1.0) * (end_ - begin_);
    const double e = begin_ + std::clamp(end, 0.0, 1.0) * (end_ - begin_);
    return JobControl(s_, b, std::max(b, e), silent_, labels_);
}

JobControl JobControl::sub(double begin, double end, std::string_view label) const {
    JobControl c = sub(begin, end);
    if (!label.empty()) c.labels_ += std::string(label) + "|";
    return c;
}

JobControl JobControl::silent() const { return JobControl(s_, begin_, end_, true, labels_); }

}  // namespace fruit
