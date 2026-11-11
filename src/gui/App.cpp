// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "App.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <cctype>
#include <filesystem>
#include <fstream>

#include <SDL3/SDL.h>

#include "core/Parallel.h"
#include "formats/GooExport.h"
#include "formats/GooSetup.h"
#include "mesh/ModelCheck.h"
#include "mesh/ModelLoad.h"
#include "mesh/StlIO.h"
#include "net/testing/MockPrinter.h"
#include "support/SupportSettings.h"

#include "Dialogs.h"
#include "Format.h"
#include "Icons.h"
#include "Log.h"
#include "Measure.h"
#include "Setup.h"
#include "Theme.h"
#include "Widgets.h"
#include "platform/Platform.h"
#include "screens/JobParts.h"
#include "screens/Screens.h"

namespace gui {

namespace {
std::string settingsFile() { return (std::filesystem::path(platform::dataDir()) / "settings.json").string(); }
std::string preferencesFile() { return (std::filesystem::path(platform::dataDir()) / "preferences.json").string(); }
}  // namespace

App::App(SDL_Window* window, AppOptions opts, std::function<void()> wake)
    : window_(window), opts_(opts), jobs_(std::move(wake)) {
    camera_.preset(ViewPreset::Angled);
    if (opts_.deterministic) {
        // Each scripted run starts from an empty scratch folder (never this machine's own).
        const std::filesystem::path scratch = dataFolder();
        std::error_code ec;
        if (scratch.filename() == "fruitslicer-script-data") std::filesystem::remove_all(scratch, ec);
    }
    // Scripted captures start from FruitSlicer's own settings, never this machine's.
    if (!opts_.deterministic) {
        printer_setup_ = loadPrinterSetup(settingsFile());
        recent_ = loadRecentJobs((std::filesystem::path(platform::dataDir()) / "recent.json").string());
        prefs_ = loadPreferences(preferencesFile());
        try {
            resins_ = firstResins(resinsDir(), settingsFile());
        } catch (const std::exception& e) {
            log::error("resins: " + std::string(e.what()));
        }
    }
    ab_record_ = loadABRecord((std::filesystem::path(abDir()) / "test.json").string());
    if (resins_.empty()) {  // scripted runs, or a folder that can't be written
        Resin r;
        r.id = "my-resin";
        r.name = "My resin";
        resins_.push_back(r);
    }
    // New jobs stay with this resin until another is chosen (adding one doesn't change it).
    if (!resinById(printer_setup_.resin)) printer_setup_.resin = resins_.front().id;
    resin_selected_ = newJobResin().id;
    // The first launch starts with setup (Settings can run it again).
    if (!opts_.deterministic && !printer_setup_.setup_done) startSetup();
    fruit::setDefaultThreads(prefs_.cores);
}

App::~App() {
    freePictures();
    freeGooPictures();
    Renderer::freeTexture(layer_tex_);
    Renderer::freeTexture(orient_tex_[0]);
    Renderer::freeTexture(orient_tex_[1]);
}

bool App::initGraphics(std::string* error) {
    gl_info_ = Renderer::glInfo();
    log::info("OpenGL: " + gl_info_);
    return renderer_.init(printer_, error);
}

void App::showError(std::string title, std::string message) {
    error_title_ = std::move(title);
    error_message_ = std::move(message);
    error_open_ = true;
    log::error(error_title_ + ": " + error_message_);
}

void App::frameModel() {
    if (!doc_ || doc_->parts().empty()) {
        camera_.frame({0, 0, 15}, 110);
        return;
    }
    const fruit::BBox3 b = doc_->placedBox();
    const fruit::Vec3 c = b.center();
    const fruit::Vec3 s = b.size();
    // Frame the model and the plate under it, so the lift stays readable.
    const double r = 0.5 * std::sqrt(s.x * s.x + s.y * s.y + (s.z + b.min.z) * (s.z + b.min.z));
    camera_.frame({static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>((b.max.z) / 2)},
                  static_cast<float>(std::max(r, 20.0)));
}

void App::setView(ViewPreset v) {
    camera_.preset(v);
    view_ = v;
    view_set_ = true;
    frameModel();
}

void App::fitView() { frameModel(); }

namespace {
std::string recentFile() { return (std::filesystem::path(platform::dataDir()) / "recent.json").string(); }
bool isJobFile(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == std::string(".") + kJobExtension;
}
}  // namespace

void App::openPath(const std::string& path) {
    leaveJob([this, path] {
        if (isJobFile(path)) openJobFile(path);
        else openModel(path);
    });
}

void App::openModel(const std::string& path, bool add) {
    const std::string name = std::filesystem::path(path).filename().string();
    screen_ = Screen::Job;
    auto result = std::make_shared<std::shared_ptr<Document>>();
    const fruit::PrinterProfile printer = printer_;
    jobs_.start(
        "Opening " + name,
        [path, printer, result](const fruit::JobControl& ctl) { *result = loadDocument(path, printer, ctl); },
        [this, result, name, add](std::exception_ptr err) {
            if (!err) {
                checking_ = *result;
                adding_part_ = add && doc_ != nullptr;
                const fruit::PlateFit& f = checking_->fit();
                health_ = HealthDialog{};
                health_.open = true;
                // Full size first whenever it fits, turned a quarter if need be: the size it was drawn at.
                const bool full = f.fits || f.fits_turned;
                health_.choice = full ? SizeChoice::Full : SizeChoice::Largest;
                health_.custom_percent = full ? 100 : std::round(std::max(f.largest, f.largest_turned) * 100);
                return;
            }
            try {
                std::rethrow_exception(err);
            } catch (const fruit::Cancelled&) {
            } catch (const std::exception& e) {
                showError("Couldn't open " + name, e.what());
            }
        });
}

void App::openDialog() {
    platform::openFileDialog(window_, {{"3D models and FruitSlicer jobs", "stl;obj;3mf;fruitjob"}},
                             [this](const std::string& path) {
                                 if (!path.empty()) openPath(path);
                             });
}

void App::saveJob() {
    if (!doc_) return;
    if (doc_->job_path.empty()) saveJobAs();
    else saveJobTo(doc_->job_path);
}

void App::saveJobAs() {
    if (!doc_) return;
    const char* docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    const std::string start = !doc_->job_path.empty()
                                  ? doc_->job_path
                                  : (std::filesystem::path(docs ? docs : ".") / (doc_->name + ".fruitjob")).string();
    auto then = std::make_shared<std::function<void()>>(std::move(unsaved_.then));
    platform::saveFileDialog(window_, {{"FruitSlicer jobs", kJobExtension}}, start, [this, then](const std::string& path) {
        if (!path.empty()) saveJobTo(path, *then);
    });
}

void App::saveJobTo(const std::string& path, std::function<void()> then) {
    if (!doc_) return;
    auto snap = std::make_shared<const Document>(*doc_);
    const JobStamp stamp = doc_->stamp();
    std::weak_ptr<Document> target = doc_;
    const std::string name = std::filesystem::path(path).filename().string();
    jobs_.start(
        "Writing " + name, [snap, path](const fruit::JobControl&) { gui::saveJob(path, *snap); },
        [this, target, path, stamp, name, then](std::exception_ptr err) {
            if (err) {
                try {
                    std::rethrow_exception(err);
                } catch (const fruit::Cancelled&) {
                } catch (const std::exception& e) {
                    showError("Couldn't save " + name, e.what());
                }
                return;
            }
            const auto d = target.lock();
            if (!d) return;
            d->job_path = path;
            d->saved = stamp;
            log::info("job saved: " + path);
            remember(*d);
            if (then) then();
        });
}

void App::remember(const Document& d) {
    if (d.job_path.empty()) return;
    RecentJob r;
    r.path = d.job_path;
    r.name = d.name;
    const auto& s = d.supports;
    r.state = d.layersFresh() ? "layers cut" : s.fresh(d.revision(), d.supportSettings()) ? "supports ready"
                                                                                                    : "placed";
    if (opts_.deterministic) {
        r.when = "today";
    } else {
        const std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_r(&t, &tm);
        char b[32];
        std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tm);
        r.when = b;
    }
    if (opts_.deterministic) {
        std::erase_if(recent_, [&](const RecentJob& x) { return x.path == r.path; });
        recent_.insert(recent_.begin(), r);
        return;
    }
    try {
        rememberJob(recentFile(), r);
    } catch (const std::exception& e) {
        log::error(std::string("recent jobs not saved: ") + e.what());
    }
    recent_ = loadRecentJobs(recentFile());
}

void App::openJobFile(const std::string& path) {
    const std::string name = std::filesystem::path(path).filename().string();
    auto out = std::make_shared<OpenedJob>();
    const fruit::PrinterProfile printer = printer_;
    jobs_.start(
        "Opening " + name, [path, printer, out](const fruit::JobControl& ctl) { *out = openJob(path, printer, ctl); },
        [this, out, name](std::exception_ptr err) {
            if (err) {
                try {
                    std::rethrow_exception(err);
                } catch (const fruit::Cancelled&) {
                } catch (const std::exception& e) {
                    showError("Couldn't open " + name, e.what());
                }
                return;
            }
            if (out->model_missing) {
                relink_ = RelinkDialog{};
                relink_.open = true;
                relink_.doc = out->doc;
                relink_.model_path = out->model_path;
                return;
            }
            installJob(out->doc);
        });
}

void App::installJob(std::shared_ptr<Document> d) {
    doc_ = std::move(d);
    forgetJobTools();
    checking_.reset();
    advanced_ = doc_->supports.choices.advanced;
    applyPrinterSetup();
    screen_ = Screen::Job;
    step_ = doc_->supports.fresh(doc_->revision(), doc_->supportSettings()) ? Step::Supports : Step::Model;
    orient_view_ = OrientView::Settings;
    advanced_open_ = false;
    frameModel();
    remember(*doc_);
    log::info("job opened: " + doc_->job_path);
}

void App::leaveJob(std::function<void()> then) {
    if (doc_ && doc_->unsaved()) {
        unsaved_ = UnsavedDialog{};
        unsaved_.open = true;
        if (doc_->saved) unsaved_.changes = changesSince(*doc_->saved, doc_->stamp());
        unsaved_.then = std::move(then);
        return;
    }
    if (then) then();
}

void App::answerUnsaved(UnsavedAnswer a) {
    unsaved_.open = false;
    switch (a) {
        case UnsavedAnswer::DontSave: {
            auto then = std::move(unsaved_.then);
            unsaved_ = UnsavedDialog{};
            if (then) then();
            break;
        }
        case UnsavedAnswer::Cancel: unsaved_ = UnsavedDialog{}; break;
        case UnsavedAnswer::Save:
            // Then carries on once saved; a cancelled save dialog drops it.
            if (doc_ && doc_->job_path.empty()) {
                saveJobAs();
            } else if (doc_) {
                auto then = std::move(unsaved_.then);
                saveJobTo(doc_->job_path, std::move(then));
            }
            unsaved_ = UnsavedDialog{};
            break;
    }
}

void App::relinkOpen() {
    if (!relink_.doc) return;
    if (relink_.find) {
        platform::openFileDialog(window_, {{"3D models", "stl;obj;3mf"}}, [this](const std::string& path) {
            if (!path.empty()) relinkTo(path);
        });
        return;
    }
    auto d = relink_.doc;
    relink_ = RelinkDialog{};
    installJob(d);
}

void App::relinkTo(const std::string& model_path) {
    if (!relink_.doc) return;
    auto d = relink_.doc;
    const std::string was = relink_.model_path;
    relink_ = RelinkDialog{};
    try {
        // Every part from that file; one it changed under takes the new file, and its supports go out of date.
        // Parts made in the app have no file, and keep what was made.
        const int changed = relinkParts(*d, was, model_path, fruit::loadModel(model_path));
        d->clearHistory();  // undo can't go back to the old file
        if (changed > 0) log::info("relinked to a changed model: " + model_path);
    } catch (const std::exception& e) {
        showError("Couldn't read that model", std::string(e.what()) + " The job keeps its own copy.");
    }
    installJob(d);
}

void App::relinkCancel() { relink_ = RelinkDialog{}; }

void App::requestQuit() {
    leaveJob([this] { quit_ = true; });
}

fruit::Placement App::healthPlacement() const {
    if (!checking_) return {};
    return sizeChoicePlacement(checking_->part().mesh(), checking_->printer, health_.choice, health_.custom_percent);
}

void App::chooseSize(SizeChoice c, double custom_percent) {
    health_.choice = c;
    if (c == SizeChoice::Custom && custom_percent > 0) health_.custom_percent = custom_percent;
}

void App::placeChecked() {
    if (!checking_) return;
    if (adding_part_ && doc_) {
        placeAddedPart();
        return;
    }
    adding_part_ = false;
    checking_->place(healthPlacement());
    doc_ = std::move(checking_);
    forgetJobTools();
    checking_.reset();
    doc_->supports.choices.advanced = advanced_;  // your support settings stay with you
    applyResin(*doc_, newJobResin());  // its print settings and measured values
    applyPrinterSetup();
    doc_->saved = doc_->stamp();  // nothing to save until something changes
    health_.open = false;
    advanced_open_ = false;
    step_ = Step::Model;
    orient_view_ = OrientView::Settings;
    screen_ = Screen::Job;
    log::info("placed " + doc_->name + " at " + std::to_string(doc_->placement().scale * 100) + " %");
    frameModel();
}

void App::cancelChecked() {
    checking_.reset();
    health_.open = false;
    adding_part_ = false;
}

void App::setScalePercent(double percent) {
    if (!doc_ || percent <= 0) return;
    fruit::Placement p = doc_->placement();
    p.scale = percent / 100.0;
    doc_->edit(p);
}

void App::setRotation(const fruit::Vec3& degrees) {
    if (!doc_) return;
    fruit::Placement p = doc_->placement();
    p.rotation_deg = degrees;
    doc_->edit(p);
}

void App::setPosition(const fruit::Vec2& mm) {
    if (!doc_ || placing()) return;
    fruit::Placement p = doc_->placement();
    p.offset = mm;
    doc_->edit(p);
}

void App::centreModel() { setPosition({0, 0}); }

void App::nudge(double dx_mm, double dy_mm) {
    if (!doc_) return;
    const fruit::Vec2 o = doc_->placement().offset;
    // To the nearest tenth, so nudges from a typed position stay tidy.
    setPosition({snapTo(o.x + dx_mm, 0.1), snapTo(o.y + dy_mm, 0.1)});
}

