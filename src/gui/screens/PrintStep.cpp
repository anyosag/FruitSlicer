// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Job · Print: checks before saving, the print file's settings, saving it
// to a USB drive with a read-back check, and what to do at the printer
// (canvas boards "Export", "SaveAnyway" and "Saved").
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

#include "App.h"
#include "Format.h"
#include "Icons.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"

namespace gui::screens::parts {

namespace {

std::string duration(double s) {
    const long m = std::max(1L, std::lround(s / 60));
    return m >= 60 ? strf("%ld h %ld min", m / 60, m % 60) : strf("%ld min", m);
}

std::string fileName(const std::string& path) { return std::filesystem::path(path).filename().string(); }

std::string driveLabel(const platform::Volume& v, bool details) {
    std::string s = "USB drive · " + (v.name.empty() ? std::string("Untitled") : v.name);
    if (details) {
        if (!v.format.empty()) s += " · " + v.format;
        s += strf(" · %.1f GB free", static_cast<double>(v.free_bytes) / 1e9);
    }
    return s;
}

// The save that is running now, if any.
bool saving(App& app, JobProgress* out = nullptr) {
    const JobProgress p = app.jobs().progress();
    if (out) *out = p;
    return p.running && p.name.rfind("Saving ", 0) == 0;
}

void beforeYouSave(App& app, Document& doc, const PrintPlan* plan, float w, float h) {
    beginCard("before", ImVec2(w, h), 18, col::Panel, col::Border);
    const SupportResult& sup = *doc.supports.result;
    const LayerResult& lr = *doc.layers.result;
    const bool fits = fitsPlate(app.printer(), lr.bounds);
    const bool ready = plan && plan->ready;
    const bool blocks = !sup.validation.floating.empty() || !fits || !ready;
    {
        const float y = ImGui::GetCursorPosY();
        text("Before you save", fonts().semibold, size::Section + 1, col::Text);
        const char* state = blocks ? "Something blocks saving" : "Nothing blocks saving";
        ImGui::PushFont(fonts().medium, size::Body);
        const float sw = ImGui::CalcTextSize(state).x;
        ImGui::PopFont();
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + ImGui::GetContentRegionAvail().x - sw, y + 2));
        text(state, fonts().medium, size::Body, blocks ? col::Error : col::Ok);
    }
    ImGui::Dummy(ImVec2(0, 4));
    const std::size_t islands = sup.validation.floating.size();
    // When saving cuts the layers again at the file's height, those are not
    // the layers checked here.
    const std::string checked =
        ready && plan->reslice
            ? strf("Checked on all %s layers at %.2f mm with the supports; not on the %s layers at %.2f mm that "
                   "saving cuts",
                   thousands(lr.layers->size()).c_str(), lr.layers->layer_height, thousands(plan->layers).c_str(),
                   plan->layer_height)
            : strf("Checked on all %s layers with the supports", thousands(lr.layers->size()).c_str());
    verdictRow("mid-air", islands ? Verdict::Error : Verdict::Ok,
               islands ? "Parts start in mid-air" : "No part starts in mid-air",
               islands ? strf("%zu %s nothing under them. Add tips with the Supports tool.", islands,
                              islands == 1 ? "island has" : "islands have")
                             .c_str()
                       : checked.c_str());
    const fruit::PrinterProfile& pr = app.printer();
    verdictRow("fits", fits ? Verdict::Ok : Verdict::Error, fits ? "Fits the plate" : "Reaches past the plate",
               (dims(lr.bounds.size()) + strf(" with supports, of %.2f × %.2f × %.0f", pr.build_x, pr.build_y, pr.build_z))
                   .c_str());
    if (!ready) {
        verdictRow("settings", Verdict::Error, "No exposure times yet",
                   "Choose a SatelLite .goo saved for this printer, or type them under Print settings.");
    } else {
        verdictRow("raft", plan->raft_thick_enough ? Verdict::Ok : Verdict::Warn,
                   plan->raft_thick_enough ? "Raft thick enough" : "Raft thinner than the bottom layers",
                   doc.placement().on_plate
                       ? "It stands on the plate: no raft needed."
                       : strf("The bottom and transition layers need at least %.2f mm", plan->raft_needed).c_str());
    }
    const std::size_t review = sup.count(SupportProblem::Level::Review);
    if (review) {
        verdictRow("review", Verdict::Warn, strf("%zu %s to review", review, review == 1 ? "thing" : "things").c_str(),
                   "Listed under Checks, with the supports. Saving is still allowed.");
        if (linkButton("Review them")) {  // in the Job panel's checks, with Supports open
            app.openTool(Tool::Supports);
            app.jobTab() = 0;
        }
    }
    if (app.printerSetup().orientation_confirmed) {
        verdictRow("orientation", Verdict::Ok, "Image orientation confirmed",
                   "Checked against SatelLite's slice of the F; this file uses the fix it found.");
    } else {
        verdictRow("orientation", Verdict::Warn, "Image orientation not confirmed",
                   "Lettering and left- or right-handed parts could print mirrored. The check takes 5 minutes and "
                   "no resin.");
        if (linkButton("Run the check")) app.setScreen(Screen::OrientationCheck);
    }
    endCard();
}

