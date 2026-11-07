// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The app: which screen is showing, the open job, background work, dialogs
// and the 3D view. Everything here runs on the UI thread. Buttons, keys and
// scripts all go through the same commands below.

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Document.h"
#include "Calibration.h"
#include "JobFile.h"
#include "Jobs.h"
#include "Preferences.h"
#include "ABTest.h"
#include "Fit.h"
#include "Resins.h"
#include "Send.h"
#include "platform/Platform.h"
#include "mesh/AabbTree.h"
#include "printer/PrinterProfile.h"
#include "view3d/Camera.h"
#include "view3d/Renderer.h"

struct SDL_Window;

namespace gui {

enum class Screen { Home, Job, Calibrate, OrientationCheck, Printer, About, Settings, Setup, Resins, Plate, Score, Fit, ABSetup };
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

// The "Send to the printer" dialog (canvas board "SendWifi").
struct SendDialog {
    bool open = false;
    std::shared_ptr<const SaveResult> file;          // written and read back, waiting to go
    std::vector<fruit::net::PrinterInfo> printers;   // found, or added by address
    int chosen = -1;                                 // index into printers
    bool searched = false;                           // a search has finished
    bool start_print = false;                        // "Start printing when it arrives"; off by default
    char address[96] = {};                           // "Add a printer by its address…"
    std::string problem;                             // what went wrong last, in words
    std::shared_ptr<const SendResult> sent;          // what arrived
    double send_started = -1;                        // seconds, for the time left
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
    // A model (STL, OBJ, 3MF) or a saved job, after asking about unsaved work.
    void openPath(const std::string& path);
    void openDialog();
    // Saved jobs.
    void saveJob();    // to its file, or ask where (⌘S)
    void saveJobAs();  // ask where (⇧⌘S)
    void saveJobTo(const std::string& path, std::function<void()> then = {});
    const std::vector<RecentJob>& recentJobs() const { return recent_; }
    // Runs `then` now, or once "Save … before closing?" is answered.
    void leaveJob(std::function<void()> then);
    struct UnsavedDialog {
        bool open = false;
        std::vector<std::string> changes;  // since the last save; empty when never saved
        std::function<void()> then;
    };
    enum class UnsavedAnswer { DontSave, Cancel, Save };
    UnsavedDialog& unsavedDialog() { return unsaved_; }
    void answerUnsaved(UnsavedAnswer a);
    // A job whose model file has gone: keep its copy, or find the file.
    struct RelinkDialog {
        bool open = false;
        std::shared_ptr<Document> doc;  // opened, not yet shown
        std::string model_path;         // where the model was
        bool find = false;              // "Find the file" picked
    };
    RelinkDialog& relinkDialog() { return relink_; }
    void relinkOpen();                  // with the choice made: the copy, or find the file
    void relinkTo(const std::string& model_path);
    void relinkCancel();
    void requestQuit();                 // the window's close box or ⌘Q
    bool quitting() const { return quit_; }
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
    // The advanced support settings page. Values apply to the job at once
    // (making supports out of date) and carry over to the next model opened.
    void showAdvanced(bool show) { advanced_open_ = show; }
    bool advancedOpen() const { return advanced_open_; }
    void setAdvanced(const std::string& key, double value);
    void resetAdvanced();
    void applyAdvanced();  // back to the Supports step, generating with these values
    bool problemsOpen() const { return problems_open_; }
    void focusOn(const fruit::Vec3& where);  // turn the camera to look at a point
    // Editing tips by hand (canvas board "TipEdit"). Every edit is kept in
    // model coordinates and can be undone on its own. An edit applies on top
    // of the planner's tips (only the tree is rebuilt) unless it takes back
    // one the planner worked around: then the tips are planned again.
    enum class TipTool { Select, Add, Zone };
    void editTips(bool on);
    bool editingTips() const { return tip_editing_; }
    TipTool& tipTool() { return tip_tool_; }
    int& newTipClass() { return new_tip_class_; }  // 0 = 0.2 … 3 = 0.6 mm
    bool& showZones() { return show_zones_; }
    int& draggingTip() { return dragging_tip_; }
    void addTipAt(const fruit::Vec3& pos, const fruit::Vec3& normal);  // printer coordinates
    void addZoneAt(const fruit::Vec3& pos);
    void moveTip(int index, const fruit::Vec3& to, const fruit::Vec3& normal);
    void deleteTip(int index);
    void resizeTip(int index, double diameter);
    void pinTip(int index);
    void undoTipEdit(int index);  // the Undo of one line in "Your edits"
    void undoAllTipEdits();
    // ⌘Z and ⇧⌘Z while editing: the edits as they were before the last change.
    bool undoTipChange();
    bool redoTipChange();
    // The placed model under a ray (printer coordinates); false when it misses.
    bool pickModel(const fruit::Vec3& origin, const fruit::Vec3& dir, fruit::Vec3* pos, fruit::Vec3* normal);
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
    void savePrintTo(const std::string& path, const std::string& drive, bool to_send = false);
    void ejectDrive();
    void revealSaved();   // the saved file in Finder
    void dismissSaved();  // leave the "saved" page for the Print step
    void exportStl(bool supports_only);
    void exportStlTo(const std::string& path, bool supports_only);
    std::vector<platform::Volume>& drives() { return drives_; }
    int& driveChoice() { return drive_choice_; }          // index into drives(), -1 = choose a folder
    bool& orientationAsk() { return orientation_ask_; }  // the "save before checking?" question is open
    // Over Wi-Fi: "Save to" picks the printer; saving writes the file to a
    // temporary folder, checks it, then opens the send dialog.
    bool& sendOverWifi() { return send_wifi_; }
    SendDialog& send() { return send_; }
    void findPrinters();                       // search the network (and the addresses added by hand)
    void addPrinter(const std::string& host);  // ask one address and remember it
    void sendToPrinter();                      // upload to the chosen printer (and start it, if asked)
    void closeSend();
    void useMockPrinter(bool silent = false);  // scripts: a printer on loopback (or nothing that answers)
    // Calibrate: the printer's settings and the image orientation check.
    const PrinterSetup& printerSetup() const { return printer_setup_; }
    void saveOrientationPiece();                     // f.stl, through a save dialog
    void saveOrientationPieceTo(const std::string& path);
    void chooseOrientationSample();                  // SatelLite's .goo of the F, then compare
    void checkOrientationWith(const std::string& satellite_goo);
    const std::string& orientationPiece() const { return orient_piece_; }  // where f.stl was saved
    const std::shared_ptr<const OrientationCheck>& orientationResult() const { return orient_check_; }
    void useOrientationFix();                        // the check's result for every print from now on
    void setHinge(std::optional<HingeChoice> side);
    // The check's two layer pictures as textures (0 while there is no result).
    unsigned orientationPicture(bool ours);
    // Settings, About and first-run setup (canvas boards "Settings",
    // "About" and "Setup").
    const Preferences& preferences() const { return prefs_; }
    void setPreferences(const Preferences& p);  // kept (outside scripted runs) and applied
    int viewSamples() const;                    // multisampling for the 3D view
    void revealLog();                           // the log file in Finder
    void openSourcePage();                      // FruitSlicer's source code, in the browser
    std::string& licenceOpen() { return licence_open_; }  // About: whose licence text shows ("" none)
    void startSetup();                          // from Settings: run it again
    int& setupStep() { return setup_step_; }    // 0 welcome, 1 print settings, 2 orientation, 3 calibration
    void finishSetup(Screen then);              // marks it done, then shows `then`
    // Setup's second step: the print settings each new job starts with.
    struct TypedDefaults {
        double exposure = 0, bottom_exposure = 0;
        int bottom_layers = -1, transition_layers = -1;
    };
    bool& setupTyped() { return setup_typed_; }  // "Type them in" picked
    TypedDefaults& typedDefaults() { return typed_defaults_; }
    void chooseDefaultSample();                 // a SatelLite .goo, through a dialog
    void useDefaultSample(const std::string& goo);
    void useTypedDefaults();                    // the values typed in
    // Resins (canvas board "Resins"): each resin's print settings and the
    // support values measured for it. A job keeps a copy of its resin's.
    const std::vector<Resin>& resins() const { return resins_; }
    const Resin* resinById(const std::string& id) const;
    const Resin& newJobResin() const;                   // the resin new jobs start with
    std::string& resinSelected() { return resin_selected_; }  // the one the Resins page shows
    void setNewJobResin(const std::string& id);
    void useResinForJob(const std::string& id);         // the open job: its print settings and values
    bool& resinMenu() { return resin_menu_; }           // the job's resin list is open
    void newResinFromGoo();                             // a SatelLite .goo, through a dialog
    void newResinFromGooFile(const std::string& goo);
    void importResin();                                 // a profile (.json), through a dialog
    void importResinFile(const std::string& path);
    void duplicateResin(const std::string& id);
    void renameResin(const std::string& id, const std::string& name);
    std::string& resinToDelete() { return resin_to_delete_; }  // "Delete …?" open for this id
    void deleteResin(const std::string& id);
    void replaceResinSample(const std::string& id);     // another .goo, through a dialog
    void replaceResinSampleFile(const std::string& id, const std::string& goo);
    std::shared_ptr<const fruit::GooHeader> resinSample(const Resin& r);  // its kept header (read once)
    // Resin test plates (canvas boards "PlatePreview" and "Results"), for the
    // resin new jobs use: its manifests and scores are kept with it.
    void openPlate(const std::string& kind);
    const std::string& plateKind() const { return plate_kind_; }
    const fruit::CalibrationPlate& plate();       // the open kind's, made once
    int& plateSelected() { return plate_selected_; }  // a coupon id
    void savePlate(bool goo);                     // through a dialog
    void savePlateTo(const std::string& path);    // .stl, or .goo with the resin's print settings
    void openScores(const std::string& kind);     // scoring, for a plate saved before
    const PlateScores* scores() const { return scores_.get(); }
    int scoresRevision() const { return scores_revision_; }  // changes whenever the scores do
    int& scoreSelected() { return score_selected_; }
    bool& scoreMirrored() { return score_mirrored_; }  // the plate printed mirrored
    enum class Outcome { None, Held, Partly, FellOff };
    void setScore(int id, Outcome o, int scar, const std::string& notes);
    void importScores();                          // a filled sheet, through a dialog
    void importScoresFile(const std::string& csv);
    // The fit (canvas board "Fit"): the new-job resin's scored plates turned
    // into its support values.
    void openFit();
    const FitSummary* fitSummary() const { return fit_.get(); }
    bool fitInUse() const;                        // the resin's values are these already
    void useFit();                                // into the resin's values
    void saveFitAs();                             // as a profile (.json), through a dialog
    void saveFitTo(const std::string& path);
    bool& fitTable() { return fit_table_; }       // the chart as a table
    // The A/B test (canvas board "ABSetup"): copies of the job's model, half
    // with its supports and half bare for SatelLite's, on one plate.
    void openABSetup();
    std::string abBlocker() const;                // why it can't be set up now ("" if it can)
    const fruit::ABPlateFit& abFit();             // the job's model with its supports
    int abCopies() const { return ab_copies_; }
    void setABCopies(int n);                      // kept between 2 and what fits
    const fruit::ABPlate* abPlate();              // as it would be saved
    void saveABFiles();                           // through a dialog
    void saveABTo(const std::string& path_a);     // <name>_A.stl, <name>_B.stl, and the kept files
    const ABRecord& abRecord() const { return ab_record_; }
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
    void openModel(const std::string& path);
    void openJobFile(const std::string& path);
    void installJob(std::shared_ptr<Document> doc);
    void remember(const Document& d);
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
    bool advanced_open_ = false;
    bool tip_editing_ = false;
    TipTool tip_tool_ = TipTool::Select;
    int new_tip_class_ = 2;
    bool show_zones_ = true;
    int dragging_tip_ = -1;
    std::shared_ptr<fruit::AabbTree> pick_tree_;
    const void* pick_doc_ = nullptr;
    int pick_revision_ = -1;
    std::vector<std::vector<TipEdit>> tip_undo_, tip_redo_;  // edit lists, while editing
    // Puts in a new edit list (undoable) and rebuilds the tips; `select`
    // (printer coordinates) is the tip to select once they are rebuilt.
    void setTipEdits(std::vector<TipEdit> edits, std::optional<fruit::Vec3> select = std::nullopt);
    void rebuildTips(std::optional<fruit::Vec3> select);
    fruit::Transform modelFromPrinter() const;
    std::map<std::string, double> advanced_;  // carried over to the next model opened
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
    bool send_wifi_ = false;
    SendDialog send_;
    NetPorts net_ports_;
    std::vector<std::string> printer_hosts_;  // added by hand
    bool printer_hosts_loaded_ = false;
    std::shared_ptr<void> mock_printer_;      // scripts only
    std::vector<RecentJob> recent_;
    UnsavedDialog unsaved_;
    RelinkDialog relink_;
    bool quit_ = false;
    PrinterSetup printer_setup_;
    Preferences prefs_;
    std::string licence_open_;
    int setup_step_ = 0;
    bool setup_typed_ = false;
    TypedDefaults typed_defaults_;
    std::vector<Resin> resins_;
    std::string resin_selected_, resin_to_delete_;
    bool resin_menu_ = false;
    std::map<std::string, std::shared_ptr<const fruit::GooHeader>> resin_samples_;  // by kept path
    std::string resinsDir() const;
    void storeResin(const Resin& r);  // into the library and its file (outside scripted runs)
    void applyResin(Document& d, const Resin& r);
    std::string keepHeader(const std::string& goo);  // a SatelLite file's header in FruitSlicer's folder
    std::string plate_kind_ = "tips";
    std::map<std::string, std::shared_ptr<const fruit::CalibrationPlate>> plates_;
    int plate_selected_ = 1, score_selected_ = 1, scores_revision_ = 0;
    bool score_mirrored_ = false;
    std::shared_ptr<PlateScores> scores_;
    std::string scores_resin_;  // whose they are
    std::shared_ptr<const FitSummary> fit_;
    std::string fit_resin_;     // whose plates it fitted
    bool fit_table_ = false;
    nlohmann::json fittedProfile() const;  // the resin's profile with the fit in it
    int ab_copies_ = 0;
    fruit::ABPlateFit ab_fit_;
    std::shared_ptr<const fruit::ABPlate> ab_plate_;
    const void* ab_fit_for_ = nullptr;  // the supports result they were made from
    const void* ab_plate_for_ = nullptr;
    int ab_plate_copies_ = 0;
    ABRecord ab_record_;
    std::string abDir() const;  // where the test's manifest, sheet and record are kept
    // Where a resin's plate of this kind is kept: the file it was saved as
    // (if it was), and its manifest and score sheet in FruitSlicer's folder.
    PlateFiles keptPlate(const Resin& r, const std::string& kind) const;
    std::string platesDir(const std::string& resin_id) const;
    void notePlate(const std::string& resin_id, const std::string& kind, const PlateFiles& f);
    bool onRemovableDrive(const std::string& path) const;
    std::string dataFolder() const;   // FruitSlicer's own folder (a scratch one in scripted runs)
    std::string orient_piece_;
    std::shared_ptr<const OrientationCheck> orient_check_;
    void setPrinterSetup(const PrinterSetup& s);  // keeps it (outside scripted runs) and applies it
    void applyPrinterSetup();                      // to the open job's supports
    std::shared_ptr<const OrientationCheck> orient_tex_for_;
    unsigned orient_tex_[2] = {0, 0};
    std::shared_ptr<const PrintPlan> plan_;
    std::shared_ptr<const LayerResult> plan_layers_;
    PrintSettings plan_settings_;
    double plan_raft_ = -1;
};

}  // namespace gui
