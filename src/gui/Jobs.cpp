// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Jobs.h"

#include <chrono>

#include "Log.h"
#include "platform/Platform.h"

namespace gui {

struct JobRunner::Job {
    std::string name;
    fruit::JobControl ctl;
    Done done;
    std::thread thread;
    std::exception_ptr error;
    std::atomic<bool> finished{false};
    mutable std::mutex mu;  // guards stage / fraction
    std::string stage;
    double fraction = 0;
    std::atomic<long long> last_wake_ms{0};
    bool gives_way = false;  // the next start() sets it aside rather than waiting for it
};

JobRunner::JobRunner(std::function<void()> wake) : wake_(std::move(wake)) {}

JobRunner::~JobRunner() {
    std::vector<std::shared_ptr<Job>> all;
    {
        std::lock_guard<std::mutex> lock(mu_);
        all = aside_;
        if (current_) all.push_back(current_);
    }
    for (const auto& j : all) j->ctl.cancel();
    for (const auto& j : all)
        if (j->thread.joinable()) j->thread.join();
    joinFinished();
}

void JobRunner::start(std::string name, std::function<void(const fruit::JobControl&)> work, Done done) {
    launch(std::move(name), std::move(work), std::move(done), false);
}

void JobRunner::startGivingWay(std::string name, std::function<void(const fruit::JobControl&)> work, Done done) {
    launch(std::move(name), std::move(work), std::move(done), true);
}

void JobRunner::launch(std::string name, std::function<void(const fruit::JobControl&)> work, Done done, bool gives_way) {
    {
        // Work that gives way stops by itself, beside the new job: waiting for it would hold up the UI until
        // the step it is in ends, which may be a long one that can't be stopped.
        std::lock_guard<std::mutex> lock(mu_);
        if (current_ && current_->gives_way) {
            current_->ctl.cancel();
            log::info("job set aside: " + current_->name);
            aside_.push_back(std::move(current_));
            current_.reset();
        }
    }
    cancel();
    waitCurrent();
    auto job = std::make_shared<Job>();
    job->name = std::move(name);
    job->done = std::move(done);
    job->gives_way = gives_way;
    Job* raw = job.get();
    auto wake = wake_;
    job->ctl = fruit::JobControl([raw, wake](std::string_view stage, double f) {
        {
            std::lock_guard<std::mutex> lock(raw->mu);
            raw->stage = std::string(stage);
            raw->fraction = f;
        }
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
        long long last = raw->last_wake_ms.load();
        if (now - last >= 50 && raw->last_wake_ms.compare_exchange_strong(last, now) && wake) wake();
    });
    log::info("job start: " + job->name);
    platform::beginBusy(job->name);
    {
        std::lock_guard<std::mutex> lock(mu_);
        current_ = job;
    }
    job->thread = std::thread([raw, work = std::move(work), wake]() {
        try {
            work(raw->ctl);
        } catch (...) {
            raw->error = std::current_exception();
        }
        raw->finished = true;
        if (wake) wake();
    });
}

void JobRunner::cancel() {
    std::lock_guard<std::mutex> lock(mu_);
    if (current_) current_->ctl.cancel();
}

bool JobRunner::busy() const {
    std::lock_guard<std::mutex> lock(mu_);
    return current_ != nullptr;
}

bool JobRunner::stopping() const {
    std::lock_guard<std::mutex> lock(mu_);
    return !aside_.empty();
}

JobProgress JobRunner::progress() const {
    JobProgress p;
    std::lock_guard<std::mutex> lock(mu_);
    if (!current_) return p;
    p.name = current_->name;
    p.running = true;
    std::lock_guard<std::mutex> jl(current_->mu);
    p.stage = current_->stage;
    p.fraction = current_->fraction;
    return p;
}

void JobRunner::joinFinished() {
    if (finished_ && finished_->thread.joinable()) finished_->thread.join();
    finished_.reset();
}

void JobRunner::poll() {
    std::vector<std::shared_ptr<Job>> done;
    {
        std::lock_guard<std::mutex> lock(mu_);
        // Work set aside first: it was started before the current job.
        for (auto it = aside_.begin(); it != aside_.end();) {
            if ((*it)->finished) {
                done.push_back(std::move(*it));
                it = aside_.erase(it);
            } else {
                ++it;
            }
        }
        if (current_ && current_->finished) {
            done.push_back(current_);
            current_.reset();
        }
    }
    for (const auto& job : done) deliver(job);
}

void JobRunner::deliver(const std::shared_ptr<Job>& done) {
    if (done->thread.joinable()) done->thread.join();
    bool idle = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        idle = !current_ && aside_.empty();
    }
    if (idle) platform::endBusy();  // nothing else is running
    if (done->error) {
        try {
            std::rethrow_exception(done->error);
        } catch (const fruit::Cancelled&) {
            log::info("job cancelled: " + done->name);
        } catch (const std::exception& e) {
            log::error("job failed: " + done->name + ": " + e.what());
        } catch (...) {
            log::error("job failed: " + done->name);
        }
    } else {
        log::info("job done: " + done->name);
    }
    if (done->done) done->done(done->error);
}

void JobRunner::waitCurrent() {
    for (;;) {
        std::shared_ptr<Job> cur;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cur = current_;
        }
        if (!cur) return;
        if (cur->thread.joinable()) cur->thread.join();
        poll();
    }
}

void JobRunner::wait() {
    for (;;) {
        std::shared_ptr<Job> cur;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cur = current_ ? current_ : aside_.empty() ? nullptr : aside_.front();
        }
        if (!cur) return;
        if (cur->thread.joinable()) cur->thread.join();
        poll();
    }
}

}  // namespace gui
