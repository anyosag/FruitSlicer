// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "platform/Platform.h"

#include <SDL3/SDL.h>

#include <mutex>
#include <utility>

namespace gui::platform {

namespace {

std::mutex g_mu;
std::vector<std::pair<std::function<void(const std::string&)>, std::string>> g_pending;

struct DialogRequest {
    std::function<void(const std::string&)> done;
    std::vector<SDL_DialogFileFilter> filters;
};

void SDLCALL onDialog(void* userdata, const char* const* files, int) {
    auto* req = static_cast<DialogRequest*>(userdata);
    std::string path;
    if (files && files[0]) path = files[0];
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_pending.emplace_back(std::move(req->done), path);
    }
    delete req;
    SDL_Event e{};
    e.type = SDL_EVENT_USER;  // wake the UI loop
    SDL_PushEvent(&e);
}

}  // namespace

std::string dataDir() {
    // No organisation folder: ~/Library/Application Support/FruitSlicer/
    // on macOS, ~/.local/share/FruitSlicer/ on Linux.
    char* p = SDL_GetPrefPath(nullptr, "FruitSlicer");
    std::string s = p ? p : "./";
    SDL_free(p);
    return s;
}

void openFileDialog(SDL_Window* window, const std::vector<FileFilter>& filters,
                    std::function<void(const std::string&)> done) {
    auto* req = new DialogRequest{std::move(done), {}};
    for (const auto& f : filters) req->filters.push_back({f.name, f.pattern});
    SDL_ShowOpenFileDialog(onDialog, req, window, req->filters.data(), static_cast<int>(req->filters.size()),
                           nullptr, false);
}

void pumpCallbacks() {
    std::vector<std::pair<std::function<void(const std::string&)>, std::string>> ready;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        ready.swap(g_pending);
    }
    for (auto& [fn, path] : ready)
        if (fn) fn(path);
}

#if !defined(__APPLE__)
void beginBusy(const std::string&) {}
void endBusy() {}
#endif

}  // namespace gui::platform
