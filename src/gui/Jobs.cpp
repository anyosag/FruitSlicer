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
};

JobRunner::JobRunner(std::function<void()> wake) : wake_(std::move(wake)) {}

JobRunner::~JobRunner() {
    std::shared_ptr<Job> cur;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cur = current_;
    }
    if (cur) {
        cur->ctl.cancel();
        if (cur->thread.joinable()) cur->thread.join();
    }
    joinFinished();
}

void JobRunner::start(std::string name, std::function<void(const fruit::JobControl&)> work, Done done) {
    cancel();
    wait();
    auto job = std::make_shared<Job>();
    job->name = std::move(name);
    job->done = std::move(done);
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
    std::shared_ptr<Job> done;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (current_ && current_->finished) {
            done = current_;
            current_.reset();
        }
    }
    if (!done) return;
    if (done->thread.joinable()) done->thread.join();
    platform::endBusy();
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

void JobRunner::wait() {
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

}  // namespace gui