void App::turnQuarter() {
    if (!doc_ || placing()) return;
    doc_->edit(turnedQuarter(doc_->part().mesh(), doc_->placement(), doc_->printer));
}

void App::slideOntoPlate() {
    if (!doc_ || placing()) return;
    doc_->edit(slidOnto(doc_->part().mesh(), doc_->placement(), doc_->printer));
}

void App::mirrorModel(int axis) {
    if (!doc_ || placing()) return;
    doc_->mirror(axis);
}

void App::measureClick(const fruit::Vec3& on_plate) {
    if (!doc_) return;
    // The point is kept on the part it was picked on, whichever that is.
    const Part* part = doc_->findPart(partAt(on_plate));
    if (!part) return;
    if (!measureStillOn(measure_, *doc_)) measure_.clear();
    measure_.click(part->transform().inverse().apply(on_plate), part->id(), part->geometryId());
}

void App::clearMeasure() { measure_.clear(); }

bool App::measureShown() const { return doc_ && !measure_.empty() && measureStillOn(measure_, *doc_); }

bool App::handlesShown() const {
    // With Move or Rotate open: the arrows and rings that place it.
    return doc_ && screen_ == Screen::Job &&
           ((step_ == Step::Model && model_tool_ == Tool::Move) ||
            (step_ == Step::Orient && orient_view_ == OrientView::Settings));
}

void App::beginPlacing(const HandleDrag& d) {
    if (!doc_ || !d.active()) return;
    handle_drag_ = d;
    doc_->beginDrag();
}

void App::previewPlacement(const fruit::Placement& p) {
    if (placing()) doc_->preview(p);
}

void App::endPlacing() {
    if (placing()) doc_->endDrag();
    handle_drag_ = HandleDrag{};
}

void App::cancelPlacing() {
    if (placing()) {
        doc_->cancelDrag();
        mouse_spent_ = true;  // the button still held turns nothing until it is let go
    }
    handle_drag_ = HandleDrag{};
}

void App::dragHandleBy(Handle h, double amount, bool snap) {
    if (!doc_ || h == Handle::None) return;
    HandleDrag d;
    d.handle = h;
    d.start = doc_->placement();
    d.frame = handleFrame(doc_->part().box());
    d.now = d.start;
    beginPlacing(d);
    // In a few moves, as a hand would, then let go: one undo step.
    for (int i = 1; i <= 4; ++i) previewPlacement(draggedBy(d.start, h, amount * i / 4, snap));
    endPlacing();
}

void App::setOnPlate(bool on_plate) {
    if (!doc_ || doc_->placement().on_plate == on_plate) return;
    fruit::Placement p = doc_->placement();
    p.on_plate = on_plate;
    doc_->edit(p);
    frameModel();
}

void App::scaleToFit() {
    if (!doc_) return;
    const fruit::PlateFit& f = doc_->fit();
    fruit::Placement p = doc_->placement();
    // Largest at the current rotation, or a quarter turn more if that is
    // larger. Both are absolute scales (1.0 = 100 %).
    if (f.largest_turned > f.largest) {
        p.rotation_deg = turnAbout(p.rotation_deg, 2, 90);
        p.scale = f.largest_turned;
    } else {
        p.scale = f.largest;
    }
    p.offset = {0, 0};  // the largest that fits only fits in the middle
    doc_->edit(p);
    frameModel();
}

Need App::needFor(Step s) const { return doc_ ? gui::needFor(*doc_, s) : Need::Fit; }

void App::goToStep(Step s) {
    if (!canGoTo(s)) return;
    if (placing()) endPlacing();
    if (s != step_) {
        advanced_open_ = false;
        tip_editing_ = false;
        tool_panel_ = true;  // a place opens with its panel showing
    }
    step_ = s;
    if (!inLayersMode(s)) prepare_step_ = s;
    screen_ = Screen::Job;
    if (s == Step::Orient &&
        (orient_view_ == OrientView::Hollow || !(doc_ && doc_->orient().resultsFresh(doc_->part().revision()))))
        orient_view_ = OrientView::Settings;
}

void App::openTool(Tool t) {
    if (!doc_) return;
    if (t != Tool::None && t == tool() && tool_panel_ && !inLayersMode(step_)) {
        tool_panel_ = false;  // its panel was open: close it, stay where it is
        return;
    }
    switch (t) {
        case Tool::Rotate: goToStep(Step::Orient); break;
        case Tool::Supports: goToStep(Step::Supports); break;
        case Tool::Hollow: showHollow(); break;
        default:
            goToStep(Step::Model);
            model_tool_ = t;
            break;
    }
    tool_panel_ = t != Tool::None;
}

void App::goToPrepare() { goToStep(prepare_step_); }

void App::supportsNow() {
    if (!doc_) return;
    goToStep(Step::Supports);
    if (needFor(Step::Supports) == Need::Nothing &&
        !doc_->supports.fresh(doc_->revision(), doc_->supportSettings()))
        generateSupports();
}

namespace {

// A printer-frame direction in the model's own frame (rotation only).
fruit::Vec3 toModel(const fruit::Placement& p, const fruit::Vec3& d) {
    const fruit::Transform r = fruit::placementRotation(p);
    return fruit::normalized({r.m[0][0] * d.x + r.m[1][0] * d.y + r.m[2][0] * d.z,
                              r.m[0][1] * d.x + r.m[1][1] * d.y + r.m[2][1] * d.z,
                              r.m[0][2] * d.x + r.m[1][2] * d.y + r.m[2][2] * d.z});
}

}  // namespace

bool App::sidePicked(Side s) const {
    if (!doc_) return false;
    const fruit::Vec3 d = sideDirection(s);
    for (const fruit::Vec3& v : visibleInPrinter(doc_->orient().visible, doc_->placement()))
        if (fruit::dot(v, d) > 0.9) return true;
    return false;
}

void App::toggleSide(Side s) {
    if (!doc_) return;
    OrientState& o = doc_->orient();
    const fruit::Vec3 d = sideDirection(s);
    const auto in_printer = visibleInPrinter(o.visible, doc_->placement());
    bool removed = false;
    for (std::size_t i = in_printer.size(); i-- > 0;)
        if (fruit::dot(in_printer[i], d) > 0.9) {
            o.visible.erase(o.visible.begin() + static_cast<std::ptrdiff_t>(i));
            removed = true;
        }
    if (!removed) o.visible.push_back(toModel(doc_->placement(), d));  // sticks to the model when it turns
    ++o.settings;
    o.decided = false;
}

void App::clearSides() {
    if (!doc_ || doc_->orient().visible.empty()) return;
    doc_->orient().visible.clear();
    ++doc_->orient().settings;
    doc_->orient().decided = false;
}

void App::maintain() {
    if (placing() && !handlesShown()) endPlacing();  // its handles went (a search finished, another screen)
    if (placing()) return;  // nothing starts while a handle is held: the placement changes every frame
    // The Collisions check (several parts) gives way: the work below cancels it, and it starts again after.
    const auto busy = [this] { return jobs_.busy() && !checkingCollisions(); };
    if (doc_ && screen_ == Screen::Job && step_ == Step::Layers && !busy()) {
        // Cut the layers for the current supports, once per supports result
        // and height correction.
        const auto& sup = doc_->supports.result;
        const int base = doc_->placement().heightCorrectionLayers(kLayerHeight);
        if (sup && !doc_->layersFresh() && (sliced_for_ != sup || sliced_base_ != base) &&
            doc_->supports.fresh(doc_->revision(), doc_->supportSettings())) {
            sliced_for_ = sup;
            sliced_base_ = base;
            auto snap = std::make_shared<const Document>(*doc_);
            auto out = std::make_shared<LayerResult>();
            const double raft = doc_->placement().on_plate ? 0.0 : sup->tree.raft_top;
            std::weak_ptr<Document> target = doc_;
            jobs_.start(
                "Cutting layers",
                [snap, sup, raft, out](const fruit::JobControl& ctl) {
                    *out = sliceLayers(*snap, sup->mesh, raft, ctl, sup->hollow);
                },
                [this, target, sup, out](std::exception_ptr err) {
                    const auto d = target.lock();
                    if (!d) return;
                    if (err) {
                        try {
                            std::rethrow_exception(err);
                        } catch (const fruit::Cancelled&) {
                        } catch (const std::exception& e) {
                            showError("Couldn't cut the layers", e.what());
                        }
                        return;
                    }
                    d->layers.result = std::make_shared<const LayerResult>(std::move(*out));
                    d->layers.for_supports = sup;
                    d->layers.current = -1;
                    log::info("layers: " + std::to_string(d->layers.result->layers->size()) + " in " +
                              std::to_string(d->layers.result->seconds) + " s");
                });
        }
        return;
    }
    if (!doc_ || screen_ != Screen::Job || step_ != Step::Orient || busy()) return;
    if (orient_view_ == OrientView::Hollow) {
        planHollowPreview();
        if (busy()) return;
    }
    // How the selected part sits (part revisions are never shared between parts).
    OrientState& o = doc_->orient();
    const int part = doc_->selectedId(), rev = doc_->part().revision(), settings = o.settings;
    if (o.nowFresh(rev) || (scored_revision_ == rev && scored_settings_ == settings)) return;
    scored_revision_ = rev;
    scored_settings_ = settings;
    // The worker gets its own copy: the job can keep changing meanwhile.
    auto snap = std::make_shared<const Document>(*doc_);
    auto out = std::make_shared<OrientMetrics>();
    std::weak_ptr<Document> target = doc_;
    jobs_.start(
        "Scoring how it sits",
        [snap, out](const fruit::JobControl& ctl) { *out = measure(*snap, snap->orient().visible, ctl); },
        [this, target, out, part, rev, settings](std::exception_ptr err) {
            const auto d = target.lock();
            if (!d) return;
            if (err) {
                try {
                    std::rethrow_exception(err);
                } catch (const fruit::Cancelled&) {
                } catch (const std::exception& e) {
                    showError("Couldn't score the model", e.what());
                }
                return;
            }
            OrientState& o = d->orientOf(part);
            o.now = *out;
            o.now_revision = rev;
            o.now_settings = settings;
        });
}

void App::findOrientation() {
    if (!doc_ || !doc_->fit().fits_centred) return;  // where it stands doesn't matter to the search
    auto snap = std::make_shared<const Document>(*doc_);
    auto out = std::make_shared<OrientSearch>();
    const int part = doc_->selectedId(), rev = doc_->part().revision(), settings = doc_->orient().settings;
    std::weak_ptr<Document> target = doc_;
    jobs_.start(
        "Finding the best orientation",
        [snap, out](const fruit::JobControl& ctl) { *out = search(*snap, snap->orient().visible, ctl); },
        [this, target, out, part, rev, settings](std::exception_ptr err) {
            const auto d = target.lock();
            if (!d) return;
            if (err) {
                try {
                    std::rethrow_exception(err);
                } catch (const fruit::Cancelled&) {
                } catch (const std::exception& e) {
                    showError("Couldn't search for a better orientation", e.what());
                }
                return;
            }
            auto res = std::make_shared<const OrientSearch>(std::move(*out));
            OrientState& o = d->orientOf(part);
            o.results = res;
            o.results_revision = rev;
            o.results_settings = settings;
            if (!o.nowFresh(rev)) {
                o.now = res->now.m;
                o.now_revision = rev;
                o.now_settings = settings;
            }
            if (d == doc_ && step_ == Step::Orient && d->selectedId() == part) orient_view_ = OrientView::Results;
            log::info("orientation search: " + std::to_string(res->options.size()) + " better options in " +
                      std::to_string(res->seconds) + " s");
        });
}

void App::useOrientation(int option) {
    if (!doc_) return;
    OrientState& o = doc_->orient();
    if (option >= 0 && o.results && option < static_cast<int>(o.results->options.size())) {
        const OrientOption& op = o.results->options[static_cast<std::size_t>(option)];
        fruit::Placement p = doc_->placement();
        p.rotation_deg = op.rotation_deg;
        doc_->edit(slidOnto(doc_->part().mesh(), p, doc_->printer));  // kept where it is, if it fits there
        // Its numbers came from the same planner: no need to score it again.
        o.now = op.m;
        o.now_revision = doc_->part().revision();
        o.now_settings = o.settings;
        frameModel();
    }
    o.decided = true;
    orient_view_ = OrientView::Settings;
}

void App::showHollow() {
    if (!doc_) return;
    goToStep(Step::Orient);
    orient_view_ = OrientView::Hollow;
    tool_panel_ = true;
    planHollowPreview();
}

bool App::planningHollow() const { return jobs_.busy() && jobs_.progress().name == "Finding hollows"; }

void App::planHollowPreview() {
    if (!doc_ || !doc_->fitsCentred()) return;
    const HollowState& hs = doc_->hollow;
    const int rev = doc_->revision();
    if (hs.planFresh(rev)) return;
    if (hollow_tried_revision_ == rev && hollow_tried_choices_ == hs.choices && hollow_tried_manual_ == hs.manual)
        return;  // being made, or it failed
    if (jobs_.busy() && !planningHollow() && !checkingCollisions()) return;
    hollow_tried_revision_ = rev;
    hollow_tried_choices_ = hs.choices;
    hollow_tried_manual_ = hs.manual;
    const int generation = ++hollow_generation_;
    auto snap = std::make_shared<const Document>(*doc_);
    auto out = std::make_shared<HollowPlan>();
    const HollowChoices c = hs.choices;
    const std::vector<fruit::DrainHole> manual = hs.manual;
    std::weak_ptr<Document> target = doc_;
    jobs_.start(
        "Finding hollows",
        [snap, out, c, manual](const fruit::JobControl& ctl) { *out = planHollow(*snap, c, manual, kLayerHeight, ctl); },
        [this, target, out, rev, generation](std::exception_ptr err) {
            const auto d = target.lock();
            if (!d) return;
            if (err) {
                try {
                    std::rethrow_exception(err);
                } catch (const fruit::Cancelled&) {
                    if (generation == hollow_generation_) hollow_tried_revision_ = -1;  // made again when wanted
                } catch (const std::exception& e) {
                    showError("Couldn't look for hollows", e.what());
                }
                return;
            }
            d->hollow.plan = std::make_shared<const HollowPlan>(std::move(*out));
            d->hollow.plan_revision = rev;
            log::info("hollow: " + std::to_string(d->hollow.plan->cavities().cavities.size()) + " hollows, " +
                      std::to_string(d->hollow.plan->holes.size()) + " holes in " +
                      std::to_string(d->hollow.plan->seconds) + " s");
        });
}

