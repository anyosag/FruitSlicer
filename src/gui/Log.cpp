// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>

namespace gui::log {

namespace {

std::mutex g_mu;
std::ofstream g_file;
std::string g_path;

void write(const char* level, const std::string& msg) {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_file) g_file << stamp << ' ' << level << ' ' << msg << '\n' << std::flush;
    std::cerr << level << ' ' << msg << '\n';
}

}  // namespace

void open(const std::string& dir) {
    std::lock_guard<std::mutex> lock(g_mu);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    g_path = (std::filesystem::path(dir) / "fruitslicer.log").string();
    std::filesystem::rename(g_path, g_path + ".1", ec);
    g_file.open(g_path, std::ios::out | std::ios::trunc);
}

const std::string& path() { return g_path; }

void info(const std::string& msg) { write("INFO ", msg); }

void error(const std::string& msg) { write("ERROR", msg); }

}  // namespace gui::log
