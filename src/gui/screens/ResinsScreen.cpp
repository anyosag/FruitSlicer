// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Resins (canvas board "Resins"): each resin's print settings and the
// support values measured for it, with where each value comes from.
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

#include "App.h"
#include "Format.h"
#include "Icons.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"
#include "screens/Screens.h"

namespace gui::screens {

namespace {

using parts::tooltip;

constexpr float kListW = 300;

float textWidth(const char* s, ImFont* f, float sz) {
    ImGui::PushFont(f, sz);
    const float w = ImGui::CalcTextSize(s).x;
    ImGui::PopFont();
    return w;
}

// Words on a row's control line, centred on its 30 px fields.
void inlineWords(const char* s, ImU32 c, float row_y) {
    ImGui::PushFont(fonts().regular, size::Body);
    const ImVec2 ts = ImGui::CalcTextSize(s);
    ImGui::SetCursorPosY(row_y);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x, p.y + std::round((29 - ts.y) / 2)), c, s);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(ts.x, 29));
}

// One value you can change: the label on the left, its fields on the right
// and where it comes from at the far right. When the value is yours, what it
// was underneath, with a way back; and anything not checked about it.
struct EditRow {
    std::string label;
    std::string help;  // the label's tooltip
    float value_w = 0;
    std::function<void(float row_y)> value;
    parts::SourceBadge badge = parts::SourceBadge::Default;
    std::string was;   // "File: 1.5 s"; "" for no way back
    std::string note;  // shown in warning colour
};

// True when the way back was clicked.
bool editRow(const EditRow& e, bool first) {
    using parts::kSourceBadgeW;
    ImGui::PushID(e.label.c_str());
    const float w = ImGui::GetContentRegionAvail().x;
    if (!first) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + w, p.y), col::Border);
    }
    ImGui::Dummy(ImVec2(w, 8));
    const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
    const float left_w = w - e.value_w - kSourceBadgeW - 24;
    ImGui::SetCursorPos(ImVec2(x0, y0 + 6));
    ImGui::PushTextWrapPos(x0 + left_w);
    text(e.label.c_str(), fonts().regular, size::Body, col::TextSecondary, true);
    ImGui::PopTextWrapPos();
    if (!e.help.empty()) tooltip(e.help.c_str());
    float bottom = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(x0 + w - kSourceBadgeW - 12 - e.value_w, y0));
    ImGui::BeginGroup();
    e.value(y0);
    ImGui::EndGroup();
    ImGui::SetCursorPos(ImVec2(x0 + w - kSourceBadgeW, y0 + 5));
    parts::sourceBadge(e.badge);
    bottom = std::max(bottom, y0 + 32);
    bool back = false;
    if (!e.was.empty()) {
        ImGui::SetCursorPos(ImVec2(x0, bottom + 2));
        text(e.was.c_str(), fonts().regular, size::Help, col::TextMuted);
        ImGui::SameLine(0, 10);
        ImGui::SetCursorPosY(bottom);
        back = linkButton("Use it");
        bottom = ImGui::GetCursorPosY();
    }
    if (!e.note.empty()) {
        ImGui::SetCursorPos(ImVec2(x0, bottom + 2));
        ImGui::PushTextWrapPos(x0 + w - kSourceBadgeW - 12);
        text(e.note.c_str(), fonts().regular, size::Help, col::Warn, true);
        ImGui::PopTextWrapPos();
        bottom = ImGui::GetCursorPosY();
    }
    ImGui::SetCursorPos(ImVec2(x0, bottom + 6));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::PopID();
    return back;
}

parts::SourceBadge badgeOf(PrintRow::Source s) {
    switch (s) {
        case PrintRow::Source::File: return parts::SourceBadge::File;
        case PrintRow::Source::Yours: return parts::SourceBadge::Yours;
        default: return parts::SourceBadge::Default;
    }
}