void App::setHollowChoices(const HollowChoices& c) {
    if (!doc_ || doc_->hollow.choices == c) return;
    // One diameter for every hole, those placed by hand too.
    if (c.hole_diameter != doc_->hollow.choices.hole_diameter)
        for (fruit::DrainHole& h : doc_->hollow.manual) h.diameter = c.hole_diameter;
    doc_->hollow.choices = c;
    if (doc_->hollow.applied) ++doc_->hollow.settings;
    planHollowPreview();
}

void App::applyHollow() {
    if (!doc_) return;
    doc_->hollow.applied = true;
    ++doc_->hollow.settings;
}

void App::addHoleAt(const fruit::Vec3& pos, const fruit::Vec3& normal) {
    if (!doc_) return;
    const int part = partAt(pos);
    const Part* on = doc_->findPart(part);
    if (!on) return;
    const fruit::Transform to_model = on->transform().inverse();  // the part it goes into
    fruit::DrainHole h;
    h.at = to_model.apply(pos);
    h.axis = fruit::normalized(to_model.applyLinear(-normal));  // into the model
    h.diameter = doc_->hollow.choices.hole_diameter;
    h.part = part;
    doc_->hollow.manual.push_back(h);
    if (doc_->hollow.applied) ++doc_->hollow.settings;
    planHollowPreview();
}

void App::removeHole(int index) {
    if (!doc_ || index < 0 || index >= static_cast<int>(doc_->hollow.manual.size())) return;
    doc_->hollow.manual.erase(doc_->hollow.manual.begin() + index);
    if (doc_->hollow.applied) ++doc_->hollow.settings;
    planHollowPreview();
}

void App::generateSupports() {
    if (!doc_ || !doc_->fits()) return;
    advanced_open_ = false;  // the run shows in the 3D view
    auto snap = std::make_shared<const Document>(*doc_);
    auto out = std::make_shared<SupportResult>();
    const int rev = doc_->revision(), settings = doc_->supportSettings();
    const SupportChoices choices = doc_->supports.choices;
    const std::vector<TipEdit> edits = doc_->supports.edits;  // hand edits stay: they apply again
    std::weak_ptr<Document> target = doc_;
    jobs_.start(
        "Generating supports",
        [snap, out, choices, edits](const fruit::JobControl& ctl) {
            *out = generate(*snap, choices, snap->visibleInPrinter(), ctl, edits);
        },
        [this, target, out, rev, settings](std::exception_ptr err) {
            const auto d = target.lock();
            if (!d) return;
            if (err) {
                try {
                    std::rethrow_exception(err);
                } catch (const fruit::Cancelled&) {
                } catch (const std::exception& e) {
                    showError("Couldn't generate supports", e.what());
                }
                return;
            }
            SupportState& s = d->supports;
            s.result = std::make_shared<const SupportResult>(std::move(*out));
            s.result_revision = rev;
            s.result_settings = settings;
            s.selected_tip = -1;
            log::info("supports: " + std::to_string(s.result->tips) + " tips, " + std::to_string(s.result->pillars) +
                      " pillars in " + std::to_string(s.result->seconds) + " s");
        });
}

void App::setSupportChoices(const SupportChoices& c) {
    if (doc_) doc_->supports.choices = c;
}

namespace {
fruit::Placement withLift(fruit::Placement p, double mm) {
    p.elevation = std::clamp(std::round(mm), 1.0, 10.0);
    return p;
}
}  // namespace

void App::setLift(double mm) {
    if (doc_) doc_->edit(withLift(doc_->placement(), mm));
}

void App::beginLift() {
    if (doc_) doc_->beginDrag();
}

void App::previewLift(double mm) {
    if (doc_ && doc_->dragging()) doc_->preview(withLift(doc_->placement(), mm));
}

void App::endLift() {
    if (doc_ && doc_->dragging()) doc_->endDrag();
}

namespace {
fruit::Placement withHeightCorrection(fruit::Placement p, double mm) {
    p.height_correction = std::clamp(std::round(mm * 100) / 100, 0.0, fruit::kMaxHeightCorrection);
    return p;
}
}  // namespace

void App::setHeightCorrection(double mm) {
    if (doc_) doc_->edit(withHeightCorrection(doc_->placement(), mm));
}

void App::previewHeightCorrection(double mm) {
    if (doc_ && doc_->dragging()) doc_->preview(withHeightCorrection(doc_->placement(), mm));
}

void App::resetQuickSupport() {
    if (!doc_) return;
    SupportChoices c = doc_->supports.choices;
    resetQuick(c);
    setSupportChoices(c);
    fruit::Placement p = doc_->placement();
    p.on_plate = false;
    p.elevation = fruit::Placement{}.elevation;
    p.height_correction = fruit::Placement{}.height_correction;
    doc_->edit(p);
}

void App::setAdvanced(const std::string& key, double value) {
    if (!doc_) return;
    SupportChoices c = doc_->supports.choices;
    for (const AdvancedSetting& s : advancedSettings())
        for (const AdvancedField& f : s.fields) {
            if (key != f.key) continue;
            const double v = std::clamp(value, f.lo, f.hi);
            if (key == kUsualTipKey) {
                c.usual_tip = static_cast<int>(std::lround(v));
            } else {
                // The job's own value (FruitSlicer's, with its resin's measured
                // ones) is not stored, so the row reads as it did before.
                SupportChoices none;
                none.resin = c.resin;
                none.profile = c.profile;
                none.hold = c.hold;  // the quick sliders' values are part of what the row reads as
                none.amount = c.amount;
                none.sturdy = c.sturdy;
                const double own = f.get(supportConfig(doc_->placement(), none), treeConfig(none));
                if (std::abs(v - own) < 1e-9) c.advanced.erase(key);
                else c.advanced[key] = v;
            }
            advanced_ = c.advanced;
            setSupportChoices(c);
            return;
        }
    log::error("unknown support setting " + key);
}

void App::resetAdvanced() {
    advanced_.clear();
    if (!doc_) return;
    SupportChoices c = doc_->supports.choices;
    c.advanced.clear();
    c.usual_tip = usualTipFor(c.profile);  // the resin's measured one, if it has one
    setSupportChoices(c);
}

void App::applyAdvanced() {
    advanced_open_ = false;
    if (doc_ && !doc_->supports.fresh(doc_->revision(), doc_->supportSettings())) generateSupports();
}

void App::editTips(bool on) {
    tip_editing_ = on && doc_ && doc_->supports.fresh(doc_->revision(), doc_->supportSettings());
    if (tip_editing_) {
        step_ = Step::Supports;
        advanced_open_ = false;
    }
    dragging_tip_ = -1;
    tip_undo_.clear();
    tip_redo_.clear();
}

void App::setTipEdits(std::vector<TipEdit> edits, std::optional<fruit::Vec3> select) {
    if (!doc_ || !doc_->supports.result || edits == doc_->supports.edits) return;
    tip_undo_.push_back(doc_->supports.edits);
    tip_redo_.clear();
    doc_->supports.edits = std::move(edits);
    rebuildTips(select);
}

void App::undoTipEdit(int index) {
    if (!doc_) return;
    std::vector<TipEdit> edits = doc_->supports.edits;
    if (index < 0 || index >= static_cast<int>(edits.size())) return;
    edits.erase(edits.begin() + index);
    setTipEdits(std::move(edits));
}

void App::undoAllTipEdits() {
    if (doc_) setTipEdits({});
}

bool App::undoTipChange() {
    if (!doc_ || !doc_->supports.result || tip_undo_.empty()) return false;
    tip_redo_.push_back(std::move(doc_->supports.edits));
    doc_->supports.edits = std::move(tip_undo_.back());
    tip_undo_.pop_back();
    rebuildTips(std::nullopt);
    return true;
}

bool App::redoTipChange() {
    if (!doc_ || !doc_->supports.result || tip_redo_.empty()) return false;
    tip_undo_.push_back(std::move(doc_->supports.edits));
    doc_->supports.edits = std::move(tip_redo_.back());
    tip_redo_.pop_back();
    rebuildTips(std::nullopt);
    return true;
}

void App::rebuildTips(std::optional<fruit::Vec3> select) {
    if (!doc_ || !doc_->supports.result) return;
    const auto prev = doc_->supports.result;
    auto snap = std::make_shared<const Document>(*doc_);
    auto out = std::make_shared<SupportResult>();
    const SupportChoices choices = prev->choices;
    const std::vector<TipEdit> edits = doc_->supports.edits;
    // Edits made since the tips were planned go on top: only the tree is built
    // again. Taking back one the planner worked around would leave its hole or
    // zone behind, so then the tips are planned again, around the rest.
    const bool replan = !planStillFits(prev->plan_edits, edits);
    const int rev = doc_->supports.result_revision, settings = doc_->supports.result_settings;
    std::weak_ptr<Document> target = doc_;
    jobs_.start(
        replan ? "Planning the tips again" : "Updating the tips",
        [snap, prev, out, choices, edits, replan](const fruit::JobControl& ctl) {
            if (replan) {
                *out = generate(*snap, choices, snap->visibleInPrinter(), ctl, edits);
            } else {
                SupportPlan plan{prev->planned, prev->plan_warnings, prev->plan_edits, prev->hollow};
                *out = finishWithEdits(*snap, choices, std::move(plan), edits, ctl);
            }
        },
        [this, target, out, rev, settings, select](std::exception_ptr err) {
            const auto d = target.lock();
            if (!d) return;
            if (err) {
                try {
                    std::rethrow_exception(err);
                } catch (const fruit::Cancelled&) {
                } catch (const std::exception& e) {
                    showError("Couldn't update the supports", e.what());
                }
                return;
            }
            SupportState& s = d->supports;
            s.result = std::make_shared<const SupportResult>(std::move(*out));
            s.result_revision = rev;
            s.result_settings = settings;
            // The tips are a new list: find the one to select by where it is.
            s.selected_tip = -1;
            if (select)
                for (std::size_t i = 0; i < s.result->points.size(); ++i) {
                    const fruit::Vec3 q = s.result->points[i].pos;
                    if (std::abs(q.x - select->x) + std::abs(q.y - select->y) + std::abs(q.z - select->z) < 1e-3) {
                        s.selected_tip = static_cast<int>(i);
                        break;
                    }
                }
        });
}

void App::addTipAt(const fruit::Vec3& pos, const fruit::Vec3& normal) {
    if (!doc_) return;
    const int part = partAt(pos);
    const Part* on = doc_->findPart(part);
    if (!on) return;
    const fruit::Transform m = on->transform().inverse();  // kept on the part it is on
    TipEdit e;
    e.kind = TipEdit::Kind::Add;
    e.part = part;
    e.at = m.apply(pos);
    e.normal = m.applyLinear(normal);
    // FruitSlicer's own sizes (a resin profile may have measured fewer).
    e.diameter = fruit::SupportConfig{}.tip_classes[static_cast<std::size_t>(std::clamp(new_tip_class_, 0, 3))].diameter;
    std::vector<TipEdit> edits = doc_->supports.edits;
    edits.push_back(e);
    setTipEdits(std::move(edits), pos);  // the new tip is selected, for [ and ]
}

void App::addZoneAt(const fruit::Vec3& pos) {
    if (!doc_) return;
    const int part = partAt(pos);
    const Part* on = doc_->findPart(part);
    if (!on) return;
    TipEdit e;
    e.kind = TipEdit::Kind::Zone;
    e.part = part;
    e.at = on->transform().inverse().apply(pos);
    e.radius = kZoneRadius;
    std::vector<TipEdit> edits = doc_->supports.edits;
    edits.push_back(e);
    setTipEdits(std::move(edits));
}

namespace {
const fruit::SupportPoint* tipAt(const std::shared_ptr<Document>& d, int index) {
    if (!d || !d->supports.result || index < 0 || index >= static_cast<int>(d->supports.result->points.size()))
        return nullptr;
    return &d->supports.result->points[static_cast<std::size_t>(index)];
}
}  // namespace

void App::moveTip(int index, const fruit::Vec3& to, const fruit::Vec3& normal) {
    const fruit::SupportPoint* p = tipAt(doc_, index);
    if (!p) return;
    std::vector<TipEdit> edits = doc_->supports.edits;
    if (recordMove(edits, *p, to, normal, doc_->frames(), partAt(p->pos))) setTipEdits(std::move(edits), to);
}

void App::deleteTip(int index) {
    const fruit::SupportPoint* p = tipAt(doc_, index);
    if (!p) return;
    std::vector<TipEdit> edits = doc_->supports.edits;
    if (recordDelete(edits, *p, doc_->frames(), partAt(p->pos))) setTipEdits(std::move(edits));
}

void App::resizeTip(int index, double diameter) {
    const fruit::SupportPoint* p = tipAt(doc_, index);
    if (!p) return;
    std::vector<TipEdit> edits = doc_->supports.edits;
    if (recordResize(edits, *p, diameter, doc_->frames(), partAt(p->pos))) setTipEdits(std::move(edits), p->pos);
}

void App::pinTip(int index) {
    const fruit::SupportPoint* p = tipAt(doc_, index);
    if (!p) return;
    std::vector<TipEdit> edits = doc_->supports.edits;
    if (recordPin(edits, *p, doc_->frames(), partAt(p->pos))) setTipEdits(std::move(edits), p->pos);
}

bool App::pickModel(const fruit::Vec3& origin, const fruit::Vec3& dir, fruit::Vec3* pos, fruit::Vec3* normal) {
    if (!doc_) return false;
    const auto hit = pickSurface(pickTree(), origin, dir);
    if (!hit) return false;
    *pos = hit->point;
    *normal = hit->normal;
    return true;
}

void App::selectTip(int index) {
    if (!doc_ || !doc_->supports.result) return;
    const int n = static_cast<int>(doc_->supports.result->points.size());
    doc_->supports.selected_tip = index >= 0 && index < n ? index : -1;
}

void App::focusOn(const fruit::Vec3& where) {
    camera_.frame({static_cast<float>(where.x), static_cast<float>(where.y), static_cast<float>(where.z)}, 12);
    view_set_ = false;
}

void App::showPatch() {
    if (!doc_) return;
    // The selected part's biggest patch, or the first patched part's.
    const Part* part = &doc_->part();
    for (const Part& p : doc_->parts())
        if (part->check().repair.patch_faces == 0 && p.check().repair.patch_faces > 0) part = &p;
    if (part->check().repair.patch_faces == 0) return;
    const fruit::RepairReport& r = part->check().repair;
    const fruit::Vec3 at = part->transform().apply(r.biggest_patch_at);
    const fruit::Vec3 facing = part->transform().applyLinear(r.biggest_patch_facing);
    const double across = r.biggest_patch_across * part->placement().scale;
    camera_.frame({static_cast<float>(at.x), static_cast<float>(at.y), static_cast<float>(at.z)},
                  static_cast<float>(std::max(5.0, 0.75 * across)));
    camera_.lookFrom({static_cast<float>(facing.x), static_cast<float>(facing.y), static_cast<float>(facing.z)});
    view_set_ = false;
}