void settingsCard(App& app, Document& doc, const PrintPlan* plan, float w, float h) {
    beginCard("settings", ImVec2(w, h), 18, col::Panel, col::Border);
    text("Print settings", fonts().semibold, size::Section + 1, col::Text);
    const PrintSettings& s = doc.print.settings;
    if (s.sample) {
        text(("From your SatelLite file " + fileName(s.sample_path)).c_str(), fonts().regular, size::Body,
             col::TextSecondary, true);
    } else {
        text("FruitSlicer copies exposure and motion settings from a print file SatelLite saved for this printer, "
             "so they are ones you have printed with. Or type the exposure times.",
             fonts().regular, size::Body, col::TextSecondary, true);
    }
    if (secondaryButton(s.sample ? "Choose another .goo..." : "Choose a SatelLite .goo...", ImVec2(0, 34)))
        app.chooseSample();
    ImGui::Dummy(ImVec2(0, 4));
    // Typed exposures: without a sample, or to override it.
    text(s.sample ? "Or override its exposures" : "Exposure times", fonts().semibold, size::Body, col::Text);
    double normal = s.exposure, bottom = s.bottom_exposure;
    const float fw = (ImGui::GetContentRegionAvail().x - 10) / 2;
    const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
    text("Normal layers, s", fonts().regular, size::Help, col::TextMuted);
    ImGui::SetCursorPos(ImVec2(x0 + fw + 10, y0));
    text("Bottom layers, s", fonts().regular, size::Help, col::TextMuted);
    ImGui::SetCursorPos(ImVec2(x0, y0 + 18));
    bool changed = numberField("exposure", &normal, fw, normal > 0 ? "%g" : "—", 0, 600);
    ImGui::SetCursorPos(ImVec2(x0 + fw + 10, y0 + 18));
    changed = numberField("bottom_exposure", &bottom, fw, bottom > 0 ? "%g" : "—", 0, 600) || changed;
    if (changed) app.setExposures(normal, bottom);
    ImGui::SetCursorPos(ImVec2(x0, y0 + 18 + 46));
    ImGui::Dummy(ImVec2(0, 0));  // the fields' height belongs to the card
    if (plan && plan->ready) {
        const fruit::GooHeader& hd = plan->setup.header;
        keyValue("Layer height", strf("%.2f mm", plan->layer_height));
        keyValue("Bottom layers", strf("%d × %.1f s", static_cast<int>(hd.print.bottom_layers), hd.print.bottom_exposure));
        if (doc.placement().on_plate)
            keyValue("Height correction", plan->base_layers > 0
                                              ? strf("%d layers, %g mm", static_cast<int>(plan->base_layers),
                                                     plan->base_layers * plan->layer_height)
                                              : std::string("none"));
        keyValue("Normal exposure", strf("%.2f s", hd.print.exposure));
        keyValue("Transition layers", strf("%d", static_cast<int>(hd.print.transition_layers)));
        keyValue("Wait after retract", strf("%.1f s", hd.print.wait_after_retract));
        if (hd.print.bottom_wait_before_lift != hd.print.wait_before_lift ||
            hd.print.bottom_wait_after_retract != hd.print.wait_after_retract)
            keyValue("Bottom-layer waits", strf("%g · %g s", hd.print.bottom_wait_before_lift,
                                                hd.print.bottom_wait_after_retract));
        if (plan->reslice) {
            ImGui::Dummy(ImVec2(0, 2));
            text(strf("Saving cuts the layers again at %.2f mm, the height these exposures are for.",
                      plan->layer_height)
                     .c_str(),
                 fonts().regular, size::Help, col::Warn, true);
        }
    }
    endCard();
}

