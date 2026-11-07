// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The app: which screen is showing, the open job, background work, and the
// 3D view. Everything here runs on the UI thread.

#include <memory>
#include <string>

#include "Document.h"
#include "Jobs.h"
#include "printer/PrinterProfile.h"
#include "view3d/Camera.h"
#include "view3d/Renderer.h"

struct SDL_Window;

namespace gui {

enum class Screen { Home, Job, About };

struct AppOptions {
    bool deterministic = false;  // scripted captures: no MSAA, hide machine-specific text
    int msaa_samples = 4;
};

class App {
public:
    App(SDL_Window* window, AppOptions opts, std::function<void()> wake);

    bool initGraphics(std::string* error);
    // Builds this frame's UI (between ImGui::NewFrame and ImGui::Render).
    void frame();

    // Commands (menus, buttons, drag and drop, scripts).
    void openPath(const std::string& path);
    void openDialog();
    void setScreen(Screen s) { screen_ = s; }
    void setView(ViewPreset v);

    JobRunner& jobs() { return jobs_; }
    const AppOptions& options() const { return opts_; }
    const std::shared_ptr<Document>& document() const { return doc_; }
    const fruit::PrinterProfile& printer() const { return printer_; }
    Screen screen() const { return screen_; }
    const std::string& glInfo() const { return gl_info_; }

    // Used by the screens.
    Renderer& renderer() { return renderer_; }
    Camera& camera() { return camera_; }
    void showError(std::string title, std::string message);
    void frameModel();  // point the camera at the model

private:
    void rail();
    void errorDialog();

    SDL_Window* window_;
    AppOptions opts_;
    JobRunner jobs_;
    fruit::PrinterProfile printer_ = fruit::PrinterProfile::saturn4Ultra16k();
    Screen screen_ = Screen::Home;
    std::shared_ptr<Document> doc_;
    Renderer renderer_;
    Camera camera_;
    std::string gl_info_;
    std::string error_title_, error_message_;
    bool error_open_ = false;
};

}  // namespace gui