namespace {

// A small octahedron per tip: cheap, and readable at any zoom.
void addMarker(fruit::TriangleMesh& m, const fruit::Vec3& c, double r) {
    const auto base = static_cast<int32_t>(m.vertices.size());
    const fruit::Vec3 d[6] = {{r, 0, 0}, {-r, 0, 0}, {0, r, 0}, {0, -r, 0}, {0, 0, r}, {0, 0, -r}};
    for (const auto& v : d) m.vertices.push_back(c + v);
    const int f[8][3] = {{0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4}, {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5}};
    for (const auto& t : f) m.faces.push_back({base + t[0], base + t[1], base + t[2]});
}

}  // namespace

void App::syncCut() {
    std::shared_ptr<const HollowPlan> plan;
    // Not while holes are placed by hand: a click must land on what is seen.
    if (doc_ && screen_ == Screen::Job && step_ == Step::Orient && orient_view_ == OrientView::Hollow && cut_away_ &&
        doc_->hollow.choices.auto_holes && doc_->hollow.plan && doc_->hollow.plan_revision == doc_->revision())
        plan = doc_->hollow.plan;
    if (!plan) {
        renderer_.clearCut();
        cut_for_.reset();
        return;
    }
    if (plan != cut_for_) {
        const fruit::BBox3 b = doc_->placedBox();
        cut_mid_ = (b.min + b.max) * 0.5;
    }
    const V3 eye = camera_.eye();
    fruit::Vec2 toward{eye.x - cut_mid_.x, eye.y - cut_mid_.y};
    toward = fruit::norm(toward) > 1e-9 ? fruit::normalized(toward) : fruit::Vec2{0, -1};
    // While the view is being turned the cut stays put (a big model's
    // section takes a moment); it follows once the mouse is let go.
    const bool turning = ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
                         ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    const bool turned = fruit::dot(toward, cut_toward_) < std::cos(1.0 * 3.14159265358979 / 180.0);
    if (plan != cut_for_ || (turned && !turning)) {
        cut_for_ = plan;
        cut_toward_ = toward;
        const fruit::Vec2 along{-toward.y, toward.x};
        const fruit::Vec2 at{cut_mid_.x, cut_mid_.y};
        const HollowSection s = hollowSection(*plan, at, along);
        const fruit::BBox3 b = doc_->placedBox();
        const double rim = std::clamp(0.004 * fruit::norm(b.max - b.min), 0.1, 0.6);
        auto quads = [&](const std::vector<SectionBand>& bands, double grow, unsigned rgb) {
            Renderer::CapGroup g;
            g.rgb = rgb;
            g.triangles.reserve(bands.size() * 18);
            for (const SectionBand& band : bands) {
                const fruit::Vec2 a = at + along * (band.t0 - grow), c = at + along * (band.t1 + grow);
                const float z0 = static_cast<float>(band.layer * s.layer_height - grow);
                const float z1 = static_cast<float>((band.layer + 1) * s.layer_height + grow);
                const float ax = static_cast<float>(a.x), ay = static_cast<float>(a.y);
                const float cx = static_cast<float>(c.x), cy = static_cast<float>(c.y);
                g.triangles.insert(g.triangles.end(),
                                   {ax, ay, z0, cx, cy, z0, cx, cy, z1, ax, ay, z0, cx, cy, z1, ax, ay, z1});
            }
            return g;
        };
        // Holes show through the dark of the whole; what prints over it;
        // then the hollows, with a rim.
        cut_cap_ = {quads(s.outline, 0, 0x0E1014), quads(s.prints, 0, 0xAEB6C4), quads(s.hollow, rim, 0xF2B84B),
                    quads(s.hollow, 0, 0x2A2412)};
        ++cut_key_;
    }
    // Kept: the far side, where dot(toward, p - mid) <= 0.
    const float plane[4] = {static_cast<float>(-cut_toward_.x), static_cast<float>(-cut_toward_.y), 0.0f,
                            static_cast<float>(cut_toward_.x * cut_mid_.x + cut_toward_.y * cut_mid_.y)};
    renderer_.setCut(plane, cut_cap_, cut_key_);
}

void App::syncScene() {
    renderer_.setOverhangTint(0);
    // Every part. With several, the ones not selected are a shade darker.
    // The Model step shows where the repair patched each mesh; where it is
    // placed, a part is red while it doesn't fit there.
    const bool placing_place = screen_ == Screen::Job &&
                               (step_ == Step::Model || (step_ == Step::Orient && orient_view_ == OrientView::Settings));
    std::vector<Renderer::ModelDraw> models;
    if (doc_)
        for (const Part& p : doc_->parts()) {
            Renderer::ModelDraw m;
            m.mesh = &p.mesh();
            m.geometry = p.geometryId();
            m.transform = p.transform();
            m.rgb = doc_->partCount() > 1 && p.id() != doc_->selectedId() ? 0x6F7682u : 0xAEB6C4u;
            const std::size_t faces = p.check().triangles;
            const std::size_t patches = screen_ == Screen::Job && step_ == Step::Model ? p.check().repair.patch_faces : 0;
            m.tint_first = faces - std::min(patches, faces);
            m.tint_count = patches;
            m.tint_rgb = 0xF2B84B;
            if (placing_place && !p.fit().fits) {
                m.tint_first = 0;
                m.tint_count = faces;
                m.tint_rgb = 0xD86A6A;
            }
            models.push_back(m);
        }
    renderer_.setModels(models);
    syncCut();
    if (!doc_ || screen_ != Screen::Job || step_ != Step::Supports) {
        renderer_.clearExtras();
        return;
    }
    const SupportState& s = doc_->supports;
    if (!s.result) {
        renderer_.clearExtras();
        renderer_.setOverhangTint(supportConfig(doc_->placement(), s.choices).critical_angle_deg);
        return;
    }
    if (scene_for_ != s.result) {
        tip_meshes_.assign(5, fruit::TriangleMesh{});
        for (const auto& p : s.result->points)
            addMarker(tip_meshes_[static_cast<std::size_t>(p.category)], p.pos, 0.3 + 0.5 * p.tip_diameter);
        scene_for_ = s.result;
        ++scene_key_;
    }
    std::vector<const fruit::TriangleMesh*> meshes{&s.result->mesh};
    for (const auto& t : tip_meshes_) meshes.push_back(&t);
    renderer_.setExtras(meshes, scene_key_);
    const bool fresh = s.fresh(doc_->revision(), doc_->supportSettings());
    const bool why = supports_view_ == SupportsView::Why;
    // While editing, only the tips: the tree and raft would hide them.
    renderer_.setExtraStyle(0, fresh ? (why ? 0x6B7380u : 0xE2E6EDu) : 0x3A404Bu,
                            supports_view_ != SupportsView::TipsOnly && !tip_editing_);
    // Island, overhang, extra hold, steadying, by hand (yours: the accent while editing).
    static const unsigned kWhy[5] = {0xFF8F3F, 0x5B9BD5, 0xB48EF0, 0x5BD69A, 0xF2F4F7};
    for (std::size_t i = 0; i < 5; ++i) renderer_.setExtraStyle(i + 1, why ? kWhy[i] : 0xF2F4F7, fresh);
    if (tip_editing_) renderer_.setExtraStyle(5, 0xFF8F3F, true);
}

void App::chooseSample() {
    platform::openFileDialog(window_, {{"Print files from SatelLite", "goo"}}, [this](const std::string& path) {
        if (!path.empty()) useSample(path);
    });
}

void App::useSample(const std::string& path) {
    if (!doc_) return;
    try {
        doc_->print.settings.sample = loadSample(path);
        doc_->print.settings.sample_path = path;
        log::info("print settings from " + path);
    } catch (const std::exception& e) {
        showError("Couldn't read that print file", e.what());
    }
}

void App::setExposures(double normal_s, double bottom_s) {
    if (!doc_) return;
    doc_->print.settings.exposure = std::max(0.0, normal_s);
    doc_->print.settings.bottom_exposure = std::max(0.0, bottom_s);
}

std::shared_ptr<const PrintPlan> App::printPlan() {
    if (!doc_ || !doc_->layers.result) return nullptr;
    const auto& lr = doc_->layers.result;
    const double raft =
        doc_->placement().on_plate || !doc_->supports.result ? 0.0 : doc_->supports.result->tree.raft_top;
    PrintSettings s = doc_->print.settings;
    s.flip_x = printer_setup_.orientation_confirmed && printer_setup_.flip_x;
    s.flip_y = printer_setup_.orientation_confirmed && printer_setup_.flip_y;
    const bool same = plan_ && plan_layers_ == lr && plan_raft_ == raft && plan_settings_.sample == s.sample &&
                      static_cast<const PrintEdits&>(plan_settings_) == static_cast<const PrintEdits&>(s) &&
                      plan_settings_.flip_x == s.flip_x && plan_settings_.flip_y == s.flip_y;
    if (!same) {
        plan_ = std::make_shared<const PrintPlan>(planPrint(*doc_, *lr, raft, s));
        plan_layers_ = lr;
        plan_settings_ = s;
        plan_raft_ = raft;
    }
    return plan_;
}

std::string App::printBlocker() {
    switch (needFor(Step::Print)) {
        case Need::Nothing: break;
        case Need::Fit: return "Make it fit the plate first.";
        case Need::Supports: return "Generate supports first; the Layers place then cuts the layers.";
        case Need::FreshSupports: return "The supports are out of date: generate them again first.";
        case Need::Layers: return "Open Layers first: it cuts the layers.";
    }
    if (!doc_->supports.result->validation.floating.empty())
        return "Parts start in mid-air: add tips with the Supports tool.";
    const auto plan = printPlan();
    if (!plan->fits) return "The model and supports reach past the printer's build volume.";
    if (!plan->ready) return plan->why_not;
    if (jobs_.busy()) return "Wait for the current task to finish.";
    return "";
}

void App::savePrint() {
    if (!printBlocker().empty()) return;
    // Asked once per launch until the orientation check has confirmed it.
    if (!orientation_asked_ && !printer_setup_.orientation_confirmed) {
        orientation_ask_ = true;
        return;
    }
    const std::string name = doc_->name + ".goo";
    if (send_wifi_) {
        // Written and checked here first, then sent from the dialog.
        const auto dir = std::filesystem::temp_directory_path() / "FruitSlicer outgoing";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        savePrintTo((dir / name).string(), "", true);
        return;
    }
    if (drive_choice_ >= 0 && drive_choice_ < static_cast<int>(drives_.size())) {
        const platform::Volume& v = drives_[static_cast<std::size_t>(drive_choice_)];
        savePrintTo((std::filesystem::path(v.path) / name).string(), v.name);
        return;
    }
    const char* docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    std::error_code ec;
    const std::filesystem::path folder = !prefs_.goo_folder.empty() && std::filesystem::is_directory(prefs_.goo_folder, ec)
                                             ? std::filesystem::path(prefs_.goo_folder)
                                             : std::filesystem::path(docs ? docs : ".");
    platform::saveFileDialog(window_, {{"Print files", "goo"}}, (folder / name).string(), [this](const std::string& path) {
        if (path.empty()) return;
        Preferences p = prefs_;  // the next save starts here
        p.goo_folder = std::filesystem::path(path).parent_path().string();
        setPreferences(p);
        savePrintTo(path, "");
    });
}

void App::savePrintAfterAsking() {
    orientation_asked_ = true;
    savePrint();
}

void App::savePrintTo(const std::string& path, const std::string& drive, bool to_send) {
    if (const std::string why = printBlocker(); !why.empty()) {
        showError("Couldn't save the print file", why);
        return;
    }
    const auto plan = printPlan();
    orientation_asked_ = true;
    orientation_ask_ = false;
    auto snap = std::make_shared<const Document>(*doc_);
    auto layers = doc_->layers.result;
    auto supports = doc_->supports.result;
    auto out = std::make_shared<SaveResult>();
    std::weak_ptr<Document> target = doc_;
    const std::string name = std::filesystem::path(path).filename().string();
    jobs_.start(
        "Saving " + name,
        [snap, layers, supports, plan, path, drive, out](const fruit::JobControl& ctl) {
            *out = gui::savePrint(*snap, *layers, supports->mesh, *plan, path, !drive.empty(), ctl);
        },
        [this, target, layers, drive, out, name, to_send](std::exception_ptr err) {
            const auto d = target.lock();
            if (!d) return;
            if (err) {
                try {
                    std::rethrow_exception(err);
                } catch (const fruit::Cancelled&) {
                } catch (const std::exception& e) {
                    showError("Couldn't save " + name, e.what());
                }
                return;
            }
            if (to_send) {
                send_ = SendDialog{};
                send_.open = true;
                send_.file = std::make_shared<const SaveResult>(std::move(*out));
                if (!send_.file->good) {
                    send_.problem = "The file did not read back as it was written, so it was not sent. Save it again.";
                    return;
                }
                findPrinters();
                return;
            }
            d->print.saved = std::make_shared<const SaveResult>(std::move(*out));
            d->print.saved_from = layers;
            d->print.drive = drive;
            log::info("saved " + d->print.saved->path + ": " + std::to_string(d->print.saved->written.layers) +
                      " layers, read back " + (d->print.saved->check.checksums_ok ? "ok" : "FAILED"));
        });
}

void App::findPrinters() {
    if (!printer_hosts_loaded_ && !opts_.deterministic) {
        printer_hosts_ = loadPrinterAddresses((std::filesystem::path(platform::dataDir()) / "printers.json").string());
        printer_hosts_loaded_ = true;
    }
    send_.problem.clear();
    auto found = std::make_shared<std::vector<fruit::net::PrinterInfo>>();
    const auto hosts = printer_hosts_;
    const NetPorts ports = net_ports_;
    jobs_.start(
        "Looking for printers",
        [found, hosts, ports](const fruit::JobControl& ctl) { *found = gui::findPrinters(hosts, ports, ctl); },
        [this, found](std::exception_ptr err) {
            send_.searched = true;
            if (err) {
                try {
                    std::rethrow_exception(err);
                } catch (const fruit::Cancelled&) {
                } catch (const std::exception& e) {
                    send_.problem = std::string("Couldn't search the network: ") + e.what();
                }
                return;
            }
            // Keep the one chosen, if it answered again.
            const std::string was = send_.chosen >= 0 && send_.chosen < static_cast<int>(send_.printers.size())
                                        ? send_.printers[static_cast<std::size_t>(send_.chosen)].mainboardId
                                        : std::string();
            send_.printers = *found;
            send_.chosen = -1;
            for (std::size_t i = 0; i < send_.printers.size(); ++i)
                if (send_.printers[i].mainboardId == was) send_.chosen = static_cast<int>(i);
            if (send_.chosen < 0 && send_.printers.size() == 1) send_.chosen = 0;
            log::info("printers found: " + std::to_string(send_.printers.size()));
        });
}

