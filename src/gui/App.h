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
#include "LayFace.h"
#include "Jobs.h"
#include "Measure.h"
#include "PartsTools.h"
#include "Collisions.h"
#include "Preferences.h"
#include "ShapeTools.h"
#include "ABTest.h"
#include "Fit.h"
#include "GooTools.h"
#include "Navigation.h"
#include "Placing.h"
#include "Resins.h"
#include "Send.h"
#include "platform/Platform.h"
#include "mesh/AabbTree.h"
#include "printer/PrinterProfile.h"
#include "view3d/Camera.h"
#include "view3d/Renderer.h"

struct SDL_Window;

namespace gui {

enum class Screen {
    Home, Job, Calibrate, OrientationCheck, Printer, About, Settings, Setup, Resins, Plate, Score, Fit, ABSetup, ABResults,
    Tools
};

struct AppOptions {
    bool deterministic = false;  // scripted captures: no MSAA, hide machine-specific text
    int msaa_samples = 4;
};

// The Orient step shows its settings, the search's results, or the Hollow
// screen (the model's own hollows, hollowing it, drain holes).
enum class OrientView { Settings, Results, Hollow };

// How the Supports step draws the result.
enum class SupportsView { Supports, TipsOnly, Why };

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
        std::string model_path;         // where the model was (every part from that file is relinked)
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
    void setPosition(const fruit::Vec2& mm);  // the middle of its outline, from the plate's centre
    void centreModel();
    void nudge(double dx_mm, double dy_mm);    // the arrow keys
    // Mirror: the selected part as its mirror image along the plate's axis (0
    // X: left to right, 1 Y: front to back, 2 Z: top to bottom); one undoable edit.
    void mirrorModel(int axis);
    // Measure: a point picked on any part (plate coordinates), the points so
    // far, whether they are still on the job to show, and starting again.
    // The measure changes nothing in the job.
    void measureClick(const fruit::Vec3& on_plate);
    void clearMeasure();
    const MeasureState& measured() const { return measure_; }
    bool measureShown() const;
    // Shape tools: Cut, Label and Combine (ShapeTools.h). Each works in the background and its result goes
    // into the job as one undo step; nothing changes when it is cancelled, fails, or the job changed meanwhile.
    ShapeState& shape();  // for the open job: they start again with another job
    void cutApply();                                                         // the selected part, by the plane set
    void labelPick(const fruit::Vec3& on_plate, const fruit::Vec3& normal);  // where the letters go, on the selected part
    void labelApply();
    void combinePick(int part);                                              // the second part (an id)
    void combineApply();                                                     // the selected part and the second
    bool shapeBusy() const { return shape_.running; }
    void shapeCancel();
    void turnQuarter();                        // 90° about the vertical, slid back onto the plate if need be
    void slideOntoPlate();                     // over the edge: the least move that puts it back on
    void setOnPlate(bool on_plate);
    // On plate: the selected part turned so its `region`-th flat face (0-based, largest first: see
    // LayFace.h) lies on the plate; one undoable edit. False when there is no such face or it is
    // already there.
    bool layFace(int region);
    // On plate, as Select Base in other slicers: any surface of the selected part (a flat face or a
    // spot on a curved one) turned to face the plate; one undoable edit. False when it already does.
    bool laySurface(const SurfacePick& s);
    // The note after laying one ("Laid the back face..."), only until the next edit or undo.
    struct LaidNote {
        std::string name;
        SurfacePick::Kind kind = SurfacePick::Kind::Resting;
        int part = -1, revision = -1;
    };
    const LaidNote* laidNote() const;
    // The selected part's surface under a ray (printer coordinates); kind None when it misses.
    SurfacePick surfaceUnder(const fruit::Vec3& origin, const fruit::Vec3& dir);
    // The selected part's surface nearest a point (printer coordinates): scripts.
    SurfacePick surfaceNear(const fruit::Vec3& at);
    // The flat face of the selected part under a ray (printer coordinates), or -1.
    int pickRestingFace(const fruit::Vec3& origin, const fruit::Vec3& dir);
    // A face lit as if the mouse were on it (scripts, for shots); -1 for none.
    void setHoverFace(int region) { hover_face_ = region; }
    int hoverFace() const { return hover_face_; }
    // Or the surface nearest a point (printer coordinates), lit the same way.
    void setHoverPoint(std::optional<fruit::Vec3> at) { hover_point_ = at; }
    const std::optional<fruit::Vec3>& hoverPoint() const { return hover_point_; }
    void scaleToFit();
    void undo();
    void redo();
    // The parts on the plate (see Parts.h): the tools above act on the
    // selected one. Adding loads a model, shows the health dialog, and puts
    // it beside the others (undoable); Open still starts a new job.
    void addPartDialog();                       // through a dialog (⇧⌘O)
    void addPartFrom(const std::string& path);  // a model file
    bool addingPart() const { return adding_part_; }  // the health dialog is for a part being added
    void selectPart(int id);
    void removePart(int id);                    // undoable; never the last one
    // The part under a ray (printer coordinates): its id, or -1.
    int pickPart(const fruit::Vec3& origin, const fruit::Vec3& dir);
    // A tool's change to the parts (Duplicate, Arrange, Split, Mirror…) as
    // one undo step, once any drag is let go (see Document::editParts).
    // False when nothing changed.
    bool editParts(const std::function<void(PartsEdit&)>& fn);
    // The parts tools (PartsTools.h): each is one undo step and makes
    // supports and layers out of date. They report what they did in
    // partsTools().last.
    PartsToolState& partsTools() { return parts_tools_; }
    void duplicateParts();            // copies of the selected part, partsTools().copies of them
    void duplicateParts(int n);       // n copies of the selected part
    // The menu for one part, from a double-click or right-click on it in the
    // Parts list or the view: Duplicate, More copies…, Remove. It opens at
    // (x, y) on screen; the view's overlay takes the request and draws it.
    void openPartMenu(int id, float x, float y);
    int takePartMenu();                  // the part a menu was asked for (-1: none), once
    int partMenuPart() const { return part_menu_part_; }
    float partMenuX() const { return part_menu_x_; }
    float partMenuY() const { return part_menu_y_; }
    void duplicateOnce(int id);          // the menu's Duplicate: one copy of part `id`, beside it
    void arrangeAll();                // every part, partsTools().arrange_gap apart
    void splitSelected();             // the selected part into its shells
    void mergeParts(bool fuse);       // the parts not left out (partsTools().merge_off), joined or fused
    std::vector<int> mergeChosen() const;  // ids of the parts Merge would join
    void mergePick(int id, bool on);
    // The Collisions check (Collisions.h): for the job as it is now, or null
    // while it is being made (or with one part). `collisionGap()` is the
    // least gap, mm; changing it makes the check again.
    const CollisionReport* collisions() const;
    bool checkingCollisions() const;
    double& collisionGap() { return collision_gap_; }
    void finishCollisions();          // scripts and tests: starts the check if it is due and waits for it
    void settleCollisions();          // the same, only when no other work is running (a script's shot)
    void selectCollision(int part);   // from the Collisions tab: selects the part, shows it
    // The handles in the 3D view (Model, and Orient's settings): a drag is
    // previewed as it goes and is one undo step when it ends.
    HandleDrag& handleDrag() { return handle_drag_; }
    Handle& hoverHandle() { return hover_handle_; }
    bool handlesShown() const;     // on this screen, with a model open
    bool placing() const { return doc_ && doc_->dragging(); }
    void beginPlacing(const HandleDrag& d);
    void previewPlacement(const fruit::Placement& p);
    void endPlacing();
    void cancelPlacing();          // Escape: back to where the drag started
    bool& mouseSpent() { return mouse_spent_; }  // after Escape, until the button is let go
    void dragHandleBy(Handle h, double amount, bool snap);  // scripts: one whole drag
    void showShortcuts(bool show) { shortcuts_open_ = show; }
    // Places (not steps: any opens at any time; each says what it needs).
    bool canGoTo(Step s) const { return doc_ != nullptr && s >= Step::Model; }
    Need needFor(Step s) const;  // Need::Nothing: it can do its work now
    void goToStep(Step s);
    // The tools around the 3D view (see Tool): a tool opens its panel and the
    // place behind it; picking the tool whose panel is open closes the panel.
    void openTool(Tool t);
    Tool tool() const { return toolShown(step_, orient_view_ == OrientView::Hollow, model_tool_); }
    bool toolPanelOpen() const { return tool_panel_ && tool() != Tool::None; }
    void closeToolPanel() { tool_panel_ = false; }
    void goToPrepare();                 // from Layers back to the place left last
    int& jobTab() { return job_tab_; }  // the Job panel's tabs: 0 checks, 1 hollows
    bool& printSettingsOpen() { return print_settings_open_; }  // Layers: the print settings unfolded
    // The model with supports for where it is now, from any place: makes
    // them (or makes them again), showing the Supports place.
    void supportsNow();
    // Orient: sides people will see, the search, and the choice.
    void toggleSide(Side s);
    void clearSides();
    bool sidePicked(Side s) const;
    void findOrientation();
    void useOrientation(int option);  // -1 keeps how it sits
    void showOrientSettings() { orient_view_ = OrientView::Settings; }
    OrientView orientView() const { return orient_view_; }
    // The Hollow screen (canvas board "Hollow"). Its plan is made in the
    // background for the current choices; nothing changes what prints until
    // they are applied, and after that every change counts at once.
    void showHollow();
    void setHollowChoices(const HollowChoices& c);
    void applyHollow();
    void addHoleAt(const fruit::Vec3& pos, const fruit::Vec3& normal);  // printer coordinates, by hand
    void removeHole(int index);                                          // one placed by hand
    // Starts the screen's plan unless it is up to date or being made. It
    // only ever replaces a plan being made for earlier choices, never other
    // work: then maintain() starts it once that is done.
    void planHollowPreview();
    bool planningHollow() const;
    // The Hollow screen's view cut in half, through the middle and square
    // to the camera, showing what prints and the hollows inside.
    bool cutAway() const { return cut_away_; }
    void setCutAway(bool on) { cut_away_ = on; }
    // The results' pictures from below, as textures (-1: how it sits now).
    unsigned orientPicture(int option);
    // Supports.
    void generateSupports();
    void setSupportChoices(const SupportChoices& c);
    // Quick support: the lift (1 to 10 mm, the job's) is a placement edit like
    // the raft's (setOnPlate); dragging its slider is one undo step.
    void setLift(double mm);
    void beginLift();
    void previewLift(double mm);
    void endLift();
    // The height correction (0 to 0.5 mm, the job's, used with no raft): the same, beginLift/endLift
    // bracket the slider's drag.
    void setHeightCorrection(double mm);  // the job's print setting
    void resetQuickSupport();  // sliders, raft and lift back to the resin's
    // Scripts: scrolls the open tool's panel to its end or start over the next few frames.
    void scrollToolPanel(bool to_end) { panel_scroll_end_ = to_end, panel_scroll_frames_ = 4; }
    // The scroll still asked for (to_end says which way); counts a frame off.
    bool takePanelScroll(bool* to_end) {
        if (panel_scroll_frames_ <= 0) return false;
        --panel_scroll_frames_;
        *to_end = panel_scroll_end_;
        return true;
    }
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
    // Turns the view to the biggest patch the repair put on the model, seen
    // from outside.
    void showPatch();
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
    // With adaptive layers on: what saving will write, once worked out in the
    // background (`numbers`), or that it is being worked out or could not be.
    struct AdaptiveView {
        const PrintEstimate* numbers = nullptr;
        bool working = false, failed = false;
    };
    AdaptiveView adaptiveView();  // all false when adaptive layers are off or there is nothing to plan
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
    // A resin's own values, as the Resins page edits them (see setPrintValue
    // and withResinValue); a row back to the file's, measured or FruitSlicer's own.
    void setResinPrintValue(const std::string& id, const std::string& key, double value);
    void resetResinPrintRow(const std::string& id, const std::string& row);
    void setResinSupportValue(const std::string& id, const std::string& key, double value);
    void resetResinSupportRow(const std::string& id, const std::string& label);
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
    // Its results (canvas board "ABResults"): each copy scored with its side
    // hidden until revealed, then the two sides compared.
    void openABResults();
    const nlohmann::json& abManifest() const { return ab_manifest_; }  // null: none kept
    const std::vector<fruit::ABCopyResult>& abScores() const { return ab_scores_; }
    int abScoresRevision() const { return ab_scores_rev_; }
    int& abSelected() { return ab_selected_; }  // a copy's id
    bool& abRevealed() { return ab_revealed_; }
    void setABScore(int id, Outcome o, int scar, double removal_s, const std::string& notes);
    const fruit::ABSummary& abSummary() const { return ab_summary_; }
    // Tools (canvas boards "GooInspect" and "GooCompare"): a print file in
    // plain words, and two files compared.
    int& toolsTab() { return tools_tab_; }  // 0: inspect, 1: compare
    struct GooInspected {
        std::string path;
        fruit::GooHeader header;
        GooSummary summary;
        unsigned small_tex = 0, big_tex = 0;  // the stored pictures (0: none)
        std::shared_ptr<const fruit::GooVerifyResult> check;
    };
    const GooInspected* gooInspected() const { return goo_inspected_.get(); }
    void inspectGoo();                                // through a dialog
    void inspectGooFile(const std::string& path);
    void checkGoo();                                  // reads every layer back, as a task
    void chooseGooToCompare(int side);                // through a dialog; 0: A, 1: B
    void setGooToCompare(int side, const std::string& path);
    const std::string& gooToCompare(int side) const { return goo_compare_[side & 1]; }
    std::shared_ptr<const fruit::GooCompareResult> gooComparison() const { return goo_comparison_; }
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
    void openModel(const std::string& path, bool add = false);  // `add`: as another part of the job
    void placeAddedPart();  // the health dialog's choice, for a part being added
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
    Tool model_tool_ = Tool::None;    // the panel picked on Model
    bool tool_panel_ = true;          // the open tool's panel is showing
    Step prepare_step_ = Step::Model;  // where Prepare goes back to
    int job_tab_ = 0;
    bool print_settings_open_ = false;
    std::shared_ptr<Document> doc_, checking_;
    HealthDialog health_;
    bool adding_part_ = false;  // checking_ is a part to add to doc_
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
    // The Hollow screen's last plan started: for this revision, choices and
    // hand-placed holes (cleared when it is cancelled, so it is made again).
    int hollow_tried_revision_ = -1;
    HollowChoices hollow_tried_choices_;
    std::vector<fruit::DrainHole> hollow_tried_manual_;
    int hollow_generation_ = 0;
    std::shared_ptr<const OrientSearch> pictures_for_;
    std::vector<unsigned> pictures_;  // options, then how it sits now
    SupportsView supports_view_ = SupportsView::Supports;
    bool problems_open_ = false;
    bool advanced_open_ = false;
    bool panel_scroll_end_ = false;
    int panel_scroll_frames_ = 0;
    HandleDrag handle_drag_;
    Handle hover_handle_ = Handle::None;
    bool mouse_spent_ = false;
    bool tip_editing_ = false;
    TipTool tip_tool_ = TipTool::Select;
    int new_tip_class_ = 2;
    bool show_zones_ = true;
    int dragging_tip_ = -1;
    MeasureState measure_;
    LaidNote laid_note_;
    int hover_face_ = -1;
    std::optional<fruit::Vec3> hover_point_;
    PartsToolState parts_tools_;
    int part_menu_request_ = -1, part_menu_part_ = -1;
    float part_menu_x_ = 0, part_menu_y_ = 0;
    std::shared_ptr<const CollisionReport> collision_report_;
    std::weak_ptr<const Document> collision_doc_;  // the job it is for (sameJob: an address can repeat)
    int collision_tried_ = -1;                     // the revision whose check was started
    std::weak_ptr<const Document> collision_tried_doc_;
    double collision_tried_gap_ = 0;
    double collision_gap_ = kMinPartGap;
    int collision_generation_ = 0;
    void startCollisionCheck();
    void forgetJobTools();  // another job is open: what the tools kept was for the one before
    ShapeState shape_;
    void runShape(const std::string& job, std::function<ShapeOutcome(const fruit::JobControl&)> work,
                  std::function<bool(const ShapeOutcome&)> install, Tool tool);
    std::shared_ptr<fruit::AabbTree> pick_tree_;
    std::shared_ptr<const fruit::TriangleMesh> pick_mesh_;  // what it was made over, kept while it is used
    const fruit::AabbTree& pickTree();  // over the job's placed mesh, made again when it changes
    std::vector<std::vector<TipEdit>> tip_undo_, tip_redo_;  // edit lists, while editing
    // Puts in a new edit list (undoable) and rebuilds the tips; `select`
    // (printer coordinates) is the tip to select once they are rebuilt.
    void setTipEdits(std::vector<TipEdit> edits, std::optional<fruit::Vec3> select = std::nullopt);
    void rebuildTips(std::optional<fruit::Vec3> select);
    int partAt(const fruit::Vec3& at);  // the part (an id) whose surface is nearest a point
    std::map<std::string, double> advanced_;  // carried over to the next model opened
    std::shared_ptr<const SupportResult> scene_for_;  // the result the tip markers were built from
    void syncCut();
    bool cut_away_ = true;
    std::shared_ptr<const HollowPlan> cut_for_;  // the plan the cap was built from
    fruit::Vec2 cut_toward_;                     // horizontal, from the model towards the camera
    fruit::Vec3 cut_mid_;
    std::vector<Renderer::CapGroup> cut_cap_;
    int cut_key_ = 0;
    std::vector<fruit::TriangleMesh> tip_meshes_;      // one per tip kind
    int scene_key_ = 0;
    std::shared_ptr<const SupportResult> sliced_for_;  // last slicing started (it may have been cancelled)
    int sliced_base_ = -1;                             // its height correction, in layers
    // The adaptive estimate last started: for these layers and settings
    // (once per combination, as with the cut above; one that failed or was cancelled is not started again).
    struct EstimateKey {
        std::shared_ptr<const LayerResult> layers;
        PrintEdits edits;
        std::string sample_path;
        bool operator==(const EstimateKey&) const = default;
    };
    std::optional<EstimateKey> estimate_started_;
    bool estimate_failed_ = false;
    bool startEstimate();  // starts it when one is wanted and nothing is busy
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
    nlohmann::json ab_manifest_;
    std::vector<fruit::ABCopyResult> ab_scores_;
    fruit::ABSummary ab_summary_;
    int ab_selected_ = 0, ab_scores_rev_ = 0;
    bool ab_revealed_ = false;
    int tools_tab_ = 0;
    std::shared_ptr<GooInspected> goo_inspected_;
    std::string goo_compare_[2];
    std::shared_ptr<const fruit::GooCompareResult> goo_comparison_;
    void freeGooPictures();
    void compareGoos();  // once both files are chosen
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