// What a print field is, for its tooltip.
std::string fieldTip(const PrintField& f) {
    if (f.key == "adaptive")
        return "Thickest layer, mm. A value snaps to the nearest height the layer height allows; the layer height itself is off.";
    if (f.key == "light_depth") return strf("Light depth, mm: from %g to %g", f.lo, f.hi);
    const char* what = f.key == "bottom_layers"        ? "Bottom layers"
                       : f.key == "bottom_exposure"    ? "Bottom exposure, s"
                       : f.key == "transition_layers"  ? "Transition layers"
                       : f.key == "wait_before_lift"   ? "Wait before lift (after the light), s"
                       : f.key == "wait_after_retract" ? "Wait after retract (before the light), s"
                       : f.key == "bottom_wait_before_lift"   ? "Bottom layers: wait before lift (after the light), s"
                       : f.key == "bottom_wait_after_retract" ? "Bottom layers: wait after retract (before the light), s"
                                                       : "";
    const std::string range = strf("from %g to %g", f.lo, f.hi);
    return *what ? std::string(what) + ": " + range : "From " + range.substr(5);
}

// Counts and waits are short; the rest take the advanced page's width.
float fieldWidth(const PrintField& f) {
    const bool narrow = f.key == "bottom_layers" || f.key == "transition_layers" || f.key.rfind("wait_", 0) == 0 ||
                        f.key.rfind("bottom_wait_", 0) == 0;
    return narrow ? 50 : 62;
}

// The print settings card's rows.
void printCard(App& app, const Resin& r, const fruit::GooHeader* sample) {
    const bool file = sample && !r.sample_path.empty();
    const auto rows = printRows(r, sample, app.printer());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const PrintRow& row = rows[i];
        EditRow e;
        e.label = row.label;
        e.badge = badgeOf(row.source);
        e.note = row.note;
        if (row.source == PrintRow::Source::Yours && !row.base.empty()) {
            // Values no print file has are FruitSlicer's own; the bottom layers go back to the other layers' waits.
            const bool ours = row.key == "density" || row.key == "adaptive" || row.key == "light_depth" ||
                              row.key == "height_correction";
            const bool as_others = row.key == "bottom_waits" && i > 0 && rows[i - 1].key == "waits" &&
                                   rows[i - 1].value == row.base;
            e.was = (as_others ? "The other layers': " : file && !ours ? "File: " : "FruitSlicer's: ") + row.base;
        }
        const float gap = 6;
        for (const PrintField& f : row.fields) {
            e.value_w += fieldWidth(f) + gap;
            if (*f.suffix) e.value_w += textWidth(f.suffix, fonts().regular, size::Body) + gap;
        }
        if (!row.after.empty()) e.value_w += textWidth(row.after.c_str(), fonts().regular, size::Body) + gap;
        e.value_w -= gap;
        const std::string id = r.id;
        e.value = [&app, &row, id, gap](float row_y) {
            for (std::size_t k = 0; k < row.fields.size(); ++k) {
                const PrintField& f = row.fields[k];
                if (k > 0) ImGui::SameLine(0, gap);
                ImGui::SetCursorPosY(row_y);
                double v = f.value;
                const char* fmt = v > 0 || f.lo <= 0 ? "%g" : "—";
                if (numberField(f.key.c_str(), &v, fieldWidth(f), fmt, f.lo, f.hi))
                    app.setResinPrintValue(id, f.key, v);
                tooltip(fieldTip(f).c_str());
                if (*f.suffix) {
                    ImGui::SameLine(0, gap);
                    inlineWords(f.suffix, col::TextSecondary, row_y);
                }
            }
            if (!row.after.empty()) {
                ImGui::SameLine(0, gap);
                inlineWords(row.after.c_str(), col::TextSecondary, row_y);
            }
        };
        if (editRow(e, i == 0)) app.resetResinPrintRow(r.id, row.key);
    }
}

