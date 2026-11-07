// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Runs engine work off the UI thread. One compute job at a time; a newer
// request replaces (cancels) the running one. Progress is read by the UI
// each frame; finished jobs hand their result back on the UI thread.

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

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

    // Starts `work` on a worker thread, cancelling any running job first.
    void start(std::string name, std::function<void(const fruit::JobControl&)> work, Done done);
    void cancel();
    bool busy() const;
    JobProgress progress() const;
    // UI thread, once per frame: delivers finished jobs' callbacks.
    void poll();
    // Blocks until the current job is finished and delivered (scripts, tests).
    void wait();

private:
    struct Job;
    void joinFinished();

    std::function<void()> wake_;
    mutable std::mutex mu_;
    std::shared_ptr<Job> current_;
    std::shared_ptr<Job> finished_;
};

}  // namespace gui
