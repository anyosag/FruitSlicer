// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The job runner: one job at a time, and work that gives way (the Collisions
// check) not holding up the job started after it.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "Jobs.h"
#include "platform/Platform.h"

using namespace gui;
using namespace std::chrono_literals;

// The runner tells the system it is busy; here that is only counted.
namespace {
int g_busy_ended = 0;
}  // namespace

namespace gui::platform {
void beginBusy(const std::string&) {}
void endBusy() { ++g_busy_ended; }
}  // namespace gui::platform

namespace {

// A step that can't be stopped part way (as a Manifold boolean): it ends when
// `release` is set, or after a few seconds so that a failing test still ends.
void stepThatCantStop(const std::atomic<bool>& release) {
    const auto until = std::chrono::steady_clock::now() + 5s;
    while (!release && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(1ms);
}

bool cancelled(std::exception_ptr e) {
    if (!e) return false;
    try {
        std::rethrow_exception(e);
    } catch (const fruit::Cancelled&) {
        return true;
    } catch (...) {
    }
    return false;
}

}  // namespace

TEST_CASE("Jobs: work that gives way does not hold up the job started after it", "[gui][jobs-runner]") {
    g_busy_ended = 0;
    JobRunner jobs(nullptr);
    std::atomic<bool> release{false}, started{false}, slow_ended{false};
    std::vector<std::string> delivered;
    bool slow_cancelled = false, next_ok = false;
    jobs.startGivingWay(
        "slow",
        [&](const fruit::JobControl& ctl) {
            started = true;
            stepThatCantStop(release);
            slow_ended = true;
            ctl.check();
        },
        [&](std::exception_ptr e) {
            delivered.push_back("slow");
            slow_cancelled = cancelled(e);
        });
    while (!started) std::this_thread::sleep_for(1ms);

    jobs.start("next", [](const fruit::JobControl&) {}, [&](std::exception_ptr e) {
        delivered.push_back("next");
        next_ok = !e;
    });
    // start() came back while the slow step was still on: it did not wait for it.
    CHECK_FALSE(slow_ended);
    CHECK(jobs.stopping());
    CHECK(jobs.progress().name == "next");

    // The new job finishes and is delivered while the old one is still stopping; the app is still busy.
    while (jobs.busy()) {
        jobs.poll();
        std::this_thread::sleep_for(1ms);
    }
    CHECK(next_ok);
    CHECK(delivered == std::vector<std::string>{"next"});
    CHECK(jobs.stopping());
    CHECK(g_busy_ended == 0);

    release = true;
    jobs.wait();  // waits for the work set aside too
    CHECK_FALSE(jobs.stopping());
    CHECK(delivered == std::vector<std::string>{"next", "slow"});
    CHECK(slow_cancelled);  // its done still came, saying it was cancelled
    CHECK(g_busy_ended == 1);
}

TEST_CASE("Jobs: other work is cancelled and waited for, one job at a time", "[gui][jobs-runner]") {
    JobRunner jobs(nullptr);
    std::atomic<bool> started{false}, first_ended{false};
    bool first_cancelled = false;
    jobs.start(
        "first",
        [&](const fruit::JobControl& ctl) {
            started = true;
            while (!ctl.cancelled()) std::this_thread::sleep_for(1ms);
            first_ended = true;
            ctl.check();
        },
        [&](std::exception_ptr e) { first_cancelled = cancelled(e); });
    while (!started) std::this_thread::sleep_for(1ms);
    jobs.start("second", [](const fruit::JobControl&) {}, nullptr);
    CHECK(first_ended);  // stopped before the second began
    CHECK(first_cancelled);
    CHECK_FALSE(jobs.stopping());
    jobs.wait();
    CHECK_FALSE(jobs.busy());
}

TEST_CASE("Jobs: work that gives way and got to the end first hands back its result", "[gui][jobs-runner]") {
    JobRunner jobs(nullptr);
    std::atomic<bool> ended{false};
    bool ok = false;
    jobs.startGivingWay("quick", [&](const fruit::JobControl&) { ended = true; }, [&](std::exception_ptr e) { ok = !e; });
    while (!ended) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(5ms);
    jobs.start("next", [](const fruit::JobControl&) {}, nullptr);
    jobs.wait();
    CHECK(ok);
    CHECK_FALSE(jobs.stopping());
}