// The support values card's rows: the advanced page's rows, for the resin.
void supportCard(App& app, const Resin& r) {
    const SupportChoices c = resinChoices(r.profile);
    const fruit::SupportConfig cfg = supportConfig(fruit::Placement{}, c);
    const fruit::TreeConfig tc = treeConfig(c);
    const fruit::Placement place;
    AdvancedContext x{cfg, tc, c, place};
    x.raft_layer_height = app.printer().layer_height;
    bool first = true;
    for (const char* label : kResinSupportRows) {
        const AdvancedSetting* s = advancedRow(label);
        if (!s) continue;
        EditRow e;
        e.label = s->label;
        e.help = s->help ? s->help(x) : "";
        e.value_w = parts::advancedValueWidth(*s, x);
        const std::string id = r.id;
        e.value = [&app, s, &x, id](float row_y) {
            parts::advancedValue(*s, x, row_y,
                                 [&app, &id](const char* key, double v) { app.setResinSupportValue(id, key, v); });
        };
        const bool yours = resinChanged(*s, r.profile);
        const bool measured = measuredByResin(*s, c);
        using B = parts::SourceBadge;
        e.badge = yours      ? B::Yours
                  : measured ? B::Measured
                  : s->source == AdvancedSetting::Source::Borrowed   ? B::Borrowed
                  : s->source == AdvancedSetting::Source::Assumption ? B::Assumption
                                                                     : B::Default;
        if (yours) {
            // What the row reads as without the resin's own value.
            const SupportChoices before = resinChoices(withoutResinRow(r.profile, *s));
            const char* from = measuredByResin(*s, before)                  ? "Measured: "
                               : s->source == AdvancedSetting::Source::Borrowed ? "Borrowed: "
                                                                                : "FruitSlicer's: ";
            e.was = from + advancedValueText(*s, supportConfig(fruit::Placement{}, before), treeConfig(before));
        }
        if (editRow(e, first)) app.resetResinSupportRow(r.id, s->label);
        first = false;
    }
}

