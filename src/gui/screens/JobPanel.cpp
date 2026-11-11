// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The Job panel at the window's right in Prepare (canvas "Job panel"): the
// printer and resin, the parts on and off the plate, the checks and hollows,
// and the main button, which is always the next thing the job needs: Open a
// model, Make supports, Remake supports, Slice.
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "App.h"
#include "Format.h"
#include "Hollows.h"
#include "Icons.h"
#include "Repairs.h"
#include "Resins.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"

namespace gui::screens::parts {

namespace {

constexpr float kMainH = 112;  // the note and the main button, at the panel's foot

void rule() {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x - 16, p.y), ImVec2(p.x + w + 16, p.y), col::Border);
    ImGui::Dummy(ImVec2(w, 1));
}

// A label at the left and what it is at x = 96.
void fact(const char* label, const std::string& value) {
    const float y = ImGui::GetCursorPosY(), x = ImGui::GetCursorPosX();
    text(label, fonts().regular, size::Body, col::TextMuted);
    ImGui::SetCursorPos(ImVec2(x + 96, y));
    text(value.c_str(), fonts().regular, size::Body, col::Text);
}

// A row in the checks: a mark, a title and a line under it.
enum class Tick { Ok, Warn, Error, Todo };

void checkLine(const char* id, Tick t, const char* title, const std::string& detail) {
    const ImU32 bg = t == Tick::Error ? col::ErrorBg : t == Tick::Warn ? col::WarnBg : t == Tick::Todo ? col::Panel : col::Card;
    const ImU32 border = t == Tick::Error ? IM_COL32(0x6B, 0x2A, 0x2A, 255) : t == Tick::Warn ? col::WarnBorder : col::Border;
    beginCard(id, ImVec2(0, 0), 10, bg, border);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 c(p.x + 9, p.y + 9);
    switch (t) {
        case Tick::Ok:
            dl->AddCircleFilled(c, 9, col::OkBg, 20);
            icon::check(dl, ImVec2(p.x + 3, p.y + 3), 12, col::Ok);
            break;
        case Tick::Warn:
            dl->AddCircleFilled(c, 9, IM_COL32(0x4A, 0x3A, 0x12, 255), 20);
            ImGui::PushFont(fonts().bold, size::Help);
            dl->AddText(ImVec2(c.x - 2, c.y - 7), col::Warn, "!");
            ImGui::PopFont();
            break;
        case Tick::Error:
            dl->AddCircleFilled(c, 9, IM_COL32(0x4A, 0x1C, 0x1C, 255), 20);
            icon::cross(dl, ImVec2(p.x + 4, p.y + 4), 10, col::Error);
            break;
        case Tick::Todo:
            for (int i = 0; i < 12; i += 2) {
                const float a0 = i * 3.14159265f / 6, a1 = (i + 1) * 3.14159265f / 6;
                dl->PathArcTo(c, 7, a0, a1, 4);
                dl->PathStroke(col::TextMuted, 1.4f);
            }
            break;
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + 28, p.y));
    ImGui::BeginGroup();
    text(title, fonts().semibold, size::Body, t == Tick::Todo ? col::TextSecondary : col::Text);
    if (!detail.empty()) text(detail.c_str(), fonts().regular, size::Help, col::TextMuted, true);
    ImGui::EndGroup();
    endCard();
}

