// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Thread helpers that never let an exception escape a worker thread (which
// would call std::terminate and take a GUI down with it).

#include <atomic>
#include <cstddef>
#include <functional>

namespace fruit {

// Number of worker threads to use: `requested`, or when 0 the process-wide
// default (all cores unless setDefaultThreads says fewer).
unsigned resolveThreads(unsigned requested);

// What a request for 0 threads means from now on: at most `n` threads, or
// all cores again when 0. For an app's "use fewer cores" setting; work
// already running keeps the threads it started with.
void setDefaultThreads(unsigned n);

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