// Print time, resin, layers and the file, as four tiles.
void printTiles(Document& doc, const PrintPlan* plan) {
    const float cw = (ImGui::GetContentRegionAvail().x - 10) / 2;
    auto tile = [&](const char* id, const char* label, const std::string& big, const std::string& small) {
        beginCard(id, ImVec2(cw, 82), 12, col::Card, col::Border);
        text(label, fonts().regular, size::Help, col::TextMuted);
        text(big.c_str(), fonts().bold, size::Section + 3, col::Text);
        text(small.c_str(), fonts().regular, size::Help, col::TextMuted);
        endCard();
    };
    const bool ready = plan && plan->ready;
    tile("time", "Print time", ready ? duration(plan->print_time_s) : "—", ready ? "estimate" : "needs exposures");
    ImGui::SameLine(0, 10);
    tile("resin", "Resin", strf("%.1f ml", doc.layers.result->resin_ml),
         strf("%.1f g at %g g/ml", doc.layers.result->resin_ml * resinDensity(doc.print.settings),
              resinDensity(doc.print.settings)));
    tile("layers", "Layers", ready ? thousands(plan->layers) : thousands(doc.layers.result->layers->size()),
         strf("%.1f mm tall", doc.layers.result->top));
    ImGui::SameLine(0, 10);
    tile("file", "File", ".goo", ready ? strf("%.2f mm layers", plan->layer_height) : "");
}