// The resin this job prints in, as a chip: a click opens the resins.
void resinChooser(App& app, Document* doc) {
    const std::string label = doc ? resinLabel(doc->supports.choices.resin, doc->supports.choices.profile)
                                  : resinLabel(app.newJobResin().name, app.newJobResin().profile);
    const ImVec2 chip_at = ImGui::GetCursorScreenPos();
    if (chip("resin", label.c_str(), col::TextMuted, col::Text, col::Input, col::BorderStrong) && doc)
        ImGui::OpenPopup("resin_menu");
    tooltip(doc ? "The resin this job is printed in: its print settings and support values. Click to change it."
                : "The resin new jobs start with.");
    if (app.resinMenu() && doc) {  // opened by a script
        ImGui::OpenPopup("resin_menu");
        app.resinMenu() = false;
    }
    ImGui::SetNextWindowPos(ImVec2(chip_at.x - 120, chip_at.y + 36));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, col::Panel);
    ImGui::PushStyleColor(ImGuiCol_Border, col::BorderStrong);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10);
    if (ImGui::BeginPopup("resin_menu")) {
        text("Print this job in", fonts().semibold, size::Help, col::TextMuted);
        ImGui::Dummy(ImVec2(0, 2));
        for (const Resin& r : app.resins()) {
            ImGui::PushID(r.id.c_str());
            const bool now = doc && doc->supports.choices.resin == r.name && doc->supports.choices.profile == r.profile;
            const auto sample = app.resinSample(r);
            const std::string sub = resinSummary(r, sample.get());
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("pick", ImVec2(300, 44))) app.useResinForJob(r.id);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (ImGui::IsItemHovered() || now) dl->AddRectFilled(p, ImVec2(p.x + 300, p.y + 44), col::Card, 8);
            if (now) icon::check(dl, ImVec2(p.x + 8, p.y + 14), 14, col::Accent);
            ImGui::PushFont(fonts().semibold, size::Body);
            dl->AddText(ImVec2(p.x + 30, p.y + 6), col::Text, r.name.c_str());
            ImGui::PopFont();
            ImGui::PushFont(fonts().regular, size::Help);
            dl->AddText(ImVec2(p.x + 30, p.y + 24), col::TextMuted, sub.c_str());
            ImGui::PopFont();
            ImGui::PopID();
        }
        ImGui::Dummy(ImVec2(0, 2));
        if (linkButton("Manage resins…")) {
            ImGui::CloseCurrentPopup();
            app.resinSelected() = app.newJobResin().id;
            app.setScreen(Screen::Resins);
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void jobSection(App& app, Document* doc) {
    text("Job", fonts().semibold, size::Section, col::Text);
    ImGui::Dummy(ImVec2(0, 2));
    const fruit::PrinterProfile& pr = app.printer();
    {
        const float y = ImGui::GetCursorPosY(), x = ImGui::GetCursorPosX();
        text("Printer", fonts().regular, size::Body, col::TextMuted);
        ImGui::SetCursorPos(ImVec2(x + 96, y));
        text(pr.name.c_str(), fonts().regular, size::Body, col::Text);
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPos(ImVec2(x + w - 26, y - 5));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("printer_menu", ImVec2(26, 26))) ImGui::OpenPopup("printer_popup");
        const bool h = ImGui::IsItemHovered();
        if (h) ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + 26, p.y + 26), col::Active, 7);
        icon::dots(ImGui::GetWindowDrawList(), ImVec2(p.x + 3, p.y + 3), 20, col::TextSecondary);
        tooltip("Printer settings and calibration");
        ImGui::PushStyleColor(ImGuiCol_PopupBg, col::Panel);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
        if (ImGui::BeginPopup("printer_popup")) {
            if (ImGui::Selectable("Printer settings…")) app.setScreen(Screen::Printer);
            if (ImGui::Selectable("Calibrate…")) app.setScreen(Screen::Calibrate);
            if (ImGui::Selectable("Check the image orientation…")) app.setScreen(Screen::OrientationCheck);
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::SetCursorPos(ImVec2(x, y + 24));
    }
    {
        const float y = ImGui::GetCursorPosY(), x = ImGui::GetCursorPosX();
        ImGui::SetCursorPosY(y + 6);
        text("Resin", fonts().regular, size::Body, col::TextMuted);
        ImGui::SetCursorPos(ImVec2(x + 96, y));
        resinChooser(app, doc);
    }
    fact("Plate", strf("%.2f × %.2f mm, %.0f mm tall", pr.build_x, pr.build_y, pr.build_z));
    std::string resin = "—";
    if (doc) {
        double model_ml = 0;
        for (const Part& part : doc->parts()) {
            const double k = part.placement().scale;
            model_ml += part.check().volume_ml * k * k * k;
        }
        if (doc->layers.fresh(doc->supports.result)) resin = strf("%.1f ml with supports", doc->layers.result->resin_ml);
        else if (doc->supports.result && doc->supports.fresh(doc->revision(), doc->supportSettings()))
            resin = strf("%.1f ml + %.1f ml of supports", model_ml, doc->supports.result->support_ml);
        else resin = strf("%.1f ml for the model", model_ml);
    }
    fact("Resin to print", resin);
}

