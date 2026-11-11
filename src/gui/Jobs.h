// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Runs engine work off the UI thread. One compute job at a time; a newer
// request replaces (cancels) the running one. Progress is read by the UI
// each frame; finished jobs hand their result back on the UI thread.
//
// Work that only checks for cancelling between long steps it can't stop
// (Manifold's booleans) is started with startGivingWay: a newer job does
// not wait for it to stop, so a click never waits on it. It finishes on its
// own beside the newer one, and its `done` is still called.

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/JobControl.h"

namespace gui {

struct JobProgress {
    std::string name;   // what the job is, e.g. "Opening armadillo.obj"
    std::string stage;  // engine stage key, e.g. "slice"
    double fraction = 0;
    bool running = false;
};

class JobRunner {
public:
    // Called on the UI thread when a job finishes: `error` is null on
    // success, holds fruit::Cancelled when cancelled.
    using Done = std::function<void(std::exception_ptr error)>;

    // `wake` is called (from worker threads, at most ~20 times a second)
    // so the UI can redraw progress; it must be thread-safe.
    explicit JobRunner(std::function<void()> wake);
    ~JobRunner();
    JobRunner(const JobRunner&) = delete;
    JobRunner& operator=(const JobRunner&) = delete;

    // Starts `work` on a worker thread, cancelling any running job first
    // (and waiting for it to stop, unless it was started giving way).
    void start(std::string name, std::function<void(const fruit::JobControl&)> work, Done done);
    // The same for work that gives way: the next start() cancels it and sets
    // it aside to stop by itself. Its `done` gets fruit::Cancelled, or its
    // result if it got to the end first.
    void startGivingWay(std::string name, std::function<void(const fruit::JobControl&)> work, Done done);
    void cancel();
    bool busy() const;      // a job is running (not counting work set aside)
    bool stopping() const;  // work set aside has not stopped yet
    JobProgress progress() const;
    // UI thread, once per frame: delivers finished jobs' callbacks.
    void poll();
    // Blocks until the current job and any set aside are finished and
    // delivered (scripts, tests).
    void wait();

private:
    struct Job;
    void launch(std::string name, std::function<void(const fruit::JobControl&)> work, Done done, bool gives_way);
    void waitCurrent();
    void deliver(const std::shared_ptr<Job>& job);
    void joinFinished();

    std::function<void()> wake_;
    mutable std::mutex mu_;
    std::shared_ptr<Job> current_;
    std::vector<std::shared_ptr<Job>> aside_;  // cancelled work that gives way, still stopping (oldest first)
    std::shared_ptr<Job> finished_;
};

}  // namespace gui