// Where it goes, and the button that saves or sends it (or the save running now).
void saveArea(App& app, Document& doc) {
    JobProgress jp;
    if (saving(app, &jp)) {
        // The save in progress: writing, then reading the file back.
        text(jp.name.c_str(), fonts().semibold, size::Body, col::Text);
        const char* what = jp.stage == "verify"  ? "Reading the file back to check every layer"
                           : jp.stage == "slice" ? "Cutting the layers at the file's height"
                                                 : "Writing the layers";
        text(what, fonts().regular, size::Help, col::TextSecondary);
        ImGui::Dummy(ImVec2(0, 2));
        const ImVec2 bp = ImGui::GetCursorScreenPos();
        const float bw = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + 6), col::Input, 3);
        dl->AddRectFilled(bp, ImVec2(bp.x + bw * static_cast<float>(jp.fraction), bp.y + 6), col::Accent, 3);
        ImGui::Dummy(ImVec2(bw, 12));
        if (secondaryButton("Cancel", ImVec2(0, 34))) app.jobs().cancel();
        return;
    }

    text("Save to", fonts().semibold, size::Body, col::Text);
    auto& drives = app.drives();
    int& choice = app.driveChoice();
    bool& wifi = app.sendOverWifi();
    const bool on_drive = !wifi && choice >= 0 && choice < static_cast<int>(drives.size());
    const std::string current = wifi       ? std::string("Printer over Wi-Fi")
                                : on_drive ? driveLabel(drives[static_cast<std::size_t>(choice)], false)
                                           : std::string("A folder you choose");
    ImGui::SetNextItemWidth(-1);
    ImGui::PushFont(fonts().regular, size::Body);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, col::Viewport);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, col::Viewport);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, col::Card);
    if (ImGui::BeginCombo("##save_to", current.c_str())) {
        for (int i = 0; i < static_cast<int>(drives.size()); ++i)
            if (ImGui::Selectable(driveLabel(drives[static_cast<std::size_t>(i)], true).c_str(), on_drive && choice == i)) {
                choice = i;
                wifi = false;
            }
        if (ImGui::Selectable("A folder you choose", !on_drive && !wifi)) {
            choice = -1;
            wifi = false;
        }
        if (ImGui::Selectable("Printer over Wi-Fi", wifi)) wifi = true;
        ImGui::EndCombo();
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
    if (wifi) {
        text("FruitSlicer writes the file, reads it back to check it, then finds your printer on the network.",
             fonts().regular, size::Help, col::TextMuted, true);
    } else if (on_drive) {
        text(strf("Saved as %s.goo at the top level of the drive.", doc.name.c_str()).c_str(),
             fonts().regular, size::Help, col::TextMuted, true);
    } else if (drives.empty()) {
        text("No USB drive found. Plug one in, or save to a folder and copy it over.", fonts().regular, size::Help,
             col::TextMuted, true);
    }
    ImGui::Dummy(ImVec2(0, 4));
    const std::string blocker = app.printBlocker();
    if (primaryButton(wifi ? "Send to printer" : "Save .goo", ImVec2(-1, 44), blocker.empty())) app.savePrint();
    if (!blocker.empty()) tooltip(blocker.c_str());
    ImGui::Dummy(ImVec2(0, 2));
    if (secondaryButton("Export STL instead", ImVec2(-1, 34), !app.jobs().busy())) app.exportStl(false);
    tooltip("The model and its supports as one mesh, for another slicer.");
    if (doc.supports.result && doc.supports.result->hollow) {
        // A mesh can't carry what was cut from the layers.
        ImGui::Dummy(ImVec2(0, 2));
        const HollowPlan& hp = *doc.supports.result->hollow;
        text(hp.hollowing()  ? "An STL has the model as it is: it is hollowed and drilled only in FruitSlicer's own "
                               "layers, so another slicer would print it solid."
             : hp.filling() ? "An STL has the model as it is: its hollows are filled only in FruitSlicer's own "
                              "layers, so another slicer would leave them sealed, full of resin."
                            : "An STL has the model as it is: its holes are only in FruitSlicer's own layers, so "
                              "another slicer would leave its hollows sealed.",
             fonts().regular, size::Help, col::Warn, true);
    }
}