// The Collisions check's verdict, as one line of the Checks tab.
void collisionsLine(App& app, const Document& d) {
    const CollisionReport* r = app.collisions();
    if (!r) {
        checkLine("collisions", Tick::Todo, "No parts overlap",
                  app.checkingCollisions() ? "Not checked yet: checking now." : "Not checked yet.");
        return;
    }
    if (!r->problems.empty()) {
        const int over = r->overlapping();
        const std::size_t n = r->problems.size();
        checkLine("collisions", over ? Tick::Error : Tick::Warn,
                  over ? (over == 1 ? "Two parts overlap" : "Parts overlap") : (n == 1 ? "Two parts are close" : "Parts are close"),
                  strf("%zu %s than %g mm apart, or overlap%s.", n, n == 1 ? "pair is closer" : "pairs are closer", r->min_gap,
                       n == 1 ? "s" : ""));
        if (linkButton("Open Collisions")) app.jobTab() = 2;
        return;
    }
    if (!r->not_checked.empty()) {
        const Part* p = d.findPart(r->not_checked.front().part);
        checkLine("collisions", Tick::Warn, "Not every part was checked",
                  (p ? p->name() : std::string("A part")) + ": " + r->not_checked.front().why + ".");
        if (linkButton("Open Collisions")) app.jobTab() = 2;
        return;
    }
    checkLine("collisions", Tick::Ok, "No parts overlap or touch",
              r->pairs_checked == 1 ? strf("Checked where they are now: they are %g mm or more apart.", r->min_gap)
                                    : strf("%d pairs checked: none is closer than %g mm.", r->pairs_checked, r->min_gap));
}

void checksTab(App& app, Document* doc) {
    if (!doc) {
        checkLine("none", Tick::Todo, "Nothing checked yet",
                  "Open a model: FruitSlicer checks the mesh, then its size against the plate.");
        return;
    }
    Document& d = *doc;
    if (d.partCount() > 1) {
        // Each part alone: whether it fits where it is, and its mesh.
        std::vector<std::string> off, open, check, repaired;
        for (const Part& part : d.parts()) {
            if (!part.fit().fits) off.push_back(part.name());
            const RepairWords r = repairWords(part.check());
            if (r.level == RepairWords::Level::Open) open.push_back(part.name());
            else if (r.level == RepairWords::Level::Check) check.push_back(part.name());
            else if (r.level == RepairWords::Level::Repaired) repaired.push_back(part.name());
        }
        auto names = [](const std::vector<std::string>& v) {
            std::string s;
            for (std::size_t i = 0; i < v.size(); ++i) s += (i ? (i + 1 == v.size() ? " and " : ", ") : "") + v[i];
            return s;
        };
        const std::size_t n = d.partCount();
        if (off.empty())
            checkLine("fit", Tick::Ok, "Every part fits the plate", strf("%zu parts, where they are now.", n));
        else
            checkLine("fit", Tick::Error, off.size() == 1 ? "A part doesn't fit the plate" : "Parts don't fit the plate",
                      names(off) + ": the Parts list above has the fix.");
        if (!open.empty())
            checkLine("mesh", Tick::Warn, "Not watertight", names(open) + ". Select it to see what the check found.");
        else if (!check.empty())
            checkLine("mesh", Tick::Warn, "Mesh repaired: check the patch", names(check) + ".");
        else if (!repaired.empty())
            checkLine("mesh", Tick::Ok, "Every mesh is closed",
                      names(repaired) + (repaired.size() == 1 ? " was" : " were") + " repaired when opened.");
        else
            checkLine("mesh", Tick::Ok, "Every mesh is clean", "Each was checked when it opened: closed, nothing repaired.");
        bool patched = false;
        for (const Part& part : d.parts()) patched = patched || part.check().repair.patch_faces > 0;
        if (patched && linkButton("Show the patch")) app.showPatch();
        collisionsLine(app, d);
    } else {
    const fruit::PlateFit& f = d.fit();
    if (f.fits) checkLine("fit", Tick::Ok, "Fits the plate", dims(f.size) + ", where it is now.");
    else checkLine("fit", Tick::Error, f.overEdge() ? "Over the plate's edge" : "Too big for the plate",
                   f.fits_turned && !f.overEdge() ? "Turned 90° it fits at this size." : dims(f.size));
    {
        const fruit::MeshCheck& check = d.part().check();
        const RepairWords r = repairWords(check);
        using L = RepairWords::Level;
        if (r.level == L::Open) checkLine("mesh", Tick::Warn, "Not watertight", r.brief);
        else if (r.level == L::Check) checkLine("mesh", Tick::Warn, "Mesh repaired: check the patch", r.brief);
        else if (r.level == L::Repaired) checkLine("mesh", Tick::Ok, "Mesh repaired", r.brief);
        else
            checkLine("mesh", Tick::Ok, "Mesh is clean",
                      (check.shells == 1 ? std::string("One closed shell") : std::to_string(check.shells) + " closed shells") +
                          ", nothing flipped.");
        if (check.repair.patch_faces > 0 && linkButton("Show the patch")) app.showPatch();
    }
    }
    if (const std::size_t n = hollowsToFix(d)) {
        checkLine("hollows", Tick::Warn, n == 1 ? "A hollow traps resin" : "Hollows trap resin",
                  "The Hollow tool shows the fixes.");
        if (linkButton("Open Hollow")) app.openTool(Tool::Hollow);
    }
    const SupportState& s = d.supports;
    const bool fresh = s.result && s.fresh(d.revision(), d.supportSettings());
    if (fresh) {
        supportChecks(app, d);
    } else {
        if (s.result)
            checkLine("stale", Tick::Warn, "Supports out of date",
                      "Made before the last change to the model or the settings. Undo brings them back.");
        const char* todo = s.result ? "Not checked: the supports are out of date." : "Not checked yet: needs supports.";
        checkLine("midair", Tick::Todo, "No part starts in mid-air", todo);
        checkLine("raft", Tick::Todo, "Raft comes off in one piece", todo);
        checkLine("stands", Tick::Todo, "No support stands on the model", todo);
    }
    if (!app.printerSetup().orientation_confirmed) {
        checkLine("orientation", Tick::Warn, "Image orientation not confirmed",
                  "Lettering could print mirrored. The check takes 5 minutes and no resin.");
        if (linkButton("Run the check")) app.setScreen(Screen::OrientationCheck);
    }
}

