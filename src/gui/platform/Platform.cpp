// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "platform/Platform.h"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <filesystem>
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

void saveFileDialog(SDL_Window* window, const std::vector<FileFilter>& filters, const std::string& name,
                    std::function<void(const std::string&)> done) {
    auto* req = new DialogRequest{std::move(done), {}};
    for (const auto& f : filters) req->filters.push_back({f.name, f.pattern});
    SDL_ShowSaveFileDialog(onDialog, req, window, req->filters.data(), static_cast<int>(req->filters.size()),
                           name.empty() ? nullptr : name.c_str());
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

// Linux (development and CI): drives mounted under /media/<user> or
// /run/media/<user>, as desktop environments do.
std::vector<Volume> removableVolumes() {
    std::vector<Volume> out;
    const char* user = std::getenv("USER");
    if (!user) return out;
    for (const std::string& base : {std::string("/media/") + user, std::string("/run/media/") + user}) {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(base, ec)) {
            if (!e.is_directory(ec)) continue;
            Volume v;
            v.name = e.path().filename().string();
            v.path = e.path().string();
            const auto sp = std::filesystem::space(e.path(), ec);
            if (!ec) v.free_bytes = sp.available;
            out.push_back(v);
        }
    }
    return out;
}

bool ejectVolume(const std::string&, std::string* error) {
    if (error) *error = "Ejecting from FruitSlicer works on macOS; use your desktop's eject here.";
    return false;
}

void revealFile(const std::string& path) {
    const std::string dir = std::filesystem::path(path).parent_path().string();
    if (!dir.empty()) SDL_OpenURL(("file://" + dir).c_str());
}
#endif

}  // namespace gui::platform