void savedPage(App& app, Document& doc, ImVec2 size) {
    beginPanel("saved", ImVec2(0, size.y), col::App, ImVec2(36, 30));
    const SaveResult& r = *doc.print.saved;
    const std::string file = fileName(r.path);
    const bool ok = r.good;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddCircleFilled(ImVec2(p.x + 20, p.y + 20), 20, ok ? col::OkBg : col::ErrorBg, 32);
    if (ok) icon::check(dl, ImVec2(p.x + 10, p.y + 10), 20, col::Ok);
    else icon::cross(dl, ImVec2(p.x + 11, p.y + 11), 18, col::Error);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 54, p.y - 2));
    ImGui::BeginGroup();
    const std::string where = doc.print.drive.empty() ? " is saved" : " is on " + doc.print.drive;
    text((file + where).c_str(), fonts().bold, size::DialogTitle + 2, col::Text);
    const std::string took =
        app.options().deterministic ? std::string() : strf(" · written in %.0f s", std::max(1.0, r.seconds));
    text((strf("%.1f MB · %s layers", static_cast<double>(r.written.bytes) / 1e6, thousands(r.written.layers).c_str()) +
          took)
             .c_str(),
         fonts().regular, size::Body, col::TextSecondary);
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, 14));
    const float w = std::min(ImGui::GetContentRegionAvail().x, 1080.0f);
    if (!ok) {
        beginCard("bad", ImVec2(w, 0), 14, col::ErrorBg, IM_COL32(0x6B, 0x2A, 0x2A, 255));
        text("Don't print this file: it did not read back as it was written.", fonts().semibold, size::Body,
             col::Error);
        text("Save it again. If it fails twice, the drive may be failing: try another one.", fonts().regular,
             size::Body, col::TextSecondary, true);
        endCard();
        ImGui::Dummy(ImVec2(0, 6));
    }
    beginCard("checked", ImVec2(w * 0.55f, 0), 18, col::Panel, col::Border);
    text("Read back and checked", fonts().semibold, size::Section + 1, col::Text);
    ImGui::Dummy(ImVec2(0, 2));
    auto item = [&](bool good, const std::string& s) {
        ImDrawList* d2 = ImGui::GetWindowDrawList();
        const ImVec2 q = ImGui::GetCursorScreenPos();
        if (good) icon::check(d2, ImVec2(q.x, q.y + 2), 13, col::Ok);
        else icon::cross(d2, ImVec2(q.x + 1, q.y + 3), 11, col::Error);
        ImGui::SetCursorScreenPos(ImVec2(q.x + 22, q.y));
        text(s.c_str(), fonts().regular, size::Body, col::TextSecondary, true);
    };
    item(r.check.checksums_ok, r.check.checksums_ok
                                   ? strf("All %s layers read back with valid checksums",
                                          thousands(r.check.layers).c_str())
                                   : strf("Layer %d does not read back: ", r.check.first_bad_layer + 1) + r.check.error);
    item(r.check.grey_values == 0, r.check.grey_values == 0 ? "Pure black and white pixels, no grey"
                                                            : strf("%zu grey levels", r.check.grey_values));
    item(r.volume_matches, strf("Decoded volume %.2f ml %s what was written", r.check.volume_mm3 / 1000.0,
                                r.volume_matches ? "matches" : "does not match"));
    if (r.check.layers != r.written.layers)
        item(false, strf("%u layers read back of %u written", r.check.layers, r.written.layers));
    if (!r.check.footer_ok) item(false, "The file's end marker is missing: it may be cut short");
    endCard();
    ImGui::SameLine(0, 16);
    beginCard("the_print", ImVec2(w * 0.45f - 16, 0), 18, col::Panel, col::Border);
    text("The print", fonts().semibold, size::Section + 1, col::Text);
    keyValue("Print time", "about " + duration(r.written.print_time_s));
    keyValue("Resin", strf("%.1f ml", r.written.volume_mm3 / 1000.0));
    keyValue("Supports", strf("%zu tips on %zu trunks", doc.supports.result->tips, doc.supports.result->trunks));
    endCard();
    ImGui::Dummy(ImVec2(0, 14));
    beginCard("next", ImVec2(w, 0), 18, col::Panel, col::Border);
    text("Next, at the printer", fonts().semibold, size::Section + 1, col::Text);
    ImGui::Dummy(ImVec2(0, 4));
    const bool drive = !doc.print.drive.empty();
    const std::string fill =
        strf("It uses about %.0f ml of resin. Stay under the vat's max line.", r.written.volume_mm3 / 1000.0);
    const std::string steps[][2] = {
        {drive ? "Eject and plug in" : "Copy it to a USB drive",
         drive ? "Put the drive in the printer's USB port." : "Then eject the drive and plug it into the printer."},
        {"Fill the vat", fill},
        {"Pick the file", "Choose it on the printer's screen and start."},
        {"After printing", "Scrape the raft off the plate, wash the print, then clip the supports at the tips."}};
    const float sw = (w - 36 - 30) / 4;
    for (int i = 0; i < 4; ++i) {
        ImGui::PushID(i);
        beginCard("s", ImVec2(sw, 96), 12, col::Card, col::Border);
        text(strf("%d. %s", i + 1, steps[i][0].c_str()).c_str(), fonts().semibold, size::Body, col::Text);
        text(steps[i][1].c_str(), fonts().regular, size::Help, col::TextSecondary, true);
        endCard();
        ImGui::PopID();
        if (i < 3) ImGui::SameLine(0, 10);
    }
    endCard();
    ImGui::Dummy(ImVec2(0, 14));
    const bool busy = app.jobs().busy();
    if (drive) {
        if (primaryButton("Eject the drive", ImVec2(140, 40), !busy)) app.ejectDrive();
        ImGui::SameLine(0, 10);
        if (secondaryButton("Show in Finder", ImVec2(0, 40))) app.revealSaved();
    } else if (primaryButton("Show in Finder", ImVec2(140, 40))) {
        app.revealSaved();
    }
    ImGui::SameLine(0, 10);
    if (secondaryButton("Back to the job", ImVec2(0, 40))) app.dismissSaved();
    endPanel();
}

}  // namespace