void hollowsTab(App& app, Document* doc) {
    if (!doc) {
        text("Open a model first.", fonts().regular, size::Body, col::TextMuted);
        return;
    }
    const HollowOutcome s = hollowSummary(*doc);
    checkLine("summary", s.ok ? Tick::Ok : Tick::Warn, s.title.c_str(), s.detail);
    ImGui::Dummy(ImVec2(0, 2));
    if (doc->hollow.plan) {
        hollowOutcomesList(app, *doc);
        ImGui::Dummy(ImVec2(0, 2));
    }
    if (secondaryButton("Open Hollow", ImVec2(0, 32))) app.openTool(Tool::Hollow);
}

// Tabs side by side, as one control.
void tabs(int& at, const std::vector<std::string>& labels) {
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 36), col::App, 10);
    dl->AddRect(p, ImVec2(p.x + w, p.y + 36), col::Border, 10);
    const int n = static_cast<int>(labels.size());
    const float tw = (w - 6) / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
        const ImVec2 q(p.x + 3 + static_cast<float>(i) * tw, p.y + 3);
        ImGui::SetCursorScreenPos(q);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("tab", ImVec2(tw, 30))) at = i;
        const bool h = ImGui::IsItemHovered();
        ImGui::PopID();
        if (at == i) dl->AddRectFilled(q, ImVec2(q.x + tw, q.y + 30), col::Active, 8);
        ImGui::PushFont(fonts().semibold, size::Body);
        const ImVec2 ts = ImGui::CalcTextSize(labels[static_cast<std::size_t>(i)].c_str());
        dl->AddText(ImVec2(q.x + (tw - ts.x) / 2, q.y + (30 - ts.y) / 2), at == i || h ? col::Text : col::TextMuted,
                    labels[static_cast<std::size_t>(i)].c_str());
        ImGui::PopFont();
        if (h) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 36));
    ImGui::Dummy(ImVec2(w, 4));
}

