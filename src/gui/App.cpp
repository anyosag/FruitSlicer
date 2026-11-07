// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "App.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

#include <SDL3/SDL.h>

#include "mesh/StlIO.h"
#include "net/testing/MockPrinter.h"

#include "Dialogs.h"
#include "Icons.h"
#include "Log.h"
#include "Theme.h"
#include "Widgets.h"
#include "platform/Platform.h"
#include "screens/JobParts.h"
#include "screens/Screens.h"

namespace gui {

App::App(SDL_Window* window, AppOptions opts, std::function<void()> wake)
    : window_(window), opts_(opts), jobs_(std::move(wake)) {
    camera_.preset(ViewPreset::Angled);
}

App::~App() {
    freePictures();
    Renderer::freeTexture(layer_tex_);
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
    if (!doc_ || doc_->placed().empty()) {
        camera_.frame({0, 0, 15}, 110);
        return;
    }
    const fruit::BBox3 b = doc_->placed().bbox();
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

void App::openPath(const std::string& path) {
    const std::string name = std::filesystem::path(path).filename().string();
    screen_ = Screen::Job;
    auto result = std::make_shared<std::shared_ptr<Document>>();
    const fruit::PrinterProfile printer = printer_;
    jobs_.start(
        "Opening " + name,
        [path, printer, result](const fruit::JobControl& ctl) { *result = loadDocument(path, printer, ctl); },
        [this, result, name](std::exception_ptr err) {
            if (!err) {
                checking_ = *result;
                const fruit::PlateFit& f = checking_->fit();
                health_ = HealthDialog{};
                health_.open = true;
                health_.choice = f.fits ? SizeChoice::Full : SizeChoice::Largest;
                health_.custom_percent = f.fits ? 100 : std::round(std::max(f.largest, f.largest_turned) * 100);
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
    platform::openFileDialog(window_, {{"3D models", "stl;obj;3mf"}}, [this](const std::string& path) {
        if (!path.empty()) openPath(path);
    });
}

fruit::Placement App::healthPlacement() const {
    fruit::Placement p;
    if (!checking_) return p;
    const fruit::PlateFit& f = checking_->fit();
    switch (health_.choice) {
        case SizeChoice::Full: p.scale = 1.0; break;
        case SizeChoice::Largest:
            p.scale = std::max(f.largest, f.largest_turned);
            if (f.largest_turned > f.largest) p.rotation_deg.z = 90;
            break;
        case SizeChoice::Custom: p.scale = health_.custom_percent / 100.0; break;
        case SizeChoice::Inches: p.scale = 25.4; break;
    }
    return p;
}

void App::chooseSize(SizeChoice c, double custom_percent) {
    health_.choice = c;
    if (c == SizeChoice::Custom && custom_percent > 0) health_.custom_percent = custom_percent;
}

void App::placeChecked() {
    if (!checking_) return;
    checking_->place(healthPlacement());
    doc_ = std::move(checking_);
    checking_.reset();
    doc_->supports.choices.advanced = advanced_;  // your support settings stay with you
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
        p.rotation_deg.z = std::fmod(p.rotation_deg.z + 90, 360);
        p.scale = f.largest_turned;
    } else {
        p.scale = f.largest;
    }
    doc_->edit(p);
    frameModel();
}

bool App::canGoTo(Step s) const {
    switch (s) {
        case Step::Model: return true;
        case Step::Orient:
        case Step::Supports: return doc_ && doc_->fit().fits;
        case Step::Layers:
            return doc_ && doc_->supports.fresh(doc_->revision(), doc_->orient.settings);
        case Step::Print:
            return doc_ && doc_->supports.fresh(doc_->revision(), doc_->orient.settings) &&
                   doc_->layers.fresh(doc_->supports.result);
        default: return false;  // the next updates
    }
}

void App::goToStep(Step s) {
    if (!canGoTo(s)) return;
    if (s != step_) advanced_open_ = false;
    step_ = s;
    screen_ = Screen::Job;
    if (s == Step::Orient && !(doc_ && doc_->orient.resultsFresh(doc_->revision()))) orient_view_ = OrientView::Settings;
}

namespace {

// A printer-frame direction in the model's own frame (rotation only).
fruit::Vec3 toModel(const fruit::Placement& p, const fruit::Vec3& d) {
    const fruit::Transform r = fruit::Transform::rotationXYZ(p.rotation_deg.x, p.rotation_deg.y, p.rotation_deg.z);
    return fruit::normalized({r.m[0][0] * d.x + r.m[1][0] * d.y + r.m[2][0] * d.z,
                              r.m[0][1] * d.x + r.m[1][1] * d.y + r.m[2][1] * d.z,
                              r.m[0][2] * d.x + r.m[1][2] * d.y + r.m[2][2] * d.z});
}

}  // namespace

bool App::sidePicked(Side s) const {
    if (!doc_) return false;
    const fruit::Vec3 d = sideDirection(s);
    for (const fruit::Vec3& v : visibleInPrinter(doc_->orient.visible, doc_->placement()))
        if (fruit::dot(v, d) > 0.9) return true;
    return false;
}

void App::toggleSide(Side s) {
    if (!doc_) return;
    OrientState& o = doc_->orient;
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
    if (!doc_ || doc_->orient.visible.empty()) return;
    doc_->orient.visible.clear();
    ++doc_->orient.settings;
    doc_->orient.decided = false;
}

void App::maintain() {
    if (doc_ && screen_ == Screen::Job && step_ == Step::Layers && !jobs_.busy()) {
        // Cut the layers for the current supports, once per supports result.
        const auto& sup = doc_->supports.result;
        if (sup && !doc_->layers.fresh(sup) && sliced_for_ != sup &&
            doc_->supports.fresh(doc_->revision(), doc_->orient.settings)) {
            sliced_for_ = sup;
            auto snap = std::make_shared<const Document>(*doc_);
            auto out = std::make_shared<LayerResult>();
            const double raft = doc_->placement().on_plate ? 0.0 : sup->tree.raft_top;
            std::weak_ptr<Document> target = doc_;
            jobs_.start(
                "Cutting layers",
                [snap, sup, raft, out](const fruit::JobControl& ctl) { *out = sliceLayers(*snap, sup->mesh, raft, ctl); },
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
    if (!doc_ || screen_ != Screen::Job || step_ != Step::Orient || jobs_.busy()) return;
    OrientState& o = doc_->orient;
    const int rev = doc_->revision(), settings = o.settings;
    if (o.nowFresh(rev) || (scored_revision_ == rev && scored_settings_ == settings)) return;
    scored_revision_ = rev;
    scored_settings_ = settings;
    // The worker gets its own copy: the job can keep changing meanwhile.
    auto snap = std::make_shared<const Document>(*doc_);
    auto out = std::make_shared<OrientMetrics>();
    std::weak_ptr<Document> target = doc_;
    jobs_.start(
        "Scoring how it sits",
        [snap, out](const fruit::JobControl& ctl) { *out = measure(*snap, snap->orient.visible, ctl); },
        [this, target, out, rev, settings](std::exception_ptr err) {
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
            d->orient.now = *out;
            d->orient.now_revision = rev;
            d->orient.now_settings = settings;
        });
}

void App::findOrientation() {
    if (!doc_ || !doc_->fit().fits) return;
    auto snap = std::make_shared<const Document>(*doc_);
    auto out = std::make_shared<OrientSearch>();
    const int rev = doc_->revision(), settings = doc_->orient.settings;
    std::weak_ptr<Document> target = doc_;
    jobs_.start(
        "Finding the best orientation",
        [snap, out](const fruit::JobControl& ctl) { *out = search(*snap, snap->orient.visible, ctl); },
        [this, target, out, rev, settings](std::exception_ptr err) {
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
            OrientState& o = d->orient;
            o.results = res;
            o.results_revision = rev;
            o.results_settings = settings;
            if (!o.nowFresh(rev)) {
                o.now = res->now.m;
                o.now_revision = rev;
                o.now_settings = settings;
            }
            if (d == doc_ && step_ == Step::Orient) orient_view_ = OrientView::Results;
            log::info("orientation search: " + std::to_string(res->options.size()) + " better options in " +
                      std::to_string(res->seconds) + " s");
        });
}

void App::useOrientation(int option) {
    if (!doc_) return;
    OrientState& o = doc_->orient;
    if (option >= 0 && o.results && option < static_cast<int>(o.results->options.size())) {
        const OrientOption& op = o.results->options[static_cast<std::size_t>(option)];
        fruit::Placement p = doc_->placement();
        p.rotation_deg = op.rotation_deg;
        doc_->edit(p);
        // Its numbers came from the same planner: no need to score it again.
        o.now = op.m;
        o.now_revision = doc_->revision();
        o.now_settings = o.settings;
        frameModel();
    }
    o.decided = true;
    orient_view_ = OrientView::Settings;
}

void App::generateSupports() {
    if (!doc_ || !doc_->fit().fits) return;
    advanced_open_ = false;  // the run shows in the 3D view
    auto snap = std::make_shared<const Document>(*doc_);
    auto out = std::make_shared<SupportResult>();
    const int rev = doc_->revision(), settings = doc_->orient.settings;
    const SupportChoices choices = doc_->supports.choices;
    std::weak_ptr<Document> target = doc_;
    jobs_.start(
        "Generating supports",
        [snap, out, choices](const fruit::JobControl& ctl) { *out = generate(*snap, choices, snap->orient.visible, ctl); },
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
                // FruitSlicer's own value is not stored, so the row reads "Default" again.
                const SupportChoices none;
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
    c.usual_tip = SupportChoices{}.usual_tip;
    setSupportChoices(c);
}

void App::applyAdvanced() {
    advanced_open_ = false;
    if (doc_ && !doc_->supports.fresh(doc_->revision(), doc_->orient.settings)) generateSupports();
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

void App::syncScene() {
    renderer_.setOverhangTint(0);
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
    const bool fresh = s.fresh(doc_->revision(), doc_->orient.settings);
    const bool why = supports_view_ == SupportsView::Why;
    renderer_.setExtraStyle(0, fresh ? (why ? 0x6B7380u : 0xE2E6EDu) : 0x3A404Bu,
                            supports_view_ != SupportsView::TipsOnly);
    // Island, overhang, extra hold, steadying, by hand.
    static const unsigned kWhy[5] = {0xFF8F3F, 0x5B9BD5, 0xB48EF0, 0x5BD69A, 0xF2F4F7};
    for (std::size_t i = 0; i < 5; ++i) renderer_.setExtraStyle(i + 1, why ? kWhy[i] : 0xF2F4F7, fresh);
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
    const PrintSettings& s = doc_->print.settings;
    const bool same = plan_ && plan_layers_ == lr && plan_raft_ == raft && plan_settings_.sample == s.sample &&
                      plan_settings_.exposure == s.exposure && plan_settings_.bottom_exposure == s.bottom_exposure &&
                      plan_settings_.bottom_layers == s.bottom_layers &&
                      plan_settings_.transition_layers == s.transition_layers;
    if (!same) {
        plan_ = std::make_shared<const PrintPlan>(planPrint(*doc_, *lr, raft, s));
        plan_layers_ = lr;
        plan_settings_ = s;
        plan_raft_ = raft;
    }
    return plan_;
}

std::string App::printBlocker() {
    if (!doc_ || !canGoTo(Step::Print)) return "Generate supports, then let the Layers step cut the layers.";
    if (!doc_->supports.result->validation.floating.empty())
        return "Parts start in mid-air: add tips in the Supports step.";
    const auto plan = printPlan();
    if (!plan->fits) return "The model and supports reach past the printer's build volume.";
    if (!plan->ready) return plan->why_not;
    if (jobs_.busy()) return "Wait for the current task to finish.";
    return "";
}

void App::savePrint() {
    if (!printBlocker().empty()) return;
    // Asked once per session until the orientation check exists (calibration).
    if (!orientation_asked_) {
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
    const std::string start = (std::filesystem::path(docs ? docs : ".") / name).string();
    platform::saveFileDialog(window_, {{"Print files", "goo"}}, start, [this](const std::string& path) {
        if (!path.empty()) savePrintTo(path, "");
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
    if (!doc_ || !doc_->orient.results) return 0;
    const auto& res = doc_->orient.results;
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
    // About sits at the bottom, where Settings will be.
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 56 - 12);
    if (railItem("rail_about", "About", icon::gear, screen_ == Screen::About)) screen_ = Screen::About;
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
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, global)) undo();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, global)) redo();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Slash, global)) shortcuts_open_ = !shortcuts_open_;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || io.KeyCtrl || io.KeyAlt || io.KeySuper) return;
    if (screen_ == Screen::Job && step_ == Step::Supports && advanced_open_) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) advanced_open_ = false;
        return;
    }
    if (screen_ == Screen::Job && step_ == Step::Layers && doc_ && doc_->layers.result) {
        const int now = doc_->layers.current >= 0 ? doc_->layers.current : static_cast<int>(doc_->layers.result->biggest);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) setLayer(now + 1);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) setLayer(now - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) setLayer(now + 10);
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) setLayer(now - 10);
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) setLayer(0);
        if (ImGui::IsKeyPressed(ImGuiKey_End, false)) setLayer(1 << 30);
        return;
    }
    if (screen_ == Screen::Job) {
        if (ImGui::IsKeyPressed(ImGuiKey_1, false)) setView(ViewPreset::Front);
        if (ImGui::IsKeyPressed(ImGuiKey_2, false)) setView(ViewPreset::Side);
        if (ImGui::IsKeyPressed(ImGuiKey_3, false)) setView(ViewPreset::Below);
        if (ImGui::IsKeyPressed(ImGuiKey_4, false)) setView(ViewPreset::Angled);
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) fitView();
    }
    if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Slash, false)) shortcuts_open_ = true;  // "?"
}

void App::frame() {
    maintain();
    // Scripted captures keep the drive list they set up: the machine's own
    // drives would change the pictures.
    if (screen_ == Screen::Job && step_ == Step::Print && !opts_.deterministic) {
        const double now = static_cast<double>(SDL_GetTicks()) / 1000.0;
        if (drives_checked_ < 0 || now - drives_checked_ > 2.0) {
            drives_ = platform::removableVolumes();
            drives_checked_ = now;
            if (drive_choice_ >= static_cast<int>(drives_.size())) drive_choice_ = -1;
            if (drive_choice_ < 0 && !drives_.empty()) drive_choice_ = 0;
        }
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    keys();
    rail();
    ImGui::SameLine(0, 0);
    ImGui::BeginChild("content", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    switch (screen_) {
        case Screen::Home: screens::home(*this); break;
        case Screen::Job: screens::job(*this); break;
        case Screen::About: screens::about(*this); break;
    }
    ImGui::EndChild();
    dialogs::health(*this);
    dialogs::shortcuts(*this, shortcuts_open_);
    screens::parts::orientationQuestion(*this);
    screens::parts::sendDialog(*this);
    errorDialog();
    ImGui::End();
}

}  // namespace gui