void App::addPrinter(const std::string& host_in) {
    std::string host = host_in;
    host.erase(0, host.find_first_not_of(" \t"));
    host.erase(host.find_last_not_of(" \t") + 1);
    if (host.empty()) return;
    send_.problem.clear();
    auto info = std::make_shared<fruit::net::PrinterInfo>();
    const NetPorts ports = net_ports_;
    jobs_.start(
        "Asking " + host, [info, host, ports](const fruit::JobControl&) { *info = probePrinter(host, ports); },
        [this, info, host](std::exception_ptr err) {
            if (err) {
                try {
                    std::rethrow_exception(err);
                } catch (const fruit::Cancelled&) {
                } catch (const std::exception& e) {
                    send_.problem = e.what();
                }
                return;
            }
            int at = -1;
            for (std::size_t i = 0; i < send_.printers.size(); ++i)
                if (send_.printers[i].mainboardId == info->mainboardId) at = static_cast<int>(i);
            if (at < 0) {
                send_.printers.push_back(*info);
                at = static_cast<int>(send_.printers.size()) - 1;
            }
            send_.chosen = at;
            send_.address[0] = 0;
            if (std::find(printer_hosts_.begin(), printer_hosts_.end(), host) == printer_hosts_.end()) {
                printer_hosts_.push_back(host);
                if (!opts_.deterministic) {
                    try {
                        savePrinterAddresses((std::filesystem::path(platform::dataDir()) / "printers.json").string(),
                                             printer_hosts_);
                    } catch (const std::exception& e) {
                        log::error(std::string("printer addresses not saved: ") + e.what());
                    }
                }
            }
        });
}

void App::sendToPrinter() {
    if (!send_.file || !send_.file->good || send_.chosen < 0 ||
        send_.chosen >= static_cast<int>(send_.printers.size()))
        return;
    send_.problem.clear();
    send_.sent.reset();
    const fruit::net::PrinterInfo printer = send_.printers[static_cast<std::size_t>(send_.chosen)];
    const std::string path = send_.file->path;
    const bool start = send_.start_print;
    const NetPorts ports = net_ports_;
    auto out = std::make_shared<SendResult>();
    send_.send_started = static_cast<double>(SDL_GetTicks()) / 1000.0;
    jobs_.start(
        "Sending " + std::filesystem::path(path).filename().string(),
        [printer, path, start, ports, out](const fruit::JobControl& ctl) {
            *out = gui::sendToPrinter(printer, path, start, ports, ctl);
        },
        [this, out](std::exception_ptr err) {
            send_.send_started = -1;
            if (err) {
                try {
                    std::rethrow_exception(err);
                } catch (const fruit::Cancelled&) {
                    send_.problem = "Sending was cancelled. Nothing was left on the printer.";
                } catch (const std::exception& e) {
                    send_.problem = std::string("Couldn't send it: ") + e.what();
                }
                return;
            }
            send_.sent = std::make_shared<const SendResult>(std::move(*out));
            log::info("sent " + send_.sent->upload.remoteName + " to " + send_.sent->printer +
                      (send_.sent->started ? ", printing" : ""));
        });
}

void App::closeSend() {
    if (jobs_.busy()) jobs_.cancel();
    send_.open = false;
}

void App::useMockPrinter(bool silent) {
    printer_hosts_ = {"127.0.0.1"};
    printer_hosts_loaded_ = true;
    net_ports_.broadcast = false;  // scripts never ask the real network
    if (silent) {
        mock_printer_.reset();
        net_ports_.udp = net_ports_.control = 9;  // the discard port: nothing answers
        return;
    }
    auto mock = std::make_shared<fruit::net::testing::MockPrinter>();
    net_ports_.udp = mock->udpPort();
    net_ports_.control = mock->controlPort();
    mock_printer_ = mock;
}

void App::setPrinterSetup(const PrinterSetup& s) {
    printer_setup_ = s;
    if (!opts_.deterministic) {
        try {
            savePrinterSetup(settingsFile(), s);
        } catch (const std::exception& e) {
            showError("Couldn't save your printer settings", e.what());
        }
    }
    applyPrinterSetup();
}

void App::applyPrinterSetup() {
    if (!doc_) return;
    // Extra hold away from the hinge only once the orientation is confirmed:
    // a mirrored image would put it on the wrong side.
    SupportChoices c = doc_->supports.choices;
    c.hinge = printer_setup_.orientation_confirmed ? printer_setup_.hinge : std::nullopt;
    if (!(c == doc_->supports.choices)) setSupportChoices(c);
}

// ---- Settings, About and first-run setup ----

std::string App::dataFolder() const {
    // Scripted runs never touch this computer's own folder.
    return opts_.deterministic ? (std::filesystem::temp_directory_path() / "fruitslicer-script-data").string()
                               : platform::dataDir();
}

void App::setPreferences(const Preferences& p) {
    if (p == prefs_) return;
    prefs_ = p;
    fruit::setDefaultThreads(p.cores);
    if (!opts_.deterministic) {
        try {
            savePreferences(preferencesFile(), p);
        } catch (const std::exception& e) {
            showError("Couldn't save your settings", e.what());
        }
    }
}

int App::viewSamples() const { return opts_.deterministic || !prefs_.smooth_view ? 0 : opts_.msaa_samples; }

void App::revealLog() { platform::revealFile(log::path()); }

void App::openSourcePage() {
    if (!opts_.deterministic) SDL_OpenURL("https://github.com/anyosag/FruitSlicer");
}

void App::startSetup() {
    setup_step_ = 0;
    // The print settings step shows what new jobs start with now.
    const Resin& r = newJobResin();
    const PrintEdits& e = r.print;
    setup_typed_ = r.sample_path.empty() && e.exposure > 0;
    typed_defaults_ = {e.exposure, e.bottom_exposure, e.bottom_layers >= 0 ? e.bottom_layers : printer_.bottom_layers,
                       e.transition_layers >= 0 ? e.transition_layers : printer_.transition_layers};
    screen_ = Screen::Setup;
}

void App::finishSetup(Screen then) {
    PrinterSetup s = printer_setup_;
    s.setup_done = true;
    setPrinterSetup(s);
    screen_ = then;
}

void App::chooseDefaultSample() {
    platform::openFileDialog(window_, {{"Print files from SatelLite", "goo"}}, [this](const std::string& path) {
        if (!path.empty()) useDefaultSample(path);
    });
}

void App::useDefaultSample(const std::string& goo) {
    replaceResinSampleFile(newJobResin().id, goo);
    if (!newJobResin().sample_path.empty()) setup_typed_ = false;
}

void App::useTypedDefaults() {
    Resin r = newJobResin();
    r.sample_path.clear();
    r.sample_name.clear();
    r.print.exposure = std::max(0.0, typed_defaults_.exposure);
    r.print.bottom_exposure = std::max(0.0, typed_defaults_.bottom_exposure);
    r.print.bottom_layers = typed_defaults_.bottom_layers;
    r.print.transition_layers = typed_defaults_.transition_layers;
    storeResin(r);
}

// ---- Resins ----

std::string App::resinsDir() const { return (std::filesystem::path(dataFolder()) / "resins").string(); }

const Resin* App::resinById(const std::string& id) const {
    for (const Resin& r : resins_)
        if (r.id == id) return &r;
    return nullptr;
}

const Resin& App::newJobResin() const {
    const Resin* r = resinById(printer_setup_.resin);
    return r ? *r : resins_.front();
}

void App::storeResin(const Resin& r) {
    bool found = false;
    for (Resin& x : resins_)
        if (x.id == r.id) {
            x = r;
            found = true;
        }
    if (!found) resins_.push_back(r);
    std::sort(resins_.begin(), resins_.end(), resinOrder);
    if (!opts_.deterministic) {
        try {
            saveResin(resinsDir(), r);
        } catch (const std::exception& e) {
            showError("Couldn't save the resin", e.what());
        }
    }
}

void App::applyResin(Document& d, const Resin& r) {
    d.print.settings = printSettingsFor(r);
    SupportChoices c = d.supports.choices;
    c.resin = r.name;
    c.profile = r.profile;
    c.usual_tip = usualTipFor(r.profile);  // the usual tip the resin measured
    d.supports.choices = c;
}

void App::setNewJobResin(const std::string& id) {
    if (!resinById(id)) return;
    PrinterSetup s = printer_setup_;
    s.resin = id;
    setPrinterSetup(s);
}

void App::useResinForJob(const std::string& id) {
    resin_menu_ = false;
    const Resin* r = resinById(id);
    if (!doc_ || !r) return;
    applyResin(*doc_, *r);
    // How it stands is scored with the resin's values: score again.
    for (const Part& part : doc_->parts()) {
        doc_->orientOf(part.id()).now_revision = -1;
        doc_->orientOf(part.id()).results_revision = -1;
    }
    log::info("resin for this job: " + r->name);
}

std::string App::keepHeader(const std::string& goo) {
    return keepSampleHeader(goo, (std::filesystem::path(dataFolder()) / "print settings").string());
}

void App::newResinFromGoo() {
    platform::openFileDialog(window_, {{"Print files from SatelLite", "goo"}}, [this](const std::string& path) {
        if (!path.empty()) newResinFromGooFile(path);
    });
}

void App::newResinFromGooFile(const std::string& goo) {
    try {
        Resin r;
        r.sample_path = keepHeader(goo);
        r.sample_name = std::filesystem::path(goo).filename().string();
        r.name = freeResinName(resins_, std::filesystem::path(goo).stem().string());
        r.id = newResinId(resins_, r.name);
        storeResin(r);
        resin_selected_ = r.id;
    } catch (const std::exception& e) {
        showError("Couldn't use that print file", e.what());
    }
}

void App::importResin() {
    platform::openFileDialog(window_, {{"Resin profiles", "json"}}, [this](const std::string& path) {
        if (!path.empty()) importResinFile(path);
    });
}

void App::importResinFile(const std::string& path) {
    try {
        const Resin r = resinFromProfileFile(path, resins_);
        storeResin(r);
        resin_selected_ = r.id;
    } catch (const std::exception& e) {
        showError("Couldn't import that profile", e.what());
    }
}

void App::duplicateResin(const std::string& id) {
    const Resin* from = resinById(id);
    if (!from) return;
    Resin r = *from;
    r.name = freeResinName(resins_, from->name);
    r.id = newResinId(resins_, r.name);
    r.plates.clear();  // the plates printed were for the other one
    storeResin(r);
    resin_selected_ = r.id;
}

void App::renameResin(const std::string& id, const std::string& name) {
    const Resin* from = resinById(id);
    if (!from || name.empty() || name == from->name) return;
    Resin r = *from;
    r.name = freeResinName(resins_, name);
    storeResin(r);
}

void App::setResinPrintValue(const std::string& id, const std::string& key, double value) {
    const Resin* from = resinById(id);
    if (!from) return;
    Resin r = *from;
    const auto sample = resinSample(r);
    setPrintValue(r, sample.get(), printer_, key, value);
    if (!(r == *from)) storeResin(r);
}

void App::resetResinPrintRow(const std::string& id, const std::string& row) {
    const Resin* from = resinById(id);
    if (!from) return;
    Resin r = *from;
    resetPrintRow(r.print, row);
    if (!(r == *from)) storeResin(r);
}

void App::setResinSupportValue(const std::string& id, const std::string& key, double value) {
    const Resin* from = resinById(id);
    if (!from) return;
    Resin r = *from;
    r.profile = withResinValue(r.profile, key, value);
    if (!(r == *from)) storeResin(r);
}

void App::resetResinSupportRow(const std::string& id, const std::string& label) {
    const Resin* from = resinById(id);
    const AdvancedSetting* s = advancedRow(label);
    if (!from || !s) return;
    Resin r = *from;
    r.profile = withoutResinRow(r.profile, *s);
    if (!(r == *from)) storeResin(r);
}

void App::deleteResin(const std::string& which) {
    const std::string id = which;  // `which` may be resin_to_delete_ or a resin's own id
    resin_to_delete_.clear();
    if (resins_.size() <= 1 || !resinById(id)) return;  // one always stays
    std::erase_if(resins_, [&](const Resin& r) { return r.id == id; });
    if (!opts_.deterministic) removeResin(resinsDir(), id);
    std::error_code ec;
    std::filesystem::remove_all(platesDir(id), ec);  // its plates' manifests and scores
    if (printer_setup_.resin == id) setNewJobResin(resins_.front().id);
    if (resin_selected_ == id) resin_selected_ = newJobResin().id;
}

void App::replaceResinSample(const std::string& id) {
    platform::openFileDialog(window_, {{"Print files from SatelLite", "goo"}}, [this, id](const std::string& path) {
        if (!path.empty()) replaceResinSampleFile(id, path);
    });
}

void App::replaceResinSampleFile(const std::string& id, const std::string& goo) {
    const Resin* from = resinById(id);
    if (!from) return;
    try {
        Resin r = *from;
        r.sample_path = keepHeader(goo);
        r.sample_name = std::filesystem::path(goo).filename().string();
        // Edits were made against the old file; the density is the resin's own.
        PrintEdits kept;
        kept.density = r.print.density;
        r.print = kept;
        resin_samples_.erase(r.sample_path);  // the same name may hold another file now
        storeResin(r);
        log::info("print settings for " + r.name + " from " + goo);
    } catch (const std::exception& e) {
        showError("Couldn't use that print file", e.what());
    }
}

// ---- Resin test plates ----

std::string App::platesDir(const std::string& resin_id) const {
    return (std::filesystem::path(dataFolder()) / "plates" / resin_id).string();
}

PlateFiles App::keptPlate(const Resin& r, const std::string& kind) const {
    PlateFiles f;
    if (const auto it = r.plates.find(kind); it != r.plates.end()) f = it->second;
    const std::string base = (std::filesystem::path(platesDir(r.id)) / kind).string();
    f.manifest = base + ".json";
    f.results = base + ".results.csv";
    return f;
}