void savedView(App& app, Document& doc, ImVec2 size) { savedPage(app, doc, size); }

bool showingSaved(const Document& doc) {
    return doc.print.saved && doc.layers.result && doc.print.saved_from == doc.layers.result;
}

void needCard(App& app, Document& doc, Need need, ImVec2 size) {
    beginPanel("need", ImVec2(0, size.y), col::App, ImVec2(0, 0));
    const ImVec2 cs(480, 0);
    ImGui::SetCursorPos(ImVec2(std::round((size.x - cs.x) / 2), std::round(size.y / 2) - 110));
    beginCard("need_card", cs, 20, col::Panel, col::BorderStrong);
    const Part* off = doc.partOff();  // the selected part, for Rotate, when it doesn't fit
    const fruit::PlateFit& f = off ? off->fit() : doc.fit();
    const bool several = doc.partCount() > 1;
    const char* title = "";
    const char* body = "";
    switch (need) {
        case Need::Nothing: break;
        case Need::Fit:
            title = f.overEdge() ? (several ? "A part is over the plate's edge" : "It's over the plate's edge")
                                 : (several ? "A part doesn't fit the plate yet" : "It doesn't fit the plate yet");
            body = "Supports and layers are made for the model where it stands. Bring it onto the plate with Move, "
                   "Rotate or Scale first.";
            break;
        case Need::Supports:
            title = "No supports yet";
            body = "The layers are cut from the model with its supports. Nothing else has to come first: Rotate › "
                   "Find a good rotation can help choose how it stands.";
            break;
        case Need::FreshSupports:
            title = "The supports are out of date";
            body = "The model or the support settings changed after they were made, so layers cut now wouldn't match "
                   "it. Remake them, or undo the change (⌘Z) to have these back.";
            break;
        case Need::Layers:
            title = "The layers aren't cut yet";
            body = "Layers cuts them from the model and its supports; it takes a moment.";
            break;
    }
    text(title, fonts().semibold, size::Section + 2, col::Text);
    text(body, fonts().regular, size::Body, col::TextSecondary, true);
    ImGui::Dummy(ImVec2(0, 6));
    switch (need) {
        case Need::Nothing: break;
        case Need::Fit:
            if (f.overEdge()) {
                if (primaryButton("Move it back on the plate", ImVec2(0, 38))) app.slideOntoPlate();
            } else if (f.fits_turned) {
                if (primaryButton("Turn 90° to fit", ImVec2(0, 38))) app.turnQuarter();
            } else if (primaryButton("Scale to fit", ImVec2(0, 38))) {
                app.scaleToFit();
            }
            break;
        case Need::Supports:
            if (primaryButton("Make supports", ImVec2(0, 38))) app.supportsNow();
            break;
        case Need::FreshSupports:
            if (primaryButton("Remake supports", ImVec2(0, 38))) app.supportsNow();
            ImGui::SameLine(0, 8);
            if (secondaryButton("Back to Prepare", ImVec2(0, 38))) app.goToPrepare();
            break;
        case Need::Layers:
            if (primaryButton("Slice", ImVec2(0, 38))) app.goToStep(Step::Layers);
            break;
    }
    endCard();
    endPanel();
}

