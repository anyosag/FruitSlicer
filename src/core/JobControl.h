// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Progress reporting and cancellation for long-running engine calls.
//
// Long engine functions take `const JobControl& ctl` (default: none(), which
// reports nothing and is never cancelled). Copies of a control share one
// cancel flag and one progress sink. `sub(begin, end)` returns a child whose
// fractions 0..1 land in [begin, end] of its parent, so nested steps report
// a single overall fraction; the fraction the sink sees never goes down.

#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace fruit {

// Thrown by JobControl::check() once the job has been cancelled.
struct Cancelled : std::runtime_error {
    Cancelled() : std::runtime_error("cancelled") {}
};

class JobControl {
public:
    // Receives the current stage name and the overall fraction (0..1). It may
    // be called from engine worker threads, but never concurrently: calls are
    // serialised by the control.
    using Sink = std::function<void(std::string_view stage, double fraction)>;

    JobControl();                    // can be cancelled; reports nowhere
    explicit JobControl(Sink sink);  // can be cancelled; reports to `sink`

    // Never cancelled, reports nothing. The default for engine entry points.
    static const JobControl& none();

    void cancel() const noexcept;
    bool cancelled() const noexcept;
    void check() const;  // throws Cancelled if cancel() was called

    // `f` is a fraction (0..1) of this control's range.
    void report(std::string_view stage, double f) const;
    void step(std::string_view stage, double f) const {
        report(stage, f);
        check();
    }

    // Child covering [begin, end] of this control's range.
    JobControl sub(double begin, double end) const;
    // Child sharing the cancel flag but reporting nothing (for work running in
    // parallel, where interleaved fractions would be meaningless).
    JobControl silent() const;

private:
    struct Shared;
    JobControl(std::shared_ptr<Shared> s, double begin, double end, bool silent)
        : s_(std::move(s)), begin_(begin), end_(end), silent_(silent) {}
    std::shared_ptr<Shared> s_;
    double begin_ = 0, end_ = 1;
    bool silent_ = false;
};

}  // namespace fruit