void App::notePlate(const std::string& resin_id, const std::string& kind, const PlateFiles& f) {
    const Resin* found = resinById(resin_id);
    if (!found) return;  // deleted meanwhile
    Resin r = *found;
    PlateFiles noted = f;
    noted.manifest.clear();  // these follow from the resin's id
    noted.results.clear();
    r.plates[kind] = noted;
    storeResin(r);
}

bool App::onRemovableDrive(const std::string& path) const {
    for (const platform::Volume& v : platform::removableVolumes()) {
        const std::string root = !v.path.empty() && v.path.back() == '/' ? v.path : v.path + "/";
        if (path.rfind(root, 0) == 0) return true;
    }
    return false;
}

void App::openPlate(const std::string& kind) {
    if (!isPlateKind(kind)) return;
    plate_kind_ = kind;
    plate_selected_ = 1;
    screen_ = Screen::Plate;
}

const fruit::CalibrationPlate& App::plate() {
    auto& p = plates_[plate_kind_];
    if (!p) p = std::make_shared<const fruit::CalibrationPlate>(makePlate(plate_kind_, printer_));
    return *p;
}

void App::savePlate(bool goo) {
    const char* docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    std::error_code ec;
    // A print file starts where the last one went.
    const std::filesystem::path folder = goo && !prefs_.goo_folder.empty() &&
                                                 std::filesystem::is_directory(prefs_.goo_folder, ec)
                                             ? std::filesystem::path(prefs_.goo_folder)
                                             : std::filesystem::path(docs ? docs : ".");
    const std::string name = plate_kind_ + (goo ? ".goo" : ".stl");
    platform::saveFileDialog(window_, {{goo ? "Print files" : "STL", goo ? "goo" : "stl"}}, (folder / name).string(),
                             [this, goo](const std::string& path) {
                                 if (path.empty()) return;
                                 if (goo) {
                                     Preferences p = prefs_;
                                     p.goo_folder = std::filesystem::path(path).parent_path().string();
                                     setPreferences(p);
                                 }
                                 savePlateTo(path);
                             });
}

void App::savePlateTo(const std::string& path) {
    const std::string kind = plate_kind_;
    plate();  // made once per kind
    const std::shared_ptr<const fruit::CalibrationPlate> made = plates_[kind];
    const Resin& resin = newJobResin();
    const std::string resin_id = resin.id;
    PlateFiles files = keptPlate(resin, kind);
    files.file = path;
    // Once the plate file is written: its manifest, and a blank score sheet
    // (or this same plate's scores so far), kept with the resin.
    auto keep = [this, made, files, resin_id, kind]() mutable {
        try {
            keepPlate(*made, files);
            notePlate(resin_id, kind, files);
            log::info("saved the " + kind + " plate to " + files.file);
        } catch (const std::exception& e) {
            showError("Couldn't keep the plate's score sheet", e.what());
        }
    };
    if (std::filesystem::path(path).extension() != ".goo") {
        try {
            fruit::writeStl(path, made->mesh);
        } catch (const std::exception& e) {
            showError("Couldn't save the plate", e.what());
            return;
        }
        keep();
        return;
    }
    // A print file with the resin's own print settings, and the image
    // orientation fix once the check has confirmed it.
    PrintSettings ps = printSettingsFor(resin);
    ps.flip_x = printer_setup_.orientation_confirmed && printer_setup_.flip_x;
    ps.flip_y = printer_setup_.orientation_confirmed && printer_setup_.flip_y;
    const fruit::GooPrintRequest req =
        printRequest(ps, ps.sample ? fruit::gooLayerHeight(*ps.sample) : printer_.layer_height);
    std::shared_ptr<const fruit::GooPrintSetup> setup;
    try {
        setup = std::make_shared<const fruit::GooPrintSetup>(fruit::buildGooSetup(printer_, req));
    } catch (const std::exception& e) {
        showError("The resin has no print settings yet",
                  "Add them on the Resins page, or save the STL and slice it in SatelLite. (" + std::string(e.what()) +
                      ")");
        return;
    }
    // Tips cured with the bottom or transition exposure would hold more than
    // they do in a model, and the fit would trust them too much.
    {
        const auto& h = setup->header.print;
        const double first = fruit::minRaftThickness(h.bottom_layers, h.transition_layers,
                                                     fruit::gooLayerHeight(setup->header));
        const double tips_from = made->settings.value("elevation", 0.0) - made->settings.value("head_length", 0.0);
        if (first > tips_from - 1e-9) {
            showError("The resin's first layers reach the plate's tips",
                      "Its bottom and transition layers come to " + strf("%.2f mm", first) + ", and the tips start at " +
                          strf("%.1f mm", tips_from) +
                          ", so they'd be cured longer than in a model. Use fewer bottom or transition layers.");
            return;
        }
    }
    const bool removable = onRemovableDrive(path);
    auto out = std::make_shared<SaveResult>();
    std::string what = plateTitle(kind);
    what[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(what[0])));
    jobs_.start(
        "Saving the " + what,
        [made, setup, path, removable, out](const fruit::JobControl& ctl) {
            *out = saveMeshPrint(made->mesh, *setup, path, removable, ctl);
        },
        [this, out, keep](std::exception_ptr err) mutable {
            if (err) {
                try {
                    std::rethrow_exception(err);
                } catch (const fruit::Cancelled&) {
                } catch (const std::exception& e) {
                    showError("Couldn't save the plate", e.what());
                }
                return;
            }
            if (!out->good) {
                std::error_code ec;
                std::filesystem::remove(out->path, ec);
                showError("The plate's print file didn't read back as it was written",
                          "It was removed, so it can't be printed by mistake. Save it again, or try another drive.");
                return;
            }
            keep();
        });
}

void App::openScores(const std::string& kind) {
    if (!isPlateKind(kind)) return;
    plate_kind_ = kind;
    scores_.reset();
    ++scores_revision_;
    screen_ = Screen::Score;
    const Resin& resin = newJobResin();
    if (!resin.plates.count(kind)) return;  // the page offers to save the plate first
    scores_resin_ = resin.id;
    try {
        scores_ = std::make_shared<PlateScores>(loadScores(keptPlate(resin, kind)));
        score_selected_ = nextToScore(*scores_, 0);  // the first not scored yet
    } catch (const std::exception& e) {
        showError("Couldn't read the scores", e.what());
    }
}

void App::setScore(int id, Outcome o, int scar, const std::string& notes) {
    const Resin* resin = scores_ ? resinById(scores_resin_) : nullptr;
    if (!resin) return;
    PlateScores next = *scores_;
    bool found = false;
    for (auto& c : next.coupons) {
        if (c.id != id) continue;
        found = true;
        c.recorded = o != Outcome::None;
        c.ok = o == Outcome::Held;
        c.partial = o == Outcome::Partly;
        c.scar = o == Outcome::Held || o == Outcome::Partly ? std::clamp(scar, 0, 5) : 0;  // nothing left to mark
        c.notes = notes;
    }
    if (!found) return;
    PlateFiles f = keptPlate(*resin, plate_kind_);
    try {
        saveScores(f, next);
    } catch (const std::exception& e) {
        showError("Couldn't save the score", e.what());
        return;
    }
    *scores_ = std::move(next);
    ++scores_revision_;
    notePlate(scores_resin_, plate_kind_, f);
}

void App::importScores() {
    platform::openFileDialog(window_, {{"Score sheets", "csv"}}, [this](const std::string& path) {
        if (!path.empty()) importScoresFile(path);
    });
}

void App::importScoresFile(const std::string& csv) {
    try {
        const PlateScores s = readScoreSheet(csv);
        const Resin& resin = newJobResin();
        const std::string resin_id = resin.id;
        PlateFiles f = keptPlate(resin, s.kind);
        // The plate the sheet came with ("tips.stl.results.csv": "tips.stl").
        f.file = csv.substr(0, csv.size() - std::string(".results.csv").size());
        keepScores(s, f);
        notePlate(resin_id, s.kind, f);
        openScores(s.kind);
        log::info("imported " + std::to_string(s.scored()) + " scores for the " + s.kind + " plate from " + csv);
    } catch (const std::exception& e) {
        showError("Couldn't read that score sheet", e.what());
    }
}

// ---- Fit ----

void App::openFit() {
    screen_ = Screen::Fit;
    fit_.reset();
    const Resin& r = newJobResin();
    fit_resin_ = r.id;
    std::vector<fruit::PlateResults> plates;
    for (const char* kind : kPlateKinds) {
        if (!r.plates.count(kind)) continue;
        const PlateFiles f = keptPlate(r, kind);
        try {
            plates.push_back(fruit::loadPlateResults(f.manifest, f.results));
        } catch (const std::exception& e) {
            showError("Couldn't read the " + std::string(kind) + " plate's scores", e.what());
        }
    }
    fruit::FitOptions o;
    o.resin = r.name;
    const PrintSettings ps = printSettingsFor(r);
    const fruit::GooHeader h = fruit::printHeader(printer_, printRequest(ps, printer_.layer_height));
    o.layer_height = fruit::gooLayerHeight(h);
    o.exposure_s = h.print.exposure;
    fit_ = std::make_shared<const FitSummary>(summarizeFit(plates, o, r));
}

nlohmann::json App::fittedProfile() const {
    const Resin* r = resinById(fit_resin_);
    if (!fit_ || !r) return nullptr;
    return mergedProfile(r->profile, fit_->profile, fit_->kinds);
}

bool App::fitInUse() const {
    const Resin* r = resinById(fit_resin_);
    return fit_ && r && !fit_->kinds.empty() && r->profile == fittedProfile();
}

void App::useFit() {
    const Resin* found = resinById(fit_resin_);
    if (!fit_ || !found || fit_->kinds.empty()) return;
    Resin r = *found;
    r.profile = fittedProfile();
    storeResin(r);
    log::info("fitted " + r.name + " from its plates: " + profileState(r.profile));
}

void App::saveFitAs() {
    const Resin* r = resinById(fit_resin_);
    if (!fit_ || !r || fit_->kinds.empty()) return;
    const char* docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    const std::string start = (std::filesystem::path(docs ? docs : ".") / (r->name + " profile.json")).string();
    platform::saveFileDialog(window_, {{"Resin profiles", "json"}}, start, [this](const std::string& path) {
        if (!path.empty()) saveFitTo(path);
    });
}

void App::saveFitTo(const std::string& path) {
    const nlohmann::json p = fittedProfile();
    if (p.is_null()) return;
    std::ofstream out(path, std::ios::trunc);
    out << p.dump(2) << "\n";
    out.flush();
    if (!out) {
        showError("Couldn't save the profile", "Writing " + std::filesystem::path(path).filename().string() + " failed.");
        return;
    }
    log::info("saved the fitted profile to " + path);
}

// ---- A/B test ----

std::string App::abDir() const { return (std::filesystem::path(dataFolder()) / "ab").string(); }

void App::openABSetup() {
    screen_ = Screen::ABSetup;
    if (!abBlocker().empty()) return;
    const fruit::ABPlateFit& f = abFit();
    if (ab_copies_ < 2 || ab_copies_ > f.capacity) ab_copies_ = defaultABCopies(f.capacity);
}

const fruit::ABPlateFit& App::abFit() {
    const void* key = doc_ && doc_->supports.result ? doc_->supports.result.get() : nullptr;
    if (key != ab_fit_for_) {
        ab_fit_for_ = key;
        ab_fit_ = key ? fruit::abPlateFit(doc_->placed(), doc_->supports.result->mesh, printer_) : fruit::ABPlateFit{};
    }
    return ab_fit_;
}

std::string App::abBlocker() const {
    if (!doc_) return "Open a job first: the test prints copies of its model.";
    if (!doc_->supports.result) return "Generate the job's supports first: the A copies carry them.";
    if (!doc_->supports.fresh(doc_->revision(), doc_->supportSettings()))
        return "The job's supports are out of date: generate them again first.";
    return "";
}

void App::setABCopies(int n) {
    if (!abBlocker().empty()) return;
    ab_copies_ = std::clamp(n, std::min(2, abFit().capacity), abFit().capacity);
}

const fruit::ABPlate* App::abPlate() {
    if (!abBlocker().empty() || ab_copies_ < 2) return nullptr;
    const void* key = doc_->supports.result.get();
    if (!ab_plate_ || key != ab_plate_for_ || ab_copies_ != ab_plate_copies_) {
        fruit::ABPlateOptions o;
        o.copies = ab_copies_;
        o.model = doc_->name;
        o.tips_per_copy = static_cast<int>(doc_->supports.result->tips);
        try {
            ab_plate_ = std::make_shared<const fruit::ABPlate>(
                fruit::makeABPlate(doc_->placed(), doc_->supports.result->mesh, printer_, o));
        } catch (const std::exception& e) {
            log::error("A/B plate: " + std::string(e.what()));
            ab_plate_.reset();
        }
        ab_plate_for_ = key;
        ab_plate_copies_ = ab_copies_;
    }
    return ab_plate_.get();
}

void App::saveABFiles() {
    if (!abBlocker().empty() || !doc_) return;
    const char* docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    const std::string start = (std::filesystem::path(docs ? docs : ".") / (doc_->name + " AB_A.stl")).string();
    platform::saveFileDialog(window_, {{"STL", "stl"}}, start, [this](const std::string& path) {
        if (!path.empty()) saveABTo(path);
    });
}

void App::saveABTo(const std::string& path_a) {
    if (const std::string why = abBlocker(); !why.empty()) {
        showError("Couldn't save the A/B test", why);
        return;
    }
    const fruit::ABPlate* p = abPlate();
    if (!p) {
        showError("Couldn't save the A/B test", "The copies don't fit on the plate.");
        return;
    }
    const std::string path_b = abFileB(path_a);
    const std::filesystem::path dir = abDir();
    try {
        fruit::writeStl(path_a, p->supported);
        fruit::writeStl(path_b, p->bare);
        keepABTest(p->manifest, (dir / "ab.json").string(), (dir / "ab.results.csv").string());
        ABRecord r;
        r.model = doc_->name;
        r.resin = doc_->supports.choices.resin;
        r.file_a = path_a;
        r.file_b = path_b;
        r.copies = ab_copies_;
        saveABRecord((dir / "test.json").string(), r);
        ab_record_ = r;
        log::info("saved the A/B test of " + r.model + " (" + std::to_string(r.copies) + " copies) to " + path_a +
                  " and " + path_b);
    } catch (const std::exception& e) {
        showError("Couldn't save the A/B test", e.what());
    }
}