// Layers mode's side panel: this print, what has been checked before saving,
// the print settings (folded away), and saving it.
void printSide(App& app, Document& doc, float h) {
    const auto plan = app.printPlan();
    beginPanel("print_side", ImVec2(kJobW, h), col::Panel, ImVec2(16, 16));
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetWindowPos();
        dl->AddLine(p, ImVec2(p.x, p.y + h), col::Border);
    }
    text("This print", fonts().semibold, size::Section + 1, col::Text);
    ImGui::Dummy(ImVec2(0, 2));
    const float save_h = doc.supports.result && doc.supports.result->hollow ? 270 : 222;
    ImGui::BeginChild("print_details", ImVec2(0, std::max(120.0f, ImGui::GetContentRegionAvail().y - save_h)),
                      ImGuiChildFlags_None);
    printTiles(doc, plan.get());
    ImGui::Dummy(ImVec2(0, 4));
    beforeYouSave(app, doc, plan.get(), 0, 0);
    ImGui::Dummy(ImVec2(0, 4));
    {
        // Print settings, folded away until wanted.
        bool& open = app.printSettingsOpen();
        const PrintSettings& st = doc.print.settings;
        const std::string summary = plan && plan->ready ? strf("%.2g s · bottom %.0f s", plan->setup.header.print.exposure,
                                                               plan->setup.header.print.bottom_exposure)
                                    : st.exposure > 0 ? strf("%.2g s · bottom %.0f s", st.exposure, st.bottom_exposure)
                                                      : std::string("not set yet");
        const float w = ImGui::GetContentRegionAvail().x;
        const ImVec2 q = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("settings_fold", ImVec2(w, 40))) open = !open;
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(q, ImVec2(q.x + w, q.y + 40), hovered ? col::Active : col::Panel, 10);
        dl->AddRect(q, ImVec2(q.x + w, q.y + 40), col::Border, 10);
        ImGui::PushFont(fonts().semibold, size::Body);
        dl->AddText(ImVec2(q.x + 12, q.y + 12), col::Text, "Print settings");
        ImGui::PopFont();
        ImGui::PushFont(fonts().regular, size::Help);
        const std::string right = summary + (open ? "  ▴" : "  ▾");
        const float rw = ImGui::CalcTextSize(right.c_str()).x;
        dl->AddText(ImVec2(q.x + w - 12 - rw, q.y + 13), plan && plan->ready ? col::TextMuted : col::Warn, right.c_str());
        ImGui::PopFont();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (open) {
            ImGui::Dummy(ImVec2(0, 2));
            settingsCard(app, doc, plan.get(), 0, 0);
        }
    }
    ImGui::EndChild();
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x - 16, p.y + 4), ImVec2(p.x + w + 16, p.y + 4), col::Border);
        ImGui::Dummy(ImVec2(w, 12));
    }
    saveArea(app, doc);
    endPanel();
}

void orientationQuestion(App& app) {
    bool& open = app.orientationAsk();
    if (open && !ImGui::IsPopupOpen("orientation_q")) ImGui::OpenPopup("orientation_q");
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + 140), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowSize(ImVec2(560, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 22));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, col::Panel);
    ImGui::PushStyleColor(ImGuiCol_Border, col::BorderStrong);
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(5, 6, 8, 184));
    if (ImGui::BeginPopupModal("orientation_q", nullptr,
                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        if (!open) ImGui::CloseCurrentPopup();
        text("Save before checking the image orientation?", fonts().bold, size::DialogTitle, col::Text);
        ImGui::Dummy(ImVec2(0, 2));
        text("FruitSlicer hasn't confirmed which way this printer flips the layer images. If the guess is wrong the "
             "print still works, but it comes out mirrored: lettering reads backwards and left-handed parts become "
             "right-handed.",
             fonts().regular, size::Body, col::TextSecondary, true);
        ImGui::Dummy(ImVec2(0, 2));
        text("The check, under Calibrate, takes 5 minutes and no resin. Until it confirms the orientation, "
             "FruitSlicer asks once each time it starts.",
             fonts().regular, size::Body, col::TextSecondary, true);
        ImGui::Dummy(ImVec2(0, 10));
        if (secondaryButton("Run the check", ImVec2(0, 40))) {
            open = false;
            ImGui::CloseCurrentPopup();
            app.setScreen(Screen::OrientationCheck);
        }
        ImGui::SameLine(0, 0);
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 24 - 110 - 10 - 80);
        if (secondaryButton("Cancel", ImVec2(80, 40)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(0, 10);
        if (primaryButton("Save anyway", ImVec2(110, 40))) {
            open = false;
            ImGui::CloseCurrentPopup();
            app.savePrintAfterAsking();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
}

}  // namespace gui::screens::parts