void list(App& app) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::Rail);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 24));
    ImGui::BeginChild("resin_list", ImVec2(kListW, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    dl->AddLine(ImVec2(wp.x + ws.x - 1, wp.y), ImVec2(wp.x + ws.x - 1, wp.y + ws.y), col::Border);
    const float w = ImGui::GetContentRegionAvail().x;
    {
        const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
        text("Resins", fonts().bold, size::PageTitle - 6, col::Text);
        const float lw = textWidth("Printer", fonts().semibold, size::Body);
        ImGui::SetCursorPos(ImVec2(x0 + w - lw, y0 + 6));
        if (linkButton("Printer")) app.setScreen(Screen::Printer);
        ImGui::SetCursorPos(ImVec2(x0, y0 + 40));
        ImGui::Dummy(ImVec2(0, 0));
    }
    const std::string for_new = app.newJobResin().id;
    for (const Resin& r : app.resins()) {
        ImGui::PushID(r.id.c_str());
        const bool on = app.resinSelected() == r.id;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("r", ImVec2(w, 52))) app.resinSelected() = r.id;
        if (on || ImGui::IsItemHovered()) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 52), on ? col::Card : col::Panel, 10);
        if (on) dl->AddRect(p, ImVec2(p.x + w, p.y + 52), col::BorderStrong, 10);
        ImGui::PushFont(fonts().semibold, size::Body);
        dl->AddText(ImVec2(p.x + 12, p.y + 9), col::Text, r.name.c_str());
        ImGui::PopFont();
        const auto sample = app.resinSample(r);
        const std::string sub = resinSummary(r, sample.get()) + (r.id == for_new ? " · new jobs" : "");
        ImGui::PushFont(fonts().regular, size::Help);
        dl->AddText(ImVec2(p.x + 12, p.y + 29), col::TextMuted, sub.c_str());
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 2));
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 8));
    if (secondaryButton("New from a SatelLite .goo…", ImVec2(w, 34))) app.newResinFromGoo();
    if (secondaryButton("Import a profile (.json)…", ImVec2(w, 34))) app.importResin();
    tooltip("A profile written by `fruitslicer fit` from scored calibration plates.");
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void deleteDialog(App& app) {
    std::string& id = app.resinToDelete();
    const Resin* r = app.resinById(id);
    if (!r) id.clear();
    if (r && !ImGui::IsPopupOpen("delete_resin")) ImGui::OpenPopup("delete_resin");
    if (!ImGui::IsPopupOpen("delete_resin")) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + vp->Size.y / 2), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460, 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, col::Panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22, 20));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14);
    if (ImGui::BeginPopupModal("delete_resin", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                                            ImGuiWindowFlags_NoMove)) {
        if (!r) {  // answered elsewhere (a script)
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
            return;
        }
        text(("Delete " + r->name + "?").c_str(), fonts().bold, size::DialogTitle, col::Text);
        ImGui::Dummy(ImVec2(0, 4));
        text("Jobs printed in it keep their own copy of its values. Its file is removed; this can't be undone.",
             fonts().regular, size::Body, col::TextSecondary, true);
        ImGui::Dummy(ImVec2(0, 12));
        const float w = ImGui::GetContentRegionAvail().x;
        const float x0 = ImGui::GetCursorPosX();
        ImGui::SetCursorPosX(x0 + w - 100 - 10 - 90);
        if (secondaryButton("Cancel", ImVec2(90, 36)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            id.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(0, 10);
        if (primaryButton("Delete", ImVec2(100, 36))) {
            app.deleteResin(r->id);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

}  // namespace

void resins(App& app) {
    list(app);
    ImGui::SameLine(0, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28, 24));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::App);
    ImGui::BeginChild("resin_page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 8));
    const Resin* found = app.resinById(app.resinSelected());
    const Resin r = found ? *found : app.newJobResin();
    if (!found) app.resinSelected() = r.id;
    const float w = std::min(ImGui::GetContentRegionAvail().x, 1000.0f);
    const bool for_new = app.newJobResin().id == r.id;

    // ---- Name, state and what to do ----
    static std::string renaming;  // the id being renamed
    static char name_buf[96];
    {
        const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
        if (renaming == r.id) {
            ImGui::SetNextItemWidth(320);
            ImGui::PushFont(fonts().semibold, size::Section);
            ImGui::SetKeyboardFocusHere();
            const bool done = ImGui::InputText("##name", name_buf, sizeof name_buf, ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::PopFont();
            if (done) {
                app.renameResin(r.id, name_buf);
                renaming.clear();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) renaming.clear();
        } else {
            text(r.name.c_str(), fonts().bold, size::PageTitle - 6, col::Text);
            ImGui::SameLine(0, 12);
            ImGui::SetCursorPosY(y0 + 8);
            if (linkButton("Rename")) {
                renaming = r.id;
                std::snprintf(name_buf, sizeof name_buf, "%s", r.name.c_str());
            }
        }
        ImGui::SetCursorPos(ImVec2(x0, y0 + 36));
        const std::string state = resinState(r) == "Defaults" ? "Defaults · not calibrated" : resinState(r);
        text((state + (for_new ? " · used for new jobs" : "")).c_str(), fonts().regular, size::Body, col::TextMuted);
        text("Change any value below. New jobs on this resin use it; jobs already made keep the values they started "
             "with.",
             fonts().regular, size::Help, col::TextMuted);
        // The open job in this resin, with other values than it has now.
        const auto& doc = app.document();
        if (doc && doc->supports.choices.resin == r.name) {
            const PrintSettings now = printSettingsFor(r);
            const bool differs = doc->supports.choices.profile != r.profile ||
                                 doc->print.settings.sample_path != now.sample_path ||
                                 !(static_cast<const PrintEdits&>(doc->print.settings) ==
                                   static_cast<const PrintEdits&>(now));
            if (differs) {
                text("The open job started with other values.", fonts().regular, size::Help, col::Warn);
                ImGui::SameLine(0, 8);
                if (linkButton("Use these in it")) app.useResinForJob(r.id);
            }
        }
        const float after = ImGui::GetCursorPosY();
        // Right: Calibrate, Use for new jobs, Duplicate, Delete…
        float right = x0 + w;
        const float dw = textWidth("Delete…", fonts().semibold, size::Body) + 28;
        right -= dw;
        ImGui::SetCursorPos(ImVec2(right, y0 + 2));
        if (secondaryButton("Delete…", ImVec2(dw, 34), app.resins().size() > 1)) app.resinToDelete() = r.id;
        if (app.resins().size() <= 1) tooltip("One resin always stays.");
        const float pw = textWidth("Duplicate", fonts().semibold, size::Body) + 28;
        right -= pw + 8;
        ImGui::SetCursorPos(ImVec2(right, y0 + 2));
        if (secondaryButton("Duplicate", ImVec2(pw, 34))) app.duplicateResin(r.id);
        if (!for_new) {
            const float uw = textWidth("Use for new jobs", fonts().semibold, size::Body) + 28;
            right -= uw + 8;
            ImGui::SetCursorPos(ImVec2(right, y0 + 2));
            if (secondaryButton("Use for new jobs", ImVec2(uw, 34))) app.setNewJobResin(r.id);
        }
        const float cw = textWidth("Calibrate", fonts().semibold, size::Body);
        right -= cw + 18;
        ImGui::SetCursorPos(ImVec2(right, y0 + 10));
        if (linkButton("Calibrate")) {
            app.setNewJobResin(r.id);  // the plates calibrate the resin new jobs use
            app.setScreen(Screen::Calibrate);
        }
        tooltip("Calibration measures the resin new jobs use; this makes it that one.");
        ImGui::SetCursorPos(ImVec2(x0, after + 12));
        ImGui::Dummy(ImVec2(0, 0));
    }

    const float gap = 16, cw = std::floor((w - gap) / 2);
    // ---- Print settings ----
    const auto sample = app.resinSample(r);
    beginCard("print", ImVec2(cw, 0), 16, col::Panel, col::Border);
    {
        const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
        const float iw = ImGui::GetContentRegionAvail().x;
        text("Print settings", fonts().semibold, size::Section, col::Text);
        const char* label = r.sample_path.empty() ? "Choose a SatelLite .goo…" : "Replace from another .goo…";
        const float bw = textWidth(label, fonts().semibold, size::Body) + 28;
        ImGui::SetCursorPos(ImVec2(x0 + iw - bw, y0 - 4));
        if (secondaryButton(label, ImVec2(bw, 30))) app.replaceResinSample(r.id);
        ImGui::SetCursorPos(ImVec2(x0, y0 + 30));
        std::string from = printSummary(r);
        if (sample) from += ", " + sample->software_name + " " + sample->software_version;
        if (r.sample_path.empty() && r.print.exposure <= 0)
            from = "None yet. Choose a SatelLite .goo, or type the exposure times below.";
        text(from.c_str(), fonts().regular, size::Body, col::TextMuted, true);
        ImGui::Dummy(ImVec2(0, 2));
        printCard(app, r, sample.get());
    }
    endCard();
    ImGui::SameLine(0, gap);
    // ---- Support values ----
    beginCard("support", ImVec2(cw, 0), 16, col::Panel, col::Border);
    {
        text("Support values for this resin", fonts().semibold, size::Section, col::Text);
        text(resinState(r) == "Defaults" ? "FruitSlicer's starting values until the resin test plates are scored."
                                         : "Measured on the resin test plates; the rest are FruitSlicer's own.",
             fonts().regular, size::Body, col::TextMuted, true);
        ImGui::Dummy(ImVec2(0, 2));
        supportCard(app, r);
        // The job's advanced page shows these, when the open job is in this resin.
        const auto& doc = app.document();
        if (doc && doc->supports.choices.resin == r.name && doc->supports.choices.profile == r.profile) {
            ImGui::Dummy(ImVec2(0, 4));
            if (linkButton("All support settings")) {
                app.setScreen(Screen::Job);
                app.goToStep(Step::Supports);
                app.showAdvanced(true);
            }
        }
    }
    endCard();
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    deleteDialog(app);
}

}  // namespace gui::screens