void App::openABResults() {
    screen_ = Screen::ABResults;
    ab_manifest_ = nullptr;
    ab_scores_.clear();
    ab_summary_ = fruit::ABSummary{};
    ++ab_scores_rev_;
    if (ab_record_.copies == 0) return;  // the page offers to set one up
    const std::filesystem::path dir = abDir();
    try {
        std::ifstream in(dir / "ab.json");
        if (!in) throw std::runtime_error("Its manifest (ab.json) isn't in FruitSlicer's folder any more.");
        ab_manifest_ = nlohmann::json::parse(in);
        ab_scores_ = fruit::loadABResults((dir / "ab.json").string(), (dir / "ab.results.csv").string());
        ab_summary_ = fruit::summarizeAB(ab_manifest_, ab_scores_);
        ab_selected_ = ab_scores_.empty() ? 0 : ab_scores_.front().id;
        for (const auto& c : ab_scores_)  // the first not scored yet
            if (!c.recorded) {
                ab_selected_ = c.id;
                break;
            }
    } catch (const std::exception& e) {
        ab_manifest_ = nullptr;
        ab_scores_.clear();
        showError("Couldn't read the A/B test's scores", e.what());
    }
}

void App::setABScore(int id, Outcome o, int scar, double removal_s, const std::string& notes) {
    if (ab_manifest_.is_null()) return;
    std::vector<fruit::ABCopyResult> next = ab_scores_;
    bool found = false;
    for (auto& c : next) {
        if (c.id != id) continue;
        found = true;
        const bool printed = o == Outcome::Held || o == Outcome::Partly;
        c.recorded = o != Outcome::None;
        c.ok = o == Outcome::Held;
        c.partial = o == Outcome::Partly;
        c.scar = printed ? std::clamp(scar, 0, 5) : 0;  // a failed copy leaves nothing to mark or clean
        c.removal_s = printed && removal_s >= 0 ? removal_s : -1;
        c.notes = notes;
    }
    if (!found) return;
    const std::filesystem::path dir = abDir();
    try {
        writeWhole((dir / "ab.results.csv").string(), fruit::blindABCsv(ab_manifest_, next));
    } catch (const std::exception& e) {
        showError("Couldn't save the score", e.what());
        return;
    }
    ab_scores_ = std::move(next);
    ab_summary_ = fruit::summarizeAB(ab_manifest_, ab_scores_);
    ++ab_scores_rev_;
    ab_record_.scored = 0;
    for (const auto& c : ab_scores_) ab_record_.scored += c.recorded ? 1 : 0;
    try {
        saveABRecord((dir / "test.json").string(), ab_record_);
    } catch (const std::exception& e) {
        log::error("A/B record: " + std::string(e.what()));
    }
}

// ---- Tools: inspect and compare print files ----

void App::freeGooPictures() {
    if (!goo_inspected_) return;
    if (goo_inspected_->small_tex) Renderer::freeTexture(goo_inspected_->small_tex);
    if (goo_inspected_->big_tex) Renderer::freeTexture(goo_inspected_->big_tex);
    goo_inspected_->small_tex = goo_inspected_->big_tex = 0;
}

void App::inspectGoo() {
    platform::openFileDialog(window_, {{"Print files", "goo"}}, [this](const std::string& path) {
        if (!path.empty()) inspectGooFile(path);
    });
}

void App::inspectGooFile(const std::string& path) {
    screen_ = Screen::Tools;
    tools_tab_ = 0;
    auto g = std::make_shared<GooInspected>();
    try {
        g->path = path;
        g->header = fruit::readGooHeader(path);
        std::error_code ec;
        const auto bytes = std::filesystem::file_size(path, ec);
        g->summary = summarizeGoo(g->header, ec ? 0 : static_cast<uint64_t>(bytes));
    } catch (const std::exception& e) {
        showError("Couldn't read that print file", e.what());
        return;
    }
    freeGooPictures();
    if (const auto px = previewRgba(g->header.small_preview, 116, 116); !px.empty())
        g->small_tex = renderer_.uploadRgba(116, 116, px);
    if (const auto px = previewRgba(g->header.big_preview, 290, 290); !px.empty())
        g->big_tex = renderer_.uploadRgba(290, 290, px);
    goo_inspected_ = g;
    log::info("inspecting " + path);
}

void App::checkGoo() {
    if (!goo_inspected_) return;
    const std::string path = goo_inspected_->path;
    auto out = std::make_shared<fruit::GooVerifyResult>();
    jobs_.start(
        "Reading every layer back",
        [path, out](const fruit::JobControl& ctl) { *out = fruit::verifyGoo(path, ctl); },
        [this, path, out](std::exception_ptr err) {
            if (err) {
                try {
                    std::rethrow_exception(err);
                } catch (const fruit::Cancelled&) {
                } catch (const std::exception& e) {
                    showError("Couldn't read the file back", e.what());
                }
                return;
            }
            if (goo_inspected_ && goo_inspected_->path == path) goo_inspected_->check = out;
        });
}

void App::chooseGooToCompare(int side) {
    platform::openFileDialog(window_, {{"Print files", "goo"}}, [this, side](const std::string& path) {
        if (!path.empty()) setGooToCompare(side, path);
    });
}

void App::setGooToCompare(int side, const std::string& path) {
    screen_ = Screen::Tools;
    tools_tab_ = 1;
    goo_compare_[side & 1] = path;
    goo_comparison_.reset();
    compareGoos();
}

void App::compareGoos() {
    if (goo_compare_[0].empty() || goo_compare_[1].empty()) return;
    const std::string a = goo_compare_[0], b = goo_compare_[1];
    auto out = std::make_shared<fruit::GooCompareResult>();
    jobs_.start(
        "Comparing the two files",
        [a, b, out](const fruit::JobControl& ctl) {
            ctl.step("read", 0.0);
            const fruit::GooFileInfo fa = fruit::readGoo(a);
            ctl.step("read", 0.5);
            const fruit::GooFileInfo fb = fruit::readGoo(b);
            ctl.step("compare", 0.0);
            *out = fruit::compareGoo(fa, fb);
        },
        [this, a, b, out](std::exception_ptr err) {
            if (err) {
                try {
                    std::rethrow_exception(err);
                } catch (const fruit::Cancelled&) {
                } catch (const std::exception& e) {
                    showError("Couldn't compare the two files", e.what());
                }
                return;
            }
            if (goo_compare_[0] == a && goo_compare_[1] == b) goo_comparison_ = out;
        });
}

std::shared_ptr<const fruit::GooHeader> App::resinSample(const Resin& r) {
    if (r.sample_path.empty()) return nullptr;
    auto it = resin_samples_.find(r.sample_path);
    if (it == resin_samples_.end()) {
        std::shared_ptr<const fruit::GooHeader> h;
        try {
            h = loadSample(r.sample_path);
        } catch (const std::exception& e) {
            log::error("resin " + r.name + ": " + e.what());
        }
        it = resin_samples_.emplace(r.sample_path, h).first;
    }
    return it->second;
}

void App::saveOrientationPiece() {
    const char* docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    const std::string start = (std::filesystem::path(docs ? docs : ".") / "f.stl").string();
    platform::saveFileDialog(window_, {{"STL", "stl"}}, start, [this](const std::string& path) {
        if (!path.empty()) saveOrientationPieceTo(path);
    });
}

void App::saveOrientationPieceTo(const std::string& path) {
    try {
        gui::saveOrientationPiece(path);
        orient_piece_ = path;
        log::info("saved the F test piece to " + path);
    } catch (const std::exception& e) {
        showError("Couldn't save f.stl", e.what());
    }
}

void App::chooseOrientationSample() {
    platform::openFileDialog(window_, {{"Print files from SatelLite", "goo"}}, [this](const std::string& path) {
        if (!path.empty()) checkOrientationWith(path);
    });
}

void App::checkOrientationWith(const std::string& satellite_goo) {
    auto out = std::make_shared<OrientationCheck>();
    const fruit::PrinterProfile printer = printer_;
    orient_check_.reset();
    jobs_.start(
        "Comparing the F",
        [out, satellite_goo, printer](const fruit::JobControl& ctl) {
            *out = checkOrientation(satellite_goo, printer, ctl);
        },
        [this, out](std::exception_ptr err) {
            if (err) {
                try {
                    std::rethrow_exception(err);
                } catch (const fruit::Cancelled&) {
                } catch (const std::exception& e) {
                    showError("Couldn't compare the two F's", e.what());
                }
                return;
            }
            orient_check_ = std::make_shared<const OrientationCheck>(std::move(*out));
            log::info("orientation check: " + std::string(fruit::verdictText(orient_check_->verdict)));
        });
}

void App::useOrientationFix() {
    if (!orient_check_ || orient_check_->verdict == fruit::OrientationVerdict::Undetermined) return;
    PrinterSetup s = printer_setup_;
    s.orientation_confirmed = true;
    s.flip_x = orient_check_->flip_x;
    s.flip_y = orient_check_->flip_y;
    s.checked_with = std::filesystem::path(orient_check_->satellite_path).filename().string();
    setPrinterSetup(s);
    log::info(std::string("orientation confirmed; flips x ") + (s.flip_x ? "yes" : "no") + ", y " +
              (s.flip_y ? "yes" : "no"));
}

void App::setHinge(std::optional<HingeChoice> side) {
    PrinterSetup s = printer_setup_;
    s.hinge = side ? std::optional<fruit::HingeSide>(plateSide(*side)) : std::nullopt;
    setPrinterSetup(s);
}

unsigned App::orientationPicture(bool ours) {
    if (!orient_check_) return 0;
    if (orient_tex_for_ != orient_check_) {
        for (int i = 0; i < 2; ++i) {
            Renderer::freeTexture(orient_tex_[i]);
            const LayerPicture& p = i == 0 ? orient_check_->ours : orient_check_->theirs;
            std::vector<unsigned char> rgba(p.lit.size() * 4, 255);
            for (std::size_t k = 0; k < p.lit.size(); ++k) {
                // Lit white on the viewport's near-black, as the layer view shows it.
                const unsigned v = p.lit[k];
                rgba[k * 4 + 0] = static_cast<unsigned char>(0x0E + (255 - 0x0E) * v / 255);
                rgba[k * 4 + 1] = static_cast<unsigned char>(0x10 + (255 - 0x10) * v / 255);
                rgba[k * 4 + 2] = static_cast<unsigned char>(0x14 + (255 - 0x14) * v / 255);
            }
            orient_tex_[i] = p.w > 0 ? renderer_.uploadRgba(p.w, p.h, rgba) : 0;
        }
        orient_tex_for_ = orient_check_;
    }
    return orient_tex_[ours ? 0 : 1];
}

void App::ejectDrive() {
    if (!doc_ || doc_->print.drive.empty()) return;
    std::string path;
    for (const auto& v : drives_)
        if (v.name == doc_->print.drive) path = v.path;
    if (path.empty()) return;
    auto error = std::make_shared<std::string>();
    auto ok = std::make_shared<bool>(false);
    std::weak_ptr<Document> target = doc_;
    jobs_.start(
        "Ejecting " + doc_->print.drive,
        [path, error, ok](const fruit::JobControl&) { *ok = platform::ejectVolume(path, error.get()); },
        [this, target, error, ok](std::exception_ptr err) {
            const auto d = target.lock();
            if (!d) return;
            if (err) {
                try {
                    std::rethrow_exception(err);
                } catch (const std::exception& e) {
                    *error = e.what();
                }
            }
            if (!*ok) {
                showError("Couldn't eject " + d->print.drive, *error);
                return;
            }
            log::info("ejected " + d->print.drive);
            d->print.drive.clear();
            drives_checked_ = -1;  // read the list again
        });
}

void App::revealSaved() {
    if (doc_ && doc_->print.saved) platform::revealFile(doc_->print.saved->path);
}

void App::dismissSaved() {
    if (doc_) doc_->print.saved.reset();
}

void App::exportStl(bool supports_only) {
    if (!doc_ || !doc_->supports.result) return;
    const std::string name = doc_->name + (supports_only ? " supports.stl" : " with supports.stl");
    const char* docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    const std::string start = (std::filesystem::path(docs ? docs : ".") / name).string();
    platform::saveFileDialog(window_, {{"STL", "stl"}}, start, [this, supports_only](const std::string& path) {
        if (!path.empty()) exportStlTo(path, supports_only);
    });
}

void App::exportStlTo(const std::string& path, bool supports_only) {
    if (!doc_ || !doc_->supports.result) return;
    auto supports = doc_->supports.result;
    auto placed = std::make_shared<const fruit::TriangleMesh>(doc_->placed());
    const std::string name = std::filesystem::path(path).filename().string();
    jobs_.start(
        "Exporting " + name,
        [supports, placed, path, supports_only](const fruit::JobControl&) {
            fruit::TriangleMesh m = supports_only ? fruit::TriangleMesh{} : *placed;
            m.append(supports->mesh);
            fruit::writeStl(path, m);
        },
        [this, name](std::exception_ptr err) {
            if (!err) {
                log::info("exported " + name);
                return;
            }
            try {
                std::rethrow_exception(err);
            } catch (const std::exception& e) {
                showError("Couldn't export " + name, e.what());
            }
        });
}

void App::setLayer(int index) {
    if (!doc_ || !doc_->layers.result) return;
    const int n = static_cast<int>(doc_->layers.result->layers->size());
    doc_->layers.current = std::clamp(index, 0, std::max(0, n - 1));
}

void App::setShowBelow(bool show) {
    if (doc_) doc_->layers.show_below = show;
}