// The note over the main button, the fix beside it when one comes first,
// and the button itself (a bar while its work runs).
void mainArea(App& app, Document* doc) {
    const JobProgress jp = app.jobs().progress();
    const bool making = jp.running && jp.name == "Generating supports";
    const bool cutting = jp.running && jp.name == "Cutting layers";
    const MainButton mb = mainButton(doc);
    std::string note;
    const char* fix = nullptr;
    const Part* off = nullptr;  // the first part that doesn't fit: the fix is for it
    if (!doc) {
        note = "Nothing on the plate yet.";
    } else if (!doc->fits() && (mb.action == MainAction::MakeSupports || mb.action == MainAction::RemakeSupports)) {
        off = doc->partOff();
        const fruit::PlateFit& f = off->fit();
        note = doc->partCount() > 1 ? "A part doesn't fit the plate where it is yet."
                                    : "It doesn't fit the plate where it is yet.";
        fix = f.overEdge() ? "Move it back on" : f.fits_turned ? "Turn 90°" : "Scale to fit";
    } else if (mb.action == MainAction::MakeSupports) {
        note = "No supports yet. Slice comes once they're made.";
    } else if (mb.action == MainAction::RemakeSupports) {
        note = "The supports were made before the last change.";
    } else if (mb.action == MainAction::Slice) {
        note = "Cuts the model and its supports into layers.";
    } else {
        note = "The layers are cut for these supports.";
    }
    const float w = ImGui::GetContentRegionAvail().x;
    {
        const float y = ImGui::GetCursorPosY();
        float fw = 0;
        if (fix) {
            ImGui::PushFont(fonts().semibold, size::Body);
            fw = ImGui::CalcTextSize(fix).x + 28;
            ImGui::PopFont();
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - (fix ? fw + 10 : 0));
        text(note.c_str(), fonts().regular, size::Help, col::TextSecondary, true);
        ImGui::PopTextWrapPos();
        if (fix) {
            const float after = ImGui::GetCursorPosY();
            ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + w - fw, y - 4));
            if (secondaryButton(fix, ImVec2(fw, 30))) {
                const fruit::PlateFit f = off->fit();
                app.selectPart(off->id());  // the fixes act on the selected part
                if (f.overEdge()) app.slideOntoPlate();
                else if (f.fits_turned) app.turnQuarter();
                else app.scaleToFit();
            }
            ImGui::SetCursorPosY(std::max(after, y + 30));
        }
    }
    ImGui::Dummy(ImVec2(0, 4));
    if (making || cutting) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 46), col::Active, 10);
        dl->AddRectFilled(p, ImVec2(p.x + w * static_cast<float>(jp.fraction), p.y + 46), IM_COL32(0xFF, 0x8F, 0x3F, 90), 10);
        const std::string label = strf("%s %d %%", making ? "Making supports…" : "Slicing…",
                                       static_cast<int>(std::lround(jp.fraction * 100)));
        ImGui::PushFont(fonts().bold, size::Body + 1);
        const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        dl->AddText(ImVec2(p.x + (w - ts.x) / 2, p.y + (46 - ts.y) / 2), col::Text, label.c_str());
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(w, 46));
        if (linkButton("Cancel")) app.jobs().cancel();
        return;
    }
    const bool busy = app.jobs().busy() && !app.checkingCollisions();  // the check gives way to it
    if (!mb.enabled || (busy && doc)) {
        // Greyed: what it would do, and (in the note above) why not yet.
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 46), col::Card, 10);
        dl->AddRect(p, ImVec2(p.x + w, p.y + 46), col::Border, 10);
        ImGui::PushFont(fonts().bold, size::Body + 1);
        const ImVec2 ts = ImGui::CalcTextSize(mainLabel(mb.action));
        dl->AddText(ImVec2(p.x + (w - ts.x) / 2, p.y + (46 - ts.y) / 2), IM_COL32(0x8A, 0x92, 0xA0, 255), mainLabel(mb.action));
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(w, 46));
        if (busy && doc) tooltip("Wait for the work running now.");
        return;
    }
    if (primaryButton(mainLabel(mb.action), ImVec2(-1, 46))) {
        switch (mb.action) {
            case MainAction::OpenModel: app.openDialog(); break;
            case MainAction::MakeSupports:
            case MainAction::RemakeSupports: app.supportsNow(); break;
            case MainAction::Slice:
            case MainAction::ShowLayers: app.goToStep(Step::Layers); break;
        }
    }
}

}  // namespace

void jobPanel(App& app, float h) {
    beginPanel("job_panel", ImVec2(kJobW, h), col::Panel, ImVec2(16, 14));
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetWindowPos();
        dl->AddLine(p, ImVec2(p.x, p.y + h), col::Border);
    }
    const auto& owned = app.document();
    Document* doc = owned.get();
    jobSection(app, doc);
    ImGui::Dummy(ImVec2(0, 4));
    rule();
    ImGui::Dummy(ImVec2(0, 4));
    partsList(app, doc);
    ImGui::Dummy(ImVec2(0, 4));
    rule();
    ImGui::Dummy(ImVec2(0, 4));
    int& tab = app.jobTab();
    std::size_t hollows = 0;
    if (doc)
        for (const Part& part : doc->parts()) hollows += static_cast<std::size_t>(std::max(0, part.check().cavities));
    std::string collisions_label = "Collisions";
    if (const CollisionReport* cr = doc ? app.collisions() : nullptr)
        if (!cr->problems.empty()) collisions_label += strf(" · %zu", cr->problems.size());
    tabs(tab, {"Checks", strf("Hollows · %zu", hollows), collisions_label});
    const float list_h = std::max(80.0f, ImGui::GetContentRegionAvail().y - kMainH);
    ImGui::BeginChild("job_checks", ImVec2(0, list_h), ImGuiChildFlags_None);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 6));
    if (tab == 0) checksTab(app, doc);
    else if (tab == 1) hollowsTab(app, doc);
    else collisionsTab(app, doc);
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, 2));
    rule();
    ImGui::Dummy(ImVec2(0, 6));
    mainArea(app, doc);
    endPanel();
}

}  // namespace gui::screens::parts
