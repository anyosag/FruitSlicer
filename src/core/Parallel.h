// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Thread helpers that never let an exception escape a worker thread (which
// would call std::terminate and take a GUI down with it).

#include <atomic>
#include <cstddef>
#include <functional>

namespace fruit {

// Number of worker threads to use: `requested`, or all cores when 0.
unsigned resolveThreads(unsigned requested);

// Runs body(t, stop) for t = 0..n-1, each on its own thread (the caller runs
// t = 0). `stop` turns true as soon as any body throws, so the others can
// return early. After every thread has joined, the first exception is
// rethrown here.
void runThreads(unsigned n, const std::function<void(unsigned t, const std::atomic<bool>& stop)>& body);

// Calls fn(i) for i = 0..count-1 on up to `threads` threads (0 = all cores),
// handing out indices in order. Exceptions behave as in runThreads; once one
// is thrown no new indices are started.
void parallelFor(size_t count, unsigned threads, const std::function<void(size_t i)>& fn);

}  // namespace fruit