unsigned App::layerPicture(int max_w, int max_h, const LayerImage** image) {
    if (image) *image = nullptr;
    if (!doc_ || !doc_->layers.result) return 0;
    const auto& res = doc_->layers.result;
    const int index = doc_->layers.current >= 0 ? doc_->layers.current : static_cast<int>(res->biggest);
    const bool below = doc_->layers.show_below;
    if (layer_tex_ && layer_tex_result_ == res && layer_tex_index_ == index && layer_tex_below_ == below &&
        layer_tex_w_ == max_w && layer_tex_h_ == max_h) {
        if (image) *image = &layer_image_;
        return layer_tex_;
    }
    // A few millimetres around the print, drawn at the printer's pixel shape.
    fruit::BBox3 crop = res->bounds;
    crop.min = crop.min - fruit::Vec3{3, 3, 0};
    crop.max = crop.max + fruit::Vec3{3, 3, 0};
    layer_image_ = drawLayer(*res->layers, static_cast<std::size_t>(index), crop, max_w, max_h, printer_, below);
    std::vector<unsigned char> rgba(layer_image_.lit.size() * 4, 0);
    for (std::size_t i = 0; i < layer_image_.lit.size(); ++i) {
        const unsigned lit = layer_image_.lit[i];
        const unsigned under = layer_image_.below.empty() ? 0 : layer_image_.below[i];
        // Lit pixels white; the layer below, where this one is dark, in dim blue-grey.
        const unsigned g = lit + (255 - lit) * under * 0x48 / (255 * 255);
        rgba[i * 4 + 0] = static_cast<unsigned char>(std::min(255u, g));
        rgba[i * 4 + 1] = static_cast<unsigned char>(std::min(255u, lit + (255 - lit) * under * 0x52 / (255 * 255)));
        rgba[i * 4 + 2] = static_cast<unsigned char>(std::min(255u, lit + (255 - lit) * under * 0x66 / (255 * 255)));
        rgba[i * 4 + 3] = 255;
    }
    Renderer::freeTexture(layer_tex_);
    layer_tex_ = renderer_.uploadRgba(layer_image_.w, layer_image_.h, rgba);
    layer_tex_result_ = res;
    layer_tex_index_ = index;
    layer_tex_below_ = below;
    layer_tex_w_ = max_w;
    layer_tex_h_ = max_h;
    if (image) *image = &layer_image_;
    return layer_tex_;
}

void App::freePictures() {
    for (unsigned t : pictures_) Renderer::freeTexture(t);
    pictures_.clear();
    pictures_for_.reset();
}

unsigned App::orientPicture(int option) {
    if (!doc_ || !doc_->orient().results) return 0;
    const auto& res = doc_->orient().results;
    if (pictures_for_ != res) {
        freePictures();
        for (const OrientOption& op : res->options)
            pictures_.push_back(renderer_.uploadMask(op.below.w, op.below.h, op.below.alpha, 0xA9B1BE));
        pictures_.push_back(renderer_.uploadMask(res->now.below.w, res->now.below.h, res->now.below.alpha, 0xA9B1BE));
        pictures_for_ = res;
    }
    if (option < 0) return pictures_.back();
    return option < static_cast<int>(pictures_.size()) - 1 ? pictures_[static_cast<std::size_t>(option)] : 0;
}

void App::undo() {
    if (doc_ && doc_->canUndo()) doc_->undo();
}

void App::redo() {
    if (doc_ && doc_->canRedo()) doc_->redo();
}

void App::rail() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::Rail);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 12));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
    ImGui::BeginChild("rail", ImVec2(76, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    dl->AddLine(ImVec2(wp.x + ws.x - 1, wp.y), ImVec2(wp.x + ws.x - 1, wp.y + ws.y), col::Border);
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("logo", ImVec2(64, 44))) screen_ = Screen::Home;
        icon::logo(dl, ImVec2(p.x + 18, p.y + 8), 28);
    }
    ImGui::Dummy(ImVec2(0, 4));
    if (railItem("rail_home", "Home", icon::home, screen_ == Screen::Home)) screen_ = Screen::Home;
    if (railItem("rail_job", "Job", icon::cube, screen_ == Screen::Job)) screen_ = Screen::Job;
    const bool calibrating =
        screen_ == Screen::Calibrate || screen_ == Screen::OrientationCheck || screen_ == Screen::Printer ||
        screen_ == Screen::Plate || screen_ == Screen::Score || screen_ == Screen::Fit || screen_ == Screen::ABSetup ||
        screen_ == Screen::ABResults;
    if (railItem("rail_calibrate", "Calibrate", icon::target, calibrating)) screen_ = Screen::Calibrate;
    if (railItem("rail_resins", "Resins", icon::droplet, screen_ == Screen::Resins)) {
        if (screen_ != Screen::Resins) resin_selected_ = newJobResin().id;
        screen_ = Screen::Resins;
    }
    if (railItem("rail_tools", "Tools", icon::wrench, screen_ == Screen::Tools)) screen_ = Screen::Tools;
    // Settings at the bottom; About opens from it.
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 56 - 12);
    if (railItem("rail_settings", "Settings", icon::gear, screen_ == Screen::Settings || screen_ == Screen::About))
        screen_ = Screen::Settings;
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

void App::errorDialog() {
    if (error_open_) {
        ImGui::OpenPopup("error");
        error_open_ = false;
    }
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(ImVec2(center.x, center.y * 0.6f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 22));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, col::Panel);
    ImGui::PushStyleColor(ImGuiCol_Border, col::BorderStrong);
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(5, 6, 8, 184));
    if (ImGui::BeginPopupModal("error", nullptr,
                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddCircleFilled(ImVec2(p.x + 18, p.y + 18), 18, col::ErrorBg);
        icon::cross(dl, ImVec2(p.x + 8, p.y + 8), 20, col::Error);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 50);
        ImGui::BeginGroup();
        text(error_title_.c_str(), fonts().bold, size::DialogTitle, col::Text);
        ImGui::Dummy(ImVec2(0, 2));
        text(error_message_.c_str(), fonts().regular, size::Body, col::TextSecondary, true);
        ImGui::EndGroup();
        ImGui::Dummy(ImVec2(0, 10));
        const float bw = 90;
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - bw - 24);
        if (primaryButton("Close", ImVec2(bw, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
}

// Keyboard shortcuts that work anywhere (listed in the shortcuts dialog).
// ImGuiMod_Ctrl is the Command key on macOS.
void App::keys() {
    // Dialogs handle their own keys; nothing reaches the job behind them.
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
    const ImGuiInputFlags global = ImGuiInputFlags_RouteGlobal;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, global)) openDialog();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_O, global)) addPartDialog();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, global)) saveJob();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, global)) saveJobAs();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Q, global)) requestQuit();
    // While editing tips, undo and redo work on the tip edits only.
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, global)) {
        if (tip_editing_) undoTipChange();
        else undo();
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, global)) {
        if (tip_editing_) redoTipChange();
        else redo();
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Slash, global)) shortcuts_open_ = !shortcuts_open_;
    // ⌘↩: the Job panel's main button (Make supports, Slice…).
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Enter, global) && doc_ && screen_ == Screen::Job &&
        (!jobs_.busy() || checkingCollisions())) {
        const MainButton mb = mainButton(doc_.get());
        if (mb.enabled && (mb.action == MainAction::MakeSupports || mb.action == MainAction::RemakeSupports))
            supportsNow();
        else if (mb.enabled && (mb.action == MainAction::Slice || mb.action == MainAction::ShowLayers))
            goToStep(Step::Layers);
    }
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || io.KeyCtrl || io.KeyAlt || io.KeySuper) return;
    if (screen_ == Screen::Job && step_ == Step::Supports && tip_editing_ && doc_) {
        const int sel = doc_->supports.selected_tip;
        if (ImGui::IsKeyPressed(ImGuiKey_V, false)) tip_tool_ = TipTool::Select;
        if (ImGui::IsKeyPressed(ImGuiKey_A, false)) tip_tool_ = TipTool::Add;
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) tip_tool_ = TipTool::Zone;
        if (sel >= 0 && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)))
            deleteTip(sel);
        if (sel >= 0 && doc_->supports.result &&
            (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false) || ImGui::IsKeyPressed(ImGuiKey_RightBracket, false))) {
            static const double sizes[] = {0.2, 0.3, 0.4, 0.6};
            const double d = doc_->supports.result->points[static_cast<std::size_t>(sel)].tip_diameter;
            int c = 0;
            for (int i = 1; i < 4; ++i)
                if (std::abs(sizes[i] - d) < std::abs(sizes[c] - d)) c = i;
            c = std::clamp(c + (ImGui::IsKeyPressed(ImGuiKey_RightBracket, false) ? 1 : -1), 0, 3);
            resizeTip(sel, sizes[c]);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (sel >= 0) selectTip(-1);
            else editTips(false);
        }
        return;
    }
    if (screen_ == Screen::Job && step_ == Step::Supports && advanced_open_) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) advanced_open_ = false;
        return;
    }
    if (screen_ == Screen::Job && inLayersMode(step_) && doc_ && doc_->layersFresh()) {
        const int now = doc_->layers.current >= 0 ? doc_->layers.current : static_cast<int>(doc_->layers.result->biggest);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) setLayer(now + 1);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) setLayer(now - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) setLayer(now + 10);
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) setLayer(now - 10);
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) setLayer(0);
        if (ImGui::IsKeyPressed(ImGuiKey_End, false)) setLayer(1 << 30);
        return;
    }
    if (handlesShown()) {
        // Escape lets go of a handle, putting it back; the arrows move the
        // model 1 mm (⇧: 10 mm) across the plate.
        if (placing() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) cancelPlacing();
        if (!placing() && !io.NavVisible) {  // not while the arrows move between buttons
            const double mm = io.KeyShift ? 10.0 : 1.0;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) nudge(-mm, 0);
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) nudge(mm, 0);
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) nudge(0, mm);
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) nudge(0, -mm);
        }
    }
    if (screen_ == Screen::Job) {
        if (ImGui::IsKeyPressed(ImGuiKey_1, false)) setView(ViewPreset::Front);
        if (ImGui::IsKeyPressed(ImGuiKey_2, false)) setView(ViewPreset::Side);
        if (ImGui::IsKeyPressed(ImGuiKey_3, false)) setView(ViewPreset::Below);
        if (ImGui::IsKeyPressed(ImGuiKey_4, false)) setView(ViewPreset::Angled);
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) fitView();
        if (ImGui::IsKeyPressed(ImGuiKey_5, false)) {  // from the top
            camera_.lookFrom({0, 0, 1});
            viewChanged();
        }
        // The tools around the view, by their first letters.
        if (doc_ && !inLayersMode(step_) && !io.KeyShift) {
            if (ImGui::IsKeyPressed(ImGuiKey_P, false)) openTool(Tool::OnPlate);
            if (ImGui::IsKeyPressed(ImGuiKey_M, false)) openTool(Tool::Move);
            if (ImGui::IsKeyPressed(ImGuiKey_R, false)) openTool(Tool::Rotate);
            if (ImGui::IsKeyPressed(ImGuiKey_S, false)) openTool(Tool::Scale);
            if (ImGui::IsKeyPressed(ImGuiKey_T, false)) openTool(Tool::Supports);
            if (ImGui::IsKeyPressed(ImGuiKey_H, false) || ImGui::IsKeyPressed(ImGuiKey_D, false)) openTool(Tool::Hollow);
            // The newer tools, where their first letter was free: mIrror, Arrange, measUre, Label,
            // comBine (a boolean), Cut and merGe. Duplicate, Split and Repair have none.
            if (ImGui::IsKeyPressed(ImGuiKey_I, false)) openTool(Tool::Mirror);
            if (ImGui::IsKeyPressed(ImGuiKey_A, false)) openTool(Tool::Arrange);
            if (ImGui::IsKeyPressed(ImGuiKey_U, false)) openTool(Tool::Measure);
            if (ImGui::IsKeyPressed(ImGuiKey_L, false)) openTool(Tool::Label);
            if (ImGui::IsKeyPressed(ImGuiKey_B, false)) openTool(Tool::Combine);
            if (ImGui::IsKeyPressed(ImGuiKey_C, false)) openTool(Tool::Cut);
            if (ImGui::IsKeyPressed(ImGuiKey_G, false)) openTool(Tool::Merge);
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !placing()) {
                // Measuring, the first Esc starts again; the next closes the panel.
                if (tool_panel_ && tool() == Tool::Measure && measureShown()) measure_.clear();
                else tool_panel_ = false;
            }
        }
    }
    if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Slash, false)) shortcuts_open_ = true;  // "?"
}

void App::frame() {
    maintain();
    if (!placing()) startCollisionCheck();  // with several parts: do they overlap? (after the work above)
    // Scripted captures keep the drive list they set up: the machine's own
    // drives would change the pictures.
    if (screen_ == Screen::Job && step_ == Step::Print && !opts_.deterministic) {
        const double now = static_cast<double>(SDL_GetTicks()) / 1000.0;
        if (drives_checked_ < 0 || now - drives_checked_ > 2.0) {
            drives_checked_ = now;
            std::vector<platform::Volume> found = platform::removableVolumes();
            const std::string chosen = drive_choice_ >= 0 && drive_choice_ < static_cast<int>(drives_.size())
                                           ? drives_[static_cast<std::size_t>(drive_choice_)].path
                                           : std::string();
            int plugged_in = -1;  // a drive that wasn't there before
            for (std::size_t i = 0; i < found.size() && plugged_in < 0; ++i)
                if (std::none_of(drives_.begin(), drives_.end(),
                                 [&](const platform::Volume& v) { return v.path == found[i].path; }))
                    plugged_in = static_cast<int>(i);
            drives_ = std::move(found);
            // Your choice stays (a folder too); a drive plugged in is picked
            // when .goo files go to drives (Settings).
            drive_choice_ = -1;
            for (std::size_t i = 0; i < drives_.size(); ++i)
                if (!chosen.empty() && drives_[i].path == chosen) drive_choice_ = static_cast<int>(i);
            if (plugged_in >= 0 && prefs_.goo_to_drive) drive_choice_ = plugged_in;
        }
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    keys();
    if (screen_ != Screen::Setup) {  // setup has its own steps down the side
        rail();
        ImGui::SameLine(0, 0);
    }
    ImGui::BeginChild("content", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    switch (screen_) {
        case Screen::Home: screens::home(*this); break;
        case Screen::Job: screens::job(*this); break;
        case Screen::Calibrate: screens::calibrate(*this); break;
        case Screen::OrientationCheck: screens::orientationCheck(*this); break;
        case Screen::Printer: screens::printer(*this); break;
        case Screen::About: screens::about(*this); break;
        case Screen::Settings: screens::settings(*this); break;
        case Screen::Setup: screens::setup(*this); break;
        case Screen::Resins: screens::resins(*this); break;
        case Screen::Plate: screens::plate(*this); break;
        case Screen::Score: screens::score(*this); break;
        case Screen::Fit: screens::fit(*this); break;
        case Screen::ABSetup: screens::abSetup(*this); break;
        case Screen::ABResults: screens::abResults(*this); break;
        case Screen::Tools: screens::tools(*this); break;
    }
    ImGui::EndChild();
    dialogs::health(*this);
    dialogs::shortcuts(*this, shortcuts_open_);
    screens::parts::orientationQuestion(*this);
    screens::parts::sendDialog(*this);
    screens::parts::unsavedDialog(*this);
    screens::parts::relinkDialog(*this);
    errorDialog();
    ImGui::End();
}

}  // namespace gui
