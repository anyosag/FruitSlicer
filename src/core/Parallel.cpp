// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "core/Parallel.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace fruit {

unsigned resolveThreads(unsigned requested) {
    return requested ? requested : std::max(1u, std::thread::hardware_concurrency());
}

void runThreads(unsigned n, const std::function<void(unsigned, const std::atomic<bool>&)>& body) {
    std::atomic<bool> stop{false};
    if (n <= 1) {
        body(0, stop);
        return;
    }
    std::mutex mu;
    std::exception_ptr first;
    auto guarded = [&](unsigned t) {
        try {
            body(t, stop);
        } catch (...) {
            std::lock_guard<std::mutex> lock(mu);
            if (!first) first = std::current_exception();
            stop = true;
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(n - 1);
    try {
        for (unsigned t = 1; t < n; ++t) pool.emplace_back(guarded, t);
    } catch (...) {
        // Could not start a thread: let the ones already running finish.
        stop = true;
        for (auto& th : pool) th.join();
        throw;
    }
    guarded(0);
    for (auto& th : pool) th.join();
    if (first) std::rethrow_exception(first);
}

void parallelFor(size_t count, unsigned threads, const std::function<void(size_t)>& fn) {
    if (count == 0) return;
    const unsigned n = static_cast<unsigned>(std::min<size_t>(resolveThreads(threads), count));
    std::atomic<size_t> next{0};
    runThreads(n, [&](unsigned, const std::atomic<bool>& stop) {
        for (size_t i = next++; i < count && !stop; i = next++) fn(i);
    });
}

}  // namespace fruit
