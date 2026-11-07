// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The app: which screen is showing, the open job, background work, dialogs
// and the 3D view. Everything here runs on the UI thread. Buttons, keys and
// scripts all go through the same commands below.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Document.h"
#include "Jobs.h"
#include "platform/Platform.h"
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

// The Orient step shows its settings, or the search's results.
enum class OrientView { Settings, Results };

// How the Supports step draws the result.
enum class SupportsView { Supports, TipsOnly, Why };

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
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

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
    // Steps.
    bool canGoTo(Step s) const;
    void goToStep(Step s);
    // Orient: sides people will see, the search, and the choice.
    void toggleSide(Side s);
    void clearSides();
    bool sidePicked(Side s) const;
    void findOrientation();
    void useOrientation(int option);  // -1 keeps how it sits
    void showOrientSettings() { orient_view_ = OrientView::Settings; }
    OrientView orientView() const { return orient_view_; }
    // The results' pictures from below, as textures (-1: how it sits now).
    unsigned orientPicture(int option);
    // Supports.
    void generateSupports();
    void setSupportChoices(const SupportChoices& c);
    void selectTip(int index);
    void setSupportsView(SupportsView v) { supports_view_ = v; }
    SupportsView supportsView() const { return supports_view_; }
    void showProblems(bool show) { problems_open_ = show; }
    bool problemsOpen() const { return problems_open_; }
    void focusOn(const fruit::Vec3& where);  // turn the camera to look at a point
    // Puts this step's extras (supports, tips, overhang tint) in the 3D view.
    void syncScene();
    // Print.
    void chooseSample();                                  // pick a SatelLite .goo for its settings
    void useSample(const std::string& path);
    void setExposures(double normal_s, double bottom_s);  // typed; 0 keeps the sample's
    std::shared_ptr<const PrintPlan> printPlan();         // for the current layers and settings
    std::string printBlocker();                           // what stops saving, in a few words ("" if nothing)
    void savePrint();                                     // asks where (and once about orientation)
    void savePrintAfterAsking();                          // "Save anyway" in the orientation question
    void savePrintTo(const std::string& path, const std::string& drive);
    void ejectDrive();
    void revealSaved();   // the saved file in Finder
    void dismissSaved();  // leave the "saved" page for the Print step
    void exportStl(bool supports_only);
    void exportStlTo(const std::string& path, bool supports_only);
    std::vector<platform::Volume>& drives() { return drives_; }
    int& driveChoice() { return drive_choice_; }          // index into drives(), -1 = choose a folder
    bool& orientationAsk() { return orientation_ask_; }  // the "save before checking?" question is open
    // Layers.
    void setLayer(int index);  // 0-based; clamped
    void setShowBelow(bool show);
    // The current layer drawn for an area of `max_w` × `max_h` pixels, as a
    // texture (0 while there is none); `image` describes it.
    unsigned layerPicture(int max_w, int max_h, const LayerImage** image);

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
    void maintain();  // starts background scoring when what is shown is stale
    void freePictures();

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
    OrientView orient_view_ = OrientView::Settings;
    int scored_revision_ = -1, scored_settings_ = -1;  // last scoring started (it may have been cancelled)
    std::shared_ptr<const OrientSearch> pictures_for_;
    std::vector<unsigned> pictures_;  // options, then how it sits now
    SupportsView supports_view_ = SupportsView::Supports;
    bool problems_open_ = false;
    std::shared_ptr<const SupportResult> scene_for_;  // the result the tip markers were built from
    std::vector<fruit::TriangleMesh> tip_meshes_;      // one per tip kind
    int scene_key_ = 0;
    std::shared_ptr<const SupportResult> sliced_for_;  // last slicing started (it may have been cancelled)
    unsigned layer_tex_ = 0;
    LayerImage layer_image_;
    std::shared_ptr<const LayerResult> layer_tex_result_;
    int layer_tex_index_ = -1, layer_tex_w_ = 0, layer_tex_h_ = 0;
    bool layer_tex_below_ = false;
    std::vector<platform::Volume> drives_;
    int drive_choice_ = -1;
    double drives_checked_ = -1;  // when the drive list was last read (seconds)
    bool orientation_ask_ = false, orientation_asked_ = false;
    std::shared_ptr<const PrintPlan> plan_;
    std::shared_ptr<const LayerResult> plan_layers_;
    PrintSettings plan_settings_;
    double plan_raft_ = -1;
};

}  // namespace gui
