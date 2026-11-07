// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The app: which screen is showing, the open job, background work, dialogs
// and the 3D view. Everything here runs on the UI thread. Buttons, keys and
// scripts all go through the same commands below.

#include <functional>
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
// The job's steps, in order. Only Model works yet; the rest follow.
enum class Step { Model, Orient, Supports, Layers, Print };

struct AppOptions {
    bool deterministic = false;  // scripted captures: no MSAA, hide machine-specific text
    int msaa_samples = 4;
};

// The size choice in the health dialog.
enum class SizeChoice { Full, Largest, Custom, Inches };

struct HealthDialog {
    bool open = false;
    SizeChoice choice = SizeChoice::Full;
    double custom_percent = 100;
};

class App {
public:
    App(SDL_Window* window, AppOptions opts, std::function<void()> wake);

    bool initGraphics(std::string* error);
    // Builds this frame's UI (between ImGui::NewFrame and ImGui::Render).
    void frame();

    // ---- Commands ----
    void openPath(const std::string& path);
    void openDialog();
    void setScreen(Screen s) { screen_ = s; }
    void setView(ViewPreset v);
    void fitView();
    // Health dialog, after a model loads: place it at the chosen size, or drop it.
    void chooseSize(SizeChoice c, double custom_percent = 0);
    void placeChecked();
    void cancelChecked();
    // Placement edits on the open job (undoable).
    void setScalePercent(double percent);
    void setRotation(const fruit::Vec3& degrees);
    void setOnPlate(bool on_plate);
    void scaleToFit();
    void undo();
    void redo();
    void showShortcuts(bool show) { shortcuts_open_ = show; }

    // ---- State ----
    JobRunner& jobs() { return jobs_; }
    const AppOptions& options() const { return opts_; }
    const std::shared_ptr<Document>& document() const { return doc_; }
    const std::shared_ptr<Document>& checking() const { return checking_; }  // loaded, awaiting the health dialog
    HealthDialog& health() { return health_; }
    const fruit::PrinterProfile& printer() const { return printer_; }
    Screen screen() const { return screen_; }
    Step step() const { return step_; }
    bool viewIs(ViewPreset v) const { return view_set_ && view_ == v; }
    void viewChanged() { view_set_ = false; }  // the user turned the camera
    const std::string& glInfo() const { return gl_info_; }

    // Used by the screens.
    Renderer& renderer() { return renderer_; }
    Camera& camera() { return camera_; }
    void showError(std::string title, std::string message);
    void frameModel();  // point the camera at the model
    // The scale (1.0 = 100 %) and rotation the health dialog's choice gives.
    fruit::Placement healthPlacement() const;

private:
    void rail();
    void errorDialog();
    void keys();

    SDL_Window* window_;
    AppOptions opts_;
    JobRunner jobs_;
    fruit::PrinterProfile printer_ = fruit::PrinterProfile::saturn4Ultra16k();
    Screen screen_ = Screen::Home;
    Step step_ = Step::Model;
    std::shared_ptr<Document> doc_, checking_;
    HealthDialog health_;
    Renderer renderer_;
    Camera camera_;
    ViewPreset view_ = ViewPreset::Angled;
    bool view_set_ = true;
    std::string gl_info_;
    std::string error_title_, error_message_;
    bool error_open_ = false;
    bool shortcuts_open_ = false;
};

}  // namespace gui
